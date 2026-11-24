import QtQuick 2.0
import Sailfish.Silica 1.0
import AndroidCompatibilityLayer.Preview 1.0
import "PreviewNavigation.js" as Navigation

ApplicationWindow {
    id: root
    readonly property bool systemDark: Theme.colorScheme === Theme.LightOnDark
    readonly property color systemAccent: Theme.highlightColor
    function previewOrientations(mask) {
        if (mask < 0) return defaultAllowedOrientations
        var allowed = 0
        if (mask & Qt.PortraitOrientation) allowed |= Orientation.Portrait
        if (mask & Qt.LandscapeOrientation) allowed |= Orientation.Landscape
        if (mask & Qt.InvertedPortraitOrientation) allowed |= Orientation.PortraitInverted
        if (mask & Qt.InvertedLandscapeOrientation) allowed |= Orientation.LandscapeInverted
        return allowed
    }
    allowedOrientations: previewOrientations(preview.orientationMask)
    AndroidFilePicker { host: pickerHost; stack: pickerStack }

    Item {
        id: pickerStack
        function push(page, properties) {
            var result = Navigation.pick(page, properties, pickerStack)
            navigationTimer.restart()
            return result
        }
    }

    Timer {
        id: navigationTimer
        interval: 0
        onTriggered: Navigation.update()
    }

    Timer {
        id: frameTimer
        interval: 5000
        onTriggered: Navigation.frameTimeout()
    }

    Connections {
        target: preview
        onFrameChanged: Navigation.frameChanged()
    }

    Connections {
        target: pageStack
        onBusyChanged: navigationTimer.restart()
        onCurrentPageChanged: navigationTimer.restart()
    }

    initialPage: Component {
        Page {
            id: backPage
            allowedOrientations: Orientation.All
            property bool navigationError: false
            Component.onCompleted: {
                Navigation.initialize(pageStack, backPage, previewPage, preview, pickerHost,
                                      PageStackAction.Immediate, PageStatus.Active, frameTimer)
                navigationTimer.restart()
            }
            Component.onDestruction: Navigation.shutdown()
            onStatusChanged: navigationTimer.restart()

            Column {
                anchors.centerIn: parent
                visible: !backPage.navigationError
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "image://theme/icon-m-back"
                }
            }
            Column {
                anchors.centerIn: parent
                visible: backPage.navigationError
                spacing: Theme.paddingMedium
                Label { text: qsTr("Unable to open Android preview") }
                Button { text: qsTr("Close"); onClicked: { Navigation.shutdown(); preview.close() } }
            }
        }
    }

    Component {
        id: previewPage
        Page {
            id: guestPage
            allowedOrientations: root.previewOrientations(preview.orientationMask)
            property string diagnosticFileError: ""
            property bool hidingCaptureControls: false
            onStatusChanged: navigationTimer.restart()
            PreviewSurface {
                anchors.fill: parent
                player: preview
                pageOrientation: {
                    switch (guestPage.orientation) {
                    case Orientation.Portrait: return Qt.PortraitOrientation
                    case Orientation.Landscape: return Qt.LandscapeOrientation
                    case Orientation.PortraitInverted: return Qt.InvertedPortraitOrientation
                    case Orientation.LandscapeInverted: return Qt.InvertedLandscapeOrientation
                    default: return Qt.PrimaryOrientation
                    }
                }
                edgeZoneWidth: Theme.dp(preview.backGestureEdgeDp)
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: preview.showStatus
                text: preview.status
            }
            Rectangle {
                objectName: "standaloneDiagnosticError"
                anchors.top: parent.top
                width: parent.width
                height: diagnosticErrorLabel.height + 2 * Theme.paddingSmall
                color: Theme.rgba(Theme.errorColor, 0.9)
                visible: guestPage.diagnosticFileError.length > 0
                Label {
                    id: diagnosticErrorLabel
                    x: Theme.horizontalPageMargin
                    y: Theme.paddingSmall
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    text: qsTr("Diagnostic file could not be saved: %1").arg(guestPage.diagnosticFileError)
                }
            }
            Connections {
                target: preview
                onFrameCaptureStatusChanged: {
                    if (!preview.frameCapturePending)
                        guestPage.hidingCaptureControls = false
                }
            }
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall
                visible: preview.frameCaptureEnabled && !guestPage.hidingCaptureControls
                Button {
                    objectName: "captureDiagnosticFrames"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: preview.frameCapturePending ? qsTr("Capturing diagnostic frames...") : qsTr("Save diagnostic frames")
                    enabled: !preview.frameCapturePending
                    onClicked: {
                        guestPage.hidingCaptureControls = true
                        preview.captureFramePair()
                        if (!preview.frameCapturePending)
                            guestPage.hidingCaptureControls = false
                    }
                }
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    visible: preview.frameCaptureStatus.length > 0
                    text: preview.frameCaptureStatus
                }
            }
            Timer {
                interval: 1000
                running: typeof standaloneFileDiagnostics !== "undefined" && standaloneFileDiagnostics
                repeat: true
                triggeredOnStart: true
                onTriggered: guestPage.diagnosticFileError = diagnosticLog.standaloneDiagnosticFileError()
            }
        }
    }
}
