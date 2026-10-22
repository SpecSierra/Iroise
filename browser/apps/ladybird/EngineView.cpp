/*
 * Iroise: the page view the browser interface is written against, backed by a Ladybird engine view.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineView.h"

#include "DirectFrameTextures.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QTouchEvent>


// How long a finger rests before it is a long press, and how far it may wander meanwhile (the engine's own
// distance for telling a tap from a scroll).
static const int longPressMilliseconds = 500;
static const qreal touchSlop = 24;
static const int kMaximumPendingScripts = 64;

EngineView::EngineView(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    bool ok = false;
    qreal scale = qgetenv("IROISE_DEVICE_PIXEL_RATIO").toDouble(&ok);
    if (ok && scale >= 1)
        m_deviceScaleFactor = scale;

    connect(LadybirdEngine::instance(), &LadybirdEngine::restarted, this, &EngineView::engineRestarted);

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

EngineView::~EngineView()
{
    if (m_view)
        LadybirdEngine::instance()->removeView(m_view);
    // NB: QQuickItem's destructor calls releaseResources() too, but its own by then, so the textures (and the
    //     engine buffers behind them) would be kept for good.
    EngineView::releaseResources();
}

void EngineView::componentComplete()
{
    QQuickItem::componentComplete();
    ensureEngineView();
}

void EngineView::ensureEngineView()
{
    // The engine lays a page out for the size it is created with, so wait until we know ours.
    if (m_view || width() < 1 || height() < 1)
        return;
    if (m_adoptedView) {
        m_view = m_adoptedView;
        LadybirdEngine::instance()->claimView(m_view, this);
        LadybirdEngine::instance()->send(m_view, LB_EMBED_RESIZE, static_cast<int>(width()), static_cast<int>(height()), 0, 0, m_deviceScaleFactor);
    } else {
        m_view = LadybirdEngine::instance()->addView(this, QSize(static_cast<int>(width()), static_cast<int>(height())), m_deviceScaleFactor, m_privateBrowsing);
    }
    if (!m_engineVisible)
        sendToEngine(LB_EMBED_SET_VISIBLE, 0);
    if (m_pendingUrl.isValid()) {
        QUrl url = m_pendingUrl;
        m_pendingUrl.clear();
        sendToEngine(LB_EMBED_LOAD, 0, 0, 0, 0, url.toString(QUrl::FullyEncoded).toUtf8());
    }
}

void EngineView::sendToEngine(lb_embed_message_type type, int a, int b, int c, int d, const QByteArray& text)
{
    if (m_view)
        LadybirdEngine::instance()->send(m_view, type, a, b, c, d, 0, text);
}

void EngineView::setUrl(const QUrl& url)
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

void EngineView::setDeviceScaleFactor(qreal scale)
{
    if (scale < 1 || qFuzzyCompare(scale, m_deviceScaleFactor))
        return;
    m_deviceScaleFactor = scale;
    if (m_view)
        LadybirdEngine::instance()->send(m_view, LB_EMBED_RESIZE, static_cast<int>(width()), static_cast<int>(height()), 0, 0, m_deviceScaleFactor);
}

void EngineView::setEngineVisible(bool visible)
{
    if (visible == m_engineVisible)
        return;
    m_engineVisible = visible;
    sendToEngine(LB_EMBED_SET_VISIBLE, visible ? 1 : 0);
}

void EngineView::goBack() { sendToEngine(LB_EMBED_GO_BACK); }
void EngineView::goForward() { sendToEngine(LB_EMBED_GO_FORWARD); }
void EngineView::reload()
{
    // A view whose page crashed, or whose engine did, has nothing to reload: its page is loaded anew.
    if (m_crashed && m_url.isValid()) {
        m_crashed = false;
        sendToEngine(LB_EMBED_LOAD, 0, 0, 0, 0, m_url.toString(QUrl::FullyEncoded).toUtf8());
        return;
    }
    sendToEngine(LB_EMBED_RELOAD);
}

void EngineView::engineRestarted()
{
    if (!m_view)
        return;
    LadybirdEngine::instance()->forgetView(m_view);
    m_scripts.clear();
    m_frame = LadybirdFrame();
    m_view = LadybirdEngine::instance()->addView(this, QSize(static_cast<int>(width()), static_cast<int>(height())), m_deviceScaleFactor, m_privateBrowsing);
    if (!m_engineVisible)
        sendToEngine(LB_EMBED_SET_VISIBLE, 0);
    update();
}
void EngineView::stop() { sendToEngine(LB_EMBED_STOP); }
void EngineView::evaluate(const QString& expression, std::function<void(const QJsonValue&)> callback)
{
    if (!m_view)
        return;
    quint32 id = m_nextScript++;
    if (callback) {
        // NB: An expression that throws never answers; a page that stays open long (a web app) must not pile them up.
        //     The oldest question is given up on, as its answer is the least likely still to come.
        if (m_scripts.size() >= kMaximumPendingScripts)
            m_scripts.erase(m_scripts.begin());
        m_scripts.insert(id, callback);
    }
    // NB: The engine only hands back a string, and answers may be lost with a page that goes away. So the value
    //     travels as JSON, together with the number that says which question it answers.
    QString wrapped = QStringLiteral("JSON.stringify({i:%1,v:(%2\n)})").arg(id).arg(expression);
    sendToEngine(LB_EMBED_RUN_SCRIPT, 0, 0, 0, 0, wrapped.toUtf8());
}

void EngineView::setLoadProgress(int progress)
{
    if (progress == m_loadProgress)
        return;
    m_loadProgress = progress;
    emit loadProgressChanged();
}

void EngineView::clearPrivateBrowsingData()
{
    LadybirdEngine::instance()->send(0, LB_EMBED_CLEAR_DATA, LB_EMBED_CLEAR_PRIVATE_SESSION);
}

static bool s_loadingCoverDark = false;

void EngineView::setLoadingCoverDark(bool dark)
{
    s_loadingCoverDark = dark;
}

void EngineView::setCovered(bool covered)
{
    if (m_covered == covered)
        return;
    m_covered = covered;
    update();
}

void EngineView::engineFrame(const LadybirdFrame& frame)
{
    static const bool logFrames = !qgetenv("IROISE_LOG_ENGINE").isEmpty();
    if (logFrames && (frame.pitch != m_frame.pitch || frame.bufferSize != m_frame.bufferSize || frame.paintedSize != m_frame.paintedSize)) {
        qWarning("EngineView: frame geometry pitch=%d buffer=%dx%d painted=%dx%d item=%dx%d", frame.pitch,
            frame.bufferSize.width(), frame.bufferSize.height(), frame.paintedSize.width(), frame.paintedSize.height(),
            int(width()), int(height()));
    }
    m_frame = frame;
    if (!m_awaitingCommit)
        setCovered(false);
    update();
    emit frameArrived();
}

void EngineView::engineBufferGone(quint64 bufferId)
{
    if (m_frame.bufferId == bufferId)
        m_frame = LadybirdFrame();
    // Our texture of it is let go of on the render thread, before the buffer could be used again.
    m_buffersGone.append(bufferId);
    update();
}

void EngineView::engineMessage(const lb_embed_header& header, const QString& text)
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
    case LB_EMBED_FAVICON:
        faviconReceived(text);
        break;
    case LB_EMBED_FULLSCREEN:
        m_pageFullscreen = header.a != 0;
        pageFullscreenChanged(m_pageFullscreen);
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
        if (m_loading)
            m_crashed = false;
        // The old page stays out of sight until the new one has a frame. A load that ends without a new page (a
        // download, an error, stopped) shows the old one again.
        m_awaitingCommit = m_loading;
        setCovered(m_loading);
        // Answers the old page still owed will not come.
        if (m_loading)
            m_scripts.clear();
        // The engine does not report how far a load has got, only that it started and that it finished.
        setLoadProgress(m_loading ? 30 : 100);
        EngineViewLoadRequest request(m_url, m_loading ? LoadStartedStatus : LoadSucceededStatus);
        emit loadingChanged(&request);
        break;
    }
    case LB_EMBED_COMMIT:
        m_awaitingCommit = false;
        emit committed();
        break;
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
    case LB_EMBED_CLOSE_VIEW:
        emit closeRequested();
        break;
    case LB_EMBED_CRASHED:
        m_crashed = true;
        m_awaitingCommit = false;
        setCovered(false);
        // A page that crashed is not in fullscreen any more.
        if (m_pageFullscreen) {
            m_pageFullscreen = false;
            pageFullscreenChanged(false);
        }
        emit engineCrashed();
        break;
    default:
        break;
    }
}

void EngineView::geometryChanged(const QRectF& newGeometry, const QRectF& oldGeometry)
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

QSGNode* EngineView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    if (m_covered) {
        QSGSimpleRectNode* cover = m_coverNodeShown ? static_cast<QSGSimpleRectNode*>(oldNode) : nullptr;
        if (!cover) {
            delete oldNode;
            cover = new QSGSimpleRectNode;
            m_coverNodeShown = true;
        }
        cover->setRect(boundingRect());
        cover->setColor(s_loadingCoverDark ? QColor(0x1e, 0x1e, 0x1e) : QColor(Qt::white));
        return cover;
    }
    if (m_coverNodeShown) {
        delete oldNode;
        oldNode = nullptr;
        m_coverNodeShown = false;
    }
    QSGSimpleTextureNode* node = static_cast<QSGSimpleTextureNode*>(oldNode);

    if (!m_directTextures)
        m_directTextures = new DirectFrameTextures;
    for (quint64 bufferId : m_buffersGone)
        m_directTextures->forget(bufferId);
    m_buffersGone.clear();

    // Where the GPU can sample the engine's buffer as it is, the frame is shown from there and nothing is copied.
    // Otherwise its pixels are uploaded. Without a frame (its buffer is gone), the last one uploaded stays.
    QSGTexture* texture = m_frame.pixels ? m_directTextures->textureFor(window(), m_frame) : nullptr;
    QSizeF painted = QSizeF(m_frame.paintedSize);
    if (m_frame.pixels && !m_loggedPath) {
        m_loggedPath = true;
        qWarning("EngineView: frames are %s", texture ? "shown from the engine's buffers, without a copy" : "uploaded to a texture");
    }
    if (texture) {
        m_directTextures->dropUploaded();
    } else if (m_frame.pixels) {
        texture = m_directTextures->upload(window(), m_frame);
    } else {
        texture = m_directTextures->uploaded();
        painted = QSizeF(m_directTextures->uploadedSize());
    }

    if (m_directTextures->generation() != m_directGeneration) {
        // NB: The node works out its texture coordinates when it is given a texture or a source rectangle that
        //     differs from the one it has. A new texture can land at the address of one that was deleted, with the
        //     same source rectangle, and the node would then keep coordinates for the old one's size and show the
        //     page stretched. So a new texture gets a new node.
        m_directGeneration = m_directTextures->generation();
        delete node;
        node = nullptr;
    }
    if (!texture) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setFiltering(QSGTexture::Linear);
    }
    node->setTexture(texture);
    node->setSourceRect(QRectF(QPointF(0, 0), painted));
    node->setRect(QRectF(QPointF(0, 0), painted));
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}

void EngineView::releaseResources()
{
    // NB: The textures live on the render thread and are let go of there.
    if (m_directTextures)
        m_directTextures->deleteLater();
    m_directTextures = nullptr;
    m_directGeneration = 0;
}

// Called by the scene graph on the render thread, with its GL context still current, before it lets go of that
// context: Silica does so while the app is in the background. Our textures go with it, now, and the frame is
// shown anew from the engine's buffers when the app comes back.
void EngineView::invalidateSceneGraph()
{
    delete m_directTextures;
    m_directTextures = nullptr;
    m_directGeneration = 0;
    // NB: The scene graph has deleted our node.
    m_coverNodeShown = false;
}

void EngineView::abandonTouch()
{
    m_longPressTimer.stop();
    if (m_touchId >= 0 && !m_touchConsumed)
        sendToEngine(LB_EMBED_TOUCH, LB_EMBED_TOUCH_CANCEL, m_touchId, static_cast<int>(m_touchStart.x()), static_cast<int>(m_touchStart.y()));
    m_touchId = -1;
    m_touchConsumed = false;
    m_pinching = false;
}

void EngineView::touchUngrabEvent()
{
    abandonTouch();
    QQuickItem::touchUngrabEvent();
}

void EngineView::touchEvent(QTouchEvent* event)
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
            // NB: In fullscreen the engine passes a touch on as a held mouse button (a seek bar is held and
            //     dragged), which a long press would cancel.
            if (!m_pageFullscreen)
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
