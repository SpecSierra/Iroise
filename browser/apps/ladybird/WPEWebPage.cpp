/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Pacific: Atlantic's page class on top of a Ladybird engine view. See WPEWebPage.h.
 */

#include "WPEWebPage.h"

#include "downloadmanager.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTouchEvent>

namespace {

QList<WPEWebPage *> &livePages()
{
    static QList<WPEWebPage *> pages;
    return pages;
}

const int kThumbnailIntervalMs = 4000;
const int kThumbnailMaxWidth = 540;

// The engine reports neither scrolling nor selections. This script, run in each page, does: it posts where the page
// is scrolled to and what is selected, at most once a frame, as page messages (LB_EMBED_PAGE_MESSAGE). Running it
// again does nothing. If the page replaced console.log, a hidden frame lends us a working one.
const char kPageAgentScript[] =
    "(function(){"
    "if(window.__pacificAgent)return 0;window.__pacificAgent=1;"
    "var log=console.log;"
    "if(!/\\[native code\\]/.test(Function.prototype.toString.call(log))){"
    "var f=document.createElement('iframe');f.style.display='none';document.documentElement.appendChild(f);log=f.contentWindow.console.log;}"
    "function post(k,d){log('\\u0001pacific',JSON.stringify([k,d]));}"
    "function selection(){"
    "var s=getSelection();if(!s||!s.rangeCount||s.isCollapsed)return {t:''};"
    "var q=s.getRangeAt(0).getClientRects();if(!q.length)return {t:''};"
    "var a=q[0],b=q[q.length-1],v=window.visualViewport,z=v?v.scale:1,ox=v?v.offsetLeft:0,oy=v?v.offsetTop:0;"
    "return {t:s.toString().slice(0,4000),sx:(a.left-ox)*z,sy:(a.bottom-oy)*z,ex:(b.right-ox)*z,ey:(b.bottom-oy)*z};}"
    "var pending={},had=false;"
    "function soon(k,f){if(pending[k])return;pending[k]=requestAnimationFrame(function(){pending[k]=0;f();});}"
    "function sendSelection(){var s=selection();if(s.t||had)post('selection',s);had=!!s.t;}"
    "addEventListener('scroll',function(e){"
    "if(e.target===document)soon('scroll',function(){var r=document.scrollingElement||document.documentElement;post('scroll',[scrollY,r.scrollHeight,innerHeight]);});"
    "if(had)soon('selection',sendSelection);"
    "},{passive:true,capture:true});"
    "document.addEventListener('selectionchange',function(){soon('selection',sendSelection);});"
    "return 0;})()";

// The kinds of <input> the interface has a picker of its own for. A tap on one hands it over.
const char kInputPickerScript[] =
    "(function(x,y){"
    "var e=document.elementFromPoint(x,y);"
    "while(e&&e.tagName!=='INPUT')e=e.parentElement;"
    "if(!e||e.disabled||e.readOnly)return null;"
    "var t=(e.getAttribute('type')||'').toLowerCase();"
    "if(['date','month','week','time','datetime-local'].indexOf(t)<0)return null;"
    "window.__pacificPendingInput=e;e.blur();"
    "return {type:t,value:e.value||'',min:e.getAttribute('min')||'',max:e.getAttribute('max')||'',step:e.getAttribute('step')||''};"
    "})(%1,%2)";

const char kResolveInputPickerScript[] =
    "(function(v){"
    "var e=window.__pacificPendingInput;window.__pacificPendingInput=null;"
    "if(!e)return 0;"
    "e.value=v;"
    "e.dispatchEvent(new Event('input',{bubbles:true,composed:true}));"
    "e.dispatchEvent(new Event('change',{bubbles:true,composed:true}));"
    "return 1;"
    "})((%1)[0])";
// The handles hang below the line they mark and are dragged by their middle, so the text a handle points at is
// this far above where the finger says it is, in CSS pixels.
const qreal kHandleToTextCssPx = 24;

// What is under a long press: a link, an image, or text, of which it then selects the word.
const char kLongPressScript[] =
    "(function(x,y){"
    "var r={link:'',image:'',page:location.href,word:false};"
    "var e=document.elementFromPoint(x,y);if(!e)return r;"
    "var a=e.closest?e.closest('a[href]'):null;if(a)r.link=a.href;"
    "if(e.tagName==='IMG')r.image=e.currentSrc||e.src||'';"
    "if(r.link||r.image)return r;"
    "var p=document.caretPositionFromPoint?document.caretPositionFromPoint(x,y):null;"
    "if(!p||!p.offsetNode||p.offsetNode.nodeType!==3)return r;"
    "var t=p.offsetNode.data,s=p.offset,n=p.offset,w=/[\\s\\u00a0.,;:!?()\\[\\]{}\"<>]/;"
    "while(s>0&&!w.test(t[s-1]))s--;"
    "while(n<t.length&&!w.test(t[n]))n++;"
    "if(n<=s)return r;"
    "getSelection().setBaseAndExtent(p.offsetNode,s,p.offsetNode,n);"
    "r.word=true;return r;"
    "})(%1,%2)";

// The selection's text and where its two ends are on screen: the bottom of the first and of the last line box,
// in CSS pixels at the zoom the user pinched to.

// Moves one end of the selection to the text nearest a point, keeping the other end where it is.
const char kMoveSelectionScript[] =
    "(function(x,y,start){"
    "var v=window.visualViewport;if(v){x=x/v.scale+v.offsetLeft;y=y/v.scale+v.offsetTop;}"
    "var s=getSelection();if(!s.rangeCount)return 0;"
    "var p=document.caretPositionFromPoint?document.caretPositionFromPoint(x,y):null;if(!p||!p.offsetNode)return 0;"
    "var r=s.getRangeAt(0);"
    "try{"
    "if(start){if(r.comparePoint(p.offsetNode,p.offset)>0)return 0;s.setBaseAndExtent(r.endContainer,r.endOffset,p.offsetNode,p.offset);}"
    "else{if(r.comparePoint(p.offsetNode,p.offset)<0)return 0;s.setBaseAndExtent(r.startContainer,r.startOffset,p.offsetNode,p.offset);}"
    "}catch(e){return 0;}"
    "return 1;"
    "})(%1,%2,%3)";

} // namespace

WPEWebPage::WPEWebPage(QQuickItem *parent)
    : WPEQtView(parent)
{
    livePages().append(this);
    m_security = new WPESecurityInfo(this);
    setFlag(ItemAcceptsInputMethod, false);

    connect(this, &WPEQtView::loadingChanged, this, &WPEWebPage::onLoadingChanged);
    connect(this, &WPEQtView::frameArrived, this, &WPEWebPage::onFrameArrived);
    connect(this, &WPEQtView::pageWantsTextInputChanged, this, &WPEWebPage::onPageWantsTextInputChanged);
    connect(this, &WPEQtView::engineCrashed, this, [this] {
        m_crashed = true;
        emit crashedChanged();
    });

    // A tab's thumbnail is a scaled copy of what is on screen, refreshed while the tab is the one showing.
    m_thumbnailTimer.setInterval(kThumbnailIntervalMs);
    connect(&m_thumbnailTimer, &QTimer::timeout, this, &WPEWebPage::captureThumbnail);

    m_chromeGestureDebounceTimer.setSingleShot(true);
    m_chromeGestureDebounceTimer.setInterval(150);
    connect(&m_chromeGestureDebounceTimer, &QTimer::timeout, this, [this] {
        if (m_chromeGestureArmed && m_chrome != m_pendingChrome)
            setChrome(m_pendingChrome);
        m_chromeGestureArmed = false;
    });
    connect(this, &WPEQtView::scrollPositionChanged, this, [this](qreal scrollY, qreal scrollHeight, qreal innerHeight) {
        bool atTop = scrollY <= 0;
        bool atBottom = (scrollHeight - scrollY - innerHeight) <= 1.0;
        if (atTop != m_atYBeginning) {
            m_atYBeginning = atTop;
            emit atYBeginningChanged();
        }
        if (atBottom != m_atYEnd) {
            m_atYEnd = atBottom;
            emit atYEndChanged();
        }
        // Scrolling down the page hides the toolbar, scrolling back up or reaching the top shows it.
        if (m_chromeGestureEnabled && !m_fixedToolbar && !m_forcedChrome) {
            qreal delta = scrollY - m_lastScrollY;
            bool targetChrome;
            if (atTop) {
                targetChrome = true;
            } else if (delta > m_chromeGestureThreshold) {
                targetChrome = false;
            } else if (delta < -m_chromeGestureThreshold) {
                targetChrome = true;
            } else {
                return;
            }
            if (targetChrome != m_pendingChrome || !m_chromeGestureArmed) {
                m_pendingChrome = targetChrome;
                m_chromeGestureArmed = true;
                m_chromeGestureDebounceTimer.start();
            }
        }
        m_lastScrollY = scrollY;
    });
}

WPEWebPage::~WPEWebPage()
{
    livePages().removeAll(this);
}

const QList<WPEWebPage *> &WPEWebPage::liveInstances()
{
    return livePages();
}

int WPEWebPage::tabId() const
{
    return m_tabId;
}

void WPEWebPage::setTabId(int tabId)
{
    if (m_tabId != tabId) {
        m_tabId = tabId;
        emit tabIdChanged();
    }
}

bool WPEWebPage::painted() const
{
    return m_painted;
}

bool WPEWebPage::loaded() const
{
    return m_loaded;
}

void WPEWebPage::updateEngineVisibility()
{
    bool visible = m_active && m_appForeground;
    setWebKitVisible(visible);
    if (visible && !privateBrowsing())
        m_thumbnailTimer.start();
    else
        m_thumbnailTimer.stop();
}

void WPEWebPage::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    updateEngineVisibility();
    emit activeChanged();
}

void WPEWebPage::setAppForeground(bool foreground)
{
    if (m_appForeground == foreground)
        return;
    m_appForeground = foreground;
    updateEngineVisibility();
}

void WPEWebPage::loadTab(const QString &url, bool force)
{
    QUrl target = QUrl::fromUserInput(url);
    if (!target.isValid())
        return;
    if (target == this->url()) {
        if (force)
            reload();
        return;
    }
    m_crashed = false;
    setUrl(target);
}

void WPEWebPage::loadCurrentUrlAgain()
{
    reload();
}

void WPEWebPage::goBack()
{
    WPEQtView::goBack();
}

void WPEWebPage::goForward()
{
    WPEQtView::goForward();
}

void WPEWebPage::onLoadingChanged(WPEQtViewLoadRequest *loadRequest)
{
    bool finished = loadRequest->status() != WPEQtView::LoadStartedStatus;
    if (m_domContentLoaded != finished) {
        m_domContentLoaded = finished;
        emit domContentLoadedChanged();
    }
    if (m_loaded != finished) {
        m_loaded = finished;
        emit loadedChanged();
    }
    if (finished) {
        m_lastCommittedUrl = url();
        evaluate(QString::fromLatin1(kPageAgentScript));
    } else {
        // What the old page had selected, was searching for or was asking is gone with it.
        handleJsSelectionClear();
        m_findText.clear();
        setFindInPageHasResult(false);
        if (m_jsDialogKind >= 0) {
            m_jsDialogKind = -1;
            emit jsDialogChanged();
        }
    }
}

void WPEWebPage::onFrameArrived()
{
    if (!m_painted) {
        m_painted = true;
        emit paintedChanged();
    }
    emit afterRendering();
}

// --- Thumbnails -------------------------------------------------------------

void WPEWebPage::grabToFile(const QSize &size)
{
    if (size.width() > 0 && size.height() > 0)
        m_thumbnailSize = size;
    captureThumbnail();
}

void WPEWebPage::grabThumbnail(const QSize &size)
{
    Q_UNUSED(size);
    // FIXME: Atlantic answered this with a PNG data: URI from a WebKit snapshot (used as a favourite's icon when
    //        the page has no usable favicon). Nothing asks the engine for a snapshot yet.
}

void WPEWebPage::captureThumbnail()
{
    if (privateBrowsing() || m_tabId <= 0 || !m_painted || m_thumbnailGrab)
        return;
    const quint32 serial = frameSerial();
    if (m_thumbnailCaptured && serial == m_thumbnailSerial)
        return; // nothing new has been drawn since the file was written

    QQuickWindow *win = window();
    if (!win || !win->isVisible() || !isVisible() || width() < 1 || height() < 1)
        return;

    QSize target = m_thumbnailSize;
    if (target.isEmpty())
        target = QSize(qRound(width()), qRound(width() * 1.4));
    const int targetWidth = qMin(target.width(), kThumbnailMaxWidth);
    const int targetHeight = qMin(qRound(targetWidth * (qreal(target.height()) / target.width())),
                                  qRound(targetWidth * (height() / width())));

    m_thumbnailGrab = grabToImage(QSize(targetWidth, qRound(targetWidth * (height() / width()))));
    if (!m_thumbnailGrab)
        return;
    connect(m_thumbnailGrab.data(), &QQuickItemGrabResult::ready, this, [this, serial, targetWidth, targetHeight] {
        QImage image = m_thumbnailGrab->image().copy(0, 0, targetWidth, targetHeight);
        m_thumbnailGrab.clear();
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                  + QStringLiteral("/thumbnails");
        if (image.isNull() || !QDir().mkpath(directory))
            return;
        // A new name for every capture: the tab switcher's Image caches by URL.
        const QString prefix = QStringLiteral("tab-%1-").arg(m_tabId);
        for (const QString &old : QDir(directory).entryList(QStringList(prefix + QLatin1Char('*')), QDir::Files))
            QFile::remove(directory + QLatin1Char('/') + old);
        const QString path = QStringLiteral("%1/%2%3.jpg").arg(directory, prefix).arg(serial);
        if (!image.save(path, "JPG", 80))
            return;
        m_thumbnailSerial = serial;
        m_thumbnailCaptured = true;
        emit fileGrabWritten(path);
    });
}

QString WPEWebPage::historyPreviewFor(const QUrl &url) const
{
    Q_UNUSED(url);
    return QString();
}

void WPEWebPage::removeHistoryPreviewsForTab(int tabId)
{
    Q_UNUSED(tabId);
}

void WPEWebPage::clearHistoryPreviews()
{
}

// --- Chrome -----------------------------------------------------------------

void WPEWebPage::setChrome(bool chrome)
{
    static const bool logChrome = !qgetenv("PACIFIC_LOG_ENGINE").isEmpty();
    if (logChrome && m_chrome != chrome)
        qWarning("WPEWebPage: chrome %d, page %dx%d painted %dx%d", chrome, int(width()), int(height()), paintedSize().width(), paintedSize().height());
    if (m_chrome != chrome) {
        m_chrome = chrome;
        emit chromeChanged();
    }
}

void WPEWebPage::forceChrome(bool forced)
{
    setForcedChrome(forced);
    if (forced)
        setChrome(true);
}

void WPEWebPage::suspendView()
{
    setWebKitVisible(false);
}

void WPEWebPage::resumeView()
{
    updateEngineVisibility();
}

void WPEWebPage::setDesktopMode(bool desktop)
{
    if (m_desktopMode == desktop)
        return;
    m_desktopMode = desktop;
    emit desktopModeChanged();
}

// --- Selection --------------------------------------------------------------

bool WPEWebPage::textSelectionActive() const
{
    return m_textSelectionActive;
}

QObject* WPEWebPage::textSelectionController()
{
    return this;
}

QString WPEWebPage::selectedText() const
{
    return m_selectedText;
}

bool WPEWebPage::isPhoneNumber() const
{
    static const QRegularExpression re(QStringLiteral("^[+\\d][\\d\\s\\-().+]{6,}$"));
    return re.match(m_selectedText.trimmed()).hasMatch();
}

QString WPEWebPage::searchUri() const
{
    return QString();
}

// --- Text input -------------------------------------------------------------
//
// The engine says when an editable element has focus. While one has, this item takes the input method's focus,
// which brings up the on-screen keyboard, and what the keyboard commits or composes goes to the engine.

void WPEWebPage::onPageWantsTextInputChanged()
{
    const bool wantsInput = pageWantsTextInput();
    setFlag(ItemAcceptsInputMethod, wantsInput);
    if (wantsInput) {
        forceActiveFocus();
        QGuiApplication::inputMethod()->update(Qt::ImEnabled | Qt::ImHints);
        QGuiApplication::inputMethod()->show();
    } else if (hasActiveFocus()) {
        QGuiApplication::inputMethod()->update(Qt::ImEnabled);
        QGuiApplication::inputMethod()->hide();
    }
}

QVariant WPEWebPage::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return pageWantsTextInput();
    case Qt::ImHints:
        return int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

void WPEWebPage::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty() || event->replacementLength() > 0) {
        // replacementStart is relative to the caret and negative for text before it.
        const int replaced = event->replacementStart() < 0 ? qMin(-event->replacementStart(), event->replacementLength()) : 0;
        sendToEngine(LB_EMBED_COMMIT_TEXT, replaced, 0, 0, 0, event->commitString().toUtf8());
    }
    if (!event->preeditString().isEmpty() || event->commitString().isEmpty())
        sendToEngine(LB_EMBED_COMPOSE_TEXT, 0, 0, 0, 0, event->preeditString().toUtf8());
    event->accept();
}

void WPEWebPage::keyPressEvent(QKeyEvent *event)
{
    int key = 0;
    switch (event->key()) {
    case Qt::Key_Backspace:
        key = LB_EMBED_KEY_BACKSPACE;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        key = LB_EMBED_KEY_RETURN;
        break;
    case Qt::Key_Tab:
        key = LB_EMBED_KEY_TAB;
        break;
    case Qt::Key_Left:
        key = LB_EMBED_KEY_LEFT;
        break;
    case Qt::Key_Right:
        key = LB_EMBED_KEY_RIGHT;
        break;
    case Qt::Key_Up:
        key = LB_EMBED_KEY_UP;
        break;
    case Qt::Key_Down:
        key = LB_EMBED_KEY_DOWN;
        break;
    case Qt::Key_Delete:
        key = LB_EMBED_KEY_DELETE;
        break;
    case Qt::Key_Escape:
        key = LB_EMBED_KEY_ESCAPE;
        break;
    default:
        break;
    }
    if (key) {
        sendToEngine(LB_EMBED_KEY, key);
    } else if (!event->text().isEmpty()) {
        sendToEngine(LB_EMBED_COMMIT_TEXT, 0, 0, 0, 0, event->text().toUtf8());
    } else {
        QQuickItem::keyPressEvent(event);
        return;
    }
    event->accept();
}

void WPEWebPage::touchEvent(QTouchEvent *event)
{
    if (event->type() == QEvent::TouchBegin) {
        emit touched();
        // A page still loading has not had the agent yet.
        evaluate(QString::fromLatin1(kPageAgentScript));
    }
    WPEQtView::touchEvent(event);
}

// --- Everything below is state the interface reads and the engine protocol does not drive yet ---------------

bool WPEWebPage::domContentLoaded() const
{
    return m_domContentLoaded;
}

bool WPEWebPage::chrome() const
{
    return m_chrome;
}

bool WPEWebPage::forcedChrome() const
{
    return m_forcedChrome;
}

void WPEWebPage::setForcedChrome(bool forcedChrome)
{
    if (m_forcedChrome == forcedChrome)
        return;
    m_forcedChrome = forcedChrome;
    emit forcedChromeChanged();
}

bool WPEWebPage::fullscreen() const
{
    return m_fullscreen;
}

bool WPEWebPage::atYBeginning() const
{
    return m_atYBeginning;
}

bool WPEWebPage::atYEnd() const
{
    return m_atYEnd;
}

QString WPEWebPage::favicon() const
{
    return m_favicon;
}

void WPEWebPage::setFavicon(const QString &favicon)
{
    if (m_favicon == favicon)
        return;
    m_favicon = favicon;
    emit faviconChanged();
}

QStringList WPEWebPage::faviconCandidates() const
{
    return m_faviconCandidates;
}

void WPEWebPage::setFaviconCandidates(const QStringList &candidates)
{
    if (m_faviconCandidates == candidates)
        return;
    m_faviconCandidates = candidates;
}

void WPEWebPage::offerSearchEngine(const QString &title, const QString &url)
{
    Q_UNUSED(title);
    Q_UNUSED(url);
}

qreal WPEWebPage::fullscreenHeight() const
{
    return m_fullscreenHeight;
}

void WPEWebPage::setFullscreenHeight(qreal height)
{
    if (m_fullscreenHeight == height)
        return;
    m_fullscreenHeight = height;
    emit fullscreenHeightChanged();
}

qreal WPEWebPage::toolbarHeight() const
{
    return m_toolbarHeight;
}

void WPEWebPage::setToolbarHeight(qreal height)
{
    if (m_toolbarHeight == height)
        return;
    m_toolbarHeight = height;
    emit toolbarHeightChanged();
}

bool WPEWebPage::active() const
{
    return m_active;
}

// The engine blocks with its own copy of Brave's adblock engine and its own list subscriptions. These two switches
// are for the whole engine, so they are sent once and not per page.
static bool s_adBlockEnabled = false;
static bool s_cookieBannerBlockingEnabled = false;

void WPEWebPage::applyAdBlockEnabledGlobally(bool enabled)
{
    s_adBlockEnabled = enabled;
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_AD_BLOCKING, enabled ? 1 : 0);
    for (WPEWebPage *page : livePages())
        emit page->adBlockEnabledChanged();
}

void WPEWebPage::applyAdBlockAllowlistGlobally(const QString &json)
{
    // The sites the user switched blocking off for become exception rules on top of the lists: nothing a page of
    // that site (or of a subdomain) asks for is blocked, and nothing in it is hidden.
    QString rules;
    for (const QJsonValue &entry : QJsonDocument::fromJson(json.toUtf8()).array()) {
        QString host = entry.toString().trimmed().toLower();
        // NB: A host goes into the rules as it is, so one that could be read as rule syntax is left out.
        static const QRegularExpression plainHost(QStringLiteral("^[a-z0-9.-]+$"));
        if (!plainHost.match(host).hasMatch())
            continue;
        rules += QStringLiteral("@@*$domain=%1\n@@||%1^$elemhide\n").arg(host);
    }
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_BLOCKER_FILTERS, 0, 0, 0, 0, 0, rules.toUtf8());
}

void WPEWebPage::applyCookieBannerBlockingGlobally(bool enabled)
{
    s_cookieBannerBlockingEnabled = enabled;
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_COOKIE_BANNER_BLOCKING, enabled ? 1 : 0);
    for (WPEWebPage *page : livePages())
        emit page->cookieBannerBlockingEnabledChanged();
}

void WPEWebPage::applySiteUaOverridesGlobally(const QString &json)
{
    Q_UNUSED(json);
}

void WPEWebPage::applyJavaScriptBlocklistGlobally(const QString &json)
{
    Q_UNUSED(json);
}

void WPEWebPage::applyJavaScriptEnabledForUrl(const QUrl &url)
{
    Q_UNUSED(url);
}

bool WPEWebPage::throttlePainting() const
{
    return m_throttlePainting;
}

void WPEWebPage::setThrottlePainting(bool throttle)
{
    if (m_throttlePainting == throttle)
        return;
    m_throttlePainting = throttle;
    emit throttlePaintingChanged();
}

bool WPEWebPage::chromeGestureEnabled() const
{
    return m_chromeGestureEnabled;
}

void WPEWebPage::setChromeGestureEnabled(bool enabled)
{
    if (m_chromeGestureEnabled == enabled)
        return;
    m_chromeGestureEnabled = enabled;
    emit chromeGestureEnabledChanged();
}

qreal WPEWebPage::chromeGestureThreshold() const
{
    return m_chromeGestureThreshold;
}

void WPEWebPage::setChromeGestureThreshold(qreal threshold)
{
    if (m_chromeGestureThreshold == threshold)
        return;
    m_chromeGestureThreshold = threshold;
    emit chromeGestureThresholdChanged();
}

bool WPEWebPage::fixedToolbar() const
{
    return m_fixedToolbar;
}

void WPEWebPage::setFixedToolbar(bool fixed)
{
    if (m_fixedToolbar == fixed)
        return;
    m_fixedToolbar = fixed;
    emit fixedToolbarChanged();
}

bool WPEWebPage::moving() const
{
    return false;
}

QVariant WPEWebPage::resurrectedContentRect() const
{
    return m_resurrectedContentRect;
}

void WPEWebPage::setResurrectedContentRect(const QVariant &rect)
{
    if (m_resurrectedContentRect == rect)
        return;
    m_resurrectedContentRect = rect;
    emit resurrectedContentRectChanged();
}

bool WPEWebPage::dragging() const
{
    return false;
}

bool WPEWebPage::desktopMode() const
{
    return m_desktopMode;
}

void WPEWebPage::applyUserAgentForUrl(const QUrl &url)
{
    Q_UNUSED(url);
}

bool WPEWebPage::urlHasUaQuirk(const QUrl &url) const
{
    Q_UNUSED(url);
    return false;
}

void WPEWebPage::notePopupRouted(const QString &url)
{
    Q_UNUSED(url);
}

bool WPEWebPage::popupGuardShouldBlock(const QString &url, int navigationType)
{
    Q_UNUSED(url);
    Q_UNUSED(navigationType);
    return false;
}

void WPEWebPage::applyInitialDeviceScale(qreal scale)
{
    Q_UNUSED(scale);
}

void WPEWebPage::setFullscreenState(bool fullscreen)
{
    Q_UNUSED(fullscreen);
}

void WPEWebPage::setNativeFullscreenRequested(bool fullscreen)
{
    if (m_nativeFullscreenRequested == fullscreen)
        return;
    m_nativeFullscreenRequested = fullscreen;
}

void WPEWebPage::setDomFullscreenActive(bool fullscreen)
{
    if (m_domFullscreenActive == fullscreen)
        return;
    m_domFullscreenActive = fullscreen;
}

void WPEWebPage::setMediaPlaybackState(bool audioActive, bool videoActive)
{
    Q_UNUSED(audioActive);
    Q_UNUSED(videoActive);
}

void WPEWebPage::notifyPagePinchConsumed()
{
}

void WPEWebPage::notifyPagePinchDeclined()
{
}

void WPEWebPage::updateObservedMediaState(bool audioActive, bool videoActive, bool fullscreenActive, qreal volume, bool muted, bool volumeChangedByPage)
{
    Q_UNUSED(audioActive);
    Q_UNUSED(videoActive);
    Q_UNUSED(fullscreenActive);
    Q_UNUSED(volume);
    Q_UNUSED(muted);
    Q_UNUSED(volumeChangedByPage);
}

void WPEWebPage::setMediaMuted(bool muted)
{
    if (m_mediaMuted == muted)
        return;
    m_mediaMuted = muted;
}

void WPEWebPage::preconnect(const QString &url)
{
    Q_UNUSED(url);
}

void WPEWebPage::revealFocusedEditable()
{
}

void WPEWebPage::sendAsyncMessage(const QString &name, const QVariant &data)
{
    Q_UNUSED(name);
    Q_UNUSED(data);
}

void WPEWebPage::addMessageListener(const QString &name)
{
    Q_UNUSED(name);
}

void WPEWebPage::clearSelection()
{
    handleJsSelectionClear();
    evaluate(QStringLiteral("getSelection().removeAllRanges(),0"));
}

void WPEWebPage::selectAll()
{
    evaluate(QStringLiteral("getSelection().selectAllChildren(document.body),0"));
}

void WPEWebPage::moveSelectionStart(qreal cssX, qreal cssY)
{
    moveSelectionEdge(true, cssX, cssY);
}

void WPEWebPage::moveSelectionEnd(qreal cssX, qreal cssY)
{
    moveSelectionEdge(false, cssX, cssY);
}

void WPEWebPage::copyToClipboard()
{
    if (!m_selectedText.isEmpty())
        QGuiApplication::clipboard()->setText(m_selectedText);
}

void WPEWebPage::handleJsSelectionClear()
{
    const bool wasActive = m_textSelectionActive;
    const bool hadSelection = !m_selectedText.isEmpty();
    m_selectedText.clear();
    m_textSelectionActive = false;
    m_selectionStartX = m_selectionStartY = m_selectionEndX = m_selectionEndY = 0.0;
    if (hadSelection)
        emit selectionTextChanged();
    if (wasActive)
        emit textSelectionActiveChanged();
    if (hadSelection || wasActive)
        emit selectionHandlesUpdated();
}

void WPEWebPage::handleJsSelectionUpdate(const QString &text, qreal startX, qreal startY, qreal endX, qreal endY)
{
    if (text.isEmpty()) {
        handleJsSelectionClear();
        return;
    }
    const bool wasActive = m_textSelectionActive;
    const bool textChanged = m_selectedText != text;
    const bool handlesChanged = !qFuzzyCompare(m_selectionStartX + 1, startX + 1) || !qFuzzyCompare(m_selectionStartY + 1, startY + 1)
        || !qFuzzyCompare(m_selectionEndX + 1, endX + 1) || !qFuzzyCompare(m_selectionEndY + 1, endY + 1);
    m_selectedText = text;
    m_textSelectionActive = true;
    m_selectionStartX = startX;
    m_selectionStartY = startY;
    m_selectionEndX = endX;
    m_selectionEndY = endY;
    if (textChanged)
        emit selectionTextChanged();
    if (!wasActive)
        emit textSelectionActiveChanged();
    if (textChanged || handlesChanged || !wasActive)
        emit selectionHandlesUpdated();
}

void WPEWebPage::handleSubframeEditableFocus(bool focused)
{
    Q_UNUSED(focused);
}

void WPEWebPage::handleEditableFocusMoved(const QString& token)
{
    Q_UNUSED(token);
}

void WPEWebPage::sendNativeTextViaKeys(const QString& text, int replaceBefore)
{
    Q_UNUSED(text);
    Q_UNUSED(replaceBefore);
}

void WPEWebPage::findText(const QString &text, bool backwards)
{
    if (text.isEmpty()) {
        findFinish();
        return;
    }
    if (text != m_findText) {
        m_findText = text;
        sendToEngine(LB_EMBED_FIND, LB_EMBED_FIND_START, 0, 0, 0, text.toUtf8());
    } else {
        sendToEngine(LB_EMBED_FIND, backwards ? LB_EMBED_FIND_PREVIOUS : LB_EMBED_FIND_NEXT);
    }
}

void WPEWebPage::findFinish()
{
    if (!m_findText.isEmpty())
        sendToEngine(LB_EMBED_FIND, LB_EMBED_FIND_END);
    m_findText.clear();
    setFindInPageHasResult(false);
}

void WPEWebPage::setFindInPageHasResult(bool has)
{
    if (m_findInPageHasResult == has)
        return;
    m_findInPageHasResult = has;
    emit findInPageHasResultChanged();
}

void WPEWebPage::fillLoginForFocusedForm()
{
}

void WPEWebPage::acceptTlsCertificate()
{
}

void WPEWebPage::resolvePermission(bool allow)
{
    Q_UNUSED(allow);
}

void WPEWebPage::offerSaveLogin(const QString &username, const QString &password)
{
    Q_UNUSED(username);
    Q_UNUSED(password);
}

void WPEWebPage::resolveSaveLogin(bool save)
{
    Q_UNUSED(save);
}

void WPEWebPage::selectMenuOption(int index)
{
    if (!m_selectMenuActive)
        return;
    sendToEngine(LB_EMBED_SELECT_MENU_CLOSED, index >= 0 && index < m_selectMenuIds.size() ? m_selectMenuIds.at(index) : -1);
    m_selectMenuActive = false;
    emit selectMenuActiveChanged();
}

void WPEWebPage::downloadUrl(const QString &url)
{
    // NB: Through the download manager rather than straight to the engine, so that the download is known to
    //     the interface from the start.
    QString error;
    if (DownloadManager::instance()->startDownload(url, QString(), false, &error) < 0)
        qWarning("WPEWebPage: cannot download %s: %s", qPrintable(url), qPrintable(error));
}

void WPEWebPage::closeSelectMenu()
{
    selectMenuOption(-1);
}

void WPEWebPage::chooseFiles(const QStringList &filePaths)
{
    if (!m_fileChooserActive)
        return;
    QStringList paths;
    for (const QString &entry : filePaths) {
        QUrl url = QUrl::fromUserInput(entry);
        QString path = url.isValid() && url.isLocalFile() ? url.toLocalFile() : entry;
        if (!path.isEmpty())
            paths.append(path);
    }
    sendToEngine(LB_EMBED_FILE_PICKER_CLOSED, 0, 0, 0, 0, paths.join(QLatin1Char('\n')).toUtf8());
    m_fileChooserActive = false;
    emit fileChooserActiveChanged();
}

void WPEWebPage::cancelFileChooser()
{
    chooseFiles(QStringList());
}

void WPEWebPage::openSelectMenu(const QStringList &options, int selectedIndex)
{
    m_selectMenuOptions = options;
    m_selectMenuSelectedIdx = selectedIndex;
    m_selectMenuActive = true;
    emit selectMenuOptionsChanged();
    emit selectMenuSelectedIndexChanged();
    emit selectMenuActiveChanged();
}

void WPEWebPage::resolveInputPicker(const QString &value)
{
    if (m_inputPickerIsEngineColor) {
        sendToEngine(LB_EMBED_COLOR_PICKED, 1, 0, 0, 0, value.toUtf8());
    } else {
        // NB: The value goes in as the one element of a JSON array, so that no quote or backslash in it needs
        //     escaping by hand.
        QJsonArray quoted;
        quoted.append(value);
        evaluate(QString::fromLatin1(kResolveInputPickerScript).arg(QString::fromUtf8(QJsonDocument(quoted).toJson(QJsonDocument::Compact))));
    }
    closeInputPicker();
}

void WPEWebPage::cancelInputPicker()
{
    if (m_inputPickerIsEngineColor)
        sendToEngine(LB_EMBED_COLOR_PICKED, 0);
    else
        evaluate(QStringLiteral("window.__pacificPendingInput=null,0"));
    closeInputPicker();
}

void WPEWebPage::openInputPicker(const QString &type, const QString &value, const QString &min, const QString &max, const QString &step)
{
    m_inputPickerType = type;
    m_inputPickerValue = value;
    m_inputPickerMin = min;
    m_inputPickerMax = max;
    m_inputPickerStep = step;
    m_inputPickerActive = true;
    emit inputPickerChanged();
    emit inputPickerActiveChanged();
}

void WPEWebPage::openImageLongPress(const QString &imageUrl)
{
    if (m_imageLongPressUrl != imageUrl) {
        m_imageLongPressUrl = imageUrl;
        emit imageLongPressUrlChanged();
    }
}

void WPEWebPage::clearImageLongPress()
{
    if (!m_imageLongPressUrl.isEmpty()) {
        m_imageLongPressUrl.clear();
        emit imageLongPressUrlChanged();
    }
    setContextInfo(QVariantMap());
}

void WPEWebPage::setContextInfo(const QVariantMap &info)
{
    if (m_contextInfo == info)
        return;
    m_contextInfo = info;
    emit contextInfoChanged();
}

void WPEWebPage::clearContextInfo()
{
}

bool WPEWebPage::adBlockEnabled() const
{
    return s_adBlockEnabled;
}

void WPEWebPage::setAdBlockEnabled(bool enabled)
{
    applyAdBlockEnabledGlobally(enabled);
}

bool WPEWebPage::cookieBannerBlockingEnabled() const
{
    return s_cookieBannerBlockingEnabled;
}

void WPEWebPage::setCookieBannerBlockingEnabled(bool enabled)
{
    applyCookieBannerBlockingGlobally(enabled);
}

// --- Long press, text selection, select menus and page dialogs -------------------------------------------------

void WPEWebPage::longPressed(const QPointF &position)
{
    const qreal scale = deviceScaleFactor();
    QString script = QString::fromLatin1(kLongPressScript).arg(position.x() / scale, 0, 'f', 2).arg(position.y() / scale, 0, 'f', 2);
    evaluate(script, [this](const QJsonValue &value) {
        QJsonObject hit = value.toObject();
        QString link = hit.value(QStringLiteral("link")).toString();
        QString image = hit.value(QStringLiteral("image")).toString();
        if (!link.isEmpty() || !image.isEmpty()) {
            QVariantMap info;
            info.insert(QStringLiteral("pageUrl"), hit.value(QStringLiteral("page")).toString());
            info.insert(QStringLiteral("linkUrl"), link);
            info.insert(QStringLiteral("srcUrl"), image);
            info.insert(QStringLiteral("mediaType"), image.isEmpty() ? QString() : QStringLiteral("image"));
            setContextInfo(info);
            openImageLongPress(image);
            return;
        }
    });
}

void WPEWebPage::pageMessage(const QString &kind, const QJsonValue &data)
{
    static const bool log = !qgetenv("PACIFIC_LOG_ENGINE").isEmpty();
    if (log)
        qWarning("WPEWebPage: page message %s", qPrintable(kind));
    if (kind == QLatin1String("scroll")) {
        QJsonArray position = data.toArray();
        if (position.size() == 3)
            emit scrollPositionChanged(position.at(0).toDouble(), position.at(1).toDouble(), position.at(2).toDouble());
    } else if (kind == QLatin1String("selection")) {
        // NB: The interface places the handles in its own pixels, which are the device's; the page answers in
        //     CSS pixels.
        QJsonObject selection = data.toObject();
        const qreal scale = deviceScaleFactor();
        handleJsSelectionUpdate(selection.value(QStringLiteral("t")).toString(),
            selection.value(QStringLiteral("sx")).toDouble() * scale, selection.value(QStringLiteral("sy")).toDouble() * scale,
            selection.value(QStringLiteral("ex")).toDouble() * scale, selection.value(QStringLiteral("ey")).toDouble() * scale);
    }
}

void WPEWebPage::moveSelectionEdge(bool start, qreal cssX, qreal cssY)
{
    // These arrive in the interface's pixels too, whatever the parameters are called.
    const qreal scale = deviceScaleFactor();
    cssX /= scale;
    cssY /= scale;
    QString script = QString::fromLatin1(kMoveSelectionScript).arg(cssX, 0, 'f', 2).arg(cssY - kHandleToTextCssPx, 0, 'f', 2).arg(start ? 1 : 0);
    evaluate(script);
}

void WPEWebPage::findResult(int current, int total)
{
    Q_UNUSED(current);
    setFindInPageHasResult(total != 0);
}

void WPEWebPage::selectMenuRequested(const QByteArray &optionsJson)
{
    // The interface's menu is a plain list, so group headings are left out and an option that cannot be chosen
    // is too.
    QStringList labels;
    int selected = 0;
    m_selectMenuIds.clear();
    for (const QJsonValue &entry : QJsonDocument::fromJson(optionsJson).array()) {
        QJsonObject option = entry.toObject();
        if (!option.contains(QStringLiteral("id")) || option.value(QStringLiteral("disabled")).toBool())
            continue;
        if (option.value(QStringLiteral("selected")).toBool())
            selected = labels.size();
        labels.append(option.value(QStringLiteral("label")).toString());
        m_selectMenuIds.append(option.value(QStringLiteral("id")).toInt());
    }
    if (labels.isEmpty()) {
        sendToEngine(LB_EMBED_SELECT_MENU_CLOSED, -1);
        return;
    }
    openSelectMenu(labels, selected);
}

void WPEWebPage::dialogRequested(int kind, const QByteArray &json)
{
    QJsonObject dialog = QJsonDocument::fromJson(json).object();
    m_jsDialogKind = kind;
    m_jsDialogMessage = dialog.value(QStringLiteral("message")).toString();
    m_jsDialogDefaultText = dialog.value(QStringLiteral("default")).toString();
    emit jsDialogChanged();
}

QString WPEWebPage::jsDialogKind() const
{
    switch (m_jsDialogKind) {
    case LB_EMBED_DIALOG_ALERT:
        return QStringLiteral("alert");
    case LB_EMBED_DIALOG_CONFIRM:
        return QStringLiteral("confirm");
    case LB_EMBED_DIALOG_PROMPT:
        return QStringLiteral("prompt");
    default:
        return QString();
    }
}

void WPEWebPage::resolveJsDialog(bool accepted, const QString &text)
{
    if (m_jsDialogKind < 0)
        return;
    sendToEngine(LB_EMBED_DIALOG_CLOSED, accepted ? 1 : 0, m_jsDialogKind, 0, 0, text.toUtf8());
    m_jsDialogKind = -1;
    m_jsDialogMessage.clear();
    m_jsDialogDefaultText.clear();
    emit jsDialogChanged();
}

void WPEWebPage::applyDarkModeGlobally(bool dark)
{
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_DARK_MODE, dark ? 1 : 0);
}

// --- Pickers, the file chooser, where the page is scrolled to, engine data -----------------------------------

void WPEWebPage::closeInputPicker()
{
    m_inputPickerIsEngineColor = false;
    if (m_inputPickerActive) {
        m_inputPickerActive = false;
        emit inputPickerActiveChanged();
    }
}

void WPEWebPage::tapped(const QPointF &position)
{
    const qreal scale = deviceScaleFactor();
    QString script = QString::fromLatin1(kInputPickerScript).arg(position.x() / scale, 0, 'f', 2).arg(position.y() / scale, 0, 'f', 2);
    evaluate(script, [this](const QJsonValue &value) {
        if (!value.isObject())
            return;
        QJsonObject input = value.toObject();
        m_inputPickerIsEngineColor = false;
        openInputPicker(input.value(QStringLiteral("type")).toString(), input.value(QStringLiteral("value")).toString(),
            input.value(QStringLiteral("min")).toString(), input.value(QStringLiteral("max")).toString(),
            input.value(QStringLiteral("step")).toString());
    });
}

void WPEWebPage::colorPickerRequested(const QString &color)
{
    m_inputPickerIsEngineColor = true;
    openInputPicker(QStringLiteral("color"), color, QString(), QString(), QString());
}

void WPEWebPage::filePickerRequested(bool multiple)
{
    if (m_fileChooserSelectMultiple != multiple) {
        m_fileChooserSelectMultiple = multiple;
        emit fileChooserSelectMultipleChanged();
    }
    m_fileChooserActive = true;
    emit fileChooserActiveChanged();
}

void WPEWebPage::clearEngineDataGlobally(bool cache, bool siteData)
{
    LadybirdEngine::instance()->send(0, LB_EMBED_CLEAR_DATA, (cache ? LB_EMBED_CLEAR_CACHE : 0) | (siteData ? LB_EMBED_CLEAR_SITE_DATA : 0));
}

void WPEWebPage::pageColorChanged(int kind, const QString &color)
{
    QColor before = surroundColor();
    if (kind == LB_EMBED_PAGE_COLOR_THEME)
        m_themeColor = color.isEmpty() ? QColor() : QColor(color);
    else if (QColor(color).isValid())
        m_backgroundColor = QColor(color);
    if (surroundColor() != before)
        emit surroundColorChanged();
}
