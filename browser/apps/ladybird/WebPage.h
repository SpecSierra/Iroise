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

// What the security panel shows about the page's connection and certificate.
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

    Q_PROPERTY(int tabId READ tabId WRITE setTabId NOTIFY tabIdChanged)
    // The tab whose page opened this one, which closing this one goes back to (0 for none).
    Q_PROPERTY(int parentId READ parentId CONSTANT)
    Q_PROPERTY(bool painted READ painted NOTIFY paintedChanged)
    Q_PROPERTY(bool domContentLoaded READ domContentLoaded NOTIFY domContentLoadedChanged)
    Q_PROPERTY(bool chrome READ chrome WRITE setChrome NOTIFY chromeChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(bool mediaAudioActive READ mediaAudioActive NOTIFY mediaAudioActiveChanged FINAL)
    Q_PROPERTY(bool mediaVideoActive READ mediaVideoActive NOTIFY mediaVideoActiveChanged FINAL)
    Q_PROPERTY(QString favicon READ favicon WRITE setFavicon NOTIFY faviconChanged)
    Q_PROPERTY(qreal toolbarHeight READ toolbarHeight WRITE setToolbarHeight NOTIFY toolbarHeightChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool fixedToolbar READ fixedToolbar WRITE setFixedToolbar NOTIFY fixedToolbarChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(bool desktopMode READ desktopMode WRITE setDesktopMode NOTIFY desktopModeChanged)
    Q_PROPERTY(bool textSelectionActive READ textSelectionActive NOTIFY textSelectionActiveChanged FINAL)
    Q_PROPERTY(QString text READ selectedText NOTIFY selectionTextChanged FINAL)
    Q_PROPERTY(qreal selectionStartX READ selectionStartX NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionStartY READ selectionStartY NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionEndX READ selectionEndX NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionEndY READ selectionEndY NOTIFY selectionHandlesUpdated FINAL)

    // Security / TLS certificate info
    Q_PROPERTY(QObject* security READ security CONSTANT FINAL)

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

    // HTML5 date/time/color input pickers — same JS-bridge + NOTIFY-property
    // pattern as selectMenu* above: QML drives the Silica DatePicker/TimePicker/ColorPicker dialogs.
    // inputPickerType is one of date|month|week|time|datetime-local|color;
    // inputPickerValue is the raw HTML string value.
    Q_PROPERTY(bool inputPickerActive READ inputPickerActive NOTIFY inputPickerActiveChanged FINAL)
    Q_PROPERTY(QString inputPickerType READ inputPickerType NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerValue READ inputPickerValue NOTIFY inputPickerChanged FINAL)

    Q_PROPERTY(bool crashed READ crashed NOTIFY crashedChanged FINAL)

public:
    explicit WebPage(QQuickItem *parent = nullptr);
    ~WebPage() override;

    int tabId() const;
    void setTabId(int tabId);
    int parentId() const { return m_parentId; }
    void setParentId(int parentId) { m_parentId = parentId; }

    bool painted() const;
    bool domContentLoaded() const;

    bool chrome() const;
    void setChrome(bool chrome);

    bool fullscreen() const;
    bool mediaAudioActive() const { return m_mediaAudioActive; }
    bool mediaVideoActive() const { return m_mediaVideoActive; }

    QString favicon() const;
    void setFavicon(const QString &favicon);

    qreal toolbarHeight() const;
    void setToolbarHeight(qreal height);

    bool active() const;
    void setActive(bool active);

    // App-level foreground state (window maximized + application active),
    // pushed down by WebContainer. Combined with the per-tab active flag
    // to decide whether the engine should treat the page as visible.
    void setAppForeground(bool foreground);

    // Ad blocking for the whole engine, on or off: the engine blocks requests and hides elements for every page.
    // Driven by the setting in BrowserPage.qml (via WebContainer::setAdBlockEnabled), so it holds with any
    // number of tabs (or none) open.
    static void applyAdBlockEnabledGlobally(bool enabled);
    static bool adBlockEnabledGlobally();
    static void applyAdBlockAllowlistGlobally(const QString &json);
    static void applyCookieBannerBlockingGlobally(bool enabled);
    // Per-site user agents (JSON object host → profile id, the site_ua_overrides key) and the hosts JavaScript is
    // off for (JSON array, the javascript_blocklist key). The engine applies both as tabs navigate.
    static void applySiteUaOverridesGlobally(const QString &json);
    static void applyJavaScriptBlocklistGlobally(const QString &json);

    bool fixedToolbar() const;
    void setFixedToolbar(bool fixed);

    bool loaded() const;
    bool desktopMode() const;
    void setDesktopMode(bool desktop);

    // Leaves the fullscreen the page asked for, from the interface's close button.
    Q_INVOKABLE void exitFullscreen();
    // Pauses the media of every tab: a call has come in.
    Q_INVOKABLE void pauseAllMedia();

    // Re-request the URL already showing (setUrl() dedupes such an assignment away).
    void loadCurrentUrlAgain();
    Q_INVOKABLE void grabThumbnail(const QSize &size);

    // Keeps the toolbar showing (true) whatever the page's scrolling does, or lets scrolling hide it again.
    Q_INVOKABLE void forceChrome(bool forced);
    Q_INVOKABLE void suspendView();
    Q_INVOKABLE void resumeView();
    // Scroll the focused editable back into view after the layout viewport
    // shrank under it (virtual keyboard opening). No-op when nothing editable
    // is focused. See WebContainer::setContentBottomInset().
    void revealFocusedEditable();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void selectAll();
    // Move one end of the selection to the text at a point, in the interface's pixels.
    Q_INVOKABLE void moveSelectionStart(qreal x, qreal y);
    Q_INVOKABLE void moveSelectionEnd(qreal x, qreal y);
    Q_INVOKABLE void copyToClipboard();
    Q_INVOKABLE void handleJsSelectionClear();
    Q_INVOKABLE void handleJsSelectionUpdate(const QString &text, qreal startX, qreal startY, qreal endX, qreal endY);

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
    // The last private tab has closed: what was trusted from private tabs is forgotten, and the engine's private
    // session let go of.
    static void clearPrivateBrowsingData();

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
    void openInputPicker(const QString &type, const QString &value);
    bool inputPickerActive() const { return m_inputPickerActive; }
    QString inputPickerType() const { return m_inputPickerType; }
    QString inputPickerValue() const { return m_inputPickerValue; }

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

    bool crashed() const { return m_crashed; }

    bool textSelectionActive() const;
    QString selectedText() const;
    qreal selectionStartX() const { return m_selectionStartX; }
    qreal selectionStartY() const { return m_selectionStartY; }
    qreal selectionEndX() const { return m_selectionEndX; }
    qreal selectionEndY() const { return m_selectionEndY; }

signals:
    void tabIdChanged();
    void paintedChanged();
    void domContentLoadedChanged();
    void chromeChanged();
    void fullscreenChanged();
    void mediaAudioActiveChanged();
    void mediaVideoActiveChanged();
    void faviconChanged();
    void toolbarHeightChanged();
    void activeChanged();
    void fixedToolbarChanged();
    void loadedChanged();
    void desktopModeChanged();
    // Fired on every new touch sequence (TouchBegin); forwarded by
    // WebContainer::touched() for the active page so QML can track
    // user activity (fullscreen close button, toolbar auto-hide).
    void touched();

    void textSelectionActiveChanged();
    void selectionTextChanged();
    void selectionHandlesUpdated();

    void tlsErrorChanged();
    void permissionChanged();
    void jsDialogChanged();
    void surroundColorChanged();
    void saveLoginChanged();
    // The user wants a login saved and the vault is locked or not created yet: show the page that opens it.
    void saveLoginNeedsUnlock();
    void findInPageHasResultChanged();

    // A new thumbnail of the page: a file, or for a private tab a data URL kept in memory only.
    void thumbnailCaptured(const QString &source);

    void selectMenuActiveChanged();
    void selectMenuOptionsChanged();
    void selectMenuSelectedIndexChanged();
    void imageLongPressUrlChanged();
    void contextInfoChanged();
    void fileChooserActiveChanged();
    void inputPickerActiveChanged();
    void inputPickerChanged();
    void crashedChanged();

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
    void installPageAgent();
    void confirmTlsError(bool claimed);
    void recheckCertificateException();
    static void forgetCertificateExceptions(bool privateOnly);
    void noteUserInput();
    static int savedLoginId(const QString &site, const QString &username, const QString &password);
    bool userActedRecently() const;
    void checkThumbnail();
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
    bool m_playsVideo = false;
    void pageFullscreenChanged(bool fullscreen) override;
    void faviconReceived(const QString &dataUrl) override { setFavicon(dataUrl); }
    void mediaStateChanged(bool playsAudio, bool keepsScreenOn) override;
    void updateMediaState();
    bool m_mediaAudioActive = false;
    bool m_mediaVideoActive = false;
    QString m_favicon;
    qreal m_toolbarHeight = 0.0;
    bool m_active = false;
    bool m_appForeground = true;
    bool m_fixedToolbar = false;
    bool m_loaded = false;
    bool m_desktopMode = false;
    qreal m_lastScrollY = 0.0;
    bool m_textSelectionActive = false;
    QString m_selectedText;
    qreal m_selectionStartX = 0.0;
    qreal m_selectionStartY = 0.0;
    qreal m_selectionEndX = 0.0;
    qreal m_selectionEndY = 0.0;
    bool m_findInPageHasResult = false;
    SecurityInfo *m_security = nullptr;
    QTimer m_thumbnailTimer;
    QTimer m_saveLoginExpiryTimer;
    QSize m_thumbnailSize;
    QSharedPointer<QQuickItemGrabResult> m_thumbnailGrab;
    quint32 m_thumbnailSerial = 0;
    quint32 m_thumbnailSerialAtCheck = 0;
    int m_thumbnailBusyChecks = 0;
    bool m_thumbnailCaptured = false;
    // The page agent answered from the page now showing.
    bool m_pageAgentInstalled = false;
    QTimer m_chromeGestureDebounceTimer;
    bool m_pendingChrome = true;
    bool m_chromeGestureArmed = false;
    void pageMessage(const QString &kind, const QJsonValue &data) override;
    void closeInputPicker();

    // The picker that is open belongs to the engine (a colour) rather than to our own script (dates and times).
    bool m_inputPickerIsEngineColor = false;
    void moveSelectionEdge(bool start, qreal x, qreal y);

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
    bool m_inputPickerActive = false;
    QString m_inputPickerType;
    QString m_inputPickerValue;
    bool m_crashed = false;
    bool m_tlsErrorPending = false;
    QString m_tlsErrorFailingUri;
    QString m_tlsErrorHost;
    QString m_tlsErrorMessage;
    QByteArray m_tlsErrorFingerprint;
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
    qint64 m_lastUserInputMs = 0;
    int m_parentId = 0;
    static QString origin(const QUrl &url);
    void offerCapturedLogin();
    void storePendingLogin();
};
