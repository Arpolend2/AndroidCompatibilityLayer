import QtQuick 2.0
import Nemo.Notifications 1.0

Item {
    id: root
    visible: false
    property string title
    property bool active: false
    property string pendingMessage
    property int pendingPercent: -1

    Component.onDestruction: {
        if (active) publishUpdate(qsTr("Download interrupted because Android App Manager closed."), -1, true)
    }

    function publishUpdate(message, percent, complete) {
        if (!active && !complete) {
            notification.replacesId = 0
            active = true
            pendingMessage = message
            pendingPercent = percent
            previewNotification.previewSummary = title
            previewNotification.previewBody = message
            previewNotification.replacesId = 0
            previewNotification.publish()
            publishPending(false)
        } else {
            pendingMessage = message
            pendingPercent = percent
            if (complete) {
                updateTimer.stop()
                publishPending(true)
                active = false
            } else if (!updateTimer.running) {
                updateTimer.start()
            }
        }
    }

    function publishPending(complete) {
        notification.summary = title
        notification.body = pendingPercent >= 0 && !complete
                ? qsTr("%1 %2%").arg(pendingMessage).arg(pendingPercent) : pendingMessage
        // Nemo treats an unset preview as the summary/body, so explicitly clear it.
        notification.previewSummary = notification.summary
        notification.previewBody = notification.body
        notification.previewSummary = ""
        notification.previewBody = ""
        notification.progress = complete ? undefined
                : pendingPercent < 0 ? Notification.ProgressIndeterminate : pendingPercent / 100
        notification.publish()
    }

    Notification {
        id: notification
        appName: qsTr("Android App Manager")
        appIcon: "ru.androidcompatibilitylayer.AndroidCompatibilityLayer"
    }
    Notification {
        id: previewNotification
        appName: qsTr("Android App Manager")
        appIcon: "ru.androidcompatibilitylayer.AndroidCompatibilityLayer"
    }
    Timer {
        id: updateTimer
        interval: 1000
        onTriggered: root.publishPending(false)
    }
}
