var fs = new ActiveXObject("Scripting.FileSystemObject")
var source = fs.OpenTextFile(WScript.Arguments(0), 1)
eval(source.ReadAll())
source.Close()

var errors = []
var console = { error: function(message) { errors.push(message) } }
var backs = 0
var closes = 0
var newFrame = false
var canBack = true
var failures = []
var playerMock = {
    closeOnBackGesture: false,
    gestureBack: function() { ++backs; newFrame = false; return canBack },
    hasFrameAfterGestureBack: function() { return newFrame },
    endGestureFrameWait: function() {},
    close: function() { ++closes },
    closeToManager: function() { ++closes }
}
var timerMock = {
    running: false,
    restart: function() { this.running = true },
    stop: function() { this.running = false }
}
var hostMock = { showFailed: function(token) { failures.push(token) } }
var base = { status: 1 }
var guest = { status: 0 }
var pushed = []
var failPush = false
var stackMock = {
    busy: false,
    currentPage: base,
    pushAttached: function() { return guest },
    navigateForward: function() {
        this.currentPage = guest
        this.busy = true
        base.status = 0
    },
    push: function(page, properties) {
        if (failPush) return null
        pushed.push(properties.requestToken)
        this.currentPage = page
        guest.status = 0
        return page
    }
}
var proxy = {}
var component = {}
var picker = {}

function check(condition, message) {
    if (!condition) {
        WScript.Echo("FAIL: " + message)
        WScript.Quit(1)
    }
}

function guestReady() {
    stackMock.busy = false
    stackMock.currentPage = guest
    guest.status = 1
    update()
}

function backFinished() {
    guest.status = 0
    base.status = 1
    stackMock.currentPage = base
    stackMock.busy = false
    update()
}

initialize(stackMock, base, component, playerMock, hostMock, 2, 1, timerMock)
update()
guestReady()
check(backs === 0 && guestPage === guest, "startup should not send back")

stackMock.busy = true
base.status = 2
update()
base.status = 0
guestReady()
check(backs === 0, "cancelled gesture must not send back")

stackMock.busy = true
check(pick(picker, { requestToken: "first" }, proxy) === proxy, "picker should wait for transition")
backFinished()
check(backs === 1 && pushed.length === 0 && !timerMock.running,
      "queued picker interrupts frame wait after completed gesture")
guestReady()
check(pushed.length === 1 && pushed[0] === "first", "queued picker should open on preview")

guestReady()
check(backs === 1, "returning from picker must not send back")
backFinished()
check(backs === 2 && stackMock.currentPage === base && timerMock.running,
      "completed gesture waits on base page")
frameChanged()
check(stackMock.currentPage === base && timerMock.running, "old frame notification must not end wait")
newFrame = true
frameChanged()
check(stackMock.currentPage === guest && !timerMock.running, "new frame restores preview")
guestReady()
check(backs === 2 && guestPage === guest, "repeated gesture should reuse preview")

backFinished()
check(backs === 3 && timerMock.running, "another gesture waits for a new frame")
check(pick(picker, { requestToken: "during-wait" }, proxy) === proxy,
      "picker during wait is queued")
check(!timerMock.running && stackMock.currentPage === guest, "picker interrupts wait")
guestReady()
check(pushed.length === 2 && pushed[1] === "during-wait", "picker opens after wait interrupted")
guestReady()
check(backs === 3, "picker return must not send back")

stackMock.busy = true
check(pick(picker, { requestToken: "failed" }, proxy) === proxy, "queue picker")
failPush = true
guestReady()
check(failures.length === 1 && failures[0] === "failed", "queued picker failure must be reported")

backFinished()
check(backs === 4 && timerMock.running, "wait before explicit shutdown")
shutdown()
guestReady()
frameTimeout()
check(backs === 4 && closes === 0 && !timerMock.running && failures.length === 1 && pick(picker, {}, proxy) === null,
      "closing must stop navigation and picker dispatch")

source = fs.OpenTextFile(WScript.Arguments(0), 1)
eval(source.ReadAll())
source.Close()
base.status = 1
base.navigationError = false
guest.status = 0
stackMock.busy = false
stackMock.currentPage = base
initialize(stackMock, base, component, playerMock, hostMock, 2, 1, timerMock)
update()
guestReady()
backFinished()
check(timerMock.running, "timeout must be armed")
frameTimeout()
check(closes === 0 && !timerMock.running && stackMock.currentPage === guest,
      "timeout restores the current preview without closing")
newFrame = true
frameChanged()
frameTimeout()
check(closes === 0 && stackMock.currentPage === guest,
      "late frame or timeout must not close or restart navigation")
guestReady()
backFinished()
check(backs === 6 && timerMock.running && stackMock.currentPage === base,
      "another gesture works after timeout")
frameTimeout()
check(closes === 0 && stackMock.currentPage === guest && !timerMock.running,
      "a repeated timeout restores the preview")

source = fs.OpenTextFile(WScript.Arguments(0), 1)
eval(source.ReadAll())
source.Close()
base.status = 1
base.navigationError = false
guest.status = 0
stackMock.busy = false
stackMock.currentPage = base
canBack = false
initialize(stackMock, base, component, playerMock, hostMock, 2, 1, timerMock)
update()
guestReady()
backFinished()
check(!timerMock.running && stackMock.currentPage === guest,
      "stopped player must not wait for a frame")

source = fs.OpenTextFile(WScript.Arguments(0), 1)
eval(source.ReadAll())
source.Close()
base.status = 1
base.navigationError = false
var brokenStack = {
    busy: false,
    currentPage: base,
    pushAttached: function() { return null }
}
initialize(brokenStack, base, component, playerMock, hostMock, 2, 1, timerMock)
check(pick(picker, { requestToken: "startup" }, proxy) === proxy, "queue during startup")
update()
check(base.navigationError && failures.length === 2 && failures[1] === "startup",
      "preview creation failure must report queued picker and expose close")
check(pick(picker, {}, proxy) === null, "failed preview must reject picker")

source = fs.OpenTextFile(WScript.Arguments(0), 1)
eval(source.ReadAll())
source.Close()
base.status = 1
base.navigationError = false
guest.status = 0
stackMock.busy = false
stackMock.currentPage = base
playerMock.closeOnBackGesture = true
var backsBeforeMicrog = backs
initialize(stackMock, base, component, playerMock, hostMock, 2, 1, timerMock)
update()
guestReady()
stackMock.busy = true
update()
guestReady()
check(closes === 0 && backs === backsBeforeMicrog, "cancelled microG gesture must not close")
backFinished()
check(closes === 1 && backs === backsBeforeMicrog && !timerMock.running,
      "microG gesture must close without sending Android Back")
backFinished()
check(closes === 1, "closing microG must happen once")
WScript.Echo("preview navigation tests passed")
