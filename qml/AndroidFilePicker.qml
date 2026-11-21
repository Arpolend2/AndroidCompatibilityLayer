import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

Item {
    id: bridge
    property var host
    property var stack

    Component.onCompleted: host.setUiReady(true)
    Component.onDestruction: host.setUiReady(false)

    Connections {
        target: bridge.host
        ignoreUnknownSignals: true
        onShowPicker: {
            var page = bridge.stack.push(typeof multiple !== "undefined" && multiple ? multiPicker : pickerPage,
                                         { requestToken: token, nameFilters: filters })
            if (!page)
                bridge.host.showFailed(token)
        }
        onShowPhoto: {
            var component = Qt.createComponent(Qt.resolvedUrl("AndroidPhotoCapture.qml"))
            if (component.status !== Component.Ready) {
                console.error("Could not load camera screen: " + component.errorString())
                bridge.host.showFailed(token)
                return
            }
            var page = bridge.stack.push(component, { requestToken: token, capturePath: path, host: bridge.host })
            if (!page)
                bridge.host.showFailed(token)
        }
    }

    Component {
        id: multiPicker
        MultiFilePickerDialog {
            id: multi
            property string requestToken
            property bool answered: false
            title: qsTr("Select files")
            onAccepted: {
                if (answered) return
                answered = true
                var paths = []
                for (var i = 0; i < selectedContent.count; ++i)
                    paths.push(selectedContent.get(i).filePath)
                if (paths.length)
                    bridge.host.selectedMany(requestToken, paths)
                else
                    bridge.host.canceled(requestToken)
            }
            onRejected: {
                if (!answered) {
                    answered = true
                    bridge.host.canceled(requestToken)
                }
            }
            Component.onDestruction: {
                if (!answered) bridge.host.canceled(requestToken)
            }
        }
    }

    Component {
        id: pickerPage
        FilePickerPage {
            id: picker
            property string requestToken
            property bool answered: false
            title: qsTr("Select a file")
            onSelectedContentPropertiesChanged: {
                if (!answered && selectedContentProperties && selectedContentProperties.filePath) {
                    answered = true
                    bridge.host.selected(requestToken, selectedContentProperties.filePath)
                }
            }
            Component.onDestruction: {
                if (!answered)
                    bridge.host.canceled(requestToken)
            }
        }
    }
}
