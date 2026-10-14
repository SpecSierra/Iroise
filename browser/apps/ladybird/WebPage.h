/*
 * Copyright (c) 2024 Jolla Ltd.
 *
 * Pacific: a browser tab's page, as the interface's QML sees it, on top of an EngineView: text selection, find,
 * dialogs, pickers, permissions, logins, certificates, the toolbar's reaction to scrolling.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <QHash>
#include <QSharedPointer>
#include <QStringList>
#include <QColor>
#include <QTimer>
#include <QVariant>
#include <QElapsedTimer>
#include <qqml.h>

#include "EngineView.h"

class QQuickItemGrabResult;
class QInputMethodEvent;

// Lightweight security info exposed to QML (mirrors QMozSecurity interface)
class SecurityInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool validState READ validState NOTIFY changed)
    Q_PROPERTY(bool allGood READ allGood NOTIFY changed)
    Q_PROPERTY(QString subjectDisplayName READ subjectDisplayName NOTIFY changed)
    Q_PROPERTY(QString issuerDisplayName READ issuerDisplayName NOTIFY changed)
    Q_PROPERTY(QString cipherName READ cipherName NOTIFY changed)
    Q_PROPERTY(bool certIsNull READ certIsNull NOTIFY changed)
    Q_PROPERTY(QString notBefore READ notBefore NOTIFY changed)
    Q_PROPERTY(QString notAfter READ notAfter NOTIFY changed)
    Q_PROPERTY(QString certificatePem READ certificatePem NOTIFY changed)
    Q_PROPERTY(QString issuerOrganization READ issuerOrganization NOTIFY changed)
    Q_PROPERTY(QString issuerCommonName READ issuerCommonName NOTIFY changed)
    Q_PROPERTY(QString subjectOrganization READ subjectOrganization NOTIFY changed)
    Q_PROPERTY(QString errorDescription READ errorDescription NOTIFY changed)
public:
    explicit SecurityInfo(QObject *parent = nullptr) : QObject(parent) {}
    bool validState() const { return m_valid; }
    bool allGood() const { return m_valid && !m_hasErrors; }
    QString subjectDisplayName() const { return m_subject; }
    QString issuerDisplayName() const { return m_issuer; }
    QString cipherName() const { return m_cipher; }
    bool certIsNull() const { return !m_valid; }
    QString notBefore() const { return m_notBefore; }
    QString notAfter() const { return m_notAfter; }
    QString certificatePem() const { return m_pem; }
    QString issuerOrganization() const { return m_issuerOrg; }
    QString issuerCommonName() const { return m_issuerCN; }
    QString subjectOrganization() const { return m_subjectOrg; }
    QString errorDescription() const { return m_errorDesc; }

    void update(bool valid, bool hasErrors, const QString &subject, const QString &issuer, const QString &cipher,
                const QString &notBefore, const QString &notAfter, const QString &pem,
                const QString &issuerOrg, const QString &issuerCN, const QString &subjectOrg,
                const QString &errorDesc) {
        m_valid = valid; m_hasErrors = hasErrors; m_subject = subject; m_issuer = issuer; m_cipher = cipher;
        m_notBefore = notBefore; m_notAfter = notAfter; m_pem = pem;
        m_issuerOrg = issuerOrg; m_issuerCN = issuerCN; m_subjectOrg = subjectOrg; m_errorDesc = errorDesc;
        emit changed();
    }
    void reset() { update(false, false, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}); }

signals:
    void changed();
private:
    bool m_valid = false;
    bool m_hasErrors = false;
    QString m_subject;
    QString m_issuer;
    QString m_cipher;
    QString m_notBefore;
    QString m_notAfter;
    QString m_pem;
    QString m_issuerOrg;
    QString m_issuerCN;
    QString m_subjectOrg;
    QString m_errorDesc;
};

class WebPage : public EngineView
{
    Q_OBJECT

public:

private:
    Q_PROPERTY(int tabId READ tabId WRITE setTabId NOTIFY tabIdChanged)
    Q_PROPERTY(bool painted READ painted NOTIFY paintedChanged)
    Q_PROPERTY(bool domContentLoaded READ domContentLoaded NOTIFY domContentLoadedChanged)
    Q_PROPERTY(bool chrome READ chrome WRITE setChrome NOTIFY chromeChanged)
    Q_PROPERTY(bool forcedChrome READ forcedChrome WRITE setForcedChrome NOTIFY forcedChromeChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(bool mediaAudioActive READ mediaAudioActive NOTIFY mediaAudioActiveChanged FINAL)
    Q_PROPERTY(bool mediaVideoActive READ mediaVideoActive NOTIFY mediaVideoActiveChanged FINAL)
    Q_PROPERTY(bool atYBeginning READ atYBeginning NOTIFY atYBeginningChanged)
    Q_PROPERTY(bool atYEnd READ atYEnd NOTIFY atYEndChanged)
    Q_PROPERTY(QString favicon READ favicon WRITE setFavicon NOTIFY faviconChanged)
    Q_PROPERTY(QStringList faviconCandidates READ faviconCandidates NOTIFY faviconChanged)
    Q_PROPERTY(qreal fullscreenHeight READ fullscreenHeight WRITE setFullscreenHeight NOTIFY fullscreenHeightChanged)
    Q_PROPERTY(qreal toolbarHeight READ toolbarHeight WRITE setToolbarHeight NOTIFY toolbarHeightChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool chromeGestureEnabled READ chromeGestureEnabled WRITE setChromeGestureEnabled NOTIFY chromeGestureEnabledChanged)
    Q_PROPERTY(qreal chromeGestureThreshold READ chromeGestureThreshold WRITE setChromeGestureThreshold NOTIFY chromeGestureThresholdChanged)
    Q_PROPERTY(bool fixedToolbar READ fixedToolbar WRITE setFixedToolbar NOTIFY fixedToolbarChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged)
    Q_PROPERTY(bool desktopMode READ desktopMode WRITE setDesktopMode NOTIFY desktopModeChanged)
    Q_PROPERTY(bool textSelectionActive READ textSelectionActive NOTIFY textSelectionActiveChanged FINAL)
    Q_PROPERTY(QObject* textSelectionController READ textSelectionController CONSTANT FINAL)
    Q_PROPERTY(bool selectionVisible READ textSelectionActive NOTIFY textSelectionActiveChanged FINAL)
    Q_PROPERTY(QString text READ selectedText NOTIFY selectionTextChanged FINAL)
    Q_PROPERTY(bool isPhoneNumber READ isPhoneNumber NOTIFY selectionTextChanged FINAL)
    Q_PROPERTY(qreal selectionStartX READ selectionStartX NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionStartY READ selectionStartY NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionEndX READ selectionEndX NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionEndY READ selectionEndY NOTIFY selectionHandlesUpdated FINAL)

    // Security / TLS certificate info
    Q_PROPERTY(QObject* security READ security NOTIFY securityChanged FINAL)

    // TLS certificate failure (self-signed, expired, ...) — the load is blocked;
    // QML shows a banner and may call acceptTlsCertificate() to proceed.
    Q_PROPERTY(bool tlsErrorPending READ tlsErrorPending NOTIFY tlsErrorChanged FINAL)
    Q_PROPERTY(QString tlsErrorHost READ tlsErrorHost NOTIFY tlsErrorChanged FINAL)
    Q_PROPERTY(QString tlsErrorMessage READ tlsErrorMessage NOTIFY tlsErrorChanged FINAL)

    // A page asks for the location or the microphone: QML shows a banner and answers with resolvePermission().
    // The answer holds for that site and kind until the browser closes (until the tab closes, in private mode).
    // permissionType is "geolocation" or "microphone".
    Q_PROPERTY(bool permissionPending READ permissionPending NOTIFY permissionChanged FINAL)
    Q_PROPERTY(QString permissionHost READ permissionHost NOTIFY permissionChanged FINAL)
    Q_PROPERTY(QString permissionType READ permissionType NOTIFY permissionChanged FINAL)

    // "Save password?" prompt (Phase 3 capture). Raised when a login is
    // submitted whose credentials are new or changed vs the store. QML shows a
    // banner and calls resolveSaveLogin(bool). saveLoginIsUpdate distinguishes
    // "Update password" (known username, changed password) from a fresh save.
    Q_PROPERTY(bool saveLoginPending READ saveLoginPending NOTIFY saveLoginChanged FINAL)
    Q_PROPERTY(QString saveLoginHost READ saveLoginHost NOTIFY saveLoginChanged FINAL)
    Q_PROPERTY(QString saveLoginUsername READ saveLoginUsername NOTIFY saveLoginChanged FINAL)
    Q_PROPERTY(bool saveLoginIsUpdate READ saveLoginIsUpdate NOTIFY saveLoginChanged FINAL)

    // Find-in-page
    Q_PROPERTY(bool findInPageHasResult READ findInPageHasResult NOTIFY findInPageHasResultChanged FINAL)

    // The colour for the parts of the screen around the page that the page is kept out of (the strip beside a
    // camera notch): the page's theme colour if it names one, otherwise what it paints behind its content.
    Q_PROPERTY(QColor surroundColor READ surroundColor NOTIFY surroundColorChanged FINAL)

    // A page's alert(), confirm() or prompt(): the page waits until resolveJsDialog() is called.
    // jsDialogKind is one of alert|confirm|prompt.
    Q_PROPERTY(bool jsDialogActive READ jsDialogActive NOTIFY jsDialogChanged FINAL)
    Q_PROPERTY(QString jsDialogKind READ jsDialogKind NOTIFY jsDialogChanged FINAL)
    Q_PROPERTY(QString jsDialogMessage READ jsDialogMessage NOTIFY jsDialogChanged FINAL)
    Q_PROPERTY(QString jsDialogDefaultText READ jsDialogDefaultText NOTIFY jsDialogChanged FINAL)

    // HTML <select> dropdown state — property bindings for reliable QML observation
    Q_PROPERTY(bool selectMenuActive READ selectMenuActive NOTIFY selectMenuActiveChanged FINAL)
    Q_PROPERTY(QStringList selectMenuOptions READ selectMenuOptions NOTIFY selectMenuOptionsChanged FINAL)
    Q_PROPERTY(int selectMenuSelectedIndex READ selectMenuSelectedIndex NOTIFY selectMenuSelectedIndexChanged FINAL)
    // Image long-press state, observed through QML bindings like selectMenu* above.
    Q_PROPERTY(QString imageLongPressUrl READ imageLongPressUrl NOTIFY imageLongPressUrlChanged FINAL)
    // What the last long press landed on: pageUrl, linkUrl, srcUrl,
    // selectionText, editable, mediaType. Feeds browser.contextMenus.
    Q_PROPERTY(QVariantMap contextInfo READ contextInfo NOTIFY contextInfoChanged FINAL)
    Q_PROPERTY(bool fileChooserActive READ fileChooserActive NOTIFY fileChooserActiveChanged FINAL)
    Q_PROPERTY(QStringList fileChooserNameFilters READ fileChooserNameFilters NOTIFY fileChooserNameFiltersChanged FINAL)
    Q_PROPERTY(bool fileChooserSelectMultiple READ fileChooserSelectMultiple NOTIFY fileChooserSelectMultipleChanged FINAL)

    // HTML5 date/time/color input pickers — same JS-bridge + NOTIFY-property
    // pattern as selectMenu* above: QML drives the Silica DatePicker/TimePicker/ColorPicker dialogs.
    // inputPickerType is one of date|month|week|time|datetime-local|color;
    // value/min/max/step are the raw HTML string values.
    Q_PROPERTY(bool inputPickerActive READ inputPickerActive NOTIFY inputPickerActiveChanged FINAL)
    Q_PROPERTY(QString inputPickerType READ inputPickerType NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerValue READ inputPickerValue NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerMin READ inputPickerMin NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerMax READ inputPickerMax NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerStep READ inputPickerStep NOTIFY inputPickerChanged FINAL)

    Q_PROPERTY(bool crashed READ crashed NOTIFY crashedChanged FINAL)

    // Pinch zoom visual scale, as the engine reports it
    Q_PROPERTY(qreal visualScale READ visualScale NOTIFY visualScaleChanged)
    Q_PROPERTY(qreal pinchCenterX READ pinchCenterX NOTIFY pinchCenterChanged)
    Q_PROPERTY(qreal pinchCenterY READ pinchCenterY NOTIFY pinchCenterChanged)

    Q_PROPERTY(bool adBlockEnabled READ adBlockEnabled WRITE setAdBlockEnabled NOTIFY adBlockEnabledChanged FINAL)

    Q_PROPERTY(bool cookieBannerBlockingEnabled READ cookieBannerBlockingEnabled WRITE setCookieBannerBlockingEnabled NOTIFY cookieBannerBlockingEnabledChanged FINAL)

public:
    explicit WebPage(QQuickItem *parent = nullptr);
    ~WebPage() override;

    int tabId() const;
    void setTabId(int tabId);

    bool painted() const;
    bool domContentLoaded() const;

    bool chrome() const;
    void setChrome(bool chrome);

    bool forcedChrome() const;
    void setForcedChrome(bool forcedChrome);

    bool fullscreen() const;
    bool mediaAudioActive() const { return m_mediaAudioActive; }
    bool mediaVideoActive() const { return m_mediaVideoActive; }
    bool atYBeginning() const;
    bool atYEnd() const;

    QString favicon() const;
    void setFavicon(const QString &favicon);
    QStringList faviconCandidates() const;
    void setFaviconCandidates(const QStringList &candidates);
    // A page advertised an OpenSearch description; offer it in settings.

    qreal fullscreenHeight() const;
    void setFullscreenHeight(qreal height);

    qreal toolbarHeight() const;
    void setToolbarHeight(qreal height);

    bool active() const;
    void setActive(bool active);

    // App-level foreground state (window maximized + application active),
    // pushed down by WebContainer. Combined with the per-tab active flag
    // to decide whether the engine should treat the page as visible.
    void setAppForeground(bool foreground);

    // All live WebPage instances. Pages get only a visual parent
    // (setParentItem), never a QObject parent, so findChildren() on the view
    // cannot discover them — the render-recovery code must use this instead.
    static const QList<WebPage *> &liveInstances();

    // Apply the ad-block toggle process-wide: flips the UI-process cosmetic
    // gate and notifies every live page's WebProcess extension. Driven by the
    // dconf binding in BrowserPage.qml (via WebContainer::setAdBlockEnabled),
    // so it works with any number of tabs (or none) open.
    static void applyAdBlockEnabledGlobally(bool enabled);
    static void applyAdBlockAllowlistGlobally(const QString &json);
    static void applyCookieBannerBlockingGlobally(bool enabled);
    // Per-site user agents (JSON object host → profile id, the site_ua_overrides key) and the hosts JavaScript is
    // off for (JSON array, the javascript_blocklist key). The engine applies both as tabs navigate.
    static void applySiteUaOverridesGlobally(const QString &json);
    static void applyJavaScriptBlocklistGlobally(const QString &json);


    bool chromeGestureEnabled() const;
    void setChromeGestureEnabled(bool enabled);

    qreal chromeGestureThreshold() const;
    void setChromeGestureThreshold(qreal threshold);

    bool fixedToolbar() const;
    void setFixedToolbar(bool fixed);

    bool loaded() const;
    bool moving() const;


    bool dragging() const;
    bool desktopMode() const;
    void setDesktopMode(bool desktop);
    // Pick and apply the UA for a destination URL (handles per-site quirks
    // such as Google Maps); called from the navigation policy handler.
    // True when the URL gets a per-site UA quirk (Maps, Cloudflare hosts).

    // Popunder guard: after a popup is routed into this view, same-tab
    // scripted redirects to an unrelated site are blocked for a short window
    // (the reverse-popunder puts the real content in the popup and JS-redirects
    // the original tab to the ad).
    // Leaves the fullscreen the page asked for, from the interface's close button.
    Q_INVOKABLE void exitFullscreen();

    Q_INVOKABLE void loadTab(const QString &url, bool force = false);
    // Re-request the URL already showing (setUrl() dedupes such an assignment away).
    void loadCurrentUrlAgain();
    // Warm the connection for a navigation the UI expects (link under the
    // finger, URL-bar completion). Fetches nothing; rate-limited per origin;
    // inert unless PACIFIC_PRECONNECT is set.
    Q_INVOKABLE void grabToFile(const QSize &size);
    Q_INVOKABLE void grabThumbnail(const QSize &size);

    // History preview ("instant Back"). During a back/forward navigation the
    // engine keeps presenting the OUTGOING page's last frame until the incoming
    // page paints, while the chrome switches to the destination URL immediately
    // — so the viewport shows one page and the URL bar names another. Measured
    // on edition.cnn.com (2026-08-19): back_forward navigation, DCL 14 988 ms,
    // load 23 774 ms, 188 resources re-run, because bfcache capacity is 0 under
    // PACIFIC_CACHE_MODEL=viewer. These overrides capture the page being left
    // and hand the UI the destination entry's last-seen pixels to cover the gap.
    //
    // Deliberately NOT virtual in the base: WebContainer calls through a
    // WebPage*, so name hiding is enough and the engine plugin stays untouched.
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    // Cached preview file for a history entry, or empty when none was stored.

    // Screenshot cache for "instant Back": nothing else deletes these files.
    Q_INVOKABLE void forceChrome(bool forced);
    Q_INVOKABLE void suspendView();
    Q_INVOKABLE void resumeView();
    // Scroll the focused editable back into view after the layout viewport
    // shrank under it (virtual keyboard opening). No-op when nothing editable
    // is focused. See WebContainer::setContentBottomInset().
    Q_INVOKABLE void revealFocusedEditable();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void moveSelectionStart(qreal cssX, qreal cssY);
    Q_INVOKABLE void moveSelectionEnd(qreal cssX, qreal cssY);
    Q_INVOKABLE void copyToClipboard();
    Q_INVOKABLE void handleJsSelectionClear();
    Q_INVOKABLE void handleJsSelectionUpdate(const QString &text, qreal startX, qreal startY, qreal endX, qreal endY);

    // Editable focus reported from cross-origin subframes (editableFocus bridge)

    // Find-in-page
    Q_INVOKABLE void findText(const QString &text, bool backwards = false);
    Q_INVOKABLE void findFinish();
    bool findInPageHasResult() const { return m_findInPageHasResult; }
    void setFindInPageHasResult(bool has);

    // Fills the saved login for the site the tab is at into the login form the user just touched (the vault
    // has to be open). The site is taken from the tab's URL, never from the page, and the fill script checks
    // the page is still at it. Never submits.
    void fillLoginForFocusedForm();

    // Security
    QObject* security() const { return m_security; }

    // TLS certificate failure
    bool tlsErrorPending() const { return m_tlsErrorPending; }
    QString tlsErrorHost() const { return m_tlsErrorHost; }
    QString tlsErrorMessage() const { return m_tlsErrorMessage; }
    Q_INVOKABLE void acceptTlsCertificate();

    // Permission prompt (geolocation / camera / microphone)
    bool permissionPending() const { return m_permissionPending; }
    QString permissionHost() const { return m_permissionHost; }
    QString permissionType() const { return m_permissionType; }
    Q_INVOKABLE void resolvePermission(bool allow);
    // Forgets the answers given so far (Settings, "clear site permissions").
    static void clearPermissionDecisions();

    QColor surroundColor() const { return m_themeColor.isValid() ? m_themeColor : m_backgroundColor; }

    // Whether the newest frame covers the whole view. It does not right after the view grew, until the engine
    // has laid the page out for the new size and painted it.
    Q_INVOKABLE bool paintedFullHeight() const { return paintedSize().height() >= static_cast<int>(height()) - 1; }

    bool jsDialogActive() const { return m_jsDialogKind >= 0; }
    QString jsDialogKind() const;
    QString jsDialogMessage() const { return m_jsDialogMessage; }
    QString jsDialogDefaultText() const { return m_jsDialogDefaultText; }
    Q_INVOKABLE void resolveJsDialog(bool accepted, const QString &text = QString());

    // For the whole engine: whether pages should prefer their dark colours.
    static void applyDarkModeGlobally(bool dark);
    // For the whole engine: forget cached files and/or cookies and what sites stored.
    static void clearEngineDataGlobally(bool cache, bool siteData);

    // Offering to save a login the page sent (the page agent's "login" message): the banner shows while
    // saveLoginPending, and resolveSaveLogin() stores it, opening the vault first if need be.
    bool saveLoginPending() const { return m_saveLoginPending; }
    QString saveLoginHost() const { return m_saveLoginHost; }
    QString saveLoginUsername() const { return m_saveLoginUsername; }
    bool saveLoginIsUpdate() const { return m_saveLoginExistingUid >= 0; }
    Q_INVOKABLE void resolveSaveLogin(bool save);

    // Save page as PDF

    // HTML <select> dropdown
    Q_INVOKABLE void selectMenuOption(int index);
    Q_INVOKABLE void downloadUrl(const QString &url);
    Q_INVOKABLE void closeSelectMenu();
    Q_INVOKABLE void chooseFiles(const QStringList &filePaths);
    Q_INVOKABLE void cancelFileChooser();
    void openSelectMenu(const QStringList &options, int selectedIndex);
    bool selectMenuActive() const { return m_selectMenuActive; }

    // Date/time/color input picker: QML calls resolveInputPicker(value) with the
    // HTML-formatted value string on accept, or cancelInputPicker() on dismiss.
    Q_INVOKABLE void resolveInputPicker(const QString &value);
    Q_INVOKABLE void cancelInputPicker();
    void openInputPicker(const QString &type, const QString &value,
                         const QString &min, const QString &max, const QString &step);
    bool inputPickerActive() const { return m_inputPickerActive; }
    QString inputPickerType() const { return m_inputPickerType; }
    QString inputPickerValue() const { return m_inputPickerValue; }
    QString inputPickerMin() const { return m_inputPickerMin; }
    QString inputPickerMax() const { return m_inputPickerMax; }
    QString inputPickerStep() const { return m_inputPickerStep; }

    // Image long-press
    void openImageLongPress(const QString &imageUrl);
    Q_INVOKABLE void clearImageLongPress();
    QString imageLongPressUrl() const { return m_imageLongPressUrl; }
    QVariantMap contextInfo() const { return m_contextInfo; }
    void setContextInfo(const QVariantMap &info);
    Q_INVOKABLE void clearContextInfo();

    QStringList selectMenuOptions() const { return m_selectMenuOptions; }
    int selectMenuSelectedIndex() const { return m_selectMenuSelectedIdx; }
    bool fileChooserActive() const { return m_fileChooserActive; }
    QStringList fileChooserNameFilters() const { return m_fileChooserNameFilters; }
    bool fileChooserSelectMultiple() const { return m_fileChooserSelectMultiple; }

    // Pinch zoom visual scale
    qreal visualScale() const { return m_visualScale; }
    qreal pinchCenterX() const { return m_pinchCenterX; }
    qreal pinchCenterY() const { return m_pinchCenterY; }

    bool crashed() const { return m_crashed; }

    bool adBlockEnabled() const;
    void setAdBlockEnabled(bool enabled);

    bool cookieBannerBlockingEnabled() const;
    void setCookieBannerBlockingEnabled(bool enabled);

    bool textSelectionActive() const;
    QObject* textSelectionController();
    QString selectedText() const;
    bool isPhoneNumber() const;
    qreal selectionStartX() const { return m_selectionStartX; }
    qreal selectionStartY() const { return m_selectionStartY; }
    qreal selectionEndX() const { return m_selectionEndX; }
    qreal selectionEndY() const { return m_selectionEndY; }

signals:
    void tabIdChanged();
    void paintedChanged();
    void domContentLoadedChanged();
    void chromeChanged();
    void forcedChromeChanged();
    void fullscreenChanged();
    void mediaAudioActiveChanged();
    void mediaVideoActiveChanged();
    void atYBeginningChanged();
    void atYEndChanged();
    void faviconChanged();
    void fullscreenHeightChanged();
    void toolbarHeightChanged();
    void activeChanged();
    void chromeGestureEnabledChanged();
    void chromeGestureThresholdChanged();
    void fixedToolbarChanged();
    void loadedChanged();
    void movingChanged();
    void draggingChanged();
    void desktopModeChanged();
    // Fired on every new touch sequence (TouchBegin); forwarded by
    // WebContainer::touched() for the active page so QML can track
    // user activity (fullscreen close button, toolbar auto-hide).
    void touched();

    void textSelectionActiveChanged();
    void selectionTextChanged();
    void selectionHandlesUpdated();

    void securityChanged();
    void tlsErrorChanged();
    void permissionChanged();
    void jsDialogChanged();
    void surroundColorChanged();
    void saveLoginChanged();
    // The user wants a login saved and the vault is locked or not created yet: show the page that opens it.
    void saveLoginNeedsUnlock();
    void loginSaved();
    void findInPageHasResultChanged();

    void recvAsyncMessage(const QString &message, const QVariant &data);
    void fileGrabWritten(const QString &fileName);
    void thumbnailResult(const QString &data);
    // Emitted at the start of a history navigation when the destination entry
    // has a stored preview. The UI shows it until the live page takes over.
    void historyPreviewReady(const QString &imagePath);
    void afterRendering();

    void selectMenuActiveChanged();
    void selectMenuOptionsChanged();
    void selectMenuSelectedIndexChanged();
    void imageLongPressUrlChanged();
    void contextInfoChanged();
    void fileChooserActiveChanged();
    void fileChooserNameFiltersChanged();
    void fileChooserSelectMultipleChanged();
    void inputPickerActiveChanged();
    void inputPickerChanged();

    void visualScaleChanged();
    void pinchCenterChanged();
    void crashedChanged();
    void adBlockEnabledChanged();
    void cookieBannerBlockingEnabledChanged();

public:
    // Last committed main-frame URL; source URL for redirect-hop ad blocking
    // (written/read by the load-changed C callbacks).
    QUrl m_lastCommittedUrl;


protected:
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void touchEvent(QTouchEvent *event) override;

    // From the engine view below.
    void longPressed(const QPointF &position) override;
    void tapped(const QPointF &position) override;
    void filePickerRequested(bool multiple) override;
    void colorPickerRequested(const QString &color) override;
    void pageColorChanged(int kind, const QString &color) override;
    void findResult(int current, int total) override;
    void selectMenuRequested(const QByteArray &optionsJson) override;
    void dialogRequested(int kind, const QByteArray &json) override;

private slots:
    void onLoadingChanged(EngineViewLoadRequest *loadRequest);
    void onFrameArrived();
    void onPageWantsTextInputChanged();
    void captureThumbnail();

private:
    void updateEngineVisibility();

    bool m_permissionPending = false;
    int m_tabId = 0;
    bool m_painted = false;
    bool m_domContentLoaded = false;
    bool m_chrome = true;
    bool m_forcedChrome = false;
    bool m_fullscreen = false;
    bool m_playsAudio = false;
    bool m_keepsScreenOn = false;
    void pageFullscreenChanged(bool fullscreen) override;
    void mediaStateChanged(bool playsAudio, bool keepsScreenOn) override;
    void updateMediaState();
    bool m_mediaAudioActive = false;
    bool m_mediaVideoActive = false;
    qreal m_mediaVolume = 1.0;
    bool m_atYBeginning = true;
    bool m_atYEnd = false;
    QString m_favicon;
    QStringList m_faviconCandidates;
    qreal m_fullscreenHeight = 0.0;
    qreal m_toolbarHeight = 0.0;
    bool m_active = false;
    bool m_appForeground = true;
    bool m_chromeGestureEnabled = true;
    qreal m_chromeGestureThreshold = 10.0;
    bool m_fixedToolbar = false;
    bool m_loaded = false;
    bool m_desktopMode = false;
    qint64 m_popupRoutedAtMs = 0;
    QString m_popupRoutedUrl;
    qreal m_lastScrollY = 0.0;
    bool m_textSelectionActive = false;
    QString m_selectedText;
    qreal m_selectionStartX = 0.0;
    qreal m_selectionStartY = 0.0;
    qreal m_selectionEndX = 0.0;
    qreal m_selectionEndY = 0.0;
    bool m_findInPageHasResult = false;
    bool m_findInitialized = false;
    SecurityInfo *m_security = nullptr;
    bool m_pinchZoomActive = false;
    qint64 m_pinchPendingSinceMs = 0;
    bool m_pagePinchConsumerHint = false;
    bool m_pagePinchDeclined = false;
    qreal m_pinchStartDistance = 0.0;
    double m_pinchStartZoomLevel = 1.0;
    double m_defaultZoomLevel = 1.0;
    bool m_defaultZoomLevelInitialized = false;
    qreal m_visualScale = 1.0;
    qreal m_pinchStartVisualScale = 1.0;
    double m_pinchStartPageScale = 1.0;
    double m_pendingPageScale = 1.0;
    QPointF m_pendingPageScaleCenter;
    qreal m_pinchCenterX = 0.0;
    qreal m_pinchCenterY = 0.0;
    QTimer m_framePump;
    QTimer m_thumbnailTimer;
    QSize m_thumbnailSize;
    QSharedPointer<QQuickItemGrabResult> m_thumbnailGrab;
    QElapsedTimer m_thumbnailGrabStarted;
    quint32 m_thumbnailSerial = 0;
    quint32 m_thumbnailSerialAtCheck = 0;
    int m_thumbnailBusyChecks = 0;
    bool m_thumbnailCaptured = false;
    QTimer m_mediaInactiveDebounceTimer;
    QTimer m_deferredFullscreenLeaveTimer;
    QTimer m_chromeGestureDebounceTimer;
    bool m_pendingChrome = true;
    bool m_chromeGestureArmed = false;
    qreal m_uiGestureLastY = 0.0;
    qreal m_uiGestureAccumDown = 0.0;
    bool m_uiGestureTracking = false;
    bool m_pendingFullscreenEntry = false;
    QTimer m_pendingFullscreenEntryGuard;
    QElapsedTimer m_lastNativeFullscreenEnter;
    QElapsedTimer m_perfFrameLogWindow;
    int m_perfFramesInWindow = 0;
    qreal m_lastInteractionX = -1.0;
    qreal m_lastInteractionY = -1.0;
    QPointF m_touchStartPos;
    bool m_touchDragged = false;
    QHash<int, QTouchEvent::TouchPoint> m_trackedTouchPoints;
    QString m_lastSoftKeyboardText;
    QString m_lastPreeditText;
    QString m_lastEditableFocusToken;
    qint64 m_lastSoftKeyboardTextTimeMs = 0;
    qint64 m_lastSoftBackspaceTimeMs = 0;
    qint64 m_lastSoftEnterTimeMs = 0;
    void pageMessage(const QString &kind, const QJsonValue &data) override;
    void closeInputPicker();

    // The picker that is open belongs to the engine (a colour) rather than to our own script (dates and times).
    bool m_inputPickerIsEngineColor = false;
    void moveSelectionEdge(bool start, qreal cssX, qreal cssY);

    // The engine's ids of the options in m_selectMenuOptions.
    QList<int> m_selectMenuIds;
    QString m_findText;
    QColor m_themeColor;
    QColor m_backgroundColor = QColor(Qt::white);
    int m_jsDialogKind = -1;
    QString m_jsDialogMessage;
    QString m_jsDialogDefaultText;
    bool m_selectMenuActive = false;
    QStringList m_selectMenuOptions;
    int m_selectMenuSelectedIdx = 0;
    QString m_imageLongPressUrl;
    QVariantMap m_contextInfo;
    bool m_fileChooserActive = false;
    QStringList m_fileChooserNameFilters;
    bool m_fileChooserSelectMultiple = false;
    bool m_inputPickerActive = false;
    QString m_inputPickerType;
    QString m_inputPickerValue;
    QString m_inputPickerMin;
    QString m_inputPickerMax;
    QString m_inputPickerStep;
    bool m_crashed = false;
    bool m_tlsErrorPending = false;
    QString m_tlsErrorFailingUri;
    QString m_tlsErrorHost;
    QString m_tlsErrorMessage;
    void setTlsError(bool failed);
    void updateSecurity();
    QString m_permissionHost;
    QString m_permissionType;
    // The engine's questions not answered yet, the one showing first, as (question id, origin|kind).
    QList<QPair<int, QString>> m_permissionQuestions;
    QHash<QString, bool> m_permissionDecisions;
    void permissionRequested(int question, const QString &name, const QString &origin) override;
    void answerPermissions(const QString &key, bool allow);
    void showNextPermissionQuestion();
    bool m_saveLoginPending = false;
    QString m_saveLoginHost;
    QString m_saveLoginUsername;
    QString m_saveLoginPassword;
    int m_saveLoginExistingUid = -1;
    QString m_saveLoginOrigin;
    QVariantMap m_saveLoginFields;
    bool m_saveLoginWaitsForVault = false;
    // A login the page sent, waiting to be offered (when the page goes on, or after m_offerLoginTimer).
    QVariantMap m_capturedLogin;
    QString m_capturedLoginOrigin;
    QTimer m_offerLoginTimer;
    qint64 m_lastTouchMs = 0;
    static QString origin(const QUrl &url);
    void offerCapturedLogin();
    void storePendingLogin();
    bool m_autoRecovered = false;
};
