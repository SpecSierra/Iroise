/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Pacific: Atlantic's page class on top of a Ladybird engine view. See WebPage.h.
 */

#include "WebPage.h"

#include "credentialstore.h"
#include "DownloadModel.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
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
#include <QSslCipher>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTouchEvent>

namespace {

QList<WebPage *> &livePages()
{
    static QList<WebPage *> pages;
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
    // A login: the password field and the text field before it, when a form is sent or a button pressed.
    "var lastLogin='';"
    "function login(scope){"
    "var p=(scope||document).querySelector('input[type=password]');if(!p||!p.value)return;"
    "var all=Array.prototype.slice.call((p.form||document).querySelectorAll('input')),u=null;"
    "for(var i=all.indexOf(p)-1;i>=0;i--){var t=all[i].type;if((t==='text'||t==='email'||t==='tel')&&all[i].value){u=all[i];break;}}"
    "var l={u:u?u.value:'',p:p.value,uf:u?(u.name||u.id):'',pf:p.name||p.id},k=JSON.stringify(l);"
    "if(k!==lastLogin){lastLogin=k;post('login',l);}}"
    "document.addEventListener('submit',function(e){login(e.target);},true);"
    "document.addEventListener('click',function(e){var b=e.target.closest&&e.target.closest('button,input[type=submit],[role=button]');if(b)login(b.closest('form'));},true);"
    "document.addEventListener('keydown',function(e){if(e.key==='Enter'&&e.target.type==='password')login(e.target.form);},true);"
    "return 0;})()";

// Fills a saved login into the login form the user just focused, if the page is still at the origin it was saved
// for. %1 = JSON {o: origin, u: username, p: password}. Values go through the input's own setter, with input and
// change events, so that pages built on frameworks see them.
const char kFillLoginScript[] =
    "(function(l){"
    "if(location.origin!==l.o)return 0;"
    "var f=document.activeElement;if(!f||f.tagName!=='INPUT')return 0;"
    "var scope=f.form||document,p=scope.querySelector('input[type=password]');if(!p)return 0;"
    "var all=Array.prototype.slice.call(scope.querySelectorAll('input')),u=null;"
    "for(var i=all.indexOf(p)-1;i>=0;i--){var t=all[i].type;if(t==='text'||t==='email'||t==='tel'){u=all[i];break;}}"
    "if(f!==p&&f!==u)return 0;"
    "var set=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;"
    "function fill(e,v){set.call(e,v);e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));}"
    "if(u&&l.u)fill(u,l.u);fill(p,l.p);return 1;"
    "})(%1)";

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

WebPage::WebPage(QQuickItem *parent)
    : EngineView(parent)
{
    livePages().append(this);
    m_security = new SecurityInfo(this);
    setFlag(ItemAcceptsInputMethod, false);

    connect(this, &EngineView::loadingChanged, this, &WebPage::onLoadingChanged);
    connect(this, &EngineView::frameArrived, this, &WebPage::onFrameArrived);
    connect(this, &EngineView::pageWantsTextInputChanged, this, &WebPage::onPageWantsTextInputChanged);
    connect(this, &EngineView::engineCrashed, this, [this] {
        m_crashed = true;
        emit crashedChanged();
    });

    // A tab's thumbnail is a scaled copy of what is on screen, refreshed while the tab is the one showing.
    m_thumbnailTimer.setInterval(kThumbnailIntervalMs);
    connect(&m_thumbnailTimer, &QTimer::timeout, this, &WebPage::captureThumbnail);

    // A login sent on a page that stays where it is (a script-driven sign-in) is offered after a moment.
    m_offerLoginTimer.setSingleShot(true);
    m_offerLoginTimer.setInterval(4000);
    connect(&m_offerLoginTimer, &QTimer::timeout, this, &WebPage::offerCapturedLogin);
    connect(this, &EngineView::urlChanged, this, &WebPage::offerCapturedLogin);
    connect(CredentialStore::instance(), &CredentialStore::lockedChanged, this, [this] {
        if (m_saveLoginWaitsForVault && CredentialStore::instance()->isUnlocked())
            storePendingLogin();
    });

    m_chromeGestureDebounceTimer.setSingleShot(true);
    m_chromeGestureDebounceTimer.setInterval(150);
    connect(&m_chromeGestureDebounceTimer, &QTimer::timeout, this, [this] {
        if (m_chromeGestureArmed && m_chrome != m_pendingChrome)
            setChrome(m_pendingChrome);
        m_chromeGestureArmed = false;
    });
    connect(this, &EngineView::scrollPositionChanged, this, [this](qreal scrollY, qreal scrollHeight, qreal innerHeight) {
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

WebPage::~WebPage()
{
    livePages().removeAll(this);
}

const QList<WebPage *> &WebPage::liveInstances()
{
    return livePages();
}

int WebPage::tabId() const
{
    return m_tabId;
}

void WebPage::setTabId(int tabId)
{
    if (m_tabId != tabId) {
        m_tabId = tabId;
        emit tabIdChanged();
    }
}

bool WebPage::painted() const
{
    return m_painted;
}

bool WebPage::loaded() const
{
    return m_loaded;
}

void WebPage::updateEngineVisibility()
{
    bool visible = m_active && m_appForeground;
    setEngineVisible(visible);
    if (visible && !privateBrowsing())
        m_thumbnailTimer.start();
    else
        m_thumbnailTimer.stop();
}

void WebPage::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    updateEngineVisibility();
    emit activeChanged();
}

void WebPage::setAppForeground(bool foreground)
{
    if (m_appForeground == foreground)
        return;
    m_appForeground = foreground;
    updateEngineVisibility();
}

void WebPage::loadTab(const QString &url, bool force)
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

void WebPage::loadCurrentUrlAgain()
{
    reload();
}

void WebPage::goBack()
{
    EngineView::goBack();
}

void WebPage::goForward()
{
    EngineView::goForward();
}

void WebPage::onLoadingChanged(EngineViewLoadRequest *loadRequest)
{
    bool finished = loadRequest->status() != EngineView::LoadStartedStatus;
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
        // NB: A certificate the engine could not verify ends the load on its own error page, which says so.
        evaluate(QStringLiteral("location.href==='about:error'&&document.body.innerText.indexOf('SSL verification failed')>=0"),
            [this](const QJsonValue &failed) { setTlsError(failed.toBool()); updateSecurity(); });
    } else {
        setTlsError(false);
        // What the old page had selected, was searching for or was asking is gone with it. A login it sent is
        // offered for saving now that it has gone on.
        offerCapturedLogin();
        handleJsSelectionClear();
        if (!m_permissionQuestions.isEmpty())
            answerPermissions(QString(), false);
        m_findText.clear();
        setFindInPageHasResult(false);
        if (m_jsDialogKind >= 0) {
            m_jsDialogKind = -1;
            emit jsDialogChanged();
        }
    }
}

void WebPage::onFrameArrived()
{
    if (!m_painted) {
        m_painted = true;
        emit paintedChanged();
    }
    emit afterRendering();
}

// --- Thumbnails -------------------------------------------------------------

void WebPage::grabToFile(const QSize &size)
{
    if (size.width() > 0 && size.height() > 0)
        m_thumbnailSize = size;
    captureThumbnail();
}

void WebPage::grabThumbnail(const QSize &size)
{
    Q_UNUSED(size);
    // FIXME: Atlantic answered this with a PNG data: URI from a WebKit snapshot (used as a favourite's icon when
    //        the page has no usable favicon). Nothing asks the engine for a snapshot yet.
}

void WebPage::captureThumbnail()
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
// --- Chrome -----------------------------------------------------------------

void WebPage::setChrome(bool chrome)
{
    static const bool logChrome = !qgetenv("PACIFIC_LOG_ENGINE").isEmpty();
    if (logChrome && m_chrome != chrome)
        qWarning("WebPage: chrome %d, page %dx%d painted %dx%d", chrome, int(width()), int(height()), paintedSize().width(), paintedSize().height());
    if (m_chrome != chrome) {
        m_chrome = chrome;
        emit chromeChanged();
    }
}

void WebPage::forceChrome(bool forced)
{
    setForcedChrome(forced);
    if (forced)
        setChrome(true);
}

void WebPage::suspendView()
{
    setEngineVisible(false);
}

void WebPage::resumeView()
{
    updateEngineVisibility();
}

void WebPage::setDesktopMode(bool desktop)
{
    if (m_desktopMode == desktop)
        return;
    m_desktopMode = desktop;
    sendToEngine(LB_EMBED_SET_DESKTOP_MODE, desktop ? 1 : 0);
    emit desktopModeChanged();
}

// --- Selection --------------------------------------------------------------

bool WebPage::textSelectionActive() const
{
    return m_textSelectionActive;
}

QObject* WebPage::textSelectionController()
{
    return this;
}

QString WebPage::selectedText() const
{
    return m_selectedText;
}

bool WebPage::isPhoneNumber() const
{
    static const QRegularExpression re(QStringLiteral("^[+\\d][\\d\\s\\-().+]{6,}$"));
    return re.match(m_selectedText.trimmed()).hasMatch();
}
// --- Text input -------------------------------------------------------------
//
// The engine says when an editable element has focus. While one has, this item takes the input method's focus,
// which brings up the on-screen keyboard, and what the keyboard commits or composes goes to the engine.

void WebPage::onPageWantsTextInputChanged()
{
    const bool wantsInput = pageWantsTextInput();
    setFlag(ItemAcceptsInputMethod, wantsInput);
    // NB: Only a field the user touched: a page focusing one itself (an invisible form, say) gets no login.
    if (wantsInput && QDateTime::currentMSecsSinceEpoch() - m_lastTouchMs < 1500)
        fillLoginForFocusedForm();
    if (wantsInput) {
        forceActiveFocus();
        QGuiApplication::inputMethod()->update(Qt::ImEnabled | Qt::ImHints);
        QGuiApplication::inputMethod()->show();
    } else if (hasActiveFocus()) {
        QGuiApplication::inputMethod()->update(Qt::ImEnabled);
        QGuiApplication::inputMethod()->hide();
    }
}

QVariant WebPage::inputMethodQuery(Qt::InputMethodQuery query) const
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

void WebPage::inputMethodEvent(QInputMethodEvent *event)
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

void WebPage::keyPressEvent(QKeyEvent *event)
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

void WebPage::touchEvent(QTouchEvent *event)
{
    if (event->type() == QEvent::TouchBegin) {
        m_lastTouchMs = QDateTime::currentMSecsSinceEpoch();
        emit touched();
        // A page still loading has not had the agent yet.
        evaluate(QString::fromLatin1(kPageAgentScript));
    }
    EngineView::touchEvent(event);
}

// --- Everything below is state the interface reads and the engine protocol does not drive yet ---------------

bool WebPage::domContentLoaded() const
{
    return m_domContentLoaded;
}

bool WebPage::chrome() const
{
    return m_chrome;
}

bool WebPage::forcedChrome() const
{
    return m_forcedChrome;
}

void WebPage::setForcedChrome(bool forcedChrome)
{
    if (m_forcedChrome == forcedChrome)
        return;
    m_forcedChrome = forcedChrome;
    emit forcedChromeChanged();
}

bool WebPage::fullscreen() const
{
    return m_fullscreen;
}

bool WebPage::atYBeginning() const
{
    return m_atYBeginning;
}

bool WebPage::atYEnd() const
{
    return m_atYEnd;
}

QString WebPage::favicon() const
{
    return m_favicon;
}

void WebPage::setFavicon(const QString &favicon)
{
    if (m_favicon == favicon)
        return;
    m_favicon = favicon;
    emit faviconChanged();
}

QStringList WebPage::faviconCandidates() const
{
    return m_faviconCandidates;
}

void WebPage::setFaviconCandidates(const QStringList &candidates)
{
    if (m_faviconCandidates == candidates)
        return;
    m_faviconCandidates = candidates;
}
qreal WebPage::fullscreenHeight() const
{
    return m_fullscreenHeight;
}

void WebPage::setFullscreenHeight(qreal height)
{
    if (m_fullscreenHeight == height)
        return;
    m_fullscreenHeight = height;
    emit fullscreenHeightChanged();
}

qreal WebPage::toolbarHeight() const
{
    return m_toolbarHeight;
}

void WebPage::setToolbarHeight(qreal height)
{
    if (m_toolbarHeight == height)
        return;
    m_toolbarHeight = height;
    emit toolbarHeightChanged();
}

bool WebPage::active() const
{
    return m_active;
}

// The engine blocks with its own copy of Brave's adblock engine and its own list subscriptions. These two switches
// are for the whole engine, so they are sent once and not per page.
static bool s_adBlockEnabled = false;
static bool s_cookieBannerBlockingEnabled = false;

void WebPage::applyAdBlockEnabledGlobally(bool enabled)
{
    s_adBlockEnabled = enabled;
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_AD_BLOCKING, enabled ? 1 : 0);
    for (WebPage *page : livePages())
        emit page->adBlockEnabledChanged();
}

void WebPage::applyAdBlockAllowlistGlobally(const QString &json)
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

void WebPage::applyCookieBannerBlockingGlobally(bool enabled)
{
    s_cookieBannerBlockingEnabled = enabled;
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_COOKIE_BANNER_BLOCKING, enabled ? 1 : 0);
    for (WebPage *page : livePages())
        emit page->cookieBannerBlockingEnabledChanged();
}

// The site options the engine applies itself, as it learns where a tab is going (LB_EMBED_SET_SITE_OPTIONS).
static QJsonObject s_siteUserAgents;
static QJsonArray s_noScriptSites;

static void sendSiteOptions()
{
    QJsonObject options;
    options.insert(QStringLiteral("userAgents"), s_siteUserAgents);
    options.insert(QStringLiteral("noScript"), s_noScriptSites);
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_SITE_OPTIONS, 0, 0, 0, 0, 0, QJsonDocument(options).toJson(QJsonDocument::Compact));
}

void WebPage::applySiteUaOverridesGlobally(const QString &json)
{
    // The interface's profile ids, as the names of the engine's user agent presets.
    static const QHash<QString, QString> presets {
        { QStringLiteral("chrome-android"), QStringLiteral("Chrome Android Mobile") },
        { QStringLiteral("safari-iphone"), QStringLiteral("Safari iOS Mobile") },
        { QStringLiteral("firefox-android"), QStringLiteral("Firefox Android Mobile") },
        { QStringLiteral("chrome-desktop"), QStringLiteral("Chrome Linux Desktop") },
        { QStringLiteral("safari-mac"), QStringLiteral("Safari macOS Desktop") },
        { QStringLiteral("firefox-desktop"), QStringLiteral("Firefox Linux Desktop") },
    };
    s_siteUserAgents = QJsonObject();
    QJsonObject overrides = QJsonDocument::fromJson(json.toUtf8()).object();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        QString preset = presets.value(it.value().toString());
        if (!preset.isEmpty())
            s_siteUserAgents.insert(it.key().trimmed().toLower(), preset);
    }
    sendSiteOptions();
}

void WebPage::applyJavaScriptBlocklistGlobally(const QString &json)
{
    s_noScriptSites = QJsonArray();
    for (const QJsonValue &host : QJsonDocument::fromJson(json.toUtf8()).array())
        s_noScriptSites.append(host.toString().trimmed().toLower());
    sendSiteOptions();
}
bool WebPage::chromeGestureEnabled() const
{
    return m_chromeGestureEnabled;
}

void WebPage::setChromeGestureEnabled(bool enabled)
{
    if (m_chromeGestureEnabled == enabled)
        return;
    m_chromeGestureEnabled = enabled;
    emit chromeGestureEnabledChanged();
}

qreal WebPage::chromeGestureThreshold() const
{
    return m_chromeGestureThreshold;
}

void WebPage::setChromeGestureThreshold(qreal threshold)
{
    if (m_chromeGestureThreshold == threshold)
        return;
    m_chromeGestureThreshold = threshold;
    emit chromeGestureThresholdChanged();
}

bool WebPage::fixedToolbar() const
{
    return m_fixedToolbar;
}

void WebPage::setFixedToolbar(bool fixed)
{
    if (m_fixedToolbar == fixed)
        return;
    m_fixedToolbar = fixed;
    emit fixedToolbarChanged();
}

bool WebPage::moving() const
{
    return false;
}
bool WebPage::dragging() const
{
    return false;
}

bool WebPage::desktopMode() const
{
    return m_desktopMode;
}
void WebPage::pageFullscreenChanged(bool fullscreen)
{
    if (m_fullscreen == fullscreen)
        return;
    m_fullscreen = fullscreen;
    emit fullscreenChanged();
    updateMediaState();
}

void WebPage::exitFullscreen()
{
    if (m_fullscreen)
        sendToEngine(LB_EMBED_EXIT_FULLSCREEN);
}

void WebPage::mediaStateChanged(bool playsAudio, bool keepsScreenOn)
{
    m_playsAudio = playsAudio;
    m_keepsScreenOn = keepsScreenOn;
    updateMediaState();
}

void WebPage::updateMediaState()
{
    // NB: The engine reports sound, not video. A page in fullscreen is nearly always showing one, and a page that
    //     wants the screen on asks for it.
    bool audio = m_playsAudio;
    bool video = m_keepsScreenOn || (m_fullscreen && m_playsAudio);
    if (audio != m_mediaAudioActive) {
        m_mediaAudioActive = audio;
        emit mediaAudioActiveChanged();
    }
    if (video != m_mediaVideoActive) {
        m_mediaVideoActive = video;
        emit mediaVideoActiveChanged();
    }
}
void WebPage::revealFocusedEditable()
{
    evaluate(QStringLiteral("(function(e){if(e&&e!==document.body)e.scrollIntoView({block:'center'});return 0})(document.activeElement)"));
}
void WebPage::clearSelection()
{
    handleJsSelectionClear();
    evaluate(QStringLiteral("getSelection().removeAllRanges(),0"));
}

void WebPage::selectAll()
{
    evaluate(QStringLiteral("getSelection().selectAllChildren(document.body),0"));
}

void WebPage::moveSelectionStart(qreal cssX, qreal cssY)
{
    moveSelectionEdge(true, cssX, cssY);
}

void WebPage::moveSelectionEnd(qreal cssX, qreal cssY)
{
    moveSelectionEdge(false, cssX, cssY);
}

void WebPage::copyToClipboard()
{
    if (!m_selectedText.isEmpty())
        QGuiApplication::clipboard()->setText(m_selectedText);
}

void WebPage::handleJsSelectionClear()
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

void WebPage::handleJsSelectionUpdate(const QString &text, qreal startX, qreal startY, qreal endX, qreal endY)
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
void WebPage::findText(const QString &text, bool backwards)
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

void WebPage::findFinish()
{
    if (!m_findText.isEmpty())
        sendToEngine(LB_EMBED_FIND, LB_EMBED_FIND_END);
    m_findText.clear();
    setFindInPageHasResult(false);
}

void WebPage::setFindInPageHasResult(bool has)
{
    if (m_findInPageHasResult == has)
        return;
    m_findInPageHasResult = has;
    emit findInPageHasResultChanged();
}

QString WebPage::origin(const QUrl &url)
{
    return url.adjusted(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
}

void WebPage::fillLoginForFocusedForm()
{
    CredentialStore *store = CredentialStore::instance();
    if (url().scheme() != QLatin1String("https") || !store->isUnlocked())
        return;
    const QString site = origin(url());
    for (const auto &entry : store->all()) {
        if (entry.second.value(QStringLiteral("hostname")).toString() != site)
            continue;
        QJsonObject login;
        login.insert(QStringLiteral("o"), site);
        login.insert(QStringLiteral("u"), entry.second.value(QStringLiteral("username")).toString());
        login.insert(QStringLiteral("p"), entry.second.value(QStringLiteral("password")).toString());
        evaluate(QString::fromLatin1(kFillLoginScript).arg(QString::fromUtf8(QJsonDocument(login).toJson(QJsonDocument::Compact))));
        return;
    }
}

// The hosts whose certificates the user chose to trust although the engine could not verify them, for this run.
static QSet<QString> &certificateExceptions()
{
    static QSet<QString> hosts;
    return hosts;
}

void WebPage::setTlsError(bool failed)
{
    if (failed == m_tlsErrorPending)
        return;
    m_tlsErrorPending = failed;
    m_tlsErrorFailingUri = failed ? url().toString() : QString();
    m_tlsErrorHost = failed ? url().host() : QString();
    m_tlsErrorMessage.clear();
    emit tlsErrorChanged();
}

void WebPage::acceptTlsCertificate()
{
    if (!m_tlsErrorPending)
        return;
    certificateExceptions().insert(m_tlsErrorHost);
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_CERTIFICATE_EXCEPTIONS, 0, 0, 0, 0, 0, QStringList(certificateExceptions().values()).join(QLatin1Char('\n')).toUtf8());
    const QByteArray failing = m_tlsErrorFailingUri.toUtf8();
    setTlsError(false);
    sendToEngine(LB_EMBED_LOAD, 0, 0, 0, 0, failing);
}

namespace {

// What the security panel shows about a host's certificate. Fetched once per host and run, over a connection of
// our own: the engine does not hand out the certificates it checked.
struct CertificateDetails {
    QString subject, issuer, cipher, notBefore, notAfter, pem, issuerOrganization, issuerCommonName, subjectOrganization, errors;
};

QHash<QString, CertificateDetails> &certificateDetails()
{
    static QHash<QString, CertificateDetails> details;
    return details;
}

}

void WebPage::updateSecurity()
{
    const QUrl pageUrl = url();
    if (pageUrl.scheme() != QLatin1String("https") || m_tlsErrorPending) {
        m_security->reset();
        return;
    }
    const QString host = pageUrl.host();
    const int port = pageUrl.port(443);
    auto show = [this, host](const CertificateDetails &details) {
        if (url().host() != host)
            return;
        const bool accepted = certificateExceptions().contains(host);
        m_security->update(true, accepted, details.subject, details.issuer, details.cipher, details.notBefore, details.notAfter,
            details.pem, details.issuerOrganization, details.issuerCommonName, details.subjectOrganization, details.errors);
    };
    auto known = certificateDetails().constFind(host);
    if (known != certificateDetails().constEnd()) {
        show(known.value());
        return;
    }
    show(CertificateDetails { host, {}, {}, {}, {}, {}, {}, {}, {}, {} });

    QSslSocket *socket = new QSslSocket(this);
    QTimer::singleShot(10000, socket, &QObject::deleteLater);
    connect(socket, static_cast<void (QSslSocket::*)(const QList<QSslError> &)>(&QSslSocket::sslErrors), socket, [socket](const QList<QSslError> &errors) {
        QStringList descriptions;
        for (const QSslError &error : errors)
            descriptions.append(error.errorString());
        socket->setProperty("errors", descriptions.join(QStringLiteral("; ")));
        // Only to read the certificate: nothing is sent over this connection.
        socket->ignoreSslErrors();
    });
    connect(socket, &QSslSocket::encrypted, socket, [socket, host, show] {
        const QList<QSslCertificate> chain = socket->peerCertificateChain();
        CertificateDetails details;
        if (!chain.isEmpty()) {
            const QSslCertificate &leaf = chain.first();
            details.subject = leaf.subjectInfo(QSslCertificate::CommonName).value(0, host);
            details.subjectOrganization = leaf.subjectInfo(QSslCertificate::Organization).value(0);
            details.issuerCommonName = leaf.issuerInfo(QSslCertificate::CommonName).value(0);
            details.issuerOrganization = leaf.issuerInfo(QSslCertificate::Organization).value(0);
            details.issuer = details.issuerOrganization.isEmpty() ? details.issuerCommonName : details.issuerOrganization;
            details.notBefore = leaf.effectiveDate().toString(Qt::DefaultLocaleShortDate);
            details.notAfter = leaf.expiryDate().toString(Qt::DefaultLocaleShortDate);
            details.pem = QString::fromLatin1(leaf.toPem());
        }
        details.cipher = socket->sessionCipher().name();
        details.errors = socket->property("errors").toString();
        certificateDetails().insert(host, details);
        show(details);
        socket->disconnectFromHost();
        socket->deleteLater();
    });
    socket->connectToHostEncrypted(host, static_cast<quint16>(port));
}

// Shared by every non-private tab, for as long as the browser runs.
static QHash<QString, bool> &sessionPermissionDecisions()
{
    static QHash<QString, bool> decisions;
    return decisions;
}

void WebPage::clearPermissionDecisions()
{
    sessionPermissionDecisions().clear();
}

void WebPage::permissionRequested(int question, const QString &name, const QString &origin)
{
    const QString key = origin + QLatin1Char('|') + name;
    QHash<QString, bool> &decisions = privateBrowsing() ? m_permissionDecisions : sessionPermissionDecisions();
    if (decisions.contains(key)) {
        sendToEngine(LB_EMBED_PERMISSION_DECIDED, question, decisions.value(key) ? 1 : 0);
        return;
    }
    m_permissionQuestions.append(qMakePair(question, key));
    if (!m_permissionPending)
        showNextPermissionQuestion();
}

void WebPage::showNextPermissionQuestion()
{
    m_permissionPending = !m_permissionQuestions.isEmpty();
    if (m_permissionPending) {
        const QString key = m_permissionQuestions.first().second;
        const int separator = key.lastIndexOf(QLatin1Char('|'));
        m_permissionHost = QUrl(key.left(separator)).host();
        m_permissionType = key.mid(separator + 1);
    } else {
        m_permissionHost.clear();
        m_permissionType.clear();
    }
    emit permissionChanged();
}

void WebPage::answerPermissions(const QString &key, bool allow)
{
    for (int i = m_permissionQuestions.size() - 1; i >= 0; --i) {
        if (!key.isEmpty() && m_permissionQuestions.at(i).second != key)
            continue;
        sendToEngine(LB_EMBED_PERMISSION_DECIDED, m_permissionQuestions.at(i).first, allow ? 1 : 0);
        m_permissionQuestions.removeAt(i);
    }
    showNextPermissionQuestion();
}

void WebPage::resolvePermission(bool allow)
{
    if (m_permissionQuestions.isEmpty())
        return;
    const QString key = m_permissionQuestions.first().second;
    (privateBrowsing() ? m_permissionDecisions : sessionPermissionDecisions()).insert(key, allow);
    answerPermissions(key, allow);
}

void WebPage::offerCapturedLogin()
{
    m_offerLoginTimer.stop();
    if (m_capturedLogin.isEmpty())
        return;
    QVariantMap login = m_capturedLogin;
    m_capturedLogin.clear();
    const QString username = login.value(QStringLiteral("u")).toString();
    const QString password = login.value(QStringLiteral("p")).toString();

    // What the vault has for this site and name already: nothing to ask if it is the same password.
    m_saveLoginExistingUid = -1;
    CredentialStore *store = CredentialStore::instance();
    if (store->isUnlocked()) {
        for (const auto &entry : store->all()) {
            if (entry.second.value(QStringLiteral("hostname")).toString() != m_capturedLoginOrigin
                || entry.second.value(QStringLiteral("username")).toString() != username)
                continue;
            if (entry.second.value(QStringLiteral("password")).toString() == password)
                return;
            m_saveLoginExistingUid = entry.first;
        }
    }
    m_saveLoginOrigin = m_capturedLoginOrigin;
    m_saveLoginHost = QUrl(m_capturedLoginOrigin).host();
    m_saveLoginUsername = username;
    m_saveLoginPassword = password;
    m_saveLoginFields = login;
    m_saveLoginPending = true;
    emit saveLoginChanged();
}

void WebPage::resolveSaveLogin(bool save)
{
    if (!m_saveLoginPending)
        return;
    m_saveLoginPending = false;
    emit saveLoginChanged();
    if (!save) {
        m_saveLoginPassword.clear();
        return;
    }
    if (CredentialStore::instance()->isUnlocked()) {
        storePendingLogin();
        return;
    }
    // The login is stored as soon as the vault is opened (or created) on the page this asks for.
    m_saveLoginWaitsForVault = true;
    emit saveLoginNeedsUnlock();
}

void WebPage::storePendingLogin()
{
    m_saveLoginWaitsForVault = false;
    QVariantMap fields;
    fields.insert(QStringLiteral("hostname"), m_saveLoginOrigin);
    fields.insert(QStringLiteral("formSubmitURL"), m_saveLoginOrigin);
    fields.insert(QStringLiteral("httpRealm"), QString());
    fields.insert(QStringLiteral("username"), m_saveLoginUsername);
    fields.insert(QStringLiteral("password"), m_saveLoginPassword);
    fields.insert(QStringLiteral("usernameField"), m_saveLoginFields.value(QStringLiteral("uf")));
    fields.insert(QStringLiteral("passwordField"), m_saveLoginFields.value(QStringLiteral("pf")));
    CredentialStore *store = CredentialStore::instance();
    if (m_saveLoginExistingUid >= 0)
        store->update(m_saveLoginExistingUid, fields);
    else
        store->insert(fields);
    m_saveLoginPassword.clear();
    emit loginSaved();
}

void WebPage::selectMenuOption(int index)
{
    if (!m_selectMenuActive)
        return;
    sendToEngine(LB_EMBED_SELECT_MENU_CLOSED, index >= 0 && index < m_selectMenuIds.size() ? m_selectMenuIds.at(index) : -1);
    m_selectMenuActive = false;
    emit selectMenuActiveChanged();
}

void WebPage::downloadUrl(const QString &url)
{
    DownloadModel::instance()->start(url);
}

void WebPage::closeSelectMenu()
{
    selectMenuOption(-1);
}

void WebPage::chooseFiles(const QStringList &filePaths)
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

void WebPage::cancelFileChooser()
{
    chooseFiles(QStringList());
}

void WebPage::openSelectMenu(const QStringList &options, int selectedIndex)
{
    m_selectMenuOptions = options;
    m_selectMenuSelectedIdx = selectedIndex;
    m_selectMenuActive = true;
    emit selectMenuOptionsChanged();
    emit selectMenuSelectedIndexChanged();
    emit selectMenuActiveChanged();
}

void WebPage::resolveInputPicker(const QString &value)
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

void WebPage::cancelInputPicker()
{
    if (m_inputPickerIsEngineColor)
        sendToEngine(LB_EMBED_COLOR_PICKED, 0);
    else
        evaluate(QStringLiteral("window.__pacificPendingInput=null,0"));
    closeInputPicker();
}

void WebPage::openInputPicker(const QString &type, const QString &value, const QString &min, const QString &max, const QString &step)
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

void WebPage::openImageLongPress(const QString &imageUrl)
{
    if (m_imageLongPressUrl != imageUrl) {
        m_imageLongPressUrl = imageUrl;
        emit imageLongPressUrlChanged();
    }
}

void WebPage::clearImageLongPress()
{
    if (!m_imageLongPressUrl.isEmpty()) {
        m_imageLongPressUrl.clear();
        emit imageLongPressUrlChanged();
    }
    setContextInfo(QVariantMap());
}

void WebPage::setContextInfo(const QVariantMap &info)
{
    if (m_contextInfo == info)
        return;
    m_contextInfo = info;
    emit contextInfoChanged();
}

void WebPage::clearContextInfo()
{
    setContextInfo(QVariantMap());
}

bool WebPage::adBlockEnabled() const
{
    return s_adBlockEnabled;
}

void WebPage::setAdBlockEnabled(bool enabled)
{
    applyAdBlockEnabledGlobally(enabled);
}

bool WebPage::cookieBannerBlockingEnabled() const
{
    return s_cookieBannerBlockingEnabled;
}

void WebPage::setCookieBannerBlockingEnabled(bool enabled)
{
    applyCookieBannerBlockingGlobally(enabled);
}

// --- Long press, text selection, select menus and page dialogs -------------------------------------------------

void WebPage::longPressed(const QPointF &position)
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

void WebPage::pageMessage(const QString &kind, const QJsonValue &data)
{
    static const bool log = !qgetenv("PACIFIC_LOG_ENGINE").isEmpty();
    if (log)
        qWarning("WebPage: page message %s", qPrintable(kind));
    if (kind == QLatin1String("scroll")) {
        QJsonArray position = data.toArray();
        if (position.size() == 3)
            emit scrollPositionChanged(position.at(0).toDouble(), position.at(1).toDouble(), position.at(2).toDouble());
    } else if (kind == QLatin1String("login")) {
        // NB: The site is the one the tab is at, never anything the page says.
        if (url().scheme() != QLatin1String("https") || privateBrowsing())
            return;
        m_capturedLogin = data.toObject().toVariantMap();
        m_capturedLoginOrigin = origin(url());
        m_offerLoginTimer.start();
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

void WebPage::moveSelectionEdge(bool start, qreal cssX, qreal cssY)
{
    // These arrive in the interface's pixels too, whatever the parameters are called.
    const qreal scale = deviceScaleFactor();
    cssX /= scale;
    cssY /= scale;
    QString script = QString::fromLatin1(kMoveSelectionScript).arg(cssX, 0, 'f', 2).arg(cssY - kHandleToTextCssPx, 0, 'f', 2).arg(start ? 1 : 0);
    evaluate(script);
}

void WebPage::findResult(int current, int total)
{
    Q_UNUSED(current);
    setFindInPageHasResult(total != 0);
}

void WebPage::selectMenuRequested(const QByteArray &optionsJson)
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

void WebPage::dialogRequested(int kind, const QByteArray &json)
{
    QJsonObject dialog = QJsonDocument::fromJson(json).object();
    m_jsDialogKind = kind;
    m_jsDialogMessage = dialog.value(QStringLiteral("message")).toString();
    m_jsDialogDefaultText = dialog.value(QStringLiteral("default")).toString();
    emit jsDialogChanged();
}

QString WebPage::jsDialogKind() const
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

void WebPage::resolveJsDialog(bool accepted, const QString &text)
{
    if (m_jsDialogKind < 0)
        return;
    sendToEngine(LB_EMBED_DIALOG_CLOSED, accepted ? 1 : 0, m_jsDialogKind, 0, 0, text.toUtf8());
    m_jsDialogKind = -1;
    m_jsDialogMessage.clear();
    m_jsDialogDefaultText.clear();
    emit jsDialogChanged();
}

void WebPage::applyDarkModeGlobally(bool dark)
{
    LadybirdEngine::instance()->send(0, LB_EMBED_SET_DARK_MODE, dark ? 1 : 0);
}

// --- Pickers, the file chooser, where the page is scrolled to, engine data -----------------------------------

void WebPage::closeInputPicker()
{
    m_inputPickerIsEngineColor = false;
    if (m_inputPickerActive) {
        m_inputPickerActive = false;
        emit inputPickerActiveChanged();
    }
}

void WebPage::tapped(const QPointF &position)
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

void WebPage::colorPickerRequested(const QString &color)
{
    m_inputPickerIsEngineColor = true;
    openInputPicker(QStringLiteral("color"), color, QString(), QString(), QString());
}

void WebPage::filePickerRequested(bool multiple)
{
    if (m_fileChooserSelectMultiple != multiple) {
        m_fileChooserSelectMultiple = multiple;
        emit fileChooserSelectMultipleChanged();
    }
    m_fileChooserActive = true;
    emit fileChooserActiveChanged();
}

void WebPage::clearEngineDataGlobally(bool cache, bool siteData)
{
    LadybirdEngine::instance()->send(0, LB_EMBED_CLEAR_DATA, (cache ? LB_EMBED_CLEAR_CACHE : 0) | (siteData ? LB_EMBED_CLEAR_SITE_DATA : 0));
}

void WebPage::pageColorChanged(int kind, const QString &color)
{
    QColor before = surroundColor();
    if (kind == LB_EMBED_PAGE_COLOR_THEME)
        m_themeColor = color.isEmpty() ? QColor() : QColor(color);
    else if (QColor(color).isValid())
        m_backgroundColor = QColor(color);
    if (surroundColor() != before)
        emit surroundColorChanged();
}
