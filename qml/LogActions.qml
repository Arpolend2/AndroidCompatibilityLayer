import QtQuick 2.0
import Sailfish.Silica 1.0

Column {
    id: root
    objectName: "logActions"
    signal openYandexFormRequested()
    property string logText: ""
    property bool logAvailable: false
    property string message: ""
    readonly property bool canAct: logAvailable && logText.length > 0
    spacing: Theme.paddingMedium

    function copyLog() {
        if (!canAct) return
        var report = probe.diagnosticReport()
        Clipboard.text = report
        message = report.length > 0 && Clipboard.text === report
                ? qsTr("Copied to clipboard.")
                : qsTr("Could not copy to clipboard.")
    }

    function shareLog() {
        if (!canAct) return
        message = ""
        if (shareLoader.status === Loader.Ready)
            shareLoader.item.share(probe.diagnosticReport())
        else
            shareLoader.source = "LogShareAction.qml"
    }

    function saveLog() {
        if (!canAct) return
        message = probe.saveCoreLog()
    }

    function openYandexForm() {
        message = probe.openYandexLogForm()
        if (message.length === 0)
            openYandexFormRequested()
    }

    Label {
        objectName: "logActionMessage"
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * x
        visible: text.length > 0
        text: root.message
        color: palette.secondaryColor
        wrapMode: Text.Wrap
    }

    Loader {
        id: shareLoader
        onLoaded: {
            if (root.logAvailable && root.logText.length > 0)
                item.share(probe.diagnosticReport())
        }
        onStatusChanged: {
            if (status === Loader.Error)
                root.message = qsTr("Sharing is unavailable.")
        }
    }
}
