/****************************************************************************
**
** Copyright (c) 2014 - 2019 Jolla Ltd.
** Copyright (c) 2019 - 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.1
import Sailfish.Silica 1.0
import harbour.pacific 1.0
import "../../shared" as Shared
SilicaControl {
    id: tabView

    property bool portrait
    property bool privateMode
    property var tabModel
    property alias scaledPortraitHeight: tabsToolBar.scaledPortraitHeight
    property alias scaledLandscapeHeight: tabsToolBar.scaledLandscapeHeight
    property int horizontalMargin: Theme.horizontalPageMargin

    signal hide
    signal enterNewTabUrl
    signal activateTab(int index)
    signal closeTab(int index)
    signal closeAll
    signal closeAllCanceled
    signal closeAllPending

    property var _remorsePopup
    property bool _closingAllTabs

    property bool _emptyPrivateIcon: (privateMode && _closingAllTabs) || webView.privateTabModel.count === 0

    anchors.fill: parent

    // Leaves one set of tabs for the other; a pending "close all" of the one left happens now.
    function switchMode(toPrivate) {
        if (toPrivate === privateMode)
            return
        if (_remorsePopup)
            _remorsePopup.trigger()
        privateMode = toPrivate
    }

    Item {
        id: tabs

        anchors {
            fill: parent
            bottomMargin: tabsToolBar.height
        }

        // The two sets of tabs, each a button with its count.
        Item {
            id: header

            // Below the camera notch, at the top in portrait; the background shows beside the notch.
            y: Screen.hasCutouts && pageStack.currentOrientation === Orientation.Portrait ? Screen.topCutout.height : 0
            width: parent.width
            height: Theme.itemSizeMedium

            Shared.Background {
                anchors.fill: parent
                z: -100
            }

            Row {
                anchors.fill: parent

                Repeater {
                    model: 2

                    BackgroundItem {
                        id: modeButton

                        readonly property bool privateTabs: index === 1
                        readonly property bool isCurrentTab: privateTabs === tabView.privateMode

                        width: header.width / 2
                        height: header.height
                        highlighted: down
                        onClicked: tabView.switchMode(privateTabs)

                        Icon {
                            id: modeIcon

                            anchors.centerIn: parent
                            highlighted: modeButton.highlighted || modeButton.isCurrentTab
                            source: {
                                if (!modeButton.privateTabs) {
                                    return "image://theme/icon-m-tabs"
                                } else if (tabView._emptyPrivateIcon) {
                                    return "image://theme/icon-m-incognito"
                                }
                                return "image://theme/icon-m-incognito-selected"
                            }
                        }

                        Label {
                            anchors.centerIn: modeIcon

                            font.pixelSize: Theme.fontSizeExtraSmall
                            font.bold: true
                            color: {
                                if (modeButton.privateTabs) {
                                    return tabView.palette.colorScheme === Theme.LightOnDark
                                            ? Theme.darkPrimaryColor
                                            : Theme.lightPrimaryColor
                                } else if (modeIcon.highlighted) {
                                    return tabView.palette.highlightColor
                                }
                                return tabView.palette.colorScheme === Theme.LightOnDark
                                        ? Theme.lightPrimaryColor
                                        : Theme.darkPrimaryColor
                            }
                            text: modeButton.privateTabs
                                    ? (!tabView._emptyPrivateIcon ? webView.privateTabModel.count : "")
                                    : (!tabView.privateMode && tabView._closingAllTabs ? 0 : webView.persistentTabModel.count)
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: Theme.paddingSmall / 2
                            color: tabView.palette.highlightColor
                            visible: modeButton.isCurrentTab
                        }
                    }
                }
            }
        }

        Repeater {
            model: 2

            Item {
                id: modePage

                readonly property bool privateTabs: index === 1
                readonly property bool isCurrentItem: privateTabs === tabView.privateMode

                anchors {
                    top: header.bottom
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                visible: isCurrentItem

                function takeOver() {
                    if (!isCurrentItem)
                        return
                    _remorsePopup = Qt.binding(function() { return grid.remorsePopup })
                    _closingAllTabs = Qt.binding(function() { return grid.closingAllTabs })
                }

                onIsCurrentItemChanged: takeOver()
                Component.onCompleted: takeOver()

                TabGridView {
                    id: grid

                    // NB: Only the height: the grid places itself across, inside its margins.
                    height: parent.height
                    horizontalMargin: tabView.horizontalMargin
                    portrait: tabView.portrait
                    model: TabFilterModel {
                        sourceModel: modePage.privateTabs ? webView.privateTabModel : webView.persistentTabModel
                        showHidden: false
                    }
                    header: Item {
                        width: 1
                        height: Theme.paddingLarge
                    }

                    onHide: tabView.hide()
                    onEnterNewTabUrl: tabView.enterNewTabUrl()
                    onActivateTab: tabView.activateTab(index)
                    onCloseTab: tabView.closeTab(index)
                    onCloseAll: tabView.closeAll()
                    onCloseAllCanceled: tabView.closeAllCanceled()
                    onCloseAllPending: tabView.closeAllPending()
                }

                Connections {
                    target: popupMenu
                    onCloseAllTabs: {
                        if (modePage.isCurrentItem)
                            grid.closeAllTabs()
                    }
                }

                PrivateModeTexture {
                    visible: modePage.privateTabs
                    z: -1
                }
            }
        }
    }

    TabsToolBar {
        id: tabsToolBar

        anchors.bottom: parent.bottom
        onBack: pageStack.pop()
        onEnterNewTabUrl: tabView.enterNewTabUrl()
        onOpenMenu: popupMenu.active = true
    }

    PopUpMenu {
        id: popupMenu

        signal closeAllTabs

        anchors.fill: parent
        active: false

        menuItem: Component {
            Item {
                id: menuItem_

                readonly property int iconWidth: Theme.iconSizeMedium + Theme.paddingLarge
                readonly property int verticalPadding: 3 * Theme.paddingSmall

                height: content.height + verticalPadding * 2

                Column {
                    id: content

                    y: verticalPadding
                    width: parent.width
                    spacing: Theme.paddingLarge

                    Column {
                        width: parent.width

                        OverlayListItem {
                            height: Theme.itemSizeSmall
                            iconWidth: menuItem_.iconWidth
                            iconSource: "image://theme/icon-m-tab-new"
                            enabled: !_closingAllTabs
                            //% "New tab"
                            text: qsTrId("sailfish_browser-la-new_tab")
                            onClicked: {
                                popupMenu.visible = false
                                tabView.switchMode(false)
                                tabView.enterNewTabUrl()
                            }
                        }

                        OverlayListItem {
                            height: Theme.itemSizeSmall
                            iconWidth: menuItem_.iconWidth
                            iconSource: "image://theme/icon-m-incognito-new"
                            enabled: !_closingAllTabs
                            //% "New private tab"
                            text: qsTrId("sailfish_browser-la-new_private_tab")
                            onClicked: {
                                popupMenu.visible = false
                                tabView.switchMode(true)
                                tabView.enterNewTabUrl()
                            }
                        }

                        OverlayListItem {
                            height: Theme.itemSizeSmall
                            iconWidth: menuItem_.iconWidth
                            iconSource: "image://theme/icon-m-tab-close"
                            enabled: showCloseAllAction.value && webView.tabModel.count && !_closingAllTabs
                            //% "Close all tabs"
                            text: qsTrId("sailfish_browser-me-close_all")
                            onClicked: {
                                popupMenu.active = false
                                popupMenu.closeAllTabs()
                            }
                        }
                    }
                }
            }
        }
        onClosed: active = false
    }

    QtObject {
        id: showCloseAllAction
        property bool value: true
    }
}
