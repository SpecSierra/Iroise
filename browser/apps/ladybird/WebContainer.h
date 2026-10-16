/*
 * Pacific: the tabs. Keeps a WebPage per live tab (discarding the least recently used ones beyond a budget or under
 * memory pressure), shows the active one, and relays its state to the interface.
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <QQuickItem>
#include <QQmlParserStatus>
#include <QQmlComponent>
#include <QMap>
#include <QList>
#include <QPointer>


class QTimer;
class WebPage;
class DeclarativeTabModel;
class DeclarativeHistoryModel;
class PersistentTabModel;
class PrivateTabModel;

class WebContainer : public QQuickItem
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)

    Q_PROPERTY(WebPage *contentItem READ contentItem NOTIFY contentItemChanged FINAL)
    Q_PROPERTY(DeclarativeTabModel *tabModel READ tabModel NOTIFY tabModelChanged FINAL)
    Q_PROPERTY(DeclarativeTabModel *persistentTabModel READ persistentTabModel CONSTANT)
    Q_PROPERTY(DeclarativeTabModel *privateTabModel READ privateTabModel CONSTANT)
    Q_PROPERTY(bool completed READ completed NOTIFY completedChanged FINAL)
    Q_PROPERTY(bool foreground READ foreground WRITE setForeground NOTIFY foregroundChanged FINAL)
    Q_PROPERTY(int maxLiveTabCount MEMBER m_maxLiveTabCount NOTIFY maxLiveTabCountChanged FINAL)
    // Silica Theme.pixelRatio, set from QML (C++ has no Silica or dconf
    // access). Sets the device scale of pages created afterwards.
    Q_PROPERTY(qreal uiPixelRatio MEMBER m_uiPixelRatio NOTIFY uiPixelRatioChanged FINAL)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged FINAL)
    Q_PROPERTY(int loadProgress READ loadProgress NOTIFY loadProgressChanged FINAL)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY canGoForwardChanged FINAL)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY canGoBackChanged FINAL)
    Q_PROPERTY(int tabId READ tabId NOTIFY tabIdChanged FINAL)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged FINAL)
    Q_PROPERTY(QString url READ url NOTIFY urlChanged FINAL)
    Q_PROPERTY(bool privateMode READ privateMode WRITE setPrivateMode NOTIFY privateModeChanged FINAL)
    Q_PROPERTY(bool activeTabRendered READ activeTabRendered NOTIFY activeTabRenderedChanged FINAL)
    Q_PROPERTY(QObject *chromeWindow READ chromeWindow WRITE setChromeWindow NOTIFY chromeWindowChanged FINAL)
    Q_PROPERTY(bool readyToPaint READ readyToPaint WRITE setReadyToPaint NOTIFY readyToPaintChanged FINAL)
    Q_PROPERTY(DeclarativeHistoryModel *historyModel READ historyModel WRITE setHistoryModel NOTIFY historyModelChanged FINAL)
    Q_PROPERTY(bool hasInitialUrl READ hasInitialUrl NOTIFY hasInitialUrlChanged FINAL)
    Q_PROPERTY(int visibility READ fakeVisibility NOTIFY visibilityChanged FINAL)
    Q_PROPERTY(bool needChrome READ needChrome NOTIFY needChromeChanged FINAL)

public:
    explicit WebContainer(QQuickItem *parent = nullptr);
    ~WebContainer() override;

    WebPage *contentItem() const;
    DeclarativeTabModel *tabModel() const;
    DeclarativeTabModel *persistentTabModel() const;
    DeclarativeTabModel *privateTabModel() const;

    bool completed() const { return m_completed; }
    bool foreground() const { return m_foreground; }
    void setForeground(bool f);
    bool loading() const;
    int loadProgress() const;
    bool canGoForward() const;
    bool canGoBack() const;
    int tabId() const;
    QString title() const;
    QString url() const;
    bool privateMode() const { return m_privateMode; }
    void setPrivateMode(bool p);
    bool activeTabRendered() const { return m_activeTabRendered; }
    QObject *chromeWindow() const { return m_chromeWindow; }
    void setChromeWindow(QObject *w);
    bool readyToPaint() const { return m_readyToPaint; }
    void setReadyToPaint(bool r);
    DeclarativeHistoryModel *historyModel() const { return m_historyModel; }
    void setHistoryModel(DeclarativeHistoryModel *m);
    bool hasInitialUrl() const { return !m_initialUrl.isEmpty(); }
    // As QWindow::Visibility: FullScreen while the item is visible, Hidden otherwise.
    int fakeVisibility() const { return isVisible() ? 5 : 0; }
    bool needChrome() const;

    Q_INVOKABLE void load(const QString &url, const QString &title = QString(), bool newTab = false);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void goBack();
    Q_INVOKABLE int activateTab(int tabId, const QString &url = QString());
    Q_INVOKABLE void closeTab(int tabId);
    Q_INVOKABLE void updateContentOrientation(Qt::ScreenOrientation orientation);
    Q_INVOKABLE void applyContentOrientation(Qt::ScreenOrientation orientation);
    // Ad-block toggle for the whole engine, pushed from BrowserPage.qml's setting.
    Q_INVOKABLE void setAdBlockEnabled(bool enabled);
    Q_INVOKABLE void setAdBlockAllowlist(const QString &json);
    // Cookie-banner auto-reject (autoconsent) toggle, same dconf-driven shape.
    Q_INVOKABLE void setCookieBannerBlockingEnabled(bool enabled);
    Q_INVOKABLE void setSiteUaOverrides(const QString &json);
    // Per-site "Enable JavaScript" toggle: JSON array of hosts where JS is off.
    Q_INVOKABLE void setJavaScriptBlocklist(const QString &json);
    // Reserve `inset` device-independent px at the bottom of the page's layout
    // viewport for the (bottom) URL bar, so position:fixed bottom content lays
    // out above the chrome instead of behind it. Driven from BrowserPage.qml
    // off the *settled* chrome state (one relayout per show/hide, not per
    // frame). 0 restores the full-height viewport. dconf-gated, default OFF.
    Q_INVOKABLE void setContentBottomInset(qreal inset);

    // QQmlParserStatus
    void classBegin() override;
    void componentComplete() override;

Q_SIGNALS:
    void contentItemChanged();
    void tabModelChanged();
    void completedChanged();
    void foregroundChanged();
    void maxLiveTabCountChanged();
    void uiPixelRatioChanged();
    void loadingChanged();
    void loadProgressChanged();
    void canGoForwardChanged();
    void canGoBackChanged();
    void tabIdChanged();
    void titleChanged();
    void urlChanged();
    void privateModeChanged();
    void activeTabRenderedChanged();
    void chromeWindowChanged();
    void chromeExposed();
    void readyToPaintChanged();
    void webContentOrientationChanged(Qt::ScreenOrientation orientation);
    void historyModelChanged();
    void applicationClosing();
    void hasInitialUrlChanged();
    void visibilityChanged();
    void needChromeChanged();
    // A new touch on the active page (WebPage::touched).
    void touched();

private Q_SLOTS:
    void onWindowChangedForForeground(QQuickWindow *win);
    void updateForegroundFromWindow();
    void onActiveTabChanged(int activeTabId);
    void onTabAdded(int tabId);
    void onTabClosed(int tabId);
    void onPageUrlChanged();
    void onPageTitleChanged();
    void onPageFaviconChanged();
    void onPageLoadProgressChanged();
    void onPagePaintedChanged();

private:
    void ensureContainerHasUsableSize();
    void trackParentSize();
    void initializeTabModels(int nextTabId);
    void restoreInitialContent();
    QSizeF preferredPageSize(const QSizeF &screenSize) const;
    qreal initialPageDeviceScaleFactor(const QSizeF &screenSize) const;
    // Base page height minus the reserved bottom inset (never negative). This is
    // the height handed to every WebPage → the page's layout viewport.
    qreal insetPageHeight(qreal baseHeight) const;
    void configurePageGeometry(WebPage *page, const QSizeF &screenSize);
    WebPage *getOrCreatePage(int tabId);
    void activatePage(int tabId);
    void setActiveTabRendered(bool r);
    void connectPage(WebPage *page);
    // The active page has changed (or gone): every property read through m_contentItem changes with it, and a QML
    // binding only reads it again when its NOTIFY fires.
    void emitContentItemChanged();
    // A view the engine opened for a page, waiting for the tab that shows it to be created.
    quint32 m_viewForNextPage = 0;

    // --- Tab discarding: a tab in the background that falls outside the live-tab budget loses its page (and with
    // it the engine's view, its processes and its memory). The tab stays in the tab model (URL/title/thumbnail
    // persist); activatePage() recreates and reloads it via getOrCreatePage() on return. ---
    void touchMru(int tabId);              // mark tabId most-recently-used
    void discardPage(int tabId);           // tear down the page, keep the tab
    void enforceLiveTabBudget();           // discard tabs beyond the MRU cap
    void checkMemoryPressure();            // poll MemFree, tighten cap if low
    int effectiveLiveTabCap() const;       // current cap (shrinks under pressure)

    WebPage *m_contentItem = nullptr;
    PersistentTabModel *m_persistentTabModel = nullptr;
    PrivateTabModel *m_privateTabModel = nullptr;
    DeclarativeTabModel *m_tabModel = nullptr;
    DeclarativeHistoryModel *m_historyModel = nullptr;
    QMap<int, WebPage *> m_pages;
    qreal m_bottomInset = 0.0;              // reserved bottom URL-bar strip (dip); see setContentBottomInset
    QList<int> m_mruTabs;                   // most-recently-used first; front = active
    QTimer *m_memoryPressureTimer = nullptr;
    bool m_tabDiscardEnabled = true;        // PACIFIC_TAB_DISCARD (default on)
    int m_pressureKeepMin = 2;              // PACIFIC_TAB_DISCARD_KEEP_MIN
    int m_memFreeThresholdMb = 400;         // PACIFIC_TAB_DISCARD_MEMFREE_MB
    bool m_underMemoryPressure = false;
    QPointer<QObject> m_chromeWindow;
    QPointer<QQuickWindow> m_foregroundWindow;
    bool m_completed = false;
    bool m_foreground = true;
    bool m_privateMode = false;
    bool m_activeTabRendered = false;
    bool m_readyToPaint = false;
    bool m_waitingForFreshTitle = false;
    int m_maxLiveTabCount = 5;
    qreal m_uiPixelRatio = 0.0;             // Silica Theme.pixelRatio; 0 = unknown
    QString m_initialUrl;
};

QML_DECLARE_TYPE(WebContainer)
