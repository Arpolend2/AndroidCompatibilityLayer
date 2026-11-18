import QtQuick 2.0
import Sailfish.Silica 1.0

Column {
    id: root
    property string logText: ""
    property bool logAvailable: false
    property string message: ""
    readonly property bool canAct: logAvailable && logText.length > 0
    spacing: Theme.paddingMedium

    function copyLog() {
        if (!canAct) return
        Clipboard.text = logText
        message = Clipboard.text === logText
                ? qsTr("Copied to clipboard.")
                : qsTr("Could not copy to clipboard.")
    }

    function shareLog() {
        if (!canAct) return
        message = ""
        if (shareLoader.status === Loader.Ready)
            shareLoader.item.share(logText)
        else
            shareLoader.source = "LogShareAction.qml"
    }

    function saveLog() {
        if (!canAct) return
        message = probe.saveCoreLog()
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
                item.share(root.logText)
        }
        onStatusChanged: {
            if (status === Loader.Error)
                root.message = qsTr("Sharing is unavailable.")
        }
    }
}
