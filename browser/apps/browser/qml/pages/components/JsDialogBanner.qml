/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// A page's alert(), confirm() or prompt(). The page's script waits for the
// answer, so the rest of the page is covered until one is given. Driven
// entirely by the bound webView's contentItem (see WebPage::resolveJsDialog).
Item {
    id: jsDialog

    property var webView
    readonly property var page: webView ? webView.contentItem : null
    readonly property string kind: page ? page.jsDialogKind : ""

    visible: page !== null && page.jsDialogActive
    anchors.fill: parent
    z: 950

    onVisibleChanged: {
        if (visible) {
            answer.text = page.jsDialogDefaultText
            if (kind === "prompt")
                answer.forceActiveFocus()
        }
    }

    // Taps must not reach the page behind.
    MouseArea { anchors.fill: parent }

    Rectangle {
        anchors.fill: parent
        color: Theme.rgba("black", 0.5)
    }

    Rectangle {
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            margins: Theme.horizontalPageMargin
        }
        height: dialogColumn.height + Theme.paddingLarge * 2
        radius: Theme.paddingSmall
        color: Theme.overlayBackgroundColor

        Column {
            id: dialogColumn
            anchors {
                left: parent.left
                right: parent.right
                verticalCenter: parent.verticalCenter
                margins: Theme.paddingLarge
            }
            spacing: Theme.paddingLarge

            Label {
                width: parent.width
                wrapMode: Text.Wrap
                maximumLineCount: 12
                elide: Text.ElideRight
                color: Theme.highlightColor
                text: jsDialog.page ? jsDialog.page.jsDialogMessage : ""
            }
            TextField {
                id: answer
                width: parent.width
                visible: jsDialog.kind === "prompt"
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.onClicked: jsDialog.page.resolveJsDialog(true, text)
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.paddingLarge

                Button {
                    visible: jsDialog.kind !== "alert"
                    //% "Cancel"
                    text: qsTrId("iroise-bt-js_dialog_cancel")
                    onClicked: if (jsDialog.page) jsDialog.page.resolveJsDialog(false, "")
                }
                Button {
                    //% "OK"
                    text: qsTrId("iroise-bt-js_dialog_ok")
                    onClicked: if (jsDialog.page) jsDialog.page.resolveJsDialog(true, answer.text)
                }
            }
        }
    }
}
