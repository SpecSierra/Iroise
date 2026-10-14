/****************************************************************************
**
** Copyright (c) 2013 - 2019 Jolla Ltd.
** Copyright (c) 2020 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import QtQuick 2.2
import QtQuick.Window 2.2 as QuickWindow
import Sailfish.Silica 1.0
import harbour.pacific 1.0

ApplicationWindow {
    id: window

    readonly property bool largeScreen: Screen.sizeCategory > Screen.Medium

    property var rootPage
    property QtObject webView

    allowedOrientations: defaultAllowedOrientations
    _defaultPageOrientations: Orientation.LandscapeMask | Orientation.Portrait
    _defaultLabelFormat: Text.PlainText
    _clippingItem.opacity: 1.0
    _resizeContent: !window.rootPage.active
    _mainWindow: webView ? webView.chromeWindow : null
    _opaque: false

    cover: null

    // The band of the screen the camera notch is in, black on every page so the notch disappears into it. The
    // notch is at the top of the physical portrait screen, on a different edge of the rotated content per
    // orientation; BrowserPage keeps the web content out of the band.
    Rectangle {
        readonly property int orientation: window.pageStack.currentOrientation
        readonly property bool landscape: orientation === Orientation.Landscape || orientation === Orientation.LandscapeInverted
        readonly property real band: Screen.hasCutouts ? Screen.topCutout.height : 0

        z: 1000
        visible: band > 0
        color: "black"
        x: orientation === Orientation.LandscapeInverted ? parent.width - band : 0
        y: orientation === Orientation.PortraitInverted ? parent.height - band : 0
        width: landscape ? band : parent.width
        height: landscape ? parent.height : band
    }



}
