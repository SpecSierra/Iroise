/*
 * Pacific: one engine view (LadybirdEngine) as a Qt Quick item: it shows the view's frames and passes it input,
 * size, visibility and navigation. WebPage builds the browser's page on top of it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QHash>
#include <QMap>
#include <QJsonValue>
#include <QPointF>
#include <QTimer>
#include <QQuickItem>
#include <QUrl>

#include <functional>

#include "LadybirdEngine.h"

class DirectFrameTextures;
class QSGTexture;
class EngineViewLoadRequest;

class EngineView : public QQuickItem, public LadybirdEngineClient {
    Q_OBJECT
    Q_DISABLE_COPY(EngineView)
    Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int loadProgress READ loadProgress NOTIFY loadProgressChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY backForwardChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY backForwardChanged)
    Q_ENUMS(LoadStatus)

public:
    enum LoadStatus {
        LoadStartedStatus,
        LoadSucceededStatus
    };

    EngineView(QQuickItem* parent = nullptr);
    ~EngineView() override;

    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) final;

    QUrl url() const { return m_url; }
    void setUrl(const QUrl&);
    int loadProgress() const { return m_loadProgress; }
    QString title() const { return m_title; }
    bool canGoBack() const { return m_canGoBack; }
    bool isLoading() const { return m_loading; }
    // The colour that stands in for a page while the next one loads: white, or dark grey for dark pages.
    static void setLoadingCoverDark(bool dark);
    bool canGoForward() const { return m_canGoForward; }

    // Called before the item gets a size: show the view the engine opened for a page (LB_EMBED_NEW_VIEW).
    void adoptEngineView(quint32 view) { m_adoptedView = view; }
    Q_INVOKABLE void setDeviceScaleFactor(qreal scale);
    qreal deviceScaleFactor() const { return m_deviceScaleFactor; }

    // Whether the page counts as on screen: a hidden page stops animating and painting.
    void setEngineVisible(bool visible);

    void setPrivateBrowsing(bool p) { m_privateBrowsing = p; }
    bool privateBrowsing() const { return m_privateBrowsing; }
    // The last private tab has closed: the engine lets go of the private session (cookies, storage, downloads).
    static void clearPrivateBrowsingData();

    quint32 frameSerial() const { return static_cast<quint32>(m_frame.serial); }
    // How much of the view the newest frame covers. Less than the view right after it grew.
    QSize paintedSize() const { return m_frame.paintedSize; }

    // Runs a JavaScript expression in the page. The callback gets its value, as far as JSON can carry it; it is
    // not called at all if the expression throws or the page goes away first.
    void evaluate(const QString& expression, std::function<void(const QJsonValue&)> callback = nullptr);

    // True while an editable element in the page has focus.
    bool pageWantsTextInput() const { return m_pageWantsTextInput; }

public Q_SLOTS:
    void goBack();
    void goForward();
    void reload();
    void stop();

Q_SIGNALS:
    // The page asked to close (window.close).
    void closeRequested();
    void urlChanged();
    void titleChanged();
    void loadingChanged(EngineViewLoadRequest* loadRequest);
    void loadProgressChanged();
    void backForwardChanged();
    void scrollPositionChanged(qreal scrollY, qreal scrollHeight, qreal innerHeight);
    void frameArrived();
    // The new page of a load has taken over the view (its document exists; it may still be loading).
    void committed();
    void pageWantsTextInputChanged();
    void engineCrashed();

protected:
    void componentComplete() override;
    void geometryChanged(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void releaseResources() override;
    void touchEvent(QTouchEvent*) override;
    void touchUngrabEvent() override;
    void abandonTouch();

    // LadybirdEngineClient
    void engineFrame(const LadybirdFrame&) override;
    void engineMessage(const lb_embed_header&, const QString& text) override;
    void engineBufferGone(quint64 bufferId) override;

    // A finger that stayed down in one place; the touch is not a tap any more. In item coordinates.
    virtual void longPressed(const QPointF&) { }
    // A finger that came down and went up in one place, which the engine makes a click of.
    virtual void tapped(const QPointF&) { }
    virtual void filePickerRequested(bool multiple) { Q_UNUSED(multiple); }
    virtual void colorPickerRequested(const QString& color) { Q_UNUSED(color); }
    virtual void pageColorChanged(int kind, const QString& color) { Q_UNUSED(kind); Q_UNUSED(color); }
    // What the engine says that only the page class above knows what to do with.
    virtual void findResult(int current, int total) { Q_UNUSED(current); Q_UNUSED(total); }
    virtual void selectMenuRequested(const QByteArray& optionsJson) { Q_UNUSED(optionsJson); }
    virtual void dialogRequested(int kind, const QByteArray& json) { Q_UNUSED(kind); Q_UNUSED(json); }
    // A message from the page agent (see WebPage): a kind and what came with it.
    virtual void pageMessage(const QString& kind, const QJsonValue& data) { Q_UNUSED(kind); Q_UNUSED(data); }
    virtual void pageFullscreenChanged(bool fullscreen) { Q_UNUSED(fullscreen); }
    virtual void faviconReceived(const QString& dataUrl) { Q_UNUSED(dataUrl); }
    virtual void mediaStateChanged(bool playsAudio, bool keepsScreenOn) { Q_UNUSED(playsAudio); Q_UNUSED(keepsScreenOn); }
    virtual void permissionRequested(int question, const QString& name, const QString& origin) { Q_UNUSED(question); Q_UNUSED(name); Q_UNUSED(origin); }

    void sendToEngine(lb_embed_message_type type, int a = 0, int b = 0, int c = 0, int d = 0, const QByteArray& text = QByteArray());
    bool hasEngineView() const { return m_view != 0; }

private Q_SLOTS:
    // Qt Quick calls a slot of this name on the render thread (see EngineView.cpp).
    void invalidateSceneGraph();

private:
    void engineRestarted();
    void ensureEngineView();
    void setLoadProgress(int);
    void setCovered(bool);

    quint32 m_view = 0;
    // A view the engine opened for a page, which this one shows instead of asking for a view of its own.
    quint32 m_adoptedView = 0;
    QUrl m_url;
    QUrl m_pendingUrl;
    QString m_title;
    bool m_loading = false;
    int m_loadProgress = 0;
    bool m_canGoBack = false;
    bool m_canGoForward = false;
    bool m_engineVisible = true;
    bool m_privateBrowsing = false;
    bool m_pageWantsTextInput = false;
    bool m_pageFullscreen = false;
    qreal m_deviceScaleFactor = 2.25;

    LadybirdFrame m_frame;
    // From the start of a load until the new page's first frame, a plain fill stands in for the old page.
    bool m_covered = false;
    bool m_awaitingCommit = false;

    quint32 m_nextScript = 1;
    QMap<quint32, std::function<void(const QJsonValue&)>> m_scripts;

    // The one finger we relay to the engine as a touch, and what became of it.
    int m_touchId = -1;
    QPointF m_touchStart;
    bool m_touchConsumed = false;
    QTimer m_longPressTimer;
    // Two fingers: how far apart they were at the last pinch message.
    bool m_pinching = false;
    qreal m_pinchDistance = 0;

    // Buffers the engine connection has let go of since the last updatePaintNode.
    QVector<quint64> m_buffersGone;

    // Render thread state. The GUI thread is blocked while updatePaintNode runs, so it may read m_frame and
    // m_buffersGone.
    DirectFrameTextures* m_directTextures = nullptr;
    // The page crashed, or the engine did: reloading loads the page anew.
    bool m_crashed = false;
    quint64 m_directGeneration = 0;
    bool m_loggedPath = false;
    bool m_coverNodeShown = false;
};

class EngineViewLoadRequest : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl url READ url)
    Q_PROPERTY(EngineView::LoadStatus status READ status)
    Q_PROPERTY(QString errorString READ errorString)

public:
    EngineViewLoadRequest(const QUrl& url, EngineView::LoadStatus status, const QString& errorString = QString())
        : m_url(url)
        , m_status(status)
        , m_errorString(errorString)
    {
    }

    QUrl url() const { return m_url; }
    EngineView::LoadStatus status() const { return m_status; }
    QString errorString() const { return m_errorString; }

private:
    QUrl m_url;
    EngineView::LoadStatus m_status;
    QString m_errorString;
};
