import QtQuick 2.0
import "../../qml" as Astoria

Item {
    id: root
    property int section: 0
    property bool blocked: false
    property alias stack: testStack
    property alias launcher: launcher
    property alias page: page
    property alias dialog: dialog
    property int sectionChanges: 0

    QtObject {
        id: launcher
        property bool backNavigation: true
        property bool forwardNavigation: true
        property bool canNavigateForward: true
    }
    QtObject {
        id: page
        property bool backNavigation: true
        property bool forwardNavigation: true
        property bool canNavigateForward: true
    }
    QtObject {
        id: dialog
        property bool backNavigation: true
        property bool forwardNavigation: true
        property bool canNavigateForward: true
        function accept() {}
    }
    QtObject {
        id: testStack
        property var currentPage: launcher
        property var previous: null
        property var next: null
        property bool busy: false
        property int backCount: 0
        property int forwardCount: 0
        function previousPage() { return previous }
        function nextPage() { return next }
        function navigateBack(action) { backCount++; currentPage = previous }
        function navigateForward(action) { forwardCount++; currentPage = next }
    }
    Astoria.SideMouseNavigation {
        objectName: "navigation"
        stack: testStack
        rootPage: launcher
        section: root.section
        sectionCount: 4
        blocked: root.blocked
        onSectionRequested: {
            root.section = section
            root.sectionChanges++
        }
    }
}
