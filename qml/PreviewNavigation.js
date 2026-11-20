var stack = null
var basePage = null
var pageComponent = null
var player = null
var pickerHost = null
var immediate = null
var activeStatus = null
var guestPage = null
var armed = false
var moving = false
var closing = false
var pendingPicker = null
var waitingForFrame = false
var frameTimer = null

function initialize(pageStack, base, component, preview, host, immediateAction, active, timer) {
    stack = pageStack
    basePage = base
    pageComponent = component
    player = preview
    pickerHost = host
    immediate = immediateAction
    activeStatus = active
    frameTimer = timer
}

function update() {
    if (closing || waitingForFrame || !stack || stack.busy || basePage.navigationError)
        return

    if (guestPage && stack.currentPage === guestPage && guestPage.status === activeStatus) {
        moving = false
        armed = true
    }

    if (stack.currentPage === basePage && basePage.status === activeStatus && !moving) {
        moving = true
        if (!guestPage) {
            guestPage = stack.pushAttached(pageComponent)
            if (!guestPage) {
                moving = false
                basePage.navigationError = true
                console.error("Could not create Android preview page")
                if (pendingPicker) {
                    pickerHost.showFailed(pendingPicker.properties.requestToken)
                    pendingPicker = null
                }
                return
            }
        } else if (armed) {
            armed = false
            if (player.closeOnBackGesture) {
                shutdown()
                player.closeToManager()
                return
            }
            waitingForFrame = true
            frameTimer.restart()
            if (!player.gestureBack() || pendingPicker)
                finishWaiting()
            return
        }
        stack.navigateForward(immediate)
    }

    if (pendingPicker && guestPage && stack.currentPage === guestPage &&
            guestPage.status === activeStatus && !stack.busy) {
        var request = pendingPicker
        pendingPicker = null
        if (!stack.push(request.page, request.properties)) {
            console.error("Could not open Android file picker")
            pickerHost.showFailed(request.properties.requestToken)
        }
    }
}

function finishWaiting() {
    if (!waitingForFrame)
        return
    waitingForFrame = false
    frameTimer.stop()
    player.endGestureFrameWait()
    moving = false
    update()
}

function frameChanged() {
    if (waitingForFrame && player.hasFrameAfterGestureBack())
        finishWaiting()
}

function frameTimeout() {
    if (!waitingForFrame || closing)
        return
    finishWaiting()
}

function pick(page, properties, proxy) {
    if (closing || pendingPicker || (basePage && basePage.navigationError))
        return null
    if (stack && guestPage && stack.currentPage === guestPage &&
            guestPage.status === activeStatus && !stack.busy)
        return stack.push(page, properties)
    pendingPicker = { page: page, properties: properties }
    if (waitingForFrame)
        finishWaiting()
    return proxy
}

function shutdown() {
    closing = true
    if (waitingForFrame)
        player.endGestureFrameWait()
    waitingForFrame = false
    if (frameTimer)
        frameTimer.stop()
    pendingPicker = null
}
