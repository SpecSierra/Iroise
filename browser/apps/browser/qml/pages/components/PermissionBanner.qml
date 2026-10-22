/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// Shown when a page asks for the user's location, camera or microphone.
// Allow/deny is remembered per host+type for the session (see
// WebPage::resolvePermission). Driven entirely by the bound webView's
// contentItem.
Rectangle {
    id: permissionBanner

    property var webView
    readonly property var page: webView ? webView.contentItem : null

    readonly property string permissionText: {
        if (!page)
            return ""
        switch (page.permissionType) {
        //% "%1 wants to know your location"
        case "geolocation":       return qsTrId("iroise-la-permission_location")
        //% "%1 wants to use your camera"
        case "camera":            return qsTrId("iroise-la-permission_camera")
        //% "%1 wants to use your microphone"
        case "microphone":        return qsTrId("iroise-la-permission_microphone")
        //% "%1 wants to use your camera and microphone"
        case "camera+microphone": return qsTrId("iroise-la-permission_camera_microphone")
        //% "%1 is asking for a permission"
        default:                  return qsTrId("iroise-la-permission_other")
        }
    }
    readonly property string permissionIcon: {
        if (!page)
            return "icon-m-question"
        switch (page.permissionType) {
        case "geolocation": return "icon-m-gps"
        case "microphone":  return "icon-m-mic"
        default:            return "icon-m-camera" // camera / camera+microphone
        }
    }

    visible: page !== null && page.permissionPending
    anchors {
        left: parent.left
        right: parent.right
        bottom: parent.bottom
        bottomMargin: page ? page.toolbarHeight : 0
    }
    height: permissionColumn.height + Theme.paddingLarge * 2
    color: Theme.overlayBackgroundColor
    z: 900

    Column {
        id: permissionColumn
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            margins: Theme.paddingLarge
        }
        spacing: Theme.paddingMedium

        Image {
            source: "image://theme/" + permissionBanner.permissionIcon + "?" + Theme.highlightColor
            anchors.horizontalCenter: parent.horizontalCenter
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeMedium
            color: Theme.highlightColor
            wrapMode: Text.WordWrap
            text: permissionBanner.permissionText.arg(page ? page.permissionHost : "")
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            //% "Allow"
            text: qsTrId("iroise-bt-permission_allow")
            onClicked: if (permissionBanner.page) permissionBanner.page.resolvePermission(true)
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            //% "Deny"
            text: qsTrId("iroise-bt-permission_deny")
            onClicked: if (permissionBanner.page) permissionBanner.page.resolvePermission(false)
        }
    }
}
