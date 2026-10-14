/*
 * Pacific: the page view the browser interface is written against, backed by a Ladybird engine view.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WPEQtView.h"

#include "DirectFrameTextures.h"

#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QTouchEvent>

#ifndef GL_BGRA_EXT
#    define GL_BGRA_EXT 0x80E1
#endif

// How long a finger rests before it is a long press, and how far it may wander meanwhile (the engine's own
// distance for telling a tap from a scroll).
static const int longPressMilliseconds = 500;
static const qreal touchSlop = 24;

WPEQtView::WPEQtView(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    bool ok = false;
    qreal scale = qgetenv("PACIFIC_DEVICE_PIXEL_RATIO").toDouble(&ok);
    if (ok && scale >= 1)
        m_deviceScaleFactor = scale;

    m_longPressTimer.setSingleShot(true);
    m_longPressTimer.setInterval(longPressMilliseconds);
    connect(&m_longPressTimer, &QTimer::timeout, this, [this] {
        if (m_touchId < 0 || m_touchConsumed)
            return;
        // The engine would make a tap of this touch when the finger lifts.
        m_touchConsumed = true;
        sendToEngine(LB_EMBED_TOUCH, LB_EMBED_TOUCH_CANCEL, m_touchId, static_cast<int>(m_touchStart.x()), static_cast<int>(m_touchStart.y()));
        longPressed(m_touchStart);
    });
}

WPEQtView::~WPEQtView()
{
    if (m_view)
        LadybirdEngine::instance()->removeView(m_view);
}

void WPEQtView::componentComplete()
{
    QQuickItem::componentComplete();
    ensureEngineView();
}

void WPEQtView::ensureEngineView()
{
    // The engine lays a page out for the size it is created with, so wait until we know ours.
    if (m_view || width() < 1 || height() < 1)
        return;
    m_view = LadybirdEngine::instance()->addView(this, QSize(static_cast<int>(width()), static_cast<int>(height())), m_deviceScaleFactor);
    if (!m_engineVisible)
        sendToEngine(LB_EMBED_SET_VISIBLE, 0);
    emit webViewCreated();
    if (m_pendingUrl.isValid()) {
        QUrl url = m_pendingUrl;
        m_pendingUrl.clear();
        sendToEngine(LB_EMBED_LOAD, 0, 0, 0, 0, url.toString(QUrl::FullyEncoded).toUtf8());
    }
}

void WPEQtView::sendToEngine(lb_embed_message_type type, int a, int b, int c, int d, const QByteArray& text)
{
    if (m_view)
        LadybirdEngine::instance()->send(m_view, type, a, b, c, d, 0, text);
}

void WPEQtView::setUrl(const QUrl& url)
{
    if (!url.isValid() || url == m_url)
        return;
    m_url = url;
    emit urlChanged();
    if (!m_view) {
        m_pendingUrl = url;
        ensureEngineView();
        return;
    }
    sendToEngine(LB_EMBED_LOAD, 0, 0, 0, 0, url.toString(QUrl::FullyEncoded).toUtf8());
}

void WPEQtView::setDeviceScaleFactor(qreal scale)
{
    if (scale < 1 || qFuzzyCompare(scale, m_deviceScaleFactor))
        return;
    m_deviceScaleFactor = scale;
    if (m_view)
        LadybirdEngine::instance()->send(m_view, LB_EMBED_RESIZE, static_cast<int>(width()), static_cast<int>(height()), 0, 0, m_deviceScaleFactor);
}

void WPEQtView::setWebKitVisible(bool visible)
{
    if (visible == m_engineVisible)
        return;
    m_engineVisible = visible;
    sendToEngine(LB_EMBED_SET_VISIBLE, visible ? 1 : 0);
}

void WPEQtView::goBack() { sendToEngine(LB_EMBED_GO_BACK); }
void WPEQtView::goForward() { sendToEngine(LB_EMBED_GO_FORWARD); }
void WPEQtView::reload() { sendToEngine(LB_EMBED_RELOAD); }
void WPEQtView::stop() { sendToEngine(LB_EMBED_STOP); }

void WPEQtView::loadHtml(const QString&, const QUrl&)
{
    // FIXME: The engine protocol has no way to load a string yet.
}

void WPEQtView::evaluate(const QString& expression, std::function<void(const QJsonValue&)> callback)
{
    if (!m_view)
        return;
    quint32 id = m_nextScript++;
    if (callback)
        m_scripts.insert(id, callback);
    // NB: The engine only hands back a string, and answers may be lost with a page that goes away. So the value
    //     travels as JSON, together with the number that says which question it answers.
    QString wrapped = QStringLiteral("JSON.stringify({i:%1,v:(%2\n)})").arg(id).arg(expression);
    sendToEngine(LB_EMBED_RUN_SCRIPT, 0, 0, 0, 0, wrapped.toUtf8());
}

void WPEQtView::runJavaScript(const QString& script, const QJSValue& callback)
{
    if (!callback.isCallable()) {
        evaluate(script);
        return;
    }
    QJSValue function = callback;
    evaluate(script, [function](const QJsonValue& value) mutable {
        function.call(QJSValueList() << function.engine()->toScriptValue(value.toVariant()));
    });
}

void WPEQtView::setLoadProgress(int progress)
{
    if (progress == m_loadProgress)
        return;
    m_loadProgress = progress;
    emit loadProgressChanged();
}

void WPEQtView::engineFrame(const LadybirdFrame& frame)
{
    static const bool logFrames = !qgetenv("PACIFIC_LOG_ENGINE").isEmpty();
    if (logFrames && (frame.pitch != m_frame.pitch || frame.bufferSize != m_frame.bufferSize || frame.paintedSize != m_frame.paintedSize)) {
        qWarning("WPEQtView: frame geometry pitch=%d buffer=%dx%d painted=%dx%d item=%dx%d", frame.pitch,
            frame.bufferSize.width(), frame.bufferSize.height(), frame.paintedSize.width(), frame.paintedSize.height(),
            int(width()), int(height()));
    }
    m_frame = frame;
    update();
    emit frameArrived();
}

void WPEQtView::engineBufferGone(quint64 bufferId)
{
    if (m_frame.bufferId == bufferId)
        m_frame = LadybirdFrame();
    // Our texture of it is let go of on the render thread, before the buffer could be used again.
    m_buffersGone.append(bufferId);
    update();
}

void WPEQtView::engineMessage(const lb_embed_header& header, const QString& text)
{
    switch (header.type) {
    case LB_EMBED_URL: {
        QUrl url(text);
        if (url != m_url) {
            m_url = url;
            emit urlChanged();
        }
        break;
    }
    case LB_EMBED_TITLE:
        if (text != m_title) {
            m_title = text;
            emit titleChanged();
        }
        break;
    case LB_EMBED_SCRIPT_RESULT: {
        QJsonObject answer = QJsonDocument::fromJson(text.toUtf8()).object();
        auto callback = m_scripts.take(static_cast<quint32>(answer.value(QStringLiteral("i")).toInt()));
        if (callback)
            callback(answer.value(QStringLiteral("v")));
        break;
    }
    case LB_EMBED_FIND_RESULT:
        findResult(header.a, header.b);
        break;
    case LB_EMBED_SELECT_MENU:
        selectMenuRequested(text.toUtf8());
        break;
    case LB_EMBED_DIALOG:
        dialogRequested(header.a, text.toUtf8());
        break;
    case LB_EMBED_FILE_PICKER:
        filePickerRequested(header.a != 0);
        break;
    case LB_EMBED_COLOR_PICKER:
        colorPickerRequested(text);
        break;
    case LB_EMBED_PAGE_COLOR:
        pageColorChanged(header.a, text);
        break;
    case LB_EMBED_FULLSCREEN:
        pageFullscreenChanged(header.a != 0);
        break;
    case LB_EMBED_MEDIA:
        mediaStateChanged(header.a != 0, header.b != 0);
        break;
    case LB_EMBED_PERMISSION: {
        QJsonObject question = QJsonDocument::fromJson(text.toUtf8()).object();
        permissionRequested(header.a, question.value(QStringLiteral("name")).toString(), question.value(QStringLiteral("origin")).toString());
        break;
    }
    case LB_EMBED_PAGE_MESSAGE: {
        QJsonArray message = QJsonDocument::fromJson(text.toUtf8()).array();
        if (message.size() == 2)
            pageMessage(message.at(0).toString(), message.at(1));
        break;
    }
    case LB_EMBED_LOADING: {
        m_loading = header.a != 0;
        // Answers the old page still owed will not come.
        if (m_loading)
            m_scripts.clear();
        // The engine does not report how far a load has got, only that it started and that it finished.
        setLoadProgress(m_loading ? 30 : 100);
        WPEQtViewLoadRequest request(m_url, m_loading ? LoadStartedStatus : LoadSucceededStatus);
        emit loadingChanged(&request);
        break;
    }
    case LB_EMBED_HISTORY:
        if (m_canGoBack != (header.a != 0) || m_canGoForward != (header.b != 0)) {
            m_canGoBack = header.a != 0;
            m_canGoForward = header.b != 0;
            emit backForwardChanged();
        }
        break;
    case LB_EMBED_TEXT_INPUT:
        if (m_pageWantsTextInput != (header.a != 0)) {
            m_pageWantsTextInput = header.a != 0;
            emit pageWantsTextInputChanged();
        }
        break;
    case LB_EMBED_CRASHED:
        emit engineCrashed();
        break;
    default:
        break;
    }
}

void WPEQtView::geometryChanged(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChanged(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size() || newGeometry.width() < 1 || newGeometry.height() < 1)
        return;
    if (!m_view) {
        if (isComponentComplete())
            ensureEngineView();
        return;
    }
    LadybirdEngine::instance()->send(m_view, LB_EMBED_RESIZE, static_cast<int>(newGeometry.width()), static_cast<int>(newGeometry.height()), 0, 0, m_deviceScaleFactor);
}

QSGNode* WPEQtView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QSGSimpleTextureNode* node = static_cast<QSGSimpleTextureNode*>(oldNode);

    // Where the GPU can sample the engine's buffer as it is, the frame is shown from there and nothing is copied.
    if (!m_directTextures)
        m_directTextures = new DirectFrameTextures;
    for (quint64 bufferId : m_buffersGone)
        m_directTextures->forget(bufferId);
    m_buffersGone.clear();
    QSGTexture* direct = m_frame.pixels ? m_directTextures->textureFor(window(), m_frame) : nullptr;
    if (m_directTextures->generation() != m_directGeneration) {
        // NB: As with the uploaded texture below, a node must not carry texture coordinates over to a texture
        //     that happens to be created at the address of one that was deleted.
        m_directGeneration = m_directTextures->generation();
        delete node;
        node = nullptr;
    }
    if (m_frame.pixels && !m_loggedPath) {
        m_loggedPath = true;
        qWarning("WPEQtView: frames are %s", direct ? "shown from the engine's buffers, without a copy" : "uploaded to a texture");
    }
    if (direct) {
        if (m_texture) {
            delete node;
            node = nullptr;
            delete m_texture;
            m_texture = nullptr;
            m_textureId = 0;
            m_textureSize = QSize();
        }
        if (!node) {
            node = new QSGSimpleTextureNode;
            node->setFiltering(QSGTexture::Linear);
        }
        node->setTexture(direct);
        node->setSourceRect(QRectF(QPointF(0, 0), QSizeF(m_frame.paintedSize)));
        node->setRect(QRectF(QPointF(0, 0), QSizeF(m_frame.paintedSize)));
        node->markDirty(QSGNode::DirtyMaterial);
        return node;
    }

    if (!m_frame.pixels && !m_texture) {
        delete node;
        return nullptr;
    }

    if (m_frame.pixels && m_frame.serial != m_uploadedSerial) {
        QOpenGLContext* context = QOpenGLContext::currentContext();
        QOpenGLFunctions* gl = context->functions();
        // NB: GLES 2 cannot be told the row length, so upload whole rows including the padding at their end and
        //     leave the padding out with the node's source rectangle.
        QSize textureSize(m_frame.pitch / 4, m_frame.bufferSize.height());
        // The engine's pixels are BGRA. Without this extension the page would be shown with red and blue swapped.
        static const bool hasBgra = context->hasExtension(QByteArrayLiteral("GL_EXT_texture_format_BGRA8888"));
        GLenum format = hasBgra ? GL_BGRA_EXT : GL_RGBA;

        if (!m_textureId || textureSize != m_textureSize) {
            // NB: The node works out its texture coordinates when it is given a texture or a source rectangle that
            //     differs from the one it has. A new texture of another size can land at the old one's address
            //     with the same source rectangle, and the node would then keep coordinates for the old size and
            //     show the page stretched. So a new texture gets a new node.
            delete node;
            node = nullptr;
            delete m_texture;
            m_texture = nullptr;
            gl->glGenTextures(1, &m_textureId);
            gl->glBindTexture(GL_TEXTURE_2D, m_textureId);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
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

    if (!m_texture) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setFiltering(QSGTexture::Linear);
    }
    QSizeF painted = m_frame.pixels ? QSizeF(m_frame.paintedSize) : QSizeF(m_textureSize);
    node->setTexture(m_texture);
    node->setSourceRect(QRectF(QPointF(0, 0), painted));
    node->setRect(QRectF(QPointF(0, 0), painted));
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}

void WPEQtView::releaseResources()
{
    // NB: The scene graph deletes textures it was given through a node only on its own thread.
    if (m_texture)
        m_texture->deleteLater();
    m_texture = nullptr;
    m_textureId = 0;
    m_uploadedSerial = 0;
    if (m_directTextures)
        m_directTextures->deleteLater();
    m_directTextures = nullptr;
    m_directGeneration = 0;
}

void WPEQtView::abandonTouch()
{
    m_longPressTimer.stop();
    if (m_touchId >= 0 && !m_touchConsumed)
        sendToEngine(LB_EMBED_TOUCH, LB_EMBED_TOUCH_CANCEL, m_touchId, static_cast<int>(m_touchStart.x()), static_cast<int>(m_touchStart.y()));
    m_touchId = -1;
    m_touchConsumed = false;
    m_pinching = false;
}

void WPEQtView::touchUngrabEvent()
{
    abandonTouch();
    QQuickItem::touchUngrabEvent();
}

void WPEQtView::touchEvent(QTouchEvent* event)
{
    event->accept();
    if (event->type() == QEvent::TouchCancel) {
        abandonTouch();
        return;
    }
    // A touch we never saw the end of must not swallow this one.
    if (event->type() == QEvent::TouchBegin)
        abandonTouch();
    const QList<QTouchEvent::TouchPoint>& points = event->touchPoints();

    int down = 0;
    for (const QTouchEvent::TouchPoint& point : points) {
        if (point.state() != Qt::TouchPointReleased)
            ++down;
    }

    // Two fingers zoom the page. The first finger's touch stops being one.
    if (down >= 2) {
        QPointF first, second;
        int found = 0;
        for (const QTouchEvent::TouchPoint& point : points) {
            if (point.state() == Qt::TouchPointReleased)
                continue;
            (found++ ? second : first) = point.pos();
            if (found == 2)
                break;
        }
        qreal distance = QLineF(first, second).length();
        if (!m_pinching) {
            m_pinching = true;
            m_pinchDistance = distance;
            m_longPressTimer.stop();
            if (m_touchId >= 0 && !m_touchConsumed) {
                m_touchConsumed = true;
                sendToEngine(LB_EMBED_TOUCH, LB_EMBED_TOUCH_CANCEL, m_touchId, static_cast<int>(m_touchStart.x()), static_cast<int>(m_touchStart.y()));
            }
        } else if (m_pinchDistance > 1 && distance > 1 && !qFuzzyCompare(distance, m_pinchDistance)) {
            QPointF centre = (first + second) / 2;
            LadybirdEngine::instance()->send(m_view, LB_EMBED_PINCH, 0, 0, static_cast<int>(centre.x()), static_cast<int>(centre.y()), distance / m_pinchDistance);
            m_pinchDistance = distance;
        }
        return;
    }
    if (down == 0) {
        // Every finger is up; whatever comes next starts afresh.
        bool wasPinching = m_pinching;
        m_pinching = false;
        if (wasPinching) {
            m_touchId = -1;
            return;
        }
    }
    if (m_pinching)
        return;

    for (const QTouchEvent::TouchPoint& point : points) {
        switch (point.state()) {
        case Qt::TouchPointPressed:
            if (m_touchId >= 0)
                continue;
            m_touchId = point.id();
            m_touchStart = point.pos();
            m_touchConsumed = false;
            m_longPressTimer.start();
            sendToEngine(LB_EMBED_TOUCH, LB_EMBED_TOUCH_DOWN, point.id(), static_cast<int>(point.pos().x()), static_cast<int>(point.pos().y()));
            break;
        case Qt::TouchPointMoved:
            if (point.id() != m_touchId || m_touchConsumed)
                continue;
            if (QLineF(m_touchStart, point.pos()).length() > touchSlop)
                m_longPressTimer.stop();
            sendToEngine(LB_EMBED_TOUCH, LB_EMBED_TOUCH_MOTION, point.id(), static_cast<int>(point.pos().x()), static_cast<int>(point.pos().y()));
            break;
        case Qt::TouchPointReleased:
            if (point.id() != m_touchId)
                continue;
            m_longPressTimer.stop();
            if (!m_touchConsumed) {
                sendToEngine(LB_EMBED_TOUCH, LB_EMBED_TOUCH_UP, point.id(), static_cast<int>(point.pos().x()), static_cast<int>(point.pos().y()));
                if (QLineF(m_touchStart, point.pos()).length() <= touchSlop)
                    tapped(point.pos());
            }
            m_touchId = -1;
            break;
        default:
            break;
        }
    }
}
