/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Pacific: Atlantic's page class on top of a Ladybird engine view. See WPEWebPage.h.
 */

#include "WPEWebPage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethod>
#include <QInputMethodEvent>
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
    if (finished)
        m_lastCommittedUrl = url();
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
    if (event->type() == QEvent::TouchBegin)
        emit touched();
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

void WPEWebPage::applyAdBlockEnabledGlobally(bool enabled)
{
    Q_UNUSED(enabled);
}

void WPEWebPage::applyAdBlockAllowlistGlobally(const QString &json)
{
    Q_UNUSED(json);
}

void WPEWebPage::applyCookieBannerBlockingGlobally(bool enabled)
{
    Q_UNUSED(enabled);
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
}

void WPEWebPage::selectAll()
{
}

void WPEWebPage::moveSelectionStart(qreal cssX, qreal cssY)
{
    Q_UNUSED(cssX);
    Q_UNUSED(cssY);
}

void WPEWebPage::moveSelectionEnd(qreal cssX, qreal cssY)
{
    Q_UNUSED(cssX);
    Q_UNUSED(cssY);
}

void WPEWebPage::copyToClipboard()
{
}

void WPEWebPage::handleJsSelectionClear()
{
}

void WPEWebPage::handleJsSelectionUpdate(const QString &text, qreal startX, qreal startY, qreal endX, qreal endY)
{
    Q_UNUSED(text);
    Q_UNUSED(startX);
    Q_UNUSED(startY);
    Q_UNUSED(endX);
    Q_UNUSED(endY);
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
    Q_UNUSED(text);
    Q_UNUSED(backwards);
}

void WPEWebPage::findFinish()
{
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
    Q_UNUSED(index);
}

void WPEWebPage::downloadUrl(const QString &url)
{
    Q_UNUSED(url);
}

void WPEWebPage::closeSelectMenu()
{
}

void WPEWebPage::chooseFiles(const QStringList &filePaths)
{
    Q_UNUSED(filePaths);
}

void WPEWebPage::cancelFileChooser()
{
}

void WPEWebPage::openSelectMenu(const QStringList &options, int selectedIndex)
{
    Q_UNUSED(options);
    Q_UNUSED(selectedIndex);
}

void WPEWebPage::resolveInputPicker(const QString &value)
{
    Q_UNUSED(value);
}

void WPEWebPage::cancelInputPicker()
{
}

void WPEWebPage::openInputPicker(const QString &type, const QString &value, const QString &min, const QString &max, const QString &step)
{
    Q_UNUSED(type);
    Q_UNUSED(value);
    Q_UNUSED(min);
    Q_UNUSED(max);
    Q_UNUSED(step);
}

void WPEWebPage::openImageLongPress(const QString &imageUrl)
{
    Q_UNUSED(imageUrl);
}

void WPEWebPage::clearImageLongPress()
{
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
    return false;
}

void WPEWebPage::setAdBlockEnabled(bool enabled)
{
    Q_UNUSED(enabled);
}

bool WPEWebPage::cookieBannerBlockingEnabled() const
{
    return false;
}

void WPEWebPage::setCookieBannerBlockingEnabled(bool enabled)
{
    Q_UNUSED(enabled);
}
