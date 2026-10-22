/*
 * Iroise: this run's downloads. The engine saves each one into the Downloads folder by itself; this lists them,
 * with how far along they are, and lets the user cancel one or open what came down.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import QtQuick 2.2
import Sailfish.Silica 1.0
import harbour.iroise 1.0

Page {
    SilicaListView {
        id: list

        anchors.fill: parent
        model: Downloads

        header: PageHeader {
            //% "Downloads"
            title: qsTrId("iroise-la-downloads")
        }

        PullDownMenu {
            visible: list.count > 0
            MenuItem {
                //% "Clear finished"
                text: qsTrId("iroise-me-clear_finished_downloads")
                onClicked: Downloads.clearFinished()
            }
        }

        ViewPlaceholder {
            enabled: list.count === 0
            //% "No downloads yet"
            text: qsTrId("iroise-la-no_downloads")
            //% "Files you download are saved to the Downloads folder"
            hintText: qsTrId("iroise-la-no_downloads_hint")
        }

        delegate: ListItem {
            id: item

            readonly property bool active: model.status === DownloadModel.InProgress
            readonly property bool done: model.status === DownloadModel.Completed

            function open() {
                Qt.openUrlExternally("file://" + model.path)
            }

            contentHeight: column.height + 2 * Theme.paddingMedium
            onClicked: done ? open() : openMenu()

            menu: ContextMenu {
                MenuItem {
                    visible: item.active
                    //% "Cancel"
                    text: qsTrId("iroise-me-cancel_download")
                    onClicked: Downloads.cancel(model.downloadId)
                }
                MenuItem {
                    visible: item.done
                    //% "Open"
                    text: qsTrId("iroise-me-open_download")
                    onClicked: item.open()
                }
            }

            Column {
                id: column

                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter

                Label {
                    width: parent.width
                    text: model.fileName
                    truncationMode: TruncationMode.Fade
                    color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                }

                ProgressBar {
                    visible: item.active
                    width: parent.width
                    leftMargin: 0
                    rightMargin: 0
                    indeterminate: model.progress < 0
                    value: Math.max(0, model.progress)
                }

                Label {
                    width: parent.width
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                    truncationMode: TruncationMode.Fade
                    text: {
                        switch (model.status) {
                        case DownloadModel.InProgress:
                            return model.total > 0
                                    //% "%1 of %2"
                                    ? qsTrId("iroise-la-download_progress").arg(Format.formatFileSize(model.received)).arg(Format.formatFileSize(model.total))
                                    : Format.formatFileSize(model.received)
                        case DownloadModel.Completed:
                            return Format.formatFileSize(model.received)
                        case DownloadModel.Cancelled:
                            //% "Cancelled"
                            return qsTrId("iroise-la-download_cancelled")
                        default:
                            //% "Failed"
                            return model.error.length > 0 ? model.error : qsTrId("iroise-la-download_failed")
                        }
                    }
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
