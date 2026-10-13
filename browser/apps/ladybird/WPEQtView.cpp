/*
 * Pacific: the page view the browser interface is written against, backed by a Ladybird engine view.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WPEQtView.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QTouchEvent>

#ifndef GL_BGRA_EXT
#    define GL_BGRA_EXT 0x80E1
#endif

WPEQtView::WPEQtView(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    bool ok = false;
    qreal scale = qgetenv("PACIFIC_DEVICE_PIXEL_RATIO").toDouble(&ok);
    if (ok && scale >= 1)
        m_deviceScaleFactor = scale;
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

void WPEQtView::runJavaScript(const QString&, const QJSValue&)
{
    // FIXME: The engine protocol has no way to run script in a page yet. Atlantic drove many page features
    //        (selection handles, pickers, find in page) through injected script; they need engine-side hooks here.
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
    m_frame = frame;
    update();
    emit frameArrived();
}

void WPEQtView::engineBufferGone(const uchar* pixels)
{
    if (m_frame.pixels == pixels)
        m_frame = LadybirdFrame();
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
    case LB_EMBED_LOADING: {
        m_loading = header.a != 0;
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
}

void WPEQtView::touchEvent(QTouchEvent* event)
{
    for (const QTouchEvent::TouchPoint& point : event->touchPoints()) {
        int phase;
        switch (point.state()) {
        case Qt::TouchPointPressed:
            phase = LB_EMBED_TOUCH_DOWN;
            break;
        case Qt::TouchPointMoved:
            phase = LB_EMBED_TOUCH_MOTION;
            break;
        case Qt::TouchPointReleased:
            phase = LB_EMBED_TOUCH_UP;
            break;
        default:
            continue;
        }
        sendToEngine(LB_EMBED_TOUCH, phase, point.id(), static_cast<int>(point.pos().x()), static_cast<int>(point.pos().y()));
    }
    event->accept();
}
