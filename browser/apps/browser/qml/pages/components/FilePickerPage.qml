/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

// File chooser page for HTML <input type=file>. Pushed by BrowserPage with
// webView + browserPage set; hands the selection to the page's file chooser
// (with a short retry, since the picker reports selection async) and
// clears browserPage._filePickerOpen when done.
ContentPickerPage {
    id: filePicker
    objectName: "iroiseFilePickerPage"

    property var webView
    property var browserPage

    //% "Select file"
    title: qsTrId("iroise-he-select_file")
    property bool _selectionCommitted: false
    property int _selectionRetryCount: 0

    function submitSelection() {
        if (_selectionCommitted || !webView.contentItem || !webView.contentItem.fileChooserActive) {
            return
        }

        var candidates = []
        function addCandidate(value) {
            var normalized = String(value)
            if (!normalized || normalized === "undefined" || normalized === "null") {
                return
            }
            if (candidates.indexOf(normalized) === -1) {
                candidates.push(normalized)
            }
        }
        if (selectedContentProperties) {
            if (selectedContentProperties.filePath) {
                addCandidate(selectedContentProperties.filePath)
            }
            if (selectedContentProperties.url) {
                addCandidate(selectedContentProperties.url)
            }
        }
        if (selectedContent) {
            addCandidate(selectedContent)
        }
        if (candidates.length === 0) {
            return
        }

        _selectionCommitted = true
        delayedCancel.stop()
        webView.contentItem.chooseFiles(candidates)
        browserPage._filePickerOpen = false
        pageStack.pop(filePicker, PageStackAction.Immediate)
    }

    onSelectedContentPropertiesChanged: {
        submitSelection()
    }

    onSelectedContentChanged: {
        submitSelection()
    }

    onStatusChanged: {
        if (status === PageStatus.Inactive) {
            // NB: Opening a folder pushes the picker's own page over this one, which leaves it Inactive too. Only
            //     a picker that is gone (back on the browser page) is done; cancelling here would answer the page
            //     with no file while the user is still choosing one.
            if (pageStack.find(function(page) { return page === filePicker }) && pageStack.currentPage !== browserPage) {
                return
            }
            browserPage._filePickerOpen = false
            submitSelection()
            if (!_selectionCommitted) {
                _selectionRetryCount = 0
                delayedCancel.restart()
            }
        }
    }

    Timer {
        id: delayedCancel
        interval: 200
        repeat: true
        onTriggered: {
            _selectionRetryCount += 1
            submitSelection()
            if (_selectionCommitted) {
                delayedCancel.stop()
                return
            }
            if (_selectionRetryCount < 6) {
                return
            }
            delayedCancel.stop()
            if (!_selectionCommitted && webView.contentItem && webView.contentItem.fileChooserActive) {
                webView.contentItem.cancelFileChooser()
            }
        }
    }
}
