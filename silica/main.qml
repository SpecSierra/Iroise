// Pacific, the Sailfish OS front end for the Ladybird engine: a Silica window around a page rendered by the Ladybird engine.
import QtQuick 2.6
import Sailfish.Silica 1.0
import org.ladybird.sfos 1.0

ApplicationWindow {
    cover: undefined
    allowedOrientations: Orientation.Portrait

    initialPage: Component {
        Page {
            allowedOrientations: Orientation.Portrait

            LadybirdView {
                id: webView

                anchors { top: parent.top; left: parent.left; right: parent.right; bottom: toolBar.top }
                Component.onCompleted: load(initialUrl)
                onUrlChanged: if (!urlField.activeFocus) urlField.text = url
            }

            Rectangle {
                id: toolBar

                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: Theme.itemSizeMedium
                color: Theme.rgba(Theme.overlayBackgroundColor, 0.9)

                Rectangle {
                    anchors { left: parent.left; top: parent.top }
                    width: parent.width
                    height: Math.round(Theme.pixelSize / 3) || 2
                    color: Theme.highlightColor
                    visible: webView.loading
                }

                IconButton {
                    id: backButton

                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    icon.source: "image://theme/icon-m-back"
                    onClicked: webView.goBack()
                }

                TextField {
                    id: urlField

                    anchors { left: backButton.right; right: reloadButton.left; verticalCenter: parent.verticalCenter }
                    text: initialUrl
                    labelVisible: false
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.onClicked: {
                        webView.load(text)
                        focus = false
                    }
                    onActiveFocusChanged: if (activeFocus) selectAll()
                }

                IconButton {
                    id: reloadButton

                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    icon.source: "image://theme/icon-m-refresh"
                    onClicked: webView.reload()
                }
            }

            Label {
                anchors { right: parent.right; top: parent.top; margins: Theme.paddingSmall }
                text: webView.engineRunning ? webView.framesPerSecond + " fps" : "engine not running"
                font.pixelSize: Theme.fontSizeTiny
                color: "white"
                style: Text.Outline
                styleColor: "black"
            }
        }
    }
}
