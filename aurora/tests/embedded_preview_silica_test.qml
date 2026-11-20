import QtQuick 2.0
import Sailfish.Silica 1.0
import "../../qml" as Manager

ApplicationWindow {
    id: root
    property var launcher
    property var guest
    property int stops: 0
    property int cancels: 0
    function openGuest() {
        guest = pageStack.push(guestComponent, { "player": player, "stack": pageStack }, PageStackAction.Immediate)
    }
    function openGuestAnimated() {
        guest = pageStack.push(guestComponent, { "player": player, "stack": pageStack }, PageStackAction.Animated)
    }
    function openPicker() { player.showPicker("1", ["*"]) }
    QtObject {
        id: player
        property bool showStatus: false
        property string status: ""
        property int backGestureEdgeDp: 32
        signal showPicker(string token, var filters)
        function setUiReady(ready) {}
        function stop() { root.stops++ }
        function canceled(token) { root.cancels++ }
        function selected(token, path) {}
        function showFailed(token) { throw new Error("picker failed") }
    }
    initialPage: Component {
        Page { Component.onCompleted: root.launcher = this }
    }
    Component {
        id: guestComponent
        Manager.EmbeddedMicrogPage { onClosed: root.guest = null }
    }
}
