import QtQuick 2.0
import Sailfish.Silica 1.0
import QtMultimedia 5.6

Dialog {
    id: photo
    allowedOrientations: Orientation.All
    property var host
    property string requestToken
    property string capturePath
    property bool answered: false
    property bool taking: false
    property bool saved: false
    property int captureId: -1
    property string preview: ""
    property string startupError: ""
    canAccept: saved && capturedImage.status === Image.Ready

    function fail(message) {
        if (answered) return
        if (!host || !requestToken) {
            startupError = message || "camera initialization failed"
            return
        }
        console.error("Camera capture: " + message)
        answered = true
        camera.imageCapture.cancelCapture()
        camera.stop()
        host.showFailed(requestToken)
        reject()
    }
    function cancel() {
        if (answered) return
        answered = true
        camera.imageCapture.cancelCapture()
        camera.stop()
        if (host && requestToken) host.canceled(requestToken)
    }
    onAccepted: {
        if (answered) return
        answered = true
        camera.stop()
        host.confirmPhoto(requestToken)
    }
    onRejected: cancel()
    Component.onCompleted: {
        if (startupError)
            fail(startupError)
        else
            camera.start()
    }
    Component.onDestruction: cancel()

    Connections {
        target: photo.host
        onClosePhoto: {
            if (token === photo.requestToken && !photo.answered) {
                photo.cancel()
                photo.reject()
            }
        }
        onPhotoValidated: {
            if (token !== photo.requestToken || photo.answered || !photo.taking) return
            if (!valid) {
                photo.fail("invalid capture result")
                return
            }
            photo.saved = true
            photo.preview = "file://" + photo.capturePath
            photo.taking = false
            camera.stop()
        }
    }
    Camera {
        id: camera
        objectName: "captureCamera"
        cameraState: Camera.UnloadedState
        captureMode: Camera.CaptureStillImage
        onError: photo.fail(errorString)
        imageCapture.onImageSaved: {
            if (photo.answered || requestId !== photo.captureId) return
            host.validatePhoto(photo.requestToken, path)
        }
        imageCapture.onCaptureFailed: {
            if (!photo.answered && requestId === photo.captureId)
                photo.fail(message)
        }
    }
    DialogHeader {
        id: photoHeader
        objectName: "captureHeader"
        title: qsTr("Take a photo")
        acceptText: qsTr("Use photo")
        cancelText: qsTr("Cancel")
    }
    Item {
        objectName: "captureViewport"
        anchors.top: photoHeader.bottom
        anchors.bottom: photoButton.top
        width: parent.width
        VideoOutput {
            anchors.fill: parent
            source: camera
            fillMode: VideoOutput.PreserveAspectFit
            visible: photo.preview === ""
        }
        Image {
            id: capturedImage
            anchors.fill: parent
            source: photo.preview
            fillMode: Image.PreserveAspectFit
            visible: photo.preview !== ""
            onStatusChanged: {
                if (status === Image.Error && photo.saved)
                    photo.fail("cannot display captured image")
            }
        }
    }
    Button {
        id: photoButton
        objectName: "captureButton"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        text: qsTr("Take photo")
        enabled: !photo.answered && !photo.taking && !photo.saved && camera.imageCapture.ready
        onClicked: {
            photo.taking = true
            photo.captureId = camera.imageCapture.captureToLocation(photo.capturePath)
            if (photo.captureId < 0) photo.fail("capture request failed")
        }
    }
}
