/****************************************************************************
**
** Copyright (c) 2014 - 2021 Jolla Ltd.
** Copyright (c) 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import QtQuick.Window 2.2 as QuickWindow
import Sailfish.Silica 1.0
import harbour.pacific 1.0
import Sailfish.WebView.Pickers 1.0 as Pickers
import Sailfish.WebView.Popups 1.0 as Popups
import Sailfish.WebView.Controls 1.0
import Nemo.Configuration 1.0
import "." as Browser

WebContainer {
    id: webView

    property bool activePortalMode
    readonly property bool moving: contentItem && contentItem.moving
    property bool portrait: true
    property bool contentFullscreen: contentItem && contentItem.fullscreen
    // needChrome is provided as FINAL by WebContainer C++ (cannot override)
    property real fullscreenHeight
    property bool imOpened
    property real toolbarHeight
    uiPixelRatio: Theme.pixelRatio
    property string favicon: contentItem ? contentItem.favicon : ""
    property bool findInPageHasResult: contentItem ? contentItem.findInPageHasResult : false
    property bool canShowSelectionMarkers: true

    property var resourceController: ResourceController {
        webPage: contentItem
        background: !webView.visible
    }

    onContentItemChanged: {
        resourceController._htmlAudioActive = contentItem ? contentItem.mediaAudioActive : false
        resourceController._htmlVideoActive = contentItem ? contentItem.mediaVideoActive : false
        resourceController.calculateStatus()
    }

    property Component _selectionUIComponent: Component {
        TextSelectionController {
            opacity: canShowSelectionMarkers ? 1.0 : 0.0
            contentWidth: webView.rotationHandler ? webView.rotationHandler.width : 0
            contentHeight: Math.max(0, webView.fullscreenHeight - webView.toolbarHeight)
            // Push below the overlay
            z: -1
            anchors {
                fill: parent
                bottomMargin: webView.toolbarHeight
            }

            Behavior on opacity { FadeAnimator {} }

            onStartHandleMaskChanged: browserPage.inputRegion.selectionStartHandleMask = startHandleMask
            onEndHandleMaskChanged: browserPage.inputRegion.selectionEndHandleMask = endHandleMask
        }
    }


    property QtObject fixedToolbarConfig: ConfigurationValue {
        key: "/apps/harbour-pacific/settings/fixed_toolbar"
        defaultValue: false

        onValueChanged: {
            // Re-show the toolbar if it was hidden when the setting got enabled.
            if (value && webView.contentItem && !webView.contentItem.chrome) {
                webView.contentItem.chrome = true
            }
        }
    }

    // Pages are created in C++ (WebContainer::getOrCreatePage), not from
    // webPageComponent, so bindings inside that component never apply. Push
    // the fixed-toolbar setting onto whichever page is active.
    Binding {
        target: webView.contentItem
        property: "fixedToolbar"
        value: fixedToolbarConfig.value
        when: webView.contentItem !== null
    }

    Connections {
        target: contentItem
        ignoreUnknownSignals: true

        onMediaAudioActiveChanged: {
            resourceController._htmlAudioActive = contentItem ? contentItem.mediaAudioActive : false
            resourceController.calculateStatus()
        }
        onMediaVideoActiveChanged: {
            resourceController._htmlVideoActive = contentItem ? contentItem.mediaVideoActive : false
            resourceController.calculateStatus()
        }
    }

    function stop() {
        if (contentItem) {
            contentItem.stop()
        }
    }

    function clearSelection() {
        if (contentItem) {
            contentItem.clearSelection()
        }
    }

    function thumbnailCaptureSize() {
        if (webView.activePortalMode) {
            console.log("Thumbnail size tried accessed in captive portal mode")
            return Qt.size(0, 0)
        }

        var ratio = Math.min(
                    browserPage.width / browserPage.thumbnailSize.width,
                    browserPage.height / browserPage.thumbnailSize.height)
        var width = browserPage.thumbnailSize.width * ratio
        var height = browserPage.thumbnailSize.height * ratio

        return Qt.size(width, height)
    }

    function grabActivePage() {
        if (webView.activePortalMode) {
            console.warn("Refusing page grab in active portal mode")
            return
        }

        if (webView.contentItem && webView.activeTabRendered) {
            webView.contentItem.grabThumbnail(thumbnailCaptureSize())
        }
    }

    function handleKeyPress(key) {
        if (key == Qt.Key_F5) {
            reload()
        }
    }

    // foreground is owned by WebContainer C++ (tracks the real window
    // visibility); the old binding here evaluated false at startup and never
    // re-fired (fakeVisibility has no notify), which kept pages hidden.
    readyToPaint: resourceController.videoActive ? webView.visible && !resourceController.displayOff
                                                 : webView.visible && webView.contentItem
                                                   && (webView.contentItem.domContentLoaded
                                                       || webView.contentItem.painted)

    touchBlocked: contentItem && contentItem.popupOpener && contentItem.popupOpener.active || false

    onKeyPressed: handleKeyPress(key)

    // Refresh the active tab's thumbnail when the browser leaves the
    // foreground, so the cover shows the page as it was left (scroll position
    // included) rather than as it looked when it finished loading. The page
    // keeps its own thumbnail current while it is on screen (WebPage, "Tab
    // thumbnails"), so this only catches what was drawn in the last seconds,
    // and only if the window is still being rendered.
    onForegroundChanged: {
        if (!foreground) {
            grabActivePage()
        }
    }

    onBackButtonPressed: webView.goBack()

    onForwardButtonPressed: webView.goForward()

    // Selection drag handles (at container level so contentItem is available)
    // The selection's handles, in the interface's pixels (the page reports the selection in them already).
    Item {
        id: selHandles
        visible: webView.contentItem && webView.contentItem.textSelectionActive
        anchors.fill: parent
        z: 100

        property var ci: webView.contentItem

        Item {
            id: selectionCursor
            visible: selHandles.visible && selHandles.ci && selHandles.ci.textSelectionActive
            x: selHandles.ci ? selHandles.ci.selectionEndX : 0
            y: selHandles.ci ? selHandles.ci.selectionEndY - (Theme.iconSizeMedium / 2) : 0
            width: Theme.paddingSmall
            height: Theme.iconSizeMedium
            z: 101

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 2
                height: parent.height
                radius: 1
                color: Theme.highlightColor
            }
        }

        Image {
            id: startHandle
            // Native Silica selection handle: teardrop with its tip at the
            // top-right, so the right edge / top sits on the selection start
            // (x below subtracts width) and the body hangs down-left, clear of
            // the selected text. Colorized to the ambience highlight colour.
            width: Theme.iconSizeMedium
            height: Theme.iconSizeMedium
            sourceSize.width: Theme.iconSizeMedium
            sourceSize.height: Theme.iconSizeMedium
            source: "image://theme/icon-m-textselection-start?" + Theme.highlightColor
            visible: selHandles.visible && selHandles.ci && selHandles.ci.selectionStartX >= 0
            x: selHandles.ci ? selHandles.ci.selectionStartX - width : 0
            y: selHandles.ci ? selHandles.ci.selectionStartY : 0

            MouseArea {
                anchors.fill: parent
                anchors.margins: -Theme.paddingLarge
                drag.target: startHandle
                drag.axis: Drag.XAndYAxis
                onPositionChanged: {
                    if (drag.active && selHandles.ci)
                        selHandles.ci.moveSelectionStart(startHandle.x + startHandle.width, startHandle.y + startHandle.height / 2)
                }
                // NB: Dragging set x and y, which ended their bindings; the handle follows the selection again.
                onReleased: {
                    startHandle.x = Qt.binding(function() { return selHandles.ci ? selHandles.ci.selectionStartX - startHandle.width : 0 })
                    startHandle.y = Qt.binding(function() { return selHandles.ci ? selHandles.ci.selectionStartY : 0 })
                }
            }
        }

        Image {
            id: endHandle
            // Native Silica selection handle: teardrop with its tip at the
            // top-left, so the left edge / top sits on the selection end and the
            // body hangs down-right, clear of the selected text. Colorized to
            // the ambience highlight colour.
            width: Theme.iconSizeMedium
            height: Theme.iconSizeMedium
            sourceSize.width: Theme.iconSizeMedium
            sourceSize.height: Theme.iconSizeMedium
            source: "image://theme/icon-m-textselection-end?" + Theme.highlightColor
            visible: selHandles.visible && selHandles.ci && selHandles.ci.selectionEndX >= 0
            x: selHandles.ci ? selHandles.ci.selectionEndX : 0
            y: selHandles.ci ? selHandles.ci.selectionEndY : 0

            MouseArea {
                anchors.fill: parent
                anchors.margins: -Theme.paddingLarge
                drag.target: endHandle
                drag.axis: Drag.XAndYAxis
                onPositionChanged: {
                    if (drag.active && selHandles.ci)
                        selHandles.ci.moveSelectionEnd(endHandle.x, endHandle.y + endHandle.height / 2)
                }
                onReleased: {
                    endHandle.x = Qt.binding(function() { return selHandles.ci ? selHandles.ci.selectionEndX : 0 })
                    endHandle.y = Qt.binding(function() { return selHandles.ci ? selHandles.ci.selectionEndY : 0 })
                }
            }
        }
    }
}
