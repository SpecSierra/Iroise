/*
 * Iroise: Ladybird engine connection.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdEngine.h"

#include "DownloadModel.h"
#include "LadybirdGeolocation.h"
#include "browserpaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// The engine cycles through a few buffers per view; keep enough mapped for a handful of live tabs.
static const int maximumMappings = 24;

LadybirdEngine* LadybirdEngine::instance()
{
    static LadybirdEngine* engine = new LadybirdEngine(QCoreApplication::instance());
    return engine;
}

LadybirdEngine::LadybirdEngine(QObject* parent)
    : QObject(parent)
{
    start();
}

LadybirdEngine::~LadybirdEngine()
{
    if (m_connection >= 0)
        ::close(m_connection);
    if (m_listener >= 0)
        ::close(m_listener);
    if (!m_socketPath.isEmpty())
        ::unlink(m_socketPath.toLocal8Bit().constData());
    m_process.terminate();
    m_process.waitForFinished(2000);
    for (const Mapping& mapping : m_mappings) {
        ::munmap(mapping.pixels, mapping.length);
        ::close(mapping.fd);
    }
}

void LadybirdEngine::shutdown()
{
    m_gaveUp = true;
    if (m_connectionNotifier)
        m_connectionNotifier->setEnabled(false);
    // The engine ends its run when the connection closes.
    if (m_connection >= 0)
        ::close(m_connection);
    m_connection = -1;
    if (!m_socketPath.isEmpty())
        ::unlink(m_socketPath.toLocal8Bit().constData());
    m_socketPath.clear();
    if (!m_process.waitForFinished(1000))
        m_process.terminate();
}

void LadybirdEngine::start()
{
    // The engine connects back to this socket, which is how it knows to run without a window of its own.
    QString runtimeDirectory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDirectory.isEmpty())
        runtimeDirectory = QDir::tempPath();
    m_socketPath = QStringLiteral("%1/iroise-engine-%2.socket").arg(runtimeDirectory).arg(QCoreApplication::applicationPid());
    QByteArray path = m_socketPath.toLocal8Bit();
    ::unlink(path.constData());

    m_listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path.constData(), sizeof(address.sun_path) - 1);
    if (m_listener < 0 || ::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(m_listener, 1) < 0) {
        qWarning("LadybirdEngine: cannot listen on %s: %s", path.constData(), strerror(errno));
        return;
    }
    m_listenerNotifier = new QSocketNotifier(m_listener, QSocketNotifier::Read, this);
    connect(m_listenerNotifier, &QSocketNotifier::activated, this, &LadybirdEngine::acceptEngine);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral(LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE), m_socketPath);
    // The phone's kernel has no Landlock; this keeps the seccomp half of the engine's sandbox.
    environment.insert(QStringLiteral("LADYBIRD_UNSAFE_ALLOW_MISSING_LANDLOCK"), QStringLiteral("1"));
    // Test runs keep everything the engine writes out of the user's home directory.
    QString engineHome = QString::fromLocal8Bit(qgetenv("IROISE_ENGINE_HOME"));
    if (!engineHome.isEmpty()) {
        QDir().mkpath(engineHome);
        environment.insert(QStringLiteral("HOME"), engineHome);
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), engineHome + QStringLiteral("/.cache"));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), engineHome + QStringLiteral("/.config"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), engineHome + QStringLiteral("/.local/share"));
    } else {
        // The engine keeps its profile, cache and settings in "Ladybird" directories of its own. Put those inside
        // the browser's directories: they are the ones the sandbox lets us write to, and removing the browser's
        // data then removes the engine's with it.
        QString data = BrowserPaths::dataLocation() + QStringLiteral("/engine");
        QString cache = BrowserPaths::cacheLocation() + QStringLiteral("/engine");
        QDir().mkpath(data + QStringLiteral("/config"));
        QDir().mkpath(cache);
        environment.insert(QStringLiteral("XDG_DATA_HOME"), data);
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), data + QStringLiteral("/config"));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), cache);
    }
    // The engine saves downloads into this folder without asking, and gives up if it is not there.
    QString downloads = engineHome.isEmpty() ? BrowserPaths::downloadLocation() : engineHome + QStringLiteral("/Downloads");
    QDir().mkpath(downloads);
    environment.insert(QStringLiteral("XDG_DOWNLOAD_DIR"), downloads);
    // The engine paces its pages at 60 frames a second unless told the screen's rate (the Jolla Phone 2's is 90).
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        if (!qgetenv("IROISE_LOG_ENGINE").isEmpty())
            qWarning("LadybirdEngine: the screen refreshes at %.1f Hz", screen->refreshRate());
        // NB: One set already wins (to compare rates).
        if (screen->refreshRate() >= 30 && !environment.contains(QStringLiteral("LADYBIRD_SFOS_REFRESH_RATE")))
            environment.insert(QStringLiteral("LADYBIRD_SFOS_REFRESH_RATE"), QString::number(screen->refreshRate()));
    }
    m_process.setProcessEnvironment(environment);
    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    launch();
}

void LadybirdEngine::launch()
{
    // The engine is the app's own executable in another role (see launcher/launcher.c).
    m_process.start(QCoreApplication::applicationFilePath(), QStringList { QStringLiteral("--engine-role"), QStringLiteral("Ladybird") });
}

// The engine went away (it crashed, or was killed). Every tab says so, and a new engine is started for them, which
// gets the app's settings again; each view then makes itself anew, and reloading a tab loads its page there. An
// engine that keeps dying at once is not started again.
void LadybirdEngine::engineGone()
{
    qWarning("LadybirdEngine: the engine went away");
    m_connectionNotifier->setEnabled(false);
    m_connectionNotifier->deleteLater();
    m_connectionNotifier = nullptr;
    ::close(m_connection);
    m_connection = -1;
    emit connectedChanged();

    for (const Mapping& mapping : m_mappings) {
        for (LadybirdEngineClient* client : m_views)
            client->engineBufferGone(mapping.id);
        ::munmap(mapping.pixels, mapping.length);
        ::close(mapping.fd);
    }
    m_mappings.clear();
    m_unclaimedViews.clear();
    LadybirdGeolocation::instance()->stopAll();

    lb_embed_header crashed;
    memset(&crashed, 0, sizeof(crashed));
    crashed.type = LB_EMBED_CRASHED;
    crashed.a = 1;
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        crashed.view = it.key();
        it.value()->engineMessage(crashed, QString());
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_recentRestarts.append(now);
    while (!m_recentRestarts.isEmpty() && now - m_recentRestarts.first() > 60000)
        m_recentRestarts.removeFirst();
    if (m_recentRestarts.size() > 3) {
        qWarning("LadybirdEngine: the engine keeps going away; not starting it again");
        m_gaveUp = true;
        m_queue.clear();
        return;
    }
    if (!m_process.waitForFinished(2000)) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
    m_restarting = true;
    launch();
}

void LadybirdEngine::acceptEngine()
{
    int connection = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection < 0)
        return;
    // NB: Whoever connects first would be taken for the engine and be sent everything, the saved logins filled into
    //     pages included. Only the engine process we started may connect (it is the app's executable, not exec'd
    //     anew, so the process is the one QProcess started).
    ucred peer;
    socklen_t peerLength = sizeof(peer);
    if (m_connection >= 0 || ::getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &peer, &peerLength) < 0
        || peer.pid != static_cast<pid_t>(m_process.processId())) {
        qWarning("LadybirdEngine: refused a connection that is not from the engine");
        ::close(connection);
        return;
    }
    m_connection = connection;
    m_connectionNotifier = new QSocketNotifier(m_connection, QSocketNotifier::Read, this);
    connect(m_connectionNotifier, &QSocketNotifier::activated, this, &LadybirdEngine::receive);

    // NB: A new engine knows nothing yet: the app's settings go first, then the views make themselves anew.
    if (m_restarting) {
        m_queue.clear();
        for (const Queued& setting : m_settings)
            write(setting.header, setting.text);
    }
    QVector<Queued> queue;
    queue.swap(m_queue);
    for (const Queued& queued : queue)
        write(queued.header, queued.text);
    emit connectedChanged();
    if (m_restarting) {
        m_restarting = false;
        emit restarted();
    }
}

quint32 LadybirdEngine::addView(LadybirdEngineClient* client, const QSize& size, qreal devicePixelRatio, bool isPrivate)
{
    quint32 view = m_nextView++;
    m_views.insert(view, client);
    send(view, LB_EMBED_CREATE_VIEW, size.width(), size.height(), isPrivate ? 1 : 0, 0, devicePixelRatio);
    return view;
}

void LadybirdEngine::claimView(quint32 view, LadybirdEngineClient* client)
{
    auto early = m_unclaimedViews.take(view);
    m_views.insert(view, client);
    for (const auto& message : early)
        client->engineMessage(message.first, message.second);
}

void LadybirdEngine::forgetView(quint32 view)
{
    m_views.remove(view);
}

bool LadybirdEngine::isSetting(lb_embed_message_type type)
{
    switch (type) {
    case LB_EMBED_SET_AD_BLOCKING:
    case LB_EMBED_SET_COOKIE_BANNER_BLOCKING:
    case LB_EMBED_SET_BLOCKER_FILTERS:
    case LB_EMBED_SET_SITE_OPTIONS:
    case LB_EMBED_SET_CERTIFICATE_EXCEPTIONS:
    case LB_EMBED_SET_DARK_MODE:
        return true;
    default:
        return false;
    }
}

void LadybirdEngine::removeView(quint32 view)
{
    bool unclaimed = m_unclaimedViews.remove(view);
    if (m_views.remove(view) || unclaimed)
        send(view, LB_EMBED_DESTROY_VIEW);
    // Its buffers are its own; the view (whose textures of them go with it) does not need telling.
    for (int i = m_mappings.size() - 1; i >= 0; --i) {
        if (m_mappings[i].view != view)
            continue;
        ::munmap(m_mappings[i].pixels, m_mappings[i].length);
        ::close(m_mappings[i].fd);
        m_mappings.remove(i);
    }
}

void LadybirdEngine::send(quint32 view, lb_embed_message_type type, int a, int b, int c, int d, double scale, const QByteArray& text)
{
    lb_embed_header header;
    memset(&header, 0, sizeof(header));
    header.type = type;
    header.view = view;
    header.a = a;
    header.b = b;
    header.c = c;
    header.d = d;
    header.scale = scale;
    // IROISE_LOG_ENGINE=1 traces what the interface asks of the engine, except the touch stream.
    static const bool logMessages = !qgetenv("IROISE_LOG_ENGINE").isEmpty();
    if (logMessages && type != LB_EMBED_TOUCH)
        qWarning("LadybirdEngine: view %u message %d a=%d b=%d c=%d d=%d scale=%.3f text=%s", view, int(type), a, b, c, d, scale, text.left(80).constData());
    // NB: Refused, not cut: a cut JSON text would be read as nothing (site options would all be cleared).
    if (text.size() > LB_EMBED_TEXT_MAX) {
        qWarning("LadybirdEngine: message %d is %d bytes, more than the engine takes; not sent", int(type), text.size());
        return;
    }
    const QByteArray& payload = text;
    header.text_length = static_cast<uint32_t>(payload.size());

    // The app's settings are kept for an engine started after this one went away.
    if (view == 0 && isSetting(type)) {
        Queued setting;
        setting.header = header;
        setting.text = payload;
        m_settings.insert(type, setting);
    }
    if (m_gaveUp)
        return;
    if (m_connection < 0) {
        Queued queued;
        queued.header = header;
        queued.text = payload;
        m_queue.append(queued);
        return;
    }
    write(header, payload);
}

void LadybirdEngine::write(const lb_embed_header& header, const QByteArray& text)
{
    iovec parts[2];
    parts[0].iov_base = const_cast<lb_embed_header*>(&header);
    parts[0].iov_len = sizeof(header);
    parts[1].iov_base = const_cast<char*>(text.constData());
    parts[1].iov_len = static_cast<size_t>(text.size());
    msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = parts;
    message.msg_iovlen = text.isEmpty() ? 1 : 2;
    if (::sendmsg(m_connection, &message, MSG_NOSIGNAL) < 0)
        qWarning("LadybirdEngine: sending to the engine failed: %s", strerror(errno));
}

void LadybirdEngine::receive()
{
    struct {
        lb_embed_header header;
        char text[LB_EMBED_TEXT_MAX];
    } buffer;

    for (;;) {
        iovec part;
        part.iov_base = &buffer;
        part.iov_len = sizeof(buffer);
        char control[CMSG_SPACE(sizeof(int))];
        msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);

        ssize_t length = ::recvmsg(m_connection, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (length < 0 && errno == EAGAIN)
            return;
        if (length <= 0) {
            engineGone();
            return;
        }

        int fd = -1;
        for (cmsghdr* entry = CMSG_FIRSTHDR(&message); entry; entry = CMSG_NXTHDR(&message, entry)) {
            if (entry->cmsg_level == SOL_SOCKET && entry->cmsg_type == SCM_RIGHTS)
                memcpy(&fd, CMSG_DATA(entry), sizeof(int));
        }
        if (message.msg_flags & MSG_TRUNC) {
            qWarning("LadybirdEngine: dropped a message longer than the protocol allows");
        } else if (static_cast<size_t>(length) >= sizeof(lb_embed_header)) {
            const lb_embed_header& header = buffer.header;
            if (header.type == LB_EMBED_FRAME) {
                size_t textLength = qMin(static_cast<size_t>(header.text_length), static_cast<size_t>(length) - sizeof(lb_embed_header));
                handleFrame(header, fd, QByteArray(buffer.text, static_cast<int>(textLength)).toULongLong());
            } else if (header.type == LB_EMBED_DOWNLOAD) {
                size_t textLength = qMin(static_cast<size_t>(header.text_length), static_cast<size_t>(length) - sizeof(lb_embed_header));
                DownloadModel::instance()->report(header.a, header.b, QByteArray(buffer.text, static_cast<int>(textLength)));
            } else if (header.type == LB_EMBED_GEOLOCATION_START) {
                LadybirdGeolocation::instance()->start(header.a, header.b != 0);
            } else if (header.type == LB_EMBED_GEOLOCATION_STOP) {
                LadybirdGeolocation::instance()->stop(header.a);
            } else if (header.type == LB_EMBED_NEW_VIEW && header.view >= LB_EMBED_FIRST_ENGINE_VIEW) {
                m_unclaimedViews.insert(header.view, {});
                emit viewOffered(header.view);
            } else if (LadybirdEngineClient* client = m_views.value(header.view)) {
                size_t textLength = qMin(static_cast<size_t>(header.text_length), static_cast<size_t>(length) - sizeof(lb_embed_header));
                client->engineMessage(header, QString::fromUtf8(buffer.text, static_cast<int>(textLength)));
            } else if (m_unclaimedViews.contains(header.view)) {
                // NB: Frames are not kept: the claimant sends its size, and the engine paints for it again.
                size_t textLength = qMin(static_cast<size_t>(header.text_length), static_cast<size_t>(length) - sizeof(lb_embed_header));
                m_unclaimedViews[header.view].append(qMakePair(header, QString::fromUtf8(buffer.text, static_cast<int>(textLength))));
            }
        }
        if (fd >= 0)
            ::close(fd);
    }
}

const LadybirdEngine::Mapping* LadybirdEngine::mappingFor(quint32 view, int fd, size_t length, quint64 identity)
{
    // Every frame message carries a fresh duplicate of the buffer's file descriptor, so recognize the buffer
    // behind it by the identity the engine gives it, or else by its inode. NB: Before Linux 5.3 every dma-buf has
    // the same inode (the Xperia 10 II's 4.14), and the first buffer would be shown for all of them.
    struct stat status;
    if (::fstat(fd, &status) < 0)
        return nullptr;
    for (Mapping& mapping : m_mappings) {
        bool same = identity ? mapping.identity == identity : (mapping.device == status.st_dev && mapping.inode == status.st_ino);
        if (same && mapping.length == length) {
            mapping.lastUsed = m_frameSerial;
            return &mapping;
        }
    }

    if (m_mappings.size() >= maximumMappings) {
        int oldest = 0;
        for (int i = 1; i < m_mappings.size(); ++i) {
            if (m_mappings[i].lastUsed < m_mappings[oldest].lastUsed)
                oldest = i;
        }
        for (LadybirdEngineClient* client : m_views)
            client->engineBufferGone(m_mappings[oldest].id);
        ::munmap(m_mappings[oldest].pixels, m_mappings[oldest].length);
        ::close(m_mappings[oldest].fd);
        m_mappings.remove(oldest);
    }

    // The descriptor that came with the message is closed once the message is handled; the view needs one that
    // lasts as long as the buffer is in use, to hand to the GPU.
    int ownFd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (ownFd < 0)
        return nullptr;
    void* pixels = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        qWarning("LadybirdEngine: cannot map a frame: %s", strerror(errno));
        ::close(ownFd);
        return nullptr;
    }
    Mapping mapping;
    mapping.fd = ownFd;
    mapping.id = m_nextBufferId++;
    mapping.device = status.st_dev;
    mapping.inode = status.st_ino;
    mapping.identity = identity;
    mapping.view = view;
    mapping.pixels = static_cast<uchar*>(pixels);
    mapping.length = length;
    mapping.lastUsed = m_frameSerial;
    m_mappings.append(mapping);
    return &m_mappings.last();
}

void LadybirdEngine::handleFrame(const lb_embed_header& header, int fd, quint64 identity)
{
    LadybirdEngineClient* client = m_views.value(header.view);
    if (!client || fd < 0 || header.c < 1 || header.d < 1 || header.e < static_cast<uint32_t>(header.c) * 4)
        return;
    ++m_frameSerial;
    const Mapping* mapping = mappingFor(header.view, fd, static_cast<size_t>(header.e) * static_cast<size_t>(header.d), identity);
    if (!mapping)
        return;

    LadybirdFrame frame;
    frame.pixels = mapping->pixels;
    frame.bufferFd = mapping->fd;
    frame.bufferId = mapping->id;
    frame.pitch = static_cast<int>(header.e);
    frame.bufferSize = QSize(header.c, header.d);
    frame.paintedSize = QSize(qMin(header.a, header.c), qMin(header.b, header.d));
    frame.serial = m_frameSerial;
    client->engineFrame(frame);
}
