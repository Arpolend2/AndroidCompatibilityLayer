import QtQuick 2.0
import Sailfish.Silica 1.0
import "../../qml" as Astoria

ApplicationWindow {
    id: appWindow
    property var launcher
    property int rejected: 0
    property int accepted: 0

    function openPage() { pageStack.push(details, {}, PageStackAction.Immediate) }
    function openDialog() { pageStack.push(confirmation, {}, PageStackAction.Immediate) }

    initialPage: Component {
        Page {
            id: launcherPage
            property int section: 0
            Component.onCompleted: appWindow.launcher = launcherPage
            Astoria.SideMouseNavigation {
                stack: pageStack
                rootPage: launcherPage
                section: launcherPage.section
                sectionCount: 4
                immediateAction: PageStackAction.Immediate
                onSectionRequested: launcherPage.section = section
            }
        }
    }
    Component { id: details; Page {} }
    Component {
        id: confirmation
        Dialog {
            onAccepted: appWindow.accepted++
            onRejected: appWindow.rejected++
        }
    }
}
