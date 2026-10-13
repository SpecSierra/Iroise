/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdView.h"
#include "../ui/protocol/EmbedProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTouchEvent>

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef GL_BGRA_EXT
#    define GL_BGRA_EXT 0x80E1
#endif

// The engine cycles through a few buffers per view; keep a little more than that mapped.
static const int maximumMappings = 6;

LadybirdView::LadybirdView(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    m_enginePath = QString::fromLocal8Bit(qgetenv("LADYBIRD_SFOS_ENGINE"));
    if (m_enginePath.isEmpty())
        m_enginePath = QStringLiteral("/tmp/ladybird/bin/Ladybird");
    m_frameRateTimer = startTimer(1000);
}

LadybirdView::~LadybirdView()
{
    if (m_connection >= 0)
        ::close(m_connection);
    if (m_listener >= 0)
        ::close(m_listener);
    if (!m_socketPath.isEmpty())
        ::unlink(m_socketPath.toLocal8Bit().constData());
    m_engine.terminate();
    m_engine.waitForFinished(2000);
    for (const Mapping& mapping : m_mappings)
        ::munmap(mapping.pixels, mapping.length);
}

void LadybirdView::setEnginePath(const QString& path)
{
    if (path == m_enginePath)
        return;
    m_enginePath = path;
    emit enginePathChanged();
}

void LadybirdView::setDevicePixelRatio(qreal ratio)
{
    if (ratio < 1 || qFuzzyCompare(ratio, m_devicePixelRatio))
        return;
    m_devicePixelRatio = ratio;
    emit devicePixelRatioChanged();
    sendSize();
}

void LadybirdView::componentComplete()
{
    QQuickItem::componentComplete();
    startEngine();
}

void LadybirdView::startEngine()
{
    // The engine connects back to this socket, which is how it knows to run without a window of its own.
    QString runtimeDirectory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDirectory.isEmpty())
        runtimeDirectory = QDir::tempPath();
    m_socketPath = QStringLiteral("%1/ladybird-sfos-%2.socket").arg(runtimeDirectory).arg(QCoreApplication::applicationPid());
    QByteArray path = m_socketPath.toLocal8Bit();
    ::unlink(path.constData());

    m_listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path.constData(), sizeof(address.sun_path) - 1);
    if (m_listener < 0 || ::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(m_listener, 1) < 0) {
        qWarning("LadybirdView: cannot listen on %s: %s", path.constData(), strerror(errno));
        return;
    }
    m_listenerNotifier = new QSocketNotifier(m_listener, QSocketNotifier::Read, this);
    connect(m_listenerNotifier, &QSocketNotifier::activated, this, &LadybirdView::acceptEngine);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral(LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE), m_socketPath);
    // This phone's kernel has no Landlock; this keeps the seccomp half of the engine's sandbox.
    environment.insert(QStringLiteral("LADYBIRD_UNSAFE_ALLOW_MISSING_LANDLOCK"), QStringLiteral("1"));
    // Test runs keep everything the engine writes out of the user's home directory.
    QString engineHome = QString::fromLocal8Bit(qgetenv("LADYBIRD_SFOS_ENGINE_HOME"));
    if (!engineHome.isEmpty()) {
        QDir().mkpath(engineHome);
        environment.insert(QStringLiteral("HOME"), engineHome);
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), engineHome + QStringLiteral("/.cache"));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), engineHome + QStringLiteral("/.config"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), engineHome + QStringLiteral("/.local/share"));
    }
    m_engine.setProcessEnvironment(environment);
    m_engine.setProcessChannelMode(QProcess::ForwardedChannels);
    m_engine.start(m_enginePath, QStringList());
}

void LadybirdView::acceptEngine()
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
    connect(m_connectionNotifier, &QSocketNotifier::activated, this, &LadybirdView::receive);
    emit engineRunningChanged();

    sendSize();
    if (!m_pendingUrl.isEmpty()) {
        load(m_pendingUrl);
        m_pendingUrl.clear();
    }
}

void LadybirdView::send(const lb_embed_header& header, const QByteArray& text)
{
    if (m_connection < 0)
        return;
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
        qWarning("LadybirdView: sending to the engine failed: %s", strerror(errno));
}

void LadybirdView::sendSize()
{
    if (width() < 1 || height() < 1)
        return;
    lb_embed_header header;
    memset(&header, 0, sizeof(header));
    header.type = LB_EMBED_RESIZE;
    header.a = static_cast<int>(width());
    header.b = static_cast<int>(height());
    header.scale = m_devicePixelRatio;
    send(header);
}

void LadybirdView::load(const QString& url)
{
    if (m_connection < 0) {
        m_pendingUrl = url;
        return;
    }
    QByteArray text = url.toUtf8().left(LB_EMBED_TEXT_MAX);
    lb_embed_header header;
    memset(&header, 0, sizeof(header));
    header.type = LB_EMBED_LOAD;
    header.text_length = static_cast<uint32_t>(text.size());
    send(header, text);
}

static lb_embed_header simpleMessage(lb_embed_message_type type)
{
    lb_embed_header header;
    memset(&header, 0, sizeof(header));
    header.type = type;
    return header;
}

void LadybirdView::goBack() { send(simpleMessage(LB_EMBED_GO_BACK)); }
void LadybirdView::goForward() { send(simpleMessage(LB_EMBED_GO_FORWARD)); }
void LadybirdView::reload() { send(simpleMessage(LB_EMBED_RELOAD)); }

void LadybirdView::receive()
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
            m_connectionNotifier->setEnabled(false);
            ::close(m_connection);
            m_connection = -1;
            emit engineRunningChanged();
            return;
        }

        int fd = -1;
        for (cmsghdr* part = CMSG_FIRSTHDR(&message); part; part = CMSG_NXTHDR(&message, part)) {
            if (part->cmsg_level == SOL_SOCKET && part->cmsg_type == SCM_RIGHTS)
                memcpy(&fd, CMSG_DATA(part), sizeof(int));
        }
        if (static_cast<size_t>(length) < sizeof(lb_embed_header)) {
            if (fd >= 0)
                ::close(fd);
            continue;
        }

        const lb_embed_header& header = buffer.header;
        size_t textLength = qMin(static_cast<size_t>(header.text_length), static_cast<size_t>(length) - sizeof(lb_embed_header));
        QString text = QString::fromUtf8(buffer.text, static_cast<int>(textLength));
        switch (header.type) {
        case LB_EMBED_FRAME:
            handleFrame(header, fd);
            break;
        case LB_EMBED_URL:
            m_url = text;
            emit urlChanged();
            break;
        case LB_EMBED_TITLE:
            m_title = text;
            emit titleChanged();
            break;
        case LB_EMBED_LOADING:
            m_loading = header.a != 0;
            emit loadingChanged();
            break;
        default:
            break;
        }
        if (fd >= 0)
            ::close(fd);
    }
}

const uchar* LadybirdView::mappingFor(int fd, size_t length)
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
        ::munmap(m_mappings[oldest].pixels, m_mappings[oldest].length);
        m_mappings.remove(oldest);
    }

    void* pixels = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        qWarning("LadybirdView: cannot map a frame: %s", strerror(errno));
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

void LadybirdView::handleFrame(const lb_embed_header& header, int fd)
{
    if (fd < 0 || header.c < 1 || header.d < 1 || header.e < static_cast<uint32_t>(header.c) * 4)
        return;
    ++m_frameSerial;
    const uchar* pixels = mappingFor(fd, static_cast<size_t>(header.e) * static_cast<size_t>(header.d));
    if (!pixels)
        return;

    m_frame.pixels = pixels;
    m_frame.pitch = static_cast<int>(header.e);
    m_frame.bufferSize = QSize(header.c, header.d);
    m_frame.paintedSize = QSize(qMin(header.a, header.c), qMin(header.b, header.d));
    m_frame.serial = m_frameSerial;
    ++m_framesThisSecond;
    update();
}

void LadybirdView::timerEvent(QTimerEvent* event)
{
    if (event->timerId() != m_frameRateTimer) {
        QQuickItem::timerEvent(event);
        return;
    }
    if (m_framesThisSecond != m_framesPerSecond) {
        m_framesPerSecond = m_framesThisSecond;
        emit framesPerSecondChanged();
    }
    m_framesThisSecond = 0;
}

void LadybirdView::geometryChanged(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChanged(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        sendSize();
}

QSGNode* LadybirdView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QSGSimpleTextureNode* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!m_frame.pixels) {
        delete node;
        return nullptr;
    }

    if (m_frame.serial != m_uploadedSerial) {
        QOpenGLContext* context = QOpenGLContext::currentContext();
        QOpenGLFunctions* gl = context->functions();
        // NB: GLES 2 cannot be told the row length, so upload whole rows including the padding at their end and
        //     leave the padding out with the node's source rectangle.
        QSize textureSize(m_frame.pitch / 4, m_frame.bufferSize.height());
        // The engine's pixels are BGRA. Without this extension the page would be shown with red and blue swapped.
        static const bool hasBgra = context->hasExtension(QByteArrayLiteral("GL_EXT_texture_format_BGRA8888"));
        GLenum format = hasBgra ? GL_BGRA_EXT : GL_RGBA;

        if (!m_textureId || textureSize != m_textureSize) {
            delete m_texture;
            m_texture = nullptr;
            gl->glGenTextures(1, &m_textureId);
            gl->glBindTexture(GL_TEXTURE_2D, m_textureId);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, format, textureSize.width(), textureSize.height(), 0, format, GL_UNSIGNED_BYTE, m_frame.pixels);
            m_textureSize = textureSize;
            m_texture = window()->createTextureFromId(m_textureId, textureSize, QQuickWindow::TextureOwnsGLTexture);
        } else {
            gl->glBindTexture(GL_TEXTURE_2D, m_textureId);
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureSize.width(), textureSize.height(), format, GL_UNSIGNED_BYTE, m_frame.pixels);
        }
        m_uploadedSerial = m_frame.serial;
        window()->resetOpenGLState();
    }

    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setFiltering(QSGTexture::Nearest);
    }
    node->setTexture(m_texture);
    node->setSourceRect(QRectF(QPointF(0, 0), m_frame.paintedSize));
    node->setRect(QRectF(QPointF(0, 0), m_frame.paintedSize));
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}

void LadybirdView::releaseResources()
{
    // NB: The scene graph deletes textures it was given through a node only on its own thread.
    if (m_texture)
        m_texture->deleteLater();
    m_texture = nullptr;
    m_textureId = 0;
    m_uploadedSerial = 0;
}

void LadybirdView::touchEvent(QTouchEvent* event)
{
    for (const QTouchEvent::TouchPoint& point : event->touchPoints()) {
        lb_embed_header header;
        memset(&header, 0, sizeof(header));
        header.type = LB_EMBED_TOUCH;
        switch (point.state()) {
        case Qt::TouchPointPressed:
            header.a = LB_EMBED_TOUCH_DOWN;
            break;
        case Qt::TouchPointMoved:
            header.a = LB_EMBED_TOUCH_MOTION;
            break;
        case Qt::TouchPointReleased:
            header.a = LB_EMBED_TOUCH_UP;
            break;
        default:
            continue;
        }
        header.b = point.id();
        header.c = static_cast<int>(point.pos().x());
        header.d = static_cast<int>(point.pos().y());
        send(header);
    }
    event->accept();
}
