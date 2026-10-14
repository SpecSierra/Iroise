/****************************************************************************
**
** Copyright (c) 2013 - 2019 Jolla Ltd.
** Copyright (c) 2019 - 2021 Open Mobile Platform LLC.
** Contact: Dmitry Rozhkov <dmitry.rozhkov@jolla.com>
** Contact: Raine Makelainen <raine.makelainen@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.0
import Nemo.KeepAlive 1.2
import Nemo.DBus 2.0
import Nemo.Policy 1.0

// QtObject cannot have children
Item {
    property QtObject webPage
    property bool videoActive
    property bool audioActive
    readonly property alias displayOff: screenBlanked.blanked
    property bool background
    property bool _htmlAudioActive
    property bool _htmlVideoActive

    function calculateStatus() {
        var video = _htmlVideoActive
        var audio = _htmlAudioActive || video
        if (videoActive !== video) {
            videoActive = video
        }
        if (audioActive !== audio) {
            audioActive = audio
        }
    }

    function resumeView() {
        if (webPage) {
            webPage.resumeView()
        }
    }

    function suspendView() {
        if (webPage) {
            webPage.suspendView()
        }
    }

    onAudioActiveChanged: {
        if (!audioActive && screenBlanked.blanked) {
            delayedSuspend.suspendIntention = true
        }
    }

    // This is behind 1000ms timer
    onBackgroundChanged: {
        if (!audioActive && background) {
            suspendView()
        } else if (!background) {
            resumeView()
        }
    }


    DBusInterface {
        id: screenBlanked

        property bool blanked

        function display_status_ind(state) {
            blanked = (state === "off")
        }

        bus: DBus.SystemBus
        service: 'com.nokia.mce'
        path: '/com/nokia/mce/signal'
        iface: 'com.nokia.mce.signal'
        signalsEnabled: true

        onBlankedChanged: {
            if (blanked && !audioActive) {
                delayedSuspend.suspendIntention = true
            } else {
                // Return immediately from suspend.
                resumeView()
                // We want also reset suspendIntention to false.
                delayedSuspend.suspendIntention = false
            }
        }
    }

    DisplayBlanking {
        // This is stopping screen blank timer.
        preventBlanking: videoActive
    }

    Timer {
        id: delayedSuspend

        property bool suspendIntention

        // 1000ms was not enough to always allow buffering start of next song via 3G
        interval: 5000
        onTriggered: {
            if (!audioActive && suspendIntention) {
                suspendView()
            } else {
                resumeView()
            }
            // Reset suspend intention.
            suspendIntention = false
        }

        onSuspendIntentionChanged: {
            if (suspendIntention) {
                start()
            }
        }
    }

    Permissions {
        enabled: audioActive || videoActive
        applicationClass: "player"
        autoRelease: true

        Resource {
            type: Resource.AudioPlayback
        }

        Resource {
            type: Resource.VideoPlayback
        }
    }
}
