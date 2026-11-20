import QtQuick 2.0
import Sailfish.Silica 1.0

ApplicationWindow {
    allowedOrientations: defaultAllowedOrientations
    initialPage: Component {
        Page {
            allowedOrientations: Orientation.All
            Column {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingLarge
                Rectangle {
                    width: parent.width
                    height: message.height + 2 * Theme.paddingMedium
                    color: Theme.rgba(Theme.errorColor, 0.9)
                    Label {
                        id: message
                        x: Theme.paddingMedium
                        y: Theme.paddingMedium
                        width: parent.width - 2 * x
                        wrapMode: Text.Wrap
                        text: runtimeIncompatible
                            ? qsTr("The shared Android runtime is incompatible. Recreate the RPM in the manager and reinstall it.")
                            : qsTr("The shared Android runtime is unavailable. This app requires the manager and access to its runtime. The manager and this app must be signed with the same key.")
                    }
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Close")
                    onClicked: Qt.quit()
                }
            }
        }
    }
}
