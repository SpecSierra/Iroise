/*
 * WPE WebKit engine replacement for Sailfish Browser
 * WPEWebContainer implementation
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "WPEWebContainer.h"
#include "WPEWebPage.h"
#include "WPERuntimePaths.h"
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

#pragma push_macro("signals")
#undef signals
#pragma pop_macro("signals")

#include <unistd.h>

namespace {

QSizeF screenSizeOrFallback(QScreen *screen)
{
    if (screen) {
        return screen->size();
    }
    return QSizeF(WPERuntimePaths::kFallbackScreenWidth, WPERuntimePaths::kFallbackScreenHeight);
}

} // namespace

WPEWebContainer::WPEWebContainer(QQuickItem *parent)
    : QQuickItem(parent)
{
    setAcceptedMouseButtons(Qt::AllButtons);
    setFiltersChildMouseEvents(false);

    // --- Tab discarding config (Lever 2) ---
    // A backgrounded tab keeps its whole WebProcess resident (~250MB RSS +
    // up to ~1GB pinned GPU tiles, measured on franceinfo/revolut). Keep only
    // the active tab + a small MRU set live; discard the rest (their tab-model
    // entry survives, so activatePage() recreates + reloads them on return).
    if (qEnvironmentVariableIsSet("PACIFIC_TAB_DISCARD"))
        m_tabDiscardEnabled = qgetenv("PACIFIC_TAB_DISCARD").trimmed() != "0";
    if (qEnvironmentVariableIsSet("PACIFIC_MAX_LIVE_TABS"))
        m_maxLiveTabCount = qMax(1, qEnvironmentVariableIntValue("PACIFIC_MAX_LIVE_TABS"));
    if (qEnvironmentVariableIsSet("PACIFIC_TAB_DISCARD_KEEP_MIN"))
        m_pressureKeepMin = qMax(1, qEnvironmentVariableIntValue("PACIFIC_TAB_DISCARD_KEEP_MIN"));
    if (qEnvironmentVariableIsSet("PACIFIC_TAB_DISCARD_MEMFREE_MB"))
        m_memFreeThresholdMb = qMax(0, qEnvironmentVariableIntValue("PACIFIC_TAB_DISCARD_MEMFREE_MB"));
    qInfo() << "[WPE-DISCARD] enabled=" << m_tabDiscardEnabled
            << "maxLive=" << m_maxLiveTabCount
            << "keepMin=" << m_pressureKeepMin
            << "memFreeThreshMB=" << m_memFreeThresholdMb;

    // Poll MemFree so we can shed tabs under pressure even without tab switches.
    m_memoryPressureTimer = new QTimer(this);
    m_memoryPressureTimer->setInterval(8000);
    connect(m_memoryPressureTimer, &QTimer::timeout,
            this, &WPEWebContainer::checkMemoryPressure);
    if (m_tabDiscardEnabled)
        m_memoryPressureTimer->start();

    // Foreground tracking follows the Wayland window visibility: lipstick
    // marks the window Minimized when the app is covered/on the home screen
    // and Hidden when the display is off. (The former QML binding via
    // fakeVisibility()/Qt.application.state evaluated false at startup and
    // never re-fired, so it cannot be trusted for page visibility.)
    connect(this, &QQuickItem::windowChanged,
            this, &WPEWebContainer::onWindowChangedForForeground);
}

void WPEWebContainer::onWindowChangedForForeground(QQuickWindow *win)
{
    if (m_foregroundWindow) {
        disconnect(m_foregroundWindow.data(), &QWindow::visibilityChanged,
                   this, &WPEWebContainer::updateForegroundFromWindow);
    }
    m_foregroundWindow = win;
    if (win) {
        connect(win, &QWindow::visibilityChanged,
                this, &WPEWebContainer::updateForegroundFromWindow);
    }
    updateForegroundFromWindow();
}

void WPEWebContainer::updateForegroundFromWindow()
{
    // Fail towards visible: only an explicit Minimized/Hidden report counts
    // as background. No window yet (startup) or an unexpected visibility
    // value must never leave the active page hidden (black content).
    const QQuickWindow *win = m_foregroundWindow.data();
    const QWindow::Visibility vis = win ? win->visibility() : QWindow::AutomaticVisibility;
    const bool fg = vis != QWindow::Minimized && vis != QWindow::Hidden;
    qDebug() << "[WPE-VIS] window visibility="
             << (win ? static_cast<int>(win->visibility()) : -1)
             << "-> foreground" << fg;
    setForeground(fg);
}

WPEWebContainer::~WPEWebContainer()
{
    qDeleteAll(m_pages);
}

void WPEWebContainer::configureSandboxPaths()
{
    // The Ladybird engine process sets up its own helpers' sandboxes; there is nothing to configure from here.
}

void WPEWebContainer::trackParentSize()
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

void WPEWebContainer::ensureContainerHasUsableSize()
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

QSizeF WPEWebContainer::preferredPageSize(const QSizeF &screenSize) const
{
    return QSizeF(width() > 0 ? width() : screenSize.width(),
                  height() > 0 ? height() : screenSize.height());
}

qreal WPEWebContainer::initialPageDeviceScaleFactor(const QSizeF &screenSize) const
{
    // The carried-forward Qt bridge still maps device scale to page zoom here.
    // Keep that known-good behavior explicit until a true device-scale path exists.
    //
    // Scale from the Silica UI pixel ratio, so web content matches the size of
    // the system UI (and follows the user's display-scale setting). The screen
    // width alone oversized it on the Jolla Phone 2: 1032px / 360 = 2.87 at
    // pixelRatio 1.5. The Xperia 10 II is the reference (1.75 -> 3.0); the
    // Jolla gets 2.57, rounded to 2.5. Quarter steps keep the scale off odd
    // fractions, whose tile-edge rounding shows as seams.
    if (m_uiPixelRatio > 0.0) {
        const qreal scale = WPERuntimePaths::kReferenceDeviceScale
                            * m_uiPixelRatio / WPERuntimePaths::kReferenceUiPixelRatio;
        return qMax(qreal(1.0), qRound(scale * 4.0) / 4.0);
    }
    return screenSize.width() / WPERuntimePaths::kReferenceViewportWidth;
}

qreal WPEWebContainer::insetPageHeight(qreal baseHeight) const
{
    return qMax(qreal(0), baseHeight - m_bottomInset);
}

void WPEWebContainer::configurePageGeometry(WPEWebPage *page, const QSizeF &screenSize)
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

void WPEWebContainer::setContentBottomInset(qreal inset)
{
    if (inset < 0.0) {
        inset = 0.0;
    }
    if (qFuzzyCompare(m_bottomInset, inset)) {
        return;
    }
    const bool grew = inset > m_bottomInset;
    m_bottomInset = inset;
    // Re-lay out every live page's WebKit viewport (not just the active one) so
    // returning to a background tab already reflects the reserved strip.
    const qreal base = height();
    for (WPEWebPage *page : m_pages) {
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
    QPointer<WPEWebPage> page(m_contentItem);
    for (int delayMs : { 0, 150, 350 }) {
        QTimer::singleShot(delayMs, page, [page]() {
            if (page) {
                page->revealFocusedEditable();
            }
        });
    }
}

void WPEWebContainer::initializeTabModels(int nextTabId)
{
    m_persistentTabModel = new PersistentTabModel(nextTabId, nullptr);
    // Private ids live in their own range: both models share m_pages keyed by
    // tab id, and "+100" collided once 100 persistent tabs had been opened.
    m_privateTabModel = new PrivateTabModel(nextTabId + 1000000, nullptr);

    m_tabModel = m_persistentTabModel;
    connect(m_tabModel, SIGNAL(activeTabChanged(int)), this, SLOT(onActiveTabChanged(int)));
    connect(m_tabModel, SIGNAL(tabAdded(int)), this, SLOT(onTabAdded(int)));
    connect(m_tabModel, SIGNAL(tabClosed(int)), this, SLOT(onTabClosed(int)));
    emit tabModelChanged();
}

void WPEWebContainer::restoreInitialContent()
{
    if (!m_persistentTabModel) {
        return;
    }

    qDebug() << "[WPE-INIT] persistentLoaded: initialUrl=" << m_initialUrl
             << "tabCount=" << m_persistentTabModel->count();
    if (!m_initialUrl.isEmpty()) {
        load(m_initialUrl);
        return;
    }

    if (m_persistentTabModel->count() == 0) {
        // DeclarativeTabModel::newTab() refuses about:blank when tabs are empty,
        // so create a page directly without going through the tab model.
        WPEWebPage *page = getOrCreatePage(1);
        page->setActive(true);
        page->setVisible(true);
        m_contentItem = page;
        emit contentItemChanged();
        emit needChromeChanged();
        qDebug() << "[WPE-INIT] created initial empty page directly";
        return;
    }

    const int activeId = m_persistentTabModel->activeTabId();
    if (activeId > 0) {
        activatePage(activeId);
    } else if (!m_persistentTabModel->tabs().isEmpty()) {
        activatePage(m_persistentTabModel->tabs().first().tabId());
    }
}

WPEWebPage *WPEWebContainer::contentItem() const
{
    return m_contentItem;
}

bool WPEWebContainer::needChrome() const
{
    return !m_contentItem || (m_contentItem->chrome() && !m_contentItem->fullscreen());
}

bool WPEWebContainer::selectionActive() const
{
    return m_contentItem && m_contentItem->textSelectionActive();
}

DeclarativeTabModel *WPEWebContainer::tabModel() const
{
    return m_tabModel;
}

DeclarativeTabModel *WPEWebContainer::persistentTabModel() const
{
    return m_persistentTabModel;
}

DeclarativeTabModel *WPEWebContainer::privateTabModel() const
{
    return m_privateTabModel;
}

bool WPEWebContainer::loading() const
{
    return m_contentItem ? m_contentItem->isLoading() : false;
}

int WPEWebContainer::loadProgress() const
{
    return m_contentItem ? m_contentItem->loadProgress() : 0;
}

bool WPEWebContainer::canGoForward() const
{
    return m_contentItem ? m_contentItem->canGoForward() : false;
}

bool WPEWebContainer::canGoBack() const
{
    return m_contentItem ? m_contentItem->canGoBack() : false;
}

int WPEWebContainer::tabId() const
{
    return m_tabModel ? m_tabModel->activeTabId() : 0;
}

QString WPEWebContainer::title() const
{
    if (!m_contentItem)
        return QString();

    if (m_waitingForFreshTitle)
        return m_contentItem->url().toString();

    const QString pageTitle = m_contentItem->title();
    return pageTitle.isEmpty() ? m_contentItem->url().toString() : pageTitle;
}

QString WPEWebContainer::url() const
{
    return m_contentItem ? m_contentItem->url().toString() : QString();
}

void WPEWebContainer::setForeground(bool f)
{
    if (m_foreground != f) {
        m_foreground = f;
        // Tell every page about the app-level state so WebKit can throttle
        // pages while the app is minimized (and resume them on return).
        for (auto it = m_pages.constBegin(); it != m_pages.constEnd(); ++it) {
            if (it.value()) {
                it.value()->setAppForeground(f);
            }
        }
        emit foregroundChanged();
    }
}

void WPEWebContainer::setPrivateMode(bool p)
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

        emit privateModeChanged();
    }
}

void WPEWebContainer::setWebPageComponent(QQmlComponent *c)
{
    if (m_webPageComponent != c) {
        m_webPageComponent = c;
        emit webPageComponentChanged(c);
    }
}

void WPEWebContainer::setChromeWindow(QObject *w)
{
    if (m_chromeWindow != w) {
        m_chromeWindow = w;
        emit chromeWindowChanged();
        if (w) emit chromeExposed();
    }
}

void WPEWebContainer::setReadyToPaint(bool r)
{
    if (m_readyToPaint != r) {
        m_readyToPaint = r;
        emit readyToPaintChanged();
    }
}

void WPEWebContainer::setHistoryModel(DeclarativeHistoryModel *m)
{
    if (m_historyModel != m) {
        m_historyModel = m;
        emit historyModelChanged();
    }
}

void WPEWebContainer::classBegin()
{
}

void WPEWebContainer::componentComplete()
{
    // Configure WebKit process sandbox paths before any web view is created.
    // bubblewrap requires explicit allowlisting of paths needed by the subprocesses.
    configureSandboxPaths();

    // Previews of pages from an earlier run can never be shown again (the
    // back/forward list does not survive a restart) and are page screenshots.
    WPEWebPage::clearHistoryPreviews();

    // Qt Quick routes input events based on item bounding-box.  If the
    // container has size 0×0 (no QML anchor/size binding from the page),
    // touch events are never delivered to WPEQtView children.  Initialise
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
        QTimer::singleShot(0, this, &WPEWebContainer::restoreInitialContent);
    } else {
        connect(m_persistentTabModel, &DeclarativeTabModel::loadedChanged, this, &WPEWebContainer::restoreInitialContent);
    }

    if (!m_initialUrl.isEmpty())
        emit hasInitialUrlChanged();
}

void WPEWebContainer::load(const QString &url, const QString &title, bool newTab)
{
    Q_UNUSED(title)
    qDebug() << "[WPE-LOAD] load() url=" << url << "completed=" << m_completed
             << "tabModel=" << (m_tabModel ? m_tabModel->count() : -1)
             << "contentItem=" << m_contentItem
             << "newTab=" << newTab;

    if (!m_completed) {
        qDebug() << "[WPE-LOAD] not completed, saving as initialUrl";
        m_initialUrl = url;
        emit hasInitialUrlChanged();
        return;
    }

    if (!m_tabModel) { qDebug() << "[WPE-LOAD] no tabModel!"; return; }

    if (newTab || m_tabModel->count() == 0) {
        qDebug() << "[WPE-LOAD] newTab for" << url;
        m_tabModel->newTab(url, newTab);
    } else {
        if (m_contentItem) {
            const QUrl targetUrl(url);
            if (m_contentItem->url() == targetUrl) {
                qDebug() << "[WPE-LOAD] reload active contentItem:" << url;
                m_contentItem->reload();
            } else {
                qDebug() << "[WPE-LOAD] setUrl on contentItem:" << url;
                m_contentItem->setUrl(targetUrl);
            }
        } else {
            qDebug() << "[WPE-LOAD] contentItem is NULL, cannot load!";
        }
    }
}

void WPEWebContainer::reload(bool force)
{
    Q_UNUSED(force)
    if (m_contentItem) m_contentItem->reload();
}

void WPEWebContainer::goForward()
{
    if (m_contentItem) m_contentItem->goForward();
}

void WPEWebContainer::goBack()
{
    if (m_contentItem) m_contentItem->goBack();
}

int WPEWebContainer::activateTab(int tabId, const QString &url)
{
    if (!m_tabModel) return -1;
    m_tabModel->activateTabById(tabId);
    if (!url.isEmpty() && m_contentItem) {
        m_contentItem->setUrl(QUrl(url));
    }
    return tabId;
}

void WPEWebContainer::closeTab(int tabId)
{
    if (!m_tabModel) return;
    bool isActive = (tabId == this->tabId());
    m_tabModel->removeTabById(tabId, isActive);
}

void WPEWebContainer::releaseActiveTabOwnership()
{
    // stub - ownership tracking for D-Bus not needed in WPE
}

void WPEWebContainer::updateContentOrientation(Qt::ScreenOrientation orientation)
{
    emit webContentOrientationChanged(orientation);
}

void WPEWebContainer::applyContentOrientation(Qt::ScreenOrientation orientation)
{
    updateContentOrientation(orientation);
}

void WPEWebContainer::setAdBlockEnabled(bool enabled)
{
    WPEWebPage::applyAdBlockEnabledGlobally(enabled);
}

void WPEWebContainer::setCookieBannerBlockingEnabled(bool enabled)
{
    WPEWebPage::applyCookieBannerBlockingGlobally(enabled);
}

void WPEWebContainer::setAdBlockAllowlist(const QString &json)
{
    WPEWebPage::applyAdBlockAllowlistGlobally(json);
}

void WPEWebContainer::setSiteUaOverrides(const QString &json)
{
    WPEWebPage::applySiteUaOverridesGlobally(json);
}

void WPEWebContainer::setJavaScriptBlocklist(const QString &json)
{
    WPEWebPage::applyJavaScriptBlocklistGlobally(json);
}

void WPEWebContainer::onActiveTabChanged(int activeTabId)
{
    activatePage(activeTabId);
}

void WPEWebContainer::onTabAdded(int tabId)
{
    // Pre-create the page so it's ready
    getOrCreatePage(tabId);
    // A newly-added background tab must not blow the live-tab budget: if we're
    // already at the cap it will be discarded here and reload when first shown.
    enforceLiveTabBudget();
}

void WPEWebContainer::onTabClosed(int tabId)
{
    m_mruTabs.removeAll(tabId);
    WPEWebPage::removeHistoryPreviewsForTab(tabId);
    WPEWebPage *page = m_pages.take(tabId);
    const bool wasPrivate = page && page->privateBrowsing();
    if (page) {
        page->setVisible(false);
        page->deleteLater();
        if (m_contentItem == page) {
            m_contentItem = nullptr;
            emit contentItemChanged();
            emit needChromeChanged();
            // Every one of these reads through m_contentItem, so dropping it
            // changes them all -- but a QML binding only re-evaluates when its
            // NOTIFY fires. Closing a tab mid-load left the URL bar showing the
            // dead page's address and the progress bar frozen at whatever it
            // had reached, because loading/url/loadProgress were never
            // re-read. The same nulling further up emits the full set; this
            // path only emitted the first two.
            emit tabIdChanged();
            emit urlChanged();
            emit titleChanged();
            emit loadingChanged();
            emit loadProgressChanged();
            emit canGoBackChanged();
            emit canGoForwardChanged();
        }
    }
    // Closing the last private tab wipes the shared ephemeral session, so
    // re-entering private mode later in the same run starts with no cookies or
    // storage. (Everything was in-memory only; this just drops it eagerly.)
    if (wasPrivate && m_privateTabModel && m_privateTabModel->count() == 0)
        WPEQtView::clearPrivateBrowsingData();
}

void WPEWebContainer::activatePage(int tabId)
{
    qDebug() << "[WPE-ACTIVATE] tabId=" << tabId << "currentContent=" << m_contentItem;
    if (m_contentItem) {
        m_contentItem->setActive(false);
        m_contentItem->setVisible(false);
    }

    // Get or create new page
    WPEWebPage *page = getOrCreatePage(tabId);

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

    // The just-activated tab is the most-recently-used; discard anything that
    // now falls outside the live-tab budget.
    touchMru(tabId);
    enforceLiveTabBudget();
}

void WPEWebContainer::touchMru(int tabId)
{
    m_mruTabs.removeAll(tabId);
    m_mruTabs.prepend(tabId);
}

int WPEWebContainer::effectiveLiveTabCap() const
{
    // Normally keep the active tab + (m_maxLiveTabCount-1) MRU tabs. Under
    // memory pressure, shrink toward m_pressureKeepMin so we shed aggressively.
    if (m_underMemoryPressure)
        return qMin(m_maxLiveTabCount, m_pressureKeepMin);
    return m_maxLiveTabCount;
}

void WPEWebContainer::discardPage(int tabId)
{
    // Never discard the tab the user is looking at.
    if (m_contentItem && m_pages.value(tabId) == m_contentItem)
        return;
    WPEWebPage *page = m_pages.take(tabId);
    if (!page)
        return;
    // Keep the tab-model entry (URL/title/thumbnail) — only the live page and
    // its WebProcess go away. activatePage() recreates + reloads it on return.
    page->setActive(false);
    page->setVisible(false);
    // Destroying the page destroys its engine view, and with it the page's processes.
    page->deleteLater();
    qInfo() << "[WPE-DISCARD] discarded tab" << tabId
            << "livePages=" << m_pages.size();
}

void WPEWebContainer::enforceLiveTabBudget()
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

void WPEWebContainer::checkMemoryPressure()
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
        qInfo() << "[WPE-DISCARD] memory pressure" << (nowPressure ? "ON" : "OFF")
                << "MemFree=" << (memFreeKb / 1024) << "MB cap=" << effectiveLiveTabCap();
    }
    if (nowPressure)
        enforceLiveTabBudget();
}

WPEWebPage *WPEWebContainer::getOrCreatePage(int tabId)
{
    qDebug() << "[WPE-PAGE] getOrCreatePage tabId=" << tabId << "exists=" << m_pages.contains(tabId);
    if (m_pages.contains(tabId)) {
        return m_pages[tabId];
    }

    // Create with nullptr parent so windowChanged fires AFTER WPEQtView's connect() is set up,
    // then reparent via setParentItem() to trigger configureWindow() correctly.
    WPEWebPage *page = new WPEWebPage(nullptr);
    page->setTabId(tabId);
    // Tag the page with the mode it was born into, BEFORE it is parented (which
    // triggers web-view creation): private pages must be constructed against the
    // ephemeral network session. A page keeps this for its whole life — the two
    // tab models use disjoint id ranges, so a page never switches modes.
    page->setPrivateBrowsing(m_privateMode);
    page->setVisible(false);
    page->setActive(false);
    page->setAppForeground(m_foreground);

    const QScreen *screen = QGuiApplication::primaryScreen();
    const QSizeF screenSize = screenSizeOrFallback(QGuiApplication::primaryScreen());
    configurePageGeometry(page, screenSize);

    // Keep the current zoom-backed device-scale bootstrap explicit here.
    if (screen) {
        page->applyInitialDeviceScale(initialPageDeviceScaleFactor(screenSize));
    }

    connectPage(page);
    m_pages[tabId] = page;

    // Reparent after all connections are set up — this fires windowChanged which triggers
    // WPEQtView::configureWindow() → createWebView().
    page->setParentItem(this);
    return page;
}

void WPEWebContainer::connectPage(WPEWebPage *page)
{
    // Keep tab metadata (URL/title/mode) in sync with the tab model that created this page.
    // Using the model captured at page-creation time avoids cross-updating when switching
    // between persistent/private models later.
    DeclarativeTabModel *pageModel = m_tabModel;
    if (pageModel) {
        connect(page, &WPEQtView::urlChanged, pageModel, &DeclarativeTabModel::onUrlChanged);
        connect(page, &WPEQtView::titleChanged, pageModel, &DeclarativeTabModel::onTitleChanged);
        connect(page, &WPEWebPage::desktopModeChanged, pageModel, &DeclarativeTabModel::onDesktopModeChanged);
    }

    // Not tied to the tab model: icons are stored per host, for every page.
    connect(page, &WPEWebPage::faviconChanged, this, &WPEWebContainer::onPageFaviconChanged);

    connect(page, &WPEQtView::urlChanged, this, &WPEWebContainer::onPageUrlChanged);
    connect(page, &WPEQtView::titleChanged, this, &WPEWebContainer::onPageTitleChanged);
    // WPEQtView::loadingChanged carries a WPEQtViewLoadRequest* — use a lambda to adapt
    connect(page, &WPEQtView::loadingChanged, this, [this, page](WPEQtViewLoadRequest *) {
        if (page == m_contentItem) {
            emit loadingChanged();
            emit canGoBackChanged();
            emit canGoForwardChanged();
        }
    });
    // Same-document navigations (SPA pushState) emit no load change, so without
    // this the toolbar's back/forward state stayed stale mid-SPA and the button
    // kept rendering the home glyph.
    connect(page, &WPEQtView::backForwardChanged, this, [this, page]() {
        if (page == m_contentItem) {
            emit canGoBackChanged();
            emit canGoForwardChanged();
        }
    });
    connect(page, &WPEQtView::loadProgressChanged, this, &WPEWebContainer::onPageLoadProgressChanged);
    connect(page, &WPEWebPage::paintedChanged, this, &WPEWebContainer::onPagePaintedChanged);
    connect(page, &WPEWebPage::chromeChanged, this, &WPEWebContainer::needChromeChanged);
    connect(page, &WPEWebPage::fullscreenChanged, this, &WPEWebContainer::needChromeChanged);
    connect(page, &WPEWebPage::fileGrabWritten, this, [this, page](const QString &filePath) {
        if (m_tabModel)
            m_tabModel->updateThumbnailPath(page->tabId(), filePath);
    });
    connect(page, &WPEWebPage::touched, this, [this, page]() {
        if (page == m_contentItem)
            emit touched();
    });
    connect(page, &WPEWebPage::crashedChanged, this, [page]() {
        if (page->crashed())
            qWarning() << "[WPE] WebProcess crashed for tab" << page->tabId()
                       << "— tab isolated, browser alive";
    });
}

void WPEWebContainer::onPageUrlChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
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

// Favicon discovery has to be driven from here: the QML webPageComponent that
// used to call FaviconManager.grabIcon() is never instantiated (pages are
// created in getOrCreatePage above), so every handler inside it is dead code.
void WPEWebContainer::onPageFaviconChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
    if (!page || page->privateBrowsing() || page->faviconCandidates().isEmpty())
        return;

    // Size is only used for the page-thumbnail fallback when no candidate
    // yields a usable image.
    FaviconManager::instance()->grabIcon(QStringLiteral("history"), page, QSize(96, 96));
}

void WPEWebContainer::onPageTitleChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
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

void WPEWebContainer::onPageLoadProgressChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
    if (!page || page != m_contentItem) return;
    emit loadProgressChanged();
}

void WPEWebContainer::onPagePaintedChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
    if (!page || page != m_contentItem) return;
    if (page->painted() && !m_activeTabRendered) {
        setActiveTabRendered(true);
    }
}

void WPEWebContainer::setActiveTabRendered(bool r)
{
    if (m_activeTabRendered != r) {
        m_activeTabRendered = r;
        emit activeTabRenderedChanged();
    }
}
