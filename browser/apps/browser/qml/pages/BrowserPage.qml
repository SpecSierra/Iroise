/****************************************************************************
**
** Copyright (c) 2013 - 2021 Jolla Ltd.
** Copyright (c) 2019 - 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import QtQuick 2.2
import QtQuick.Window 2.2 as QuickWindow
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import harbour.iroise 1.0
import Nemo.Configuration 1.0
import Nemo.Notifications 1.0 as Nemo
import "components" as Browser
import "../shared" as Shared

Page {
    id: browserPage

    readonly property bool active: status == PageStatus.Active
    property bool tabPageActive
    // Capture at portrait aspect ratio to match the 2-column SFOS-style tab cards.
    readonly property real _thumbCaptureWidth: width - Theme.horizontalPageMargin * 2
    readonly property size thumbnailSize: Qt.size(
        _thumbCaptureWidth,
        _thumbCaptureWidth * 1.5 - (Theme.iconSizeSmall + Theme.paddingMedium * 2))
    property string pendingOpenUrl: ""
    // overlay.active drops and returns every time a page is pushed over the
    // browser, so this handler fires far more often than "the browser just
    // opened". Only the first pass is startup.
    property bool _startupHandled: false
    property bool _filePickerOpen: false
    // True while the toolbar is hidden by the inactivity timer (as opposed to
    // page-driven fullscreen / scroll-hide) — the next touch brings it back.
    property bool _chromeAutoHidden

    property alias overlay: overlay
    property alias tabs: webView.tabModel
    property alias history: historyModel
    property alias url: webView.url
    property alias title: webView.title
    property alias webView: webView
    property alias inputRegion: inputRegion

    ConfigurationValue {
        id: homePageConf
        key: "/apps/harbour-iroise/settings/home_page"
        defaultValue: "https://jolla.com/"
        // Settings live in dconf and only QML reads them (ConfigurationValue); each one is pushed into C++.
        onValueChanged: WebUtils.setHomePage(value)
        Component.onCompleted: WebUtils.setHomePage(value)
    }

    ConfigurationValue {
        id: privateBrowsingAutostart
        key: "/apps/harbour-iroise/settings/browser_privatebrowsing_autostart"
        defaultValue: false
        // "Start browser in private browsing mode". Startup only, deliberately no onValueChanged: flipping the switch
        // mid-session must not yank the current session into private mode.
        Component.onCompleted: if (value) webView.privateMode = true
    }

    ConfigurationValue {
        id: closeAllTabsOnExit
        key: "/apps/harbour-iroise/settings/close_all_tabs"
        defaultValue: false
        // Acted on when the app quits (SettingManager).
        onValueChanged: Settings.setCloseAllTabsOnExit(value)
        Component.onCompleted: Settings.setCloseAllTabsOnExit(value)
    }

    ConfigurationValue {
        id: adBlockEngine
        key: "/apps/harbour-iroise/settings/adblock_enabled"
        defaultValue: true
        // webView.setAdBlockEnabled applies to the whole engine: every tab,
        // open or still to come, and it holds with no tab open.
        // Component.onCompleted restores it on startup, onValueChanged covers
        // the settings-page switch.
        onValueChanged: webView.setAdBlockEnabled(value)
        Component.onCompleted: webView.setAdBlockEnabled(value)
    }

    ConfigurationValue {
        id: adBlockAllowlist
        key: "/apps/harbour-iroise/settings/adblock_allowlist"
        defaultValue: "[]"
        // JSON array of hosts on which the ad blocker is disabled ("Block ads
        // on this site" toggle in the popup menu). Same shape as adblock_enabled
        // above: dconf is the single source of truth, applied process-wide.
        onValueChanged: webView.setAdBlockAllowlist(value)
        Component.onCompleted: webView.setAdBlockAllowlist(value)

        // Is host (or a parent domain of it) on the allowlist?
        function isAllowed(host) {
            if (!host) return false
            var list = JSON.parse(value)
            for (var i = 0; i < list.length; i++) {
                if (host === list[i] || host.indexOf("." + list[i], host.length - list[i].length - 1) >= 0)
                    return true
            }
            return false
        }

        function setAllowed(host, allowed) {
            if (!host) return
            var list = JSON.parse(value).filter(function (h) { return h !== host })
            if (allowed) list.push(host)
            value = JSON.stringify(list)
        }
    }

    ConfigurationValue {
        id: javascriptBlocklist
        key: "/apps/harbour-iroise/settings/javascript_blocklist"
        defaultValue: "[]"
        // JSON array of hosts on which JavaScript is disabled ("Enable
        // JavaScript on this site" toggle in the popup menu). Default is JS-on
        // everywhere, so this is a blocklist. Same shape as adBlockAllowlist
        // above: dconf is the single source of truth, applied process-wide.
        onValueChanged: webView.setJavaScriptBlocklist(value)
        Component.onCompleted: webView.setJavaScriptBlocklist(value)

        // Is host (or a parent domain of it) on the blocklist?
        function isBlocked(host) {
            if (!host) return false
            var list = JSON.parse(value)
            for (var i = 0; i < list.length; i++) {
                if (host === list[i] || host.indexOf("." + list[i], host.length - list[i].length - 1) >= 0)
                    return true
            }
            return false
        }

        function setBlocked(host, blocked) {
            if (!host) return
            var list = JSON.parse(value).filter(function (h) { return h !== host })
            if (blocked) list.push(host)
            value = JSON.stringify(list)
        }
    }

    ConfigurationValue {
        id: cookieBannerBlocking
        key: "/apps/harbour-iroise/settings/cookie_banner_blocking"
        defaultValue: true
        // Same shape as adblock_enabled above: dconf is the single source of
        // truth, applied process-wide (autoconsent user script on every tab).
        onValueChanged: webView.setCookieBannerBlockingEnabled(value)
        Component.onCompleted: webView.setCookieBannerBlockingEnabled(value)
    }

    ConfigurationValue {
        id: globalPrivacyControl
        key: "/apps/harbour-iroise/settings/global_privacy_control"
        defaultValue: true
        onValueChanged: webView.setGlobalPrivacyControlEnabled(value)
        Component.onCompleted: webView.setGlobalPrivacyControlEnabled(value)
    }

    ConfigurationValue {
        id: siteUaOverrides
        key: "/apps/harbour-iroise/settings/site_ua_overrides"
        defaultValue: "{}"
        // Same shape as adblock_enabled above: dconf (JSON object of
        // host → UA profile id, edited by SiteUaSettingsPage) is the single
        // source of truth, applied process-wide.
        onValueChanged: webView.setSiteUaOverrides(value)
        Component.onCompleted: webView.setSiteUaOverrides(value)
    }


    function load(url, title) {
        overlay.dismiss(true)
        webView.load(url, title)
    }

    function bringToForeground(window) {
        if ((webView.visibility < QuickWindow.Window.Maximized) && window) {
            window.raise()
        }
    }

    function activateNewTabView() {
        pageStack.pop(browserPage, PageStackAction.Immediate)
        overlay.enterNewTabUrl(PageStackAction.Immediate)
        bringToForeground(webView.chromeWindow)
        // after bringToForeground, webView has focus => activate chrome
        window.activate()
    }

    function openFilePicker() {
        if (_filePickerOpen || !webView.contentItem || !webView.contentItem.fileChooserActive) {
            return
        }
        _filePickerOpen = true
        pageStack.animatorPush(Qt.resolvedUrl("components/FilePickerPage.qml"),
                               { "webView": webView, "browserPage": browserPage })
    }

    // for time being make this fullscreen. TODO: avoid drawing over cutout and corner areas.
    cutoutMode: CutoutMode.FullScreen
    background: null
    onStatusChanged: {
        if (overlay.enteringNewTabUrl || webView.tabModel.count === 0) {
            return
        }

        if (status == PageStatus.Inactive && overlay.visible) {
            overlay.animator.hide()
            overlay.toolBar.certOverlayActive = false
        }
    }

    property int pageOrientation: pageStack.currentPage._windowOrientation
    onPageOrientationChanged: {
        // When on other pages update immediately.
        if (!active) {
            webView.applyContentOrientation(pageOrientation)
        }
    }

    orientationTransitions: orientationFader.orientationTransition

    Keys.onPressed: {
        webView.handleKeyPress(event.key)
    }

    Shared.OrientationFader {
        id: orientationFader

        visible: webView.contentItem
        page: browserPage
        fadeTarget: overlay.animator.allowContentUse ? overlay : overlay.dragArea
        color: webView.contentItem ? (webView.resourceController.videoActive
                                      && webView.contentItem.fullscreen
                                      ? "black"
                                      : webView.contentItem.surroundColor)
                                   : "white"

        onApplyContentOrientation: webView.applyContentOrientation(browserPage.orientation)
    }

    HistoryModel {
        id: historyModel
    }

    Shared.VirtualKeyboardObserver {
        id: virtualKeyboardObserver

        active: webView.browserEnabled
        transpose: window._transpose
        orientation: browserPage.orientation

        onWindowChanged: webView.chromeWindow = window

        // Height of the keyboard to reserve in the page's layout viewport, so
        // an input at the bottom of the page lays out ABOVE the keyboard
        // instead of behind it. Consumed by webView._desiredContentBottomInset.
        //
        // Gated on `opened` (imSize > 0 && panelSize == imSize) — the settled
        // state, after the show animation — so the viewport relayouts exactly
        // once per show/hide instead of on every animation frame. Same
        // discipline as the toolbar inset's "chromeVisible" gating.
        readonly property real contentInset:
            (opened && webView.browserEnabled) ? imSize : 0
    }

    QtObject {
        id: maxliveTabs
        property int value: 3
    }

    // A download the user chose to save goes to the Downloads folder (see DownloadBanner and the Downloads page);
    // these say when one starts and when it is done.
    Nemo.Notification {
        id: downloadNotification
        appName: "Iroise"
        appIcon: "icon-m-downloads"
    }
    Connections {
        target: Downloads
        onStarted: {
            downloadNotification.isTransient = true
            //% "Downloading %1"
            downloadNotification.previewSummary = qsTrId("sailfish_browser-no-downloading").arg(fileName)
            downloadNotification.previewBody = ""
            downloadNotification.summary = ""
            downloadNotification.publish()
        }
        onFinished: {
            downloadNotification.isTransient = false
            //% "Download complete"
            downloadNotification.summary = completed ? qsTrId("iroise-no-download_complete")
                                                     //% "Download failed"
                                                     : qsTrId("iroise-no-download_failed")
            downloadNotification.body = fileName
            downloadNotification.previewSummary = downloadNotification.summary
            downloadNotification.previewBody = fileName
            downloadNotification.publish()
        }
    }

    // Keep the web content out of the band of the screen the camera notch is
    // in, which NotchBand blacks out. The notch sits at the top of the
    // physical portrait screen, so after Silica rotates the page it is on a
    // different page edge per orientation.
    // WebContainer sizes itself and its pages from its parent item, so
    // insetting this frame is all it takes. The chrome (Overlay) stays at the
    // page edges, so fullscreenHeight is still the page height.
    readonly property real _notchInset: Screen.hasCutouts ? Screen.topCutout.height : 0

    Shared.NotchBand {}

    // Around the page's content, in its colour. Only above the toolbar and only with a page, so that the
    // compositor's wallpaper shows behind the start page and the toolbar.
    Rectangle {
        width: parent.width
        height: Math.ceil(overlay.y)
        visible: !startPage.visible
        color: webView.contentItem ? webView.contentItem.surroundColor : "black"
    }

    Item {
        id: webViewFrame

        readonly property real _top: browserPage.orientation === Orientation.Portrait ? browserPage._notchInset : 0
        readonly property real _bottom: browserPage.orientation === Orientation.PortraitInverted ? browserPage._notchInset : 0
        readonly property real _left: browserPage.orientation === Orientation.Landscape ? browserPage._notchInset : 0
        readonly property real _right: browserPage.orientation === Orientation.LandscapeInverted ? browserPage._notchInset : 0

        x: _left
        y: _top
        width: browserPage.width - _left - _right
        height: browserPage.height - _top - _bottom
        // With no tab there is still an empty page (WebContainer makes one to be ready), which would show through
        // the start page's backdrop, the wallpaper lipstick draws behind the window.
        opacity: startPage.visible ? 0 : 1

        Shared.WebView {
            id: webView

            browserEnabled: overlay.animator.allowContentUse
            fullscreenHeight: browserPage.height
            maxLiveTabCount: maxliveTabs.value

            // Bottom URL-bar viewport inset: reserve the toolbar strip in the page's
            // layout viewport while the chrome is settled-visible, so position:fixed bottom
            // content (hover buttons, cookie bars, chat bubbles) lays out above the
            // URL bar instead of behind it. Gated on the *settled* "chromeVisible"
            // state so the viewport relayouts exactly once per show/hide — the drag
            // and fling states are distinct, so scrolling does not reflow per frame.
            // Fullscreen video keeps the full-height viewport. The value is applied
            // by the handler here plus the page's Component.onCompleted
            // (startup); C++ clamps and no-ops unchanged values.
            //
            // The virtual keyboard reserves its own strip on the same path, always
            // on (no setting): an input hidden behind the keyboard is a defect, not
            // a preference. The two insets are combined with max(), not summed —
            // the keyboard is drawn over the toolbar strip, so reserving both would
            // double-count the overlap and leave a dead band above the keyboard.
            readonly property real _desiredContentBottomInset:
                Math.max(
                    (overlay.animator.state === "chromeVisible"
                     // The toolbar is about to go: the page gets its strip now and
                     // paints it while the toolbar still covers it.
                     && !overlay.animator.hidePending
                     && !contentFullscreen)
                    ? overlay.toolBar.rowHeight : 0,
                    contentFullscreen ? 0 : virtualKeyboardObserver.contentInset)
            on_DesiredContentBottomInsetChanged: setContentBottomInset(_desiredContentBottomInset)
            historyModel: historyModel

            // Show overlay immediately at top if needed.
            onTabModelChanged: handleModelChanges(true)

            // When a page starts loading, dismiss the overlay so the user can see/interact with content.
            onLoadingChanged: {
                if (loading && !overlay.animator.allowContentUse) {
                    overlay.dismiss(true)
                }
            }
            onChromeExposed: {
                if (overlay.animator.atTop && overlay.searchField.focus && !WebUtils.firstUseDone) {
                    webView.chromeWindow.raise()
                }
            }

            onForegroundChanged: {
                if (foreground && webView.chromeWindow) {
                    webView.chromeWindow.raise()
                }
            }

            onTouched: {
                if (contentFullscreen) {
                    fullscreenCloseVisibleTimer.restart()
                }
                if (browserPage._chromeAutoHidden && !contentFullscreen) {
                    browserPage._chromeAutoHidden = false
                    overlay.animator.showChrome()
                } else if (overlay.animator.atBottom) {
                    chromeAutoHideTimer.restart()
                }
            }

            onNeedChromeChanged: {
                // Only a page-driven "show chrome" clears the auto-hidden flag:
                // hiding via showFullscreen() echoes back here as needChrome=false
                // (the animator writes contentItem.chrome), which must not clear it.
                if (needChrome) {
                    browserPage._chromeAutoHidden = false
                    overlay.animator.showChrome()
                } else {
                    overlay.animator.showFullscreen()
                }
            }

            onWebContentOrientationChanged: orientationFader.waitForWebContentOrientationChanged = false

            function applyContentOrientation(orientation) {
                orientationFader.waitForWebContentOrientationChanged = (contentItem && contentItem.active)

                switch (orientation) {
                case Orientation.None:
                case Orientation.Portrait:
                    updateContentOrientation(Qt.PortraitOrientation)
                    break
                case Orientation.Landscape:
                    updateContentOrientation(Qt.LandscapeOrientation)
                    break
                case Orientation.PortraitInverted:
                    updateContentOrientation(Qt.InvertedPortraitOrientation)
                    break
                case Orientation.LandscapeInverted:
                    updateContentOrientation(Qt.InvertedLandscapeOrientation)
                    break
                }
            }

            // Both model change and model count change are connected to this.
            function handleModelChanges(openOverlayImmediately) {
                if (webView.completed && (!webView.tabModel || webView.tabModel.count === 0)) {
                    overlay.startPage(openOverlayImmediately ? PageStackAction.Immediate
                                                             : PageStackAction.Animated)
                }
            }
        }
    }

    // Hide the toolbar after 15 s without any touch on the page; the next
    // touch (webView.touched) brings it back. Only fires from the plain
    // chrome-visible state — never over the URL entry, popup menu, cert
    // panel, start page, video fullscreen or an open keyboard — and is
    // disabled entirely when the fixed-toolbar setting is on.
    Timer {
        id: chromeAutoHideTimer

        interval: 15000
        onTriggered: {
            if (overlay.animator.state === "chromeVisible"
                    && browserPage.active
                    && !browserPage.tabPageActive
                    && webView.tabModel.count > 0
                    && !webView.contentFullscreen
                    && !virtualKeyboardObserver.opened
                    && !webView.fixedToolbarConfig.value) {
                browserPage._chromeAutoHidden = true
                overlay.animator.showFullscreen()
            }
        }
    }

    IconButton {
        id: fullscreenClose

        opacity: fullscreenCloseVisibleTimer.running || pressed ? 1.0 : 0.0
        Behavior on opacity { FadeAnimation {} }
        visible: opacity > 0
        x: Theme.paddingLarge
        y: Theme.paddingLarge
        icon.source: "image://theme/icon-m-close"
        onClicked: {
            webView.contentItem.exitFullscreen()
        }

        Timer {
            id: fullscreenCloseVisibleTimer

            interval: 2000
            running: webView.contentFullscreen
        }
    }

    // Use Connections so that target updates when model changes.
    Connections {
        target: webView && webView.tabModel || null
        ignoreUnknownSignals: true
        // Animate overlay to top if needed.
        onCountChanged: {
            if (webView.tabModel.count === 0) {
                webView.handleModelChanges(false)
            }
            window.setBrowserCover(webView.tabModel)
        }
    }

    InputRegion {
        id: inputRegion

        window: webView.chromeWindow
        orientation: browserPage.orientation // Qt and Silica orientations match
        // Chrome and web content share one QWindow. Always expose the full screen so
        // Wayland delivers touch events for the web content area to this window.
        overlayMask: Qt.rect(0, 0, browserPage.width, browserPage.height)
        closeButtonMask: fullscreenClose.visible ? Qt.rect(fullscreenClose.x, fullscreenClose.y,
                                                           fullscreenClose.width, fullscreenClose.height)
                                                 : Qt.rect(0, 0, 0, 0)
    }

    Browser.StartPage {
        id: startPage

        width: browserPage.width
        // Fill the whole page so the wallpaper backdrop sits behind the toolbar strip
        // too: when the popup menu hides the toolbar, that strip must show wallpaper,
        // not the empty white web view. Foreground layout uses contentHeight (the
        // visible area above the toolbar) so the clock/search/grid don't shift.
        height: browserPage.height
        contentHeight: Math.ceil(overlay.y)
        clip: true

        visible: !!webView.tabModel && webView.tabModel.count === 0 && !overlay.toolBar.findInPageActive
        // Stay opaque (no fade) so the empty white web view never flashes through
        // during the transition; only interactive while the overlay is collapsed.
        enabled: visible && overlay.animator.atBottom

        bookmarkModel: overlay.bookmarkModel
        historyModel: historyModel
        overlayOpen: !overlay.animator.atBottom

        onLoadUrl: overlay.loadPage(url, newTab)

        // Swipe up on the start page. Immediate, so the address bar does not
        // slide in behind the gesture that asked for it.
        onOpenSearch: overlay.enterNewTabUrl(PageStackAction.Immediate)

        // The "+" tile files a link rather than navigating to one.
        onAddFavorite: {
            overlay.animator.showChrome()
            pageStack.animatorPush(Qt.resolvedUrl("components/AddFavoriteDialog.qml"),
                                   {
                                       bookmarkModel: overlay.bookmarkModel,
                                       folderId: startPageFolder.value
                                   })
        }

        // Which folder the quick links show. Set in the bookmarks page; "" is
        // the root, which is where everything lives until folders are used, so
        // an unconfigured profile behaves exactly as before.
        startFolderId: startPageFolder.value

        // The start page is pinned to one folder, so opening a folder tile
        // hands over to the bookmarks page rather than navigating in place.
        onOpenFolder: {
            overlay.animator.showChrome()
            pageStack.push("BookmarkPage.qml", {
                               bookmarkModel: overlay.bookmarkModel,
                               folderId: folderId,
                               folderTitle: title
                           })
        }
    }

    ConfigurationValue {
        id: startPageFolder

        key: "/apps/harbour-iroise/settings/start_page_folder"
        defaultValue: ""
    }

    // Dismiss the open overlay by tapping the area above it. The contentDimmer's
    // own MouseArea can't do this anymore: the dimmer is transparent (and thus
    // not visible / not hit-testable) while the overlay is at top.
    MouseArea {
        id: overlayDismissArea

        property bool inEmptyPrivateMode: webView.privateMode && webView.privateTabModel.count === 0
                                          && webView.persistentTabModel.count > 0

        width: browserPage.width
        height: Math.ceil(overlay.y)
        enabled: overlay.animator.atTop
        onClicked: {
            if (inEmptyPrivateMode) {
                webView.privateMode = false
                //% "Leaving private mode"
                Notices.show(qsTrId("sailfish_browser-la-leaving_private_mode"), Notice.Short, Notice.Top)
            }
            overlay.dismiss(true)
        }
    }

    Browser.DimmerEffect {
        id: contentDimmer

        width: browserPage.width
        height: Math.ceil(overlay.y)

        // No dim at all: the shader has blending off, so any mid-transition
        // opacity overwrites the page with dark pixels (black flash while the
        // overlay animates). Keep the item for its MouseArea/private texture.
        dimmerOpacity: 0.0
        // The private-mode texture below is a child, so the item must stay
        // visible in private mode; blending on makes the zero-opacity shader
        // paint nothing instead of opaque black.
        blending: true
        visible: webView.privateMode && !overlay.animator.allowContentUse

        MouseArea {
            property bool inEmptyPrivateMode: webView.privateMode && webView.privateTabModel.count === 0
                                              && webView.persistentTabModel.count > 0

            anchors.fill: parent
            // Allow dismiss with no tabs too: the start page is behind the overlay.
            enabled: overlay.animator.atTop
            onClicked: {
                if (inEmptyPrivateMode) {
                    webView.privateMode = false
                    //% "Leaving private mode"
                    Notices.show(qsTrId("sailfish_browser-la-leaving_private_mode"), Notice.Short, Notice.Top)
                }
                overlay.dismiss(true)
            }
        }

        Browser.PrivateModeTexture {
            id: privateModeTexture

            anchors.fill: contentDimmer
            visible: webView.privateMode && !overlay.animator.allowContentUse
        }
    }

    Label {
        x: (contentDimmer.width - implicitWidth) / 2
        // Allow only half of the width
        width: parent.width / 2
        truncationMode: TruncationMode.Fade
        opacity: privateModeTexture.visible ? 1.0 : 0.0
        anchors {
            bottom: contentDimmer.bottom
            bottomMargin: (overlay.toolBar.rowHeight - height) / 2
        }

        //: Label for private browsing above address bar
        //% "Private browsing"
        text: qsTrId("sailfish_browser-la-private_mode")
        color: Theme.highlightColor
        font.pixelSize: Theme.fontSizeLarge

        Behavior on opacity { FadeAnimation {} }
    }

    Browser.Overlay {
        id: overlay

        active: browserPage.status == PageStatus.Active && webView.tabModel.loaded
        webView: webView
        historyModel: historyModel
        browserPage: browserPage

        animator.onAtBottomChanged: {
            if (!animator.atBottom) {
                webView.clearSelection()
                chromeAutoHideTimer.stop()
            } else {
                browserPage._chromeAutoHidden = false
                chromeAutoHideTimer.restart()
            }
        }

        onActiveChanged: {
            var isFullScreen = webView.contentItem && webView.contentItem.fullscreen
            if (!isFullScreen && active && !overlay.enteringNewTabUrl) {
                if (webView.hasInitialUrl || webView.tabModel.count !== 0) {
                    overlay.animator.showChrome()
                } else if (_startupHandled) {
                    // Coming BACK to an empty browser -- from the bookmarks
                    // page, a dialog, settings -- not starting one. Both
                    // branches below are startup behaviour: startPage() opens
                    // the URL entry ready to type, and the home page branch
                    // navigates. Re-running either on every return meant
                    // closing a folder popped the keyboard up, or worse,
                    // silently loaded the home page over what you were doing.
                    overlay.animator.showChrome()
                } else if (WebUtils.homePage !== "about:blank" && WebUtils.homePage.length > 0) {
                    // Nothing to restore and a home page is set, which is the
                    // case AddHomePageDialog describes ("shown when the browser
                    // is opened with no tabs to load"). This used to fall into
                    // the showChrome() branch above and load nothing at all, so
                    // a configured home page did nothing on startup and the
                    // start page was suppressed as well -- an empty view under
                    // bare chrome. loadPage() normalises, so a bare host typed
                    // into the dialog ("google.fr") still resolves, and it
                    // shows the chrome itself once the load starts.
                    //
                    // Safe here because overlay.active gates on
                    // tabModel.loaded, so the restored tab count is final by
                    // the time this runs.
                    overlay.loadPage(WebUtils.homePage)
                } else {
                    overlay.startPage()
                }
                _startupHandled = true
            }

            if (!active) {
                webView.clearSelection()
                if (webView.chromeWindow && webView.foreground) {
                    webView.chromeWindow.raise()
                }
            }
        }
    }

    Browser.PopUpMenu {
        id: popupMenu

        width: parent.width
        height: parent.height
        // Match the start page's wallpaper crop (it scales to overlay.y, not full
        // screen) so the menu's frosted glass lines up with the backdrop behind it.
        // Crop the glass wallpaper to the full menu height: the menu reaches the
        // bottom edge (the overlay/toolbar is hidden while it is open), so a crop
        // at overlay.y would leave the bottom slice of the glass unsampled.
        wallpaperHeight: height

        active: overlay.toolBar.secondaryToolsActive
        menuItem: Component {
            Browser.PopUpMenuItem {
                iconWidth: Theme.iconSizeMedium + Theme.paddingLarge
            }
        }

        footer: Component {
            Browser.PopUpMenuFooter {
            }
        }

        onClosed: overlay.dismiss(true)
    }

    // CoverAction itself has no enabled/visible property, so the two action
    // sets are two lists and the tab count picks between them.
    readonly property bool _coverActionsEnabled: browserPage.status === PageStatus.Active
                                                 || browserPage.tabPageActive
                                                 || !webView.tabModel
                                                 || webView.tabModel.count === 0
    readonly property bool _hasTabs: webView.tabModel && webView.tabModel.count > 0

    CoverActionList {
        enabled: browserPage._coverActionsEnabled && !browserPage._hasTabs
        iconBackground: true
        window: webView.chromeWindow

        CoverAction {
            iconSource: "image://theme/icon-cover-new"
            onTriggered: activateNewTabView()
        }
    }

    CoverActionList {
        enabled: browserPage._coverActionsEnabled && browserPage._hasTabs
        iconBackground: true
        window: webView.chromeWindow

        CoverAction {
            iconSource: "image://theme/icon-cover-new"
            onTriggered: activateNewTabView()
        }

        CoverAction {
            iconSource: "image://theme/icon-cover-cancel"
            onTriggered: {
                if (webView.tabModel && webView.tabModel.count > 0) {
                    webView.tabModel.closeActiveTab()
                }
            }
        }
    }

    Connections {
        target: WebUtils
        onOpenUrlRequested: {
            // Url is empty when user tapped icon when browser was already open.
            // In case first use not done show the overlay immediately.
            if (url == "") {
                bringToForeground(webView.chromeWindow)
                if (!WebUtils.firstUseDone) {
                    overlay.enterNewTabUrl(PageStackAction.Immediate)
                }

                window.activate()
                return
            }

            if (browserPage.status !== PageStatus.Active) {
                pageStack.pop(browserPage, PageStackAction.Immediate)
            }

            webView.grabActivePage()
            if (webView.tabModel.activateTab(url)) {
            } else if (!webView.tabModel.loaded) {
                pendingOpenUrl = url
                overlay.dismiss(true, !Qt.application.active /* immediate */)
            } else {
                webView.clearSelection()
                webView.tabModel.newTab(url, true)
                overlay.dismiss(true, !Qt.application.active /* immediate */)
            }
            bringToForeground(webView.chromeWindow)
            window.activate()
        }
        onActivateNewTabViewRequested: activateNewTabView()
        onOpenSettingsRequested: {
            pageStack.pop(browserPage, PageStackAction.Immediate)
            pageStack.push(Qt.resolvedUrl("SettingsPage.qml"), {}, PageStackAction.Immediate)
            bringToForeground(webView.chromeWindow)
            window.activate()
        }
        onFirstUseDoneChanged: window.setBrowserCover(webView.tabModel)
    }

    Connections {
        target: webView.tabModel
        onLoadedChanged: {
            if (!webView.tabModel || !webView.tabModel.loaded || pendingOpenUrl === "") {
                return
            }

            var url = pendingOpenUrl
            pendingOpenUrl = ""

            if (webView.tabModel.activateTab(url)) {
            } else {
                webView.clearSelection()
                webView.tabModel.newTab(url, true)
            }
        }
    }

    Connections {
        target: webView.contentItem
        ignoreUnknownSignals: true
        onFileChooserActiveChanged: {
            if (!webView.contentItem) {
                _filePickerOpen = false
                return
            }

            if (webView.contentItem.fileChooserActive) {
                openFilePicker()
            } else {
                _filePickerOpen = false
                if (pageStack.currentPage && pageStack.currentPage.objectName === "iroiseFilePickerPage") {
                    pageStack.pop(pageStack.currentPage, PageStackAction.Immediate)
                }
            }
        }
    }

    // Tab crash banner — shown when the active tab's page (or the engine) crashes.
    Browser.CrashBanner { webView: webView }

    // TLS certificate failure banner — accept-and-retry for self-signed sites.
    Browser.TlsErrorBanner { webView: webView }

    // Permission prompt: geolocation / camera / microphone (allow/deny,
    // remembered per host+type for the session).
    Browser.PermissionBanner { webView: webView }

    // "Save password?" prompt after a login form is submitted.
    Browser.SaveLoginBanner { webView: webView }

    // "Save this file?" for a download a page started.
    Browser.DownloadBanner { webView: webView }

    Browser.SelectMenuOverlay { webView: webView }

    // alert(), confirm() and prompt() from the page.
    Browser.JsDialogBanner { webView: webView }

    // Native Silica pickers for <input type=date|month|week|time|datetime-local|color>.
    Browser.InputPickerOverlay { webView: webView }

    Browser.ImageActionPanel { webView: webView }


    Component.onCompleted: {
        chromeAutoHideTimer.restart()
        window.setBrowserCover(webView.tabModel)
        // The inset's change handler (webView) has nothing to react to yet.
        webView.setContentBottomInset(webView._desiredContentBottomInset)
    }
}
