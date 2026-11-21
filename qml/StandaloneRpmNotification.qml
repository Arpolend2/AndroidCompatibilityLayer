import QtQuick 2.0
import Nemo.Notifications 1.0

Item {
    id: root
    visible: false
    property bool busy: false
    property bool detailsVisible: false
    property string appPackage
    property string status
    property bool awaitingId: false
    property string publishedPackage
    property string publishedStatus

    onBusyChanged: updateTimer.restart()
    onDetailsVisibleChanged: updateTimer.restart()
    onAppPackageChanged: updateTimer.restart()
    onStatusChanged: updateTimer.restart()
    Component.onDestruction: notification.close()

    function update() {
        if (awaitingId) return
        if (!busy || detailsVisible || !appPackage.length) {
            if (notification.replacesId) {
                notification.close()
                notification.replacesId = 0
            }
            publishedPackage = ""
            publishedStatus = ""
            return
        }
        if (notification.replacesId && publishedPackage === appPackage && publishedStatus === status)
            return
        if (notification.replacesId && publishedPackage !== appPackage) {
            notification.close()
            notification.replacesId = 0
        }
        publishedPackage = appPackage
        publishedStatus = status
        notification.summary = qsTr("Creating RPM for %1").arg(appPackage)
        notification.body = status
        notification.previewSummary = notification.summary
        notification.previewBody = notification.body
        awaitingId = notification.replacesId === 0
        notification.publish()
    }

    Notification {
        id: notification
        objectName: "standaloneRpmSystemNotification"
        appName: qsTr("Android App Manager")
        appIcon: "ru.androidcompatibilitylayer.appmanager"
        progress: Notification.ProgressIndeterminate
        suppressSound: true
        expireTimeout: 16 * 60 * 1000
        onReplacesIdChanged: {
            if (root.awaitingId && replacesId !== 0) {
                root.awaitingId = false
                updateTimer.restart()
            }
        }
    }
    Timer {
        id: updateTimer
        interval: 0
        onTriggered: root.update()
    }
}
