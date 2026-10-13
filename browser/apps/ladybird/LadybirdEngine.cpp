/*
 * Pacific: Ladybird engine connection.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <errno.h>
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
    for (const Mapping& mapping : m_mappings)
        ::munmap(mapping.pixels, mapping.length);
}

void LadybirdEngine::start()
{
    // The engine connects back to this socket, which is how it knows to run without a window of its own.
    QString runtimeDirectory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDirectory.isEmpty())
        runtimeDirectory = QDir::tempPath();
    m_socketPath = QStringLiteral("%1/pacific-engine-%2.socket").arg(runtimeDirectory).arg(QCoreApplication::applicationPid());
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

    QString enginePath = QString::fromLocal8Bit(qgetenv("PACIFIC_ENGINE"));
    if (enginePath.isEmpty())
        enginePath = QStringLiteral("/usr/libexec/pacific/bin/Ladybird");

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral(LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE), m_socketPath);
    // The phone's kernel has no Landlock; this keeps the seccomp half of the engine's sandbox.
    environment.insert(QStringLiteral("LADYBIRD_UNSAFE_ALLOW_MISSING_LANDLOCK"), QStringLiteral("1"));
    // Test runs keep everything the engine writes out of the user's home directory.
    QString engineHome = QString::fromLocal8Bit(qgetenv("PACIFIC_ENGINE_HOME"));
    if (!engineHome.isEmpty()) {
        QDir().mkpath(engineHome);
        environment.insert(QStringLiteral("HOME"), engineHome);
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), engineHome + QStringLiteral("/.cache"));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), engineHome + QStringLiteral("/.config"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), engineHome + QStringLiteral("/.local/share"));
    }
    m_process.setProcessEnvironment(environment);
    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    m_process.start(enginePath, QStringList());
}

void LadybirdEngine::acceptEngine()
{
    int connection = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection < 0)
        return;
    if (m_connection >= 0) {
        ::close(connection);
        return;
    }
    m_connection = connection;
    m_connectionNotifier = new QSocketNotifier(m_connection, QSocketNotifier::Read, this);
    connect(m_connectionNotifier, &QSocketNotifier::activated, this, &LadybirdEngine::receive);

    QVector<Queued> queue;
    queue.swap(m_queue);
    for (const Queued& queued : queue)
        write(queued.header, queued.text);
    emit connectedChanged();
}

quint32 LadybirdEngine::addView(LadybirdEngineClient* client, const QSize& size, qreal devicePixelRatio)
{
    quint32 view = m_nextView++;
    m_views.insert(view, client);
    send(view, LB_EMBED_CREATE_VIEW, size.width(), size.height(), 0, 0, devicePixelRatio);
    return view;
}

void LadybirdEngine::removeView(quint32 view)
{
    if (m_views.remove(view))
        send(view, LB_EMBED_DESTROY_VIEW);
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
    QByteArray payload = text.left(LB_EMBED_TEXT_MAX);
    header.text_length = static_cast<uint32_t>(payload.size());

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
            qWarning("LadybirdEngine: the engine went away");
            m_connectionNotifier->setEnabled(false);
            ::close(m_connection);
            m_connection = -1;
            emit connectedChanged();
            return;
        }

        int fd = -1;
        for (cmsghdr* entry = CMSG_FIRSTHDR(&message); entry; entry = CMSG_NXTHDR(&message, entry)) {
            if (entry->cmsg_level == SOL_SOCKET && entry->cmsg_type == SCM_RIGHTS)
                memcpy(&fd, CMSG_DATA(entry), sizeof(int));
        }
        if (static_cast<size_t>(length) >= sizeof(lb_embed_header)) {
            const lb_embed_header& header = buffer.header;
            if (header.type == LB_EMBED_FRAME) {
                handleFrame(header, fd);
            } else if (LadybirdEngineClient* client = m_views.value(header.view)) {
                size_t textLength = qMin(static_cast<size_t>(header.text_length), static_cast<size_t>(length) - sizeof(lb_embed_header));
                client->engineMessage(header, QString::fromUtf8(buffer.text, static_cast<int>(textLength)));
            }
        }
        if (fd >= 0)
            ::close(fd);
    }
}

const uchar* LadybirdEngine::mappingFor(int fd, size_t length)
{
    // Every frame message carries a fresh duplicate of the buffer's file descriptor, so recognize the buffer
    // behind it by its inode.
    struct stat status;
    if (::fstat(fd, &status) < 0)
        return nullptr;
    for (Mapping& mapping : m_mappings) {
        if (mapping.device == status.st_dev && mapping.inode == status.st_ino && mapping.length == length) {
            mapping.lastUsed = m_frameSerial;
            return mapping.pixels;
        }
    }

    if (m_mappings.size() >= maximumMappings) {
        int oldest = 0;
        for (int i = 1; i < m_mappings.size(); ++i) {
            if (m_mappings[i].lastUsed < m_mappings[oldest].lastUsed)
                oldest = i;
        }
        for (LadybirdEngineClient* client : m_views)
            client->engineBufferGone(m_mappings[oldest].pixels);
        ::munmap(m_mappings[oldest].pixels, m_mappings[oldest].length);
        m_mappings.remove(oldest);
    }

    void* pixels = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        qWarning("LadybirdEngine: cannot map a frame: %s", strerror(errno));
        return nullptr;
    }
    Mapping mapping;
    mapping.device = status.st_dev;
    mapping.inode = status.st_ino;
    mapping.pixels = static_cast<uchar*>(pixels);
    mapping.length = length;
    mapping.lastUsed = m_frameSerial;
    m_mappings.append(mapping);
    return mapping.pixels;
}

void LadybirdEngine::handleFrame(const lb_embed_header& header, int fd)
{
    LadybirdEngineClient* client = m_views.value(header.view);
    if (!client || fd < 0 || header.c < 1 || header.d < 1 || header.e < static_cast<uint32_t>(header.c) * 4)
        return;
    ++m_frameSerial;
    const uchar* pixels = mappingFor(fd, static_cast<size_t>(header.e) * static_cast<size_t>(header.d));
    if (!pixels)
        return;

    LadybirdFrame frame;
    frame.pixels = pixels;
    frame.pitch = static_cast<int>(header.e);
    frame.bufferSize = QSize(header.c, header.d);
    frame.paintedSize = QSize(qMin(header.a, header.c), qMin(header.b, header.d));
    frame.serial = m_frameSerial;
    client->engineFrame(frame);
}
