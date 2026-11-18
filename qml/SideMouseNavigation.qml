import QtQuick 2.0
import Megler.Navigation 1.0

Item {
    id: sideNavigation
    visible: false
    property var stack
    property var rootPage
    property int section: 0
    property int sectionCount: 0
    property bool blocked: false
    property int immediateAction: 0

    signal sectionRequested(int section)

    SideMouseButtons {
        target: sideNavigation
        onBackRequested: sideNavigation.navigate(false)
        onForwardRequested: sideNavigation.navigate(true)
    }

    function navigate(forward) {
        if (blocked || !stack || !rootPage || stack.busy || !stack.currentPage)
            return

        var page = stack.currentPage
        if (!forward && page !== rootPage) {
            if (page.backNavigation && stack.previousPage())
                stack.navigateBack(immediateAction)
            return
        }
        if (forward && stack.nextPage() && page.forwardNavigation &&
                page.canNavigateForward && typeof page.accept !== "function") {
            stack.navigateForward(immediateAction)
            return
        }
        if (page !== rootPage)
            return
        var next = section + (forward ? 1 : -1)
        if (next >= 0 && next < sectionCount)
            sectionRequested(next)
    }
}
