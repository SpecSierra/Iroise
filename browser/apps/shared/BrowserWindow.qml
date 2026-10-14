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



}
