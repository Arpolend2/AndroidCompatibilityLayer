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
        onShowPicker: {
            var page = bridge.stack.push(pickerPage, { requestToken: token, nameFilters: filters })
            if (!page)
                bridge.host.showFailed(token)
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
