import QtQuick 2.0
import Sailfish.Silica 1.0
import AndroidCompatibilityLayer.Preview 1.0

Page {
    id: page
    property var player
    property var stack
    signal closed()
    allowedOrientations: Orientation.All
    Component.onDestruction: {
        player.stop()
        closed()
    }
    AndroidFilePicker { host: page.player; stack: pickerStack }
    Item {
        id: pickerStack
        property var pending: null
        function push(component, properties) {
            if (pending) return null
            if (page.stack.currentPage === page && page.status === PageStatus.Active && !page.stack.busy)
                return page.stack.push(component, properties)
            pending = { "component": component, "properties": properties }
            pickerTimer.restart()
            return pickerStack
        }
    }
    Timer {
        id: pickerTimer
        interval: 0
        onTriggered: {
            if (!pickerStack.pending || page.stack.busy || page.stack.currentPage !== page ||
                    page.status !== PageStatus.Active) return
            var request = pickerStack.pending
            pickerStack.pending = null
            if (!page.stack.push(request.component, request.properties))
                page.player.showFailed(request.properties.requestToken)
        }
    }
    Connections {
        target: page.stack
        onBusyChanged: pickerTimer.restart()
        onCurrentPageChanged: pickerTimer.restart()
    }
    onStatusChanged: pickerTimer.restart()
    PreviewSurface {
        anchors.fill: parent
        player: page.player
        edgeZoneWidth: width / 2
    }
    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        width: parent.width - 2 * Theme.horizontalPageMargin
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        visible: page.player.showStatus
        text: page.player.status
    }
}
