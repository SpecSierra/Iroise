/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// Asks whether to save a download a page started. Until the user says so, the file stays in the app's own folder;
// cancelling stops it and deletes it (see DownloadModel).
Rectangle {
    id: downloadBanner

    property var webView
    readonly property var page: webView ? webView.contentItem : null

    visible: Downloads.askingId >= 0
    anchors {
        left: parent.left
        right: parent.right
        bottom: parent.bottom
        bottomMargin: page ? page.toolbarHeight : 0
    }
    height: downloadColumn.height + Theme.paddingLarge * 2
    color: Theme.overlayBackgroundColor
    z: 900

    Column {
        id: downloadColumn
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            margins: Theme.paddingLarge
        }
        spacing: Theme.paddingMedium

        Image {
            source: "image://theme/icon-m-downloads?" + Theme.highlightColor
            anchors.horizontalCenter: parent.horizontalCenter
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeMedium
            color: Theme.highlightColor
            wrapMode: Text.Wrap
            //% "Save %1?"
            text: qsTrId("iroise-la-download_ask").arg(Downloads.askingFileName)
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryHighlightColor
            wrapMode: Text.Wrap
            visible: text.length > 0
            text: {
                var parts = []
                if (Downloads.askingHost.length > 0)
                    parts.push(Downloads.askingHost)
                if (Downloads.askingSize >= 0)
                    parts.push(Format.formatFileSize(Downloads.askingSize))
                return parts.join(" · ")
            }
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            //% "Save"
            text: qsTrId("iroise-bt-download_save")
            onClicked: Downloads.answer(true)
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            //% "Cancel"
            text: qsTrId("iroise-bt-download_cancel")
            onClicked: Downloads.answer(false)
        }
    }
}
