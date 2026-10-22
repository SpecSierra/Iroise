/*
 * Iroise: the tabs (see WebContainer.h).
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "WebContainer.h"

#include "LadybirdEngine.h"
#include "WebPage.h"
#include "declarativehistorymodel.h"
#include "declarativetabmodel.h"
#include "persistenttabmodel.h"
#include "privatetabmodel.h"
#include "dbmanager.h"
#include "faviconmanager.h"
#include "tab.h"

#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <QFile>
#include <QScreen>
#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QQuickWindow>
#include <QWindow>

#include <unistd.h>

// Where the page's device pixel ratio comes from: a 411 CSS pixel wide viewport on the Xperia 10 II (as Chrome gives a
// 1080 pixel wide phone), whose Silica pixel ratio is 1.75, gives 2.625, which the quarter steps make 2.75 (393 CSS
// pixels); other devices scale from it (see initialPageDeviceScaleFactor): the Jolla Phone 2 gets 2.25, 480 CSS pixels
// wide. 3.0 (360 pixels) showed pages too large. The screen size is a fallback for when Qt does not know it yet.
static constexpr qreal kReferenceViewportWidth = 411.0;
static constexpr qreal kReferenceUiPixelRatio = 1.75;
static constexpr qreal kReferenceDeviceScale = 2.625;
static constexpr qreal kFallbackScreenWidth = 1080.0;
static constexpr qreal kFallbackScreenHeight = 2520.0;

namespace {

QSizeF screenSizeOrFallback(QScreen *screen)
{
    if (screen) {
        return screen->size();
    }
    return QSizeF(kFallbackScreenWidth, kFallbackScreenHeight);
}

} // namespace


WebContainer::WebContainer(QQuickItem *parent)
    : QQuickItem(parent)
{
    // The tab overview finishes a "close all tabs" still counting down.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &WebContainer::applicationClosing);
    // A page that opens a window (a link with a target, window.open) gets a new tab, showing the engine's view.
    connect(LadybirdEngine::instance(), &LadybirdEngine::viewOffered, this, [this](quint32 view) {
        m_viewForNextPage = view;
        if (!m_tabModel || !m_tabModel->newTab(QString(), tabId(), false, false))
            LadybirdEngine::instance()->removeView(view);
        m_viewForNextPage = 0;
    });

    setAcceptedMouseButtons(Qt::AllButtons);
    setFiltersChildMouseEvents(false);

    // --- Tab discarding ---
    // A tab in the background keeps its page alive in the engine, with its
    // processes and their memory. Keep only the active tab + a small MRU set
    // live; discard the rest (their tab-model entry survives, so activatePage()
    // recreates + reloads them on return).
    if (qEnvironmentVariableIsSet("IROISE_TAB_DISCARD"))
        m_tabDiscardEnabled = qgetenv("IROISE_TAB_DISCARD").trimmed() != "0";
    // A tab is a whole page process: phones with less memory keep fewer of them alive.
    {
        QFile meminfo(QStringLiteral("/proc/meminfo"));
        if (meminfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray line = meminfo.readLine();
            if (line.startsWith("MemTotal:")) {
                const long totalMb = line.mid(9).trimmed().split(' ').first().toLong() / 1024;
                m_maxLiveTabCount = totalMb <= 4096 ? 3 : totalMb <= 6144 ? 4 : 5;
            }
        }
    }
    if (qEnvironmentVariableIsSet("IROISE_MAX_LIVE_TABS"))
        m_maxLiveTabCount = qMax(1, qEnvironmentVariableIntValue("IROISE_MAX_LIVE_TABS"));
    if (qEnvironmentVariableIsSet("IROISE_TAB_DISCARD_KEEP_MIN"))
        m_pressureKeepMin = qMax(1, qEnvironmentVariableIntValue("IROISE_TAB_DISCARD_KEEP_MIN"));
    if (qEnvironmentVariableIsSet("IROISE_TAB_DISCARD_MEMFREE_MB"))
        m_memFreeThresholdMb = qMax(0, qEnvironmentVariableIntValue("IROISE_TAB_DISCARD_MEMFREE_MB"));
    qInfo() << "[DISCARD] enabled=" << m_tabDiscardEnabled
            << "maxLive=" << m_maxLiveTabCount
            << "keepMin=" << m_pressureKeepMin
            << "memFreeThreshMB=" << m_memFreeThresholdMb;

    // Poll MemFree so we can shed tabs under pressure even without tab switches.
    m_memoryPressureTimer = new QTimer(this);
    m_memoryPressureTimer->setInterval(8000);
    connect(m_memoryPressureTimer, &QTimer::timeout,
            this, &WebContainer::checkMemoryPressure);
    if (m_tabDiscardEnabled)
        m_memoryPressureTimer->start();

    // Foreground tracking follows the Wayland window visibility: lipstick
    // marks the window Minimized when the app is covered/on the home screen
    // and Hidden when the display is off. (The former QML binding via
    // fakeVisibility()/Qt.application.state evaluated false at startup and
    // never re-fired, so it cannot be trusted for page visibility.)
    connect(this, &QQuickItem::windowChanged,
            this, &WebContainer::onWindowChangedForForeground);
    connect(this, &QQuickItem::visibleChanged, this, &WebContainer::visibilityChanged);
}

void WebContainer::onWindowChangedForForeground(QQuickWindow *win)
{
    if (m_foregroundWindow) {
        disconnect(m_foregroundWindow.data(), &QWindow::visibilityChanged,
                   this, &WebContainer::updateForegroundFromWindow);
    }
    m_foregroundWindow = win;
    if (win) {
        connect(win, &QWindow::visibilityChanged,
                this, &WebContainer::updateForegroundFromWindow);
    }
    updateForegroundFromWindow();
}

void WebContainer::updateForegroundFromWindow()
{
    // Fail towards visible: only an explicit Minimized/Hidden report counts
    // as background. No window yet (startup) or an unexpected visibility
    // value must never leave the active page hidden (black content).
    const QQuickWindow *win = m_foregroundWindow.data();
    const QWindow::Visibility vis = win ? win->visibility() : QWindow::AutomaticVisibility;
    const bool fg = vis != QWindow::Minimized && vis != QWindow::Hidden;
    qDebug() << "[VIS] window visibility="
             << (win ? static_cast<int>(win->visibility()) : -1)
             << "-> foreground" << fg;
    setForeground(fg);
}

WebContainer::~WebContainer()
{
    qDeleteAll(m_pages);
}
void WebContainer::trackParentSize()
{
    QQuickItem *p = parentItem();
    if (!p) {
        return;
    }

    connect(p, &QQuickItem::widthChanged, this, [this]() {
        if (parentItem() && parentItem()->width() > 0) {
            setWidth(parentItem()->width());
        }
    });
    connect(p, &QQuickItem::heightChanged, this, [this]() {
        if (parentItem() && parentItem()->height() > 0) {
            setHeight(parentItem()->height());
        }
    });
}

void WebContainer::ensureContainerHasUsableSize()
{
    if (width() != 0 && height() != 0) {
        return;
    }

    const QSizeF screenSize = screenSizeOrFallback(QGuiApplication::primaryScreen());
    QQuickItem *p = parentItem();
    const qreal parentWidth = p && p->width() > 0 ? p->width() : screenSize.width();
    const qreal parentHeight = p && p->height() > 0 ? p->height() : screenSize.height();

    setWidth(parentWidth);
    setHeight(parentHeight);
    trackParentSize();
}

QSizeF WebContainer::preferredPageSize(const QSizeF &screenSize) const
{
    return QSizeF(width() > 0 ? width() : screenSize.width(),
                  height() > 0 ? height() : screenSize.height());
}

qreal WebContainer::initialPageDeviceScaleFactor(const QSizeF &screenSize) const
{
    // Scale from the Silica UI pixel ratio, so web content matches the size of
    // the system UI (and follows the user's display-scale setting). The screen
    // width alone oversized it on the Jolla Phone 2: 1032px / 360 = 2.87 at
    // pixelRatio 1.5. The Xperia 10 II is the reference (1.75 -> 3.0); the
    // Jolla gets 2.57, rounded to 2.5. Quarter steps keep the scale off odd
    // fractions, whose tile-edge rounding shows as seams. (Figures above are for the earlier reference of 3.0.)
    if (m_uiPixelRatio > 0.0) {
        const qreal scale = kReferenceDeviceScale
                            * m_uiPixelRatio / kReferenceUiPixelRatio;
        return qMax(qreal(1.0), qRound(scale * 4.0) / 4.0);
    }
    return screenSize.width() / kReferenceViewportWidth;
}

qreal WebContainer::insetPageHeight(qreal baseHeight) const
{
    return qMax(qreal(0), baseHeight - m_bottomInset);
}

void WebContainer::configurePageGeometry(WebPage *page, const QSizeF &screenSize)
{
    if (!page) {
        return;
    }

    const QSizeF size = preferredPageSize(screenSize);
    page->setWidth(size.width());
    page->setHeight(insetPageHeight(size.height()));
    connect(this, &QQuickItem::widthChanged, page, [this, page]() { page->setWidth(width()); });
    connect(this, &QQuickItem::heightChanged, page, [this, page]() { page->setHeight(insetPageHeight(height())); });
}

void WebContainer::setContentBottomInset(qreal inset)
{
    if (inset < 0.0) {
        inset = 0.0;
    }
    if (qFuzzyCompare(m_bottomInset, inset)) {
        return;
    }
    const bool grew = inset > m_bottomInset;
    m_bottomInset = inset;
    // Re-lay out every live page's layout viewport (not just the active one) so
    // returning to a background tab already reflects the reserved strip.
    const qreal base = height();
    for (WebPage *page : m_pages) {
        if (page) {
            page->setHeight(insetPageHeight(base));
        }
    }

    if (!grew || !m_contentItem) {
        return;
    }
    // The viewport just got shorter (keyboard opening, or the toolbar coming
    // back): a focused input in normal flow can now sit below the fold. Reveal
    // it on the active page only — a background tab has no keyboard. The
    // relayout is async (dispatch_set_size), so retry across a couple of frames
    // rather than racing it; the script is a no-op once the element is visible.
    QPointer<WebPage> page(m_contentItem);
    for (int delayMs : { 0, 150, 350 }) {
        QTimer::singleShot(delayMs, page, [page]() {
            if (page) {
                page->revealFocusedEditable();
            }
        });
    }
}

void WebContainer::initializeTabModels(int nextTabId)
{
    m_persistentTabModel = new PersistentTabModel(nextTabId);
    // Private ids live in their own range: both models share m_pages keyed by
    // tab id, and "+100" collided once 100 persistent tabs had been opened.
    m_privateTabModel = new PrivateTabModel(nextTabId + 1000000);

    m_tabModel = m_persistentTabModel;
    connect(m_tabModel, SIGNAL(activeTabChanged(int)), this, SLOT(onActiveTabChanged(int)));
    connect(m_tabModel, SIGNAL(tabAdded(int)), this, SLOT(onTabAdded(int)));
    connect(m_tabModel, SIGNAL(tabClosed(int)), this, SLOT(onTabClosed(int)));
    emit tabModelChanged();
}

void WebContainer::restoreInitialContent()
{
    if (!m_persistentTabModel) {
        return;
    }

    qDebug() << "[INIT] persistentLoaded: initialUrl=" << m_initialUrl
             << "tabCount=" << m_persistentTabModel->count();
    if (!m_initialUrl.isEmpty()) {
        load(m_initialUrl);
        return;
    }

    if (m_persistentTabModel->count() == 0) {
        // DeclarativeTabModel::newTab() refuses about:blank when tabs are empty,
        // so create a page directly without going through the tab model.
        WebPage *page = getOrCreatePage(1);
        page->setActive(true);
        page->setVisible(true);
        m_contentItem = page;
        emit contentItemChanged();
        emit needChromeChanged();
        qDebug() << "[INIT] created initial empty page directly";
        return;
    }

    const int activeId = m_persistentTabModel->activeTabId();
    if (activeId > 0) {
        activatePage(activeId);
    } else if (!m_persistentTabModel->tabs().isEmpty()) {
        activatePage(m_persistentTabModel->tabs().first().tabId());
    }
}

WebPage *WebContainer::contentItem() const
{
    return m_contentItem;
}

bool WebContainer::needChrome() const
{
    return !m_contentItem || (m_contentItem->chrome() && !m_contentItem->fullscreen());
}

DeclarativeTabModel *WebContainer::tabModel() const
{
    return m_tabModel;
}

DeclarativeTabModel *WebContainer::persistentTabModel() const
{
    return m_persistentTabModel;
}

DeclarativeTabModel *WebContainer::privateTabModel() const
{
    return m_privateTabModel;
}

bool WebContainer::loading() const
{
    return m_contentItem ? m_contentItem->isLoading() : false;
}

int WebContainer::loadProgress() const
{
    return m_contentItem ? m_contentItem->loadProgress() : 0;
}

bool WebContainer::canGoForward() const
{
    return m_contentItem ? m_contentItem->canGoForward() : false;
}

bool WebContainer::canGoBack() const
{
    return m_contentItem ? m_contentItem->canGoBack() : false;
}

int WebContainer::tabId() const
{
    return m_tabModel ? m_tabModel->activeTabId() : 0;
}

QString WebContainer::title() const
{
    if (!m_contentItem)
        return QString();

    if (m_waitingForFreshTitle)
        return m_contentItem->url().toString();

    const QString pageTitle = m_contentItem->title();
    return pageTitle.isEmpty() ? m_contentItem->url().toString() : pageTitle;
}

QString WebContainer::url() const
{
    return m_contentItem ? m_contentItem->url().toString() : QString();
}

void WebContainer::setForeground(bool f)
{
    if (m_foreground != f) {
        m_foreground = f;
        // Tell every page about the app-level state so the engine can throttle
        // pages while the app is minimized (and resume them on return).
        for (auto it = m_pages.constBegin(); it != m_pages.constEnd(); ++it) {
            if (it.value()) {
                it.value()->setAppForeground(f);
            }
        }
        emit foregroundChanged();
    }
}

void WebContainer::setPrivateMode(bool p)
{
    if (m_privateMode != p) {
        m_privateMode = p;
        // Disconnect old model
        if (m_tabModel) {
            disconnect(m_tabModel, nullptr, this, nullptr);
        }
        m_tabModel = p ? static_cast<DeclarativeTabModel *>(m_privateTabModel)
                       : static_cast<DeclarativeTabModel *>(m_persistentTabModel);
        if (m_tabModel) {
            connect(m_tabModel, SIGNAL(activeTabChanged(int)), this, SLOT(onActiveTabChanged(int)));
            connect(m_tabModel, SIGNAL(tabAdded(int)), this, SLOT(onTabAdded(int)));
            connect(m_tabModel, SIGNAL(tabClosed(int)), this, SLOT(onTabClosed(int)));
        }
        emit tabModelChanged();

        if (m_tabModel && m_tabModel->count() > 0) {
            const int activeId = m_tabModel->activeTabId();
            if (activeId > 0) {
                activatePage(activeId);
            } else if (!m_tabModel->tabs().isEmpty()) {
                activatePage(m_tabModel->tabs().first().tabId());
            }
        } else if (m_contentItem) {
            m_contentItem->setActive(false);
            m_contentItem->setVisible(false);
            m_contentItem = nullptr;
            emitContentItemChanged();
        }

        emit privateModeChanged();
    }
}
void WebContainer::setChromeWindow(QObject *w)
{
    if (m_chromeWindow != w) {
        m_chromeWindow = w;
        emit chromeWindowChanged();
        if (w) emit chromeExposed();
    }
}

void WebContainer::setReadyToPaint(bool r)
{
    if (m_readyToPaint != r) {
        m_readyToPaint = r;
        emit readyToPaintChanged();
    }
}

void WebContainer::setHistoryModel(DeclarativeHistoryModel *m)
{
    if (m_historyModel != m) {
        m_historyModel = m;
        emit historyModelChanged();
    }
}

void WebContainer::classBegin()
{
}

void WebContainer::componentComplete()
{

    // Previews of pages from an earlier run can never be shown again (the
    // back/forward list does not survive a restart) and are page screenshots.

    // Qt Quick routes input events based on item bounding-box.  If the
    // container has size 0×0 (no QML anchor/size binding from the page),
    // touch events are never delivered to EngineView children.  Initialise
    // to screen size here; the OverlayAnimator may later animate height.
    ensureContainerHasUsableSize();

    int maxTabId = DBManager::instance()->getMaxTabId();
    int nextTabId = maxTabId + 1;

    initializeTabModels(nextTabId);

    m_completed = true;
    emit completedChanged();

    // After the persistent model finishes its async DB load, make sure something is shown.
    // If an initial URL was requested, load it; otherwise activate the persisted tab, or
    // fall back to a blank page if there are no saved tabs.
    if (m_persistentTabModel->loaded()) {
        QTimer::singleShot(0, this, &WebContainer::restoreInitialContent);
    } else {
        connect(m_persistentTabModel, &DeclarativeTabModel::loadedChanged, this, &WebContainer::restoreInitialContent);
    }

    if (!m_initialUrl.isEmpty())
        emit hasInitialUrlChanged();
}

void WebContainer::load(const QString &url, const QString &title, bool newTab)
{
    Q_UNUSED(title)
    qDebug() << "[LOAD] load() url=" << url << "completed=" << m_completed
             << "tabModel=" << (m_tabModel ? m_tabModel->count() : -1)
             << "contentItem=" << m_contentItem
             << "newTab=" << newTab;

    if (!m_completed) {
        qDebug() << "[LOAD] not completed, saving as initialUrl";
        m_initialUrl = url;
        emit hasInitialUrlChanged();
        return;
    }

    if (!m_tabModel) { qDebug() << "[LOAD] no tabModel!"; return; }

    if (newTab || m_tabModel->count() == 0) {
        qDebug() << "[LOAD] newTab for" << url;
        m_tabModel->newTab(url, newTab);
    } else {
        if (m_contentItem) {
            const QUrl targetUrl(url);
            if (m_contentItem->url() == targetUrl) {
                qDebug() << "[LOAD] reload active contentItem:" << url;
                m_contentItem->reload();
            } else {
                qDebug() << "[LOAD] setUrl on contentItem:" << url;
                m_contentItem->setUrl(targetUrl);
            }
        } else {
            qDebug() << "[LOAD] contentItem is NULL, cannot load!";
        }
    }
}

void WebContainer::reload()
{
    if (m_contentItem) m_contentItem->reload();
}

void WebContainer::goForward()
{
    if (m_contentItem) m_contentItem->goForward();
}

void WebContainer::goBack()
{
    if (m_contentItem) m_contentItem->goBack();
}

int WebContainer::activateTab(int tabId, const QString &url)
{
    if (!m_tabModel) return -1;
    m_tabModel->activateTabById(tabId);
    if (!url.isEmpty() && m_contentItem) {
        m_contentItem->setUrl(QUrl(url));
    }
    return tabId;
}

void WebContainer::closeTab(int tabId)
{
    if (!m_tabModel) return;
    bool isActive = (tabId == this->tabId());
    m_tabModel->removeTabById(tabId, isActive);
}
void WebContainer::updateContentOrientation(Qt::ScreenOrientation orientation)
{
    emit webContentOrientationChanged(orientation);
}

void WebContainer::applyContentOrientation(Qt::ScreenOrientation orientation)
{
    updateContentOrientation(orientation);
}

void WebContainer::setAdBlockEnabled(bool enabled)
{
    WebPage::applyAdBlockEnabledGlobally(enabled);
}

void WebContainer::setCookieBannerBlockingEnabled(bool enabled)
{
    WebPage::applyCookieBannerBlockingGlobally(enabled);
}

void WebContainer::setAdBlockAllowlist(const QString &json)
{
    WebPage::applyAdBlockAllowlistGlobally(json);
}

void WebContainer::setSiteUaOverrides(const QString &json)
{
    WebPage::applySiteUaOverridesGlobally(json);
}

void WebContainer::setJavaScriptBlocklist(const QString &json)
{
    WebPage::applyJavaScriptBlocklistGlobally(json);
}

void WebContainer::onActiveTabChanged(int activeTabId)
{
    activatePage(activeTabId);
}

void WebContainer::onTabAdded(int tabId)
{
    // Pre-create the page so it's ready
    getOrCreatePage(tabId);
    // A newly-added background tab must not blow the live-tab budget: if we're
    // already at the cap it will be discarded here and reload when first shown.
    enforceLiveTabBudget();
}

void WebContainer::onTabClosed(int tabId)
{
    m_mruTabs.removeAll(tabId);
    WebPage *page = m_pages.take(tabId);
    const bool wasPrivate = page && page->privateBrowsing();
    if (page) {
        page->setVisible(false);
        page->deleteLater();
        if (m_contentItem == page) {
            m_contentItem = nullptr;
            emitContentItemChanged();
        }
    }
    // Closing the last private tab ends the private session: private mode entered again later in the same run starts
    // with no cookies, storage or trusted certificates.
    if (wasPrivate && m_privateTabModel && m_privateTabModel->count() == 0)
        WebPage::clearPrivateBrowsingData();
}

void WebContainer::activatePage(int tabId)
{
    qDebug() << "[ACTIVATE] tabId=" << tabId << "currentContent=" << m_contentItem;
    if (m_contentItem) {
        m_contentItem->setActive(false);
        m_contentItem->setVisible(false);
    }

    // Get or create new page
    WebPage *page = getOrCreatePage(tabId);

    // Load the tab's URL if page is new (empty)
    if (m_tabModel) {
        QString tabUrl = m_tabModel->url(tabId);
        if (!tabUrl.isEmpty() && page->url().isEmpty()) {
            page->setUrl(QUrl(tabUrl));
        }
    }

    page->setActive(true);
    page->setVisible(true);

    if (m_contentItem != page) {
        m_contentItem = page;
        m_waitingForFreshTitle = page->title().isEmpty();
        emitContentItemChanged();
    }

    // The just-activated tab is the most-recently-used; discard anything that
    // now falls outside the live-tab budget.
    touchMru(tabId);
    enforceLiveTabBudget();
}

void WebContainer::emitContentItemChanged()
{
    emit contentItemChanged();
    emit needChromeChanged();
    emit tabIdChanged();
    emit urlChanged();
    emit titleChanged();
    emit loadingChanged();
    emit loadProgressChanged();
    emit canGoBackChanged();
    emit canGoForwardChanged();
}

void WebContainer::touchMru(int tabId)
{
    m_mruTabs.removeAll(tabId);
    m_mruTabs.prepend(tabId);
}

int WebContainer::effectiveLiveTabCap() const
{
    // Normally keep the active tab + (m_maxLiveTabCount-1) MRU tabs. Under
    // memory pressure, shrink toward m_pressureKeepMin so we shed aggressively.
    if (m_underMemoryPressure)
        return qMin(m_maxLiveTabCount, m_pressureKeepMin);
    return m_maxLiveTabCount;
}

void WebContainer::discardPage(int tabId)
{
    // Never discard the tab the user is looking at.
    if (m_contentItem && m_pages.value(tabId) == m_contentItem)
        return;
    WebPage *page = m_pages.take(tabId);
    if (!page)
        return;
    // Keep the tab-model entry (URL/title/thumbnail) — only the live page and
    // its engine view go away. activatePage() recreates + reloads it on return.
    page->setActive(false);
    page->setVisible(false);
    // Destroying the page destroys its engine view, and with it the page's processes.
    page->deleteLater();
    qInfo() << "[DISCARD] discarded tab" << tabId
            << "livePages=" << m_pages.size();
}

void WebContainer::enforceLiveTabBudget()
{
    if (!m_tabDiscardEnabled)
        return;

    const int cap = effectiveLiveTabCap();
    if (m_pages.size() <= cap)
        return;

    // Build the discard order: least-recently-used first. Any live page not in
    // the MRU list (e.g. pre-created background tabs) is treated as coldest.
    QList<int> victims;
    for (int tabId : m_pages.keys()) {
        if (!m_mruTabs.contains(tabId))
            victims.prepend(tabId);        // never-activated → coldest
    }
    for (int i = m_mruTabs.size() - 1; i >= 0; --i) {
        int tabId = m_mruTabs.at(i);
        if (m_pages.contains(tabId))
            victims.append(tabId);         // LRU appended after the never-used
    }

    const int activeTabId = this->tabId();
    for (int tabId : victims) {
        if (m_pages.size() <= cap)
            break;
        if (tabId == activeTabId)
            continue;
        discardPage(tabId);
    }
}

void WebContainer::checkMemoryPressure()
{
    if (!m_tabDiscardEnabled)
        return;

    // MemAvailable is bogus on this vendor kernel; trust MemFree.
    QFile f(QStringLiteral("/proc/meminfo"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    long memFreeKb = -1;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (line.startsWith("MemFree:")) {
            memFreeKb = line.mid(8).trimmed().split(' ').first().toLong();
            break;
        }
    }
    f.close();
    if (memFreeKb < 0)
        return;

    const bool nowPressure = (memFreeKb / 1024) < m_memFreeThresholdMb;
    if (nowPressure != m_underMemoryPressure) {
        m_underMemoryPressure = nowPressure;
        qInfo() << "[DISCARD] memory pressure" << (nowPressure ? "ON" : "OFF")
                << "MemFree=" << (memFreeKb / 1024) << "MB cap=" << effectiveLiveTabCap();
    }
    if (nowPressure)
        enforceLiveTabBudget();
}

WebPage *WebContainer::getOrCreatePage(int tabId)
{
    qDebug() << "[PAGE] getOrCreatePage tabId=" << tabId << "exists=" << m_pages.contains(tabId);
    if (m_pages.contains(tabId)) {
        return m_pages[tabId];
    }

    // No parent yet: the page goes into the scene last, once it is set up.
    WebPage *page = new WebPage(nullptr);
    page->setTabId(tabId);
    if (m_tabModel)
        page->setParentId(m_tabModel->parentTabId(tabId));
    if (m_viewForNextPage) {
        page->adoptEngineView(m_viewForNextPage);
        m_viewForNextPage = 0;
    }
    // Tag the page with the mode it was born into, BEFORE it gets its size (with
    // which the engine's view is created): private pages must be created in the
    // engine's private session. A page keeps this for its whole life — the two
    // tab models use disjoint id ranges, so a page never switches modes.
    page->setPrivateBrowsing(m_privateMode);
    page->setVisible(false);
    page->setActive(false);
    page->setAppForeground(m_foreground);

    const QScreen *screen = QGuiApplication::primaryScreen();
    const QSizeF screenSize = screenSizeOrFallback(QGuiApplication::primaryScreen());
    configurePageGeometry(page, screenSize);

    if (screen)
        page->setDeviceScaleFactor(initialPageDeviceScaleFactor(screenSize));

    connectPage(page);
    m_pages[tabId] = page;

    // Into the scene, with its size, mode and connections in place.
    page->setParentItem(this);
    return page;
}

void WebContainer::connectPage(WebPage *page)
{
    connect(page, &EngineView::closeRequested, this, [this, page] { closeTab(page->tabId()); });
    // Keep tab metadata (URL/title/mode) in sync with the tab model that created this page.
    // Using the model captured at page-creation time avoids cross-updating when switching
    // between persistent/private models later.
    DeclarativeTabModel *pageModel = m_tabModel;
    if (pageModel) {
        connect(page, &EngineView::urlChanged, pageModel, &DeclarativeTabModel::onUrlChanged);
        connect(page, &EngineView::titleChanged, pageModel, &DeclarativeTabModel::onTitleChanged);
    }

    // Not tied to the tab model: icons are stored per host, for every page.
    connect(page, &WebPage::faviconChanged, this, &WebContainer::onPageFaviconChanged);

    connect(page, &EngineView::urlChanged, this, &WebContainer::onPageUrlChanged);
    connect(page, &EngineView::titleChanged, this, &WebContainer::onPageTitleChanged);
    // EngineView::loadingChanged carries a EngineViewLoadRequest* — use a lambda to adapt
    connect(page, &EngineView::loadingChanged, this, [this, page](EngineViewLoadRequest *) {
        if (page == m_contentItem) {
            emit loadingChanged();
            emit canGoBackChanged();
            emit canGoForwardChanged();
        }
    });
    // Same-document navigations (SPA pushState) emit no load change, so without
    // this the toolbar's back/forward state stayed stale mid-SPA and the button
    // kept rendering the home glyph.
    connect(page, &EngineView::backForwardChanged, this, [this, page]() {
        if (page == m_contentItem) {
            emit canGoBackChanged();
            emit canGoForwardChanged();
        }
    });
    connect(page, &EngineView::loadProgressChanged, this, &WebContainer::onPageLoadProgressChanged);
    connect(page, &WebPage::paintedChanged, this, &WebContainer::onPagePaintedChanged);
    connect(page, &WebPage::chromeChanged, this, &WebContainer::needChromeChanged);
    connect(page, &WebPage::fullscreenChanged, this, &WebContainer::needChromeChanged);
    // NB: The model the page was made for, as for its URL and title: private mode may have switched since.
    if (pageModel) {
        QPointer<DeclarativeTabModel> model(pageModel);
        connect(page, &WebPage::thumbnailCaptured, this, [model, page](const QString &source) {
            if (model)
                model->updateThumbnailPath(page->tabId(), source);
        });
    }
    connect(page, &WebPage::touched, this, [this, page]() {
        if (page == m_contentItem)
            emit touched();
    });
    connect(page, &WebPage::crashedChanged, this, [page]() {
        if (page->crashed())
            qWarning() << "[tabs] engine view crashed for tab" << page->tabId()
                       << "— tab isolated, browser alive";
    });
}

void WebContainer::onPageUrlChanged()
{
    WebPage *page = qobject_cast<WebPage *>(sender());
    if (!page || page != m_contentItem) return;

    QString newUrl = page->url().toString();
    if (newUrl == QStringLiteral("about:blank")) return;

    m_waitingForFreshTitle = true;
    emit urlChanged();
    emit titleChanged();

    // Update tab model
    if (m_tabModel) {
        // The tab model tracks URL via its own signals; here we record history.
        // Private tabs never touch the history store.
        if (m_historyModel && !newUrl.isEmpty() && !page->privateBrowsing()) {
            m_historyModel->add(newUrl, page->title());
        }
    }
}

// The engine's icon for the page, kept for the tab overview, favourites and history (not for private tabs).
void WebContainer::onPageFaviconChanged()
{
    WebPage *page = qobject_cast<WebPage *>(sender());
    if (!page || page->privateBrowsing() || !FaviconManager::isRealIcon(page->favicon()))
        return;
    FaviconManager::instance()->add(QStringLiteral("history"), page->url().toString(), page->favicon(), true);
}

void WebContainer::onPageTitleChanged()
{
    WebPage *page = qobject_cast<WebPage *>(sender());
    if (!page || page != m_contentItem) return;
    m_waitingForFreshTitle = page->title().isEmpty();
    emit titleChanged();
    // Update history entry once title is known. Private tabs are never recorded.
    QString url = page->url().toString();
    QString title = page->title();
    if (m_historyModel && !url.isEmpty() && url != QStringLiteral("about:blank")
            && !title.isEmpty() && !page->privateBrowsing()) {
        m_historyModel->add(url, title);
    }
}

void WebContainer::onPageLoadProgressChanged()
{
    WebPage *page = qobject_cast<WebPage *>(sender());
    if (!page || page != m_contentItem) return;
    emit loadProgressChanged();
}

void WebContainer::onPagePaintedChanged()
{
    WebPage *page = qobject_cast<WebPage *>(sender());
    if (!page || page != m_contentItem) return;
    if (page->painted() && !m_activeTabRendered) {
        setActiveTabRendered(true);
    }
}

void WebContainer::setActiveTabRendered(bool r)
{
    if (m_activeTabRendered != r) {
        m_activeTabRendered = r;
        emit activeTabRenderedChanged();
    }
}
