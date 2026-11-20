import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import ru.auroraos.WebView 1.0
import ru.auroraos.WebView.Global 1.0

Page {
    id: root
    objectName: "yandexFormPage"
    allowedOrientations: Orientation.All
    property url formUrl
    property var pendingDialogRequest: null
    property string loadError: ""

    function isSubmissionResult(currentUrl) {
        var base = formUrl.toString().replace(/\/$/, "")
        var url = currentUrl.toString().split("#")[0]
        var queryStart = url.indexOf("?")
        if (queryStart < 0) return false
        // Yandex routes here after accepting the response and assigning its answer key.
        return url.slice(0, queryStart).replace(/\/$/, "") === base + "/success"
                && /(?:^|&)akey=[^&]+(?:&|$)/.test(url.slice(queryStart + 1))
    }

    onStatusChanged: if (status === PageStatus.Active) closeAfterSubmit.restart()

    Timer {
        id: closeAfterSubmit
        interval: 0
        onTriggered: {
            if (root.status === PageStatus.Active && pageStack.currentPage === root
                    && !root.pendingDialogRequest && root.isSubmissionResult(form.url))
                pageStack.pop()
        }
    }

    function finishFileDialogRequest(request, paths) {
        if (pendingDialogRequest === request)
            pendingDialogRequest = null
        if (!request || !request.isValid) return
        if (paths && paths.length > 0)
            request.pickFiles(paths)
        else
            request.cancel()
        closeAfterSubmit.restart()
    }

    function handleFileDialogRequest(request) {
        handlePickerRequest(request, false)
    }

    function handleMultiFileDialogRequest(request) {
        handlePickerRequest(request, true)
    }

    function handlePickerRequest(request, multiple) {
        if (!request || !request.isValid) return
        if (pendingDialogRequest || pageStack.currentPage !== root) {
            request.cancel()
            return
        }
        pendingDialogRequest = request
        var picker = multiple ? pageStack.push(multiFilePickerDialog)
                              : pageStack.push(filePickerPage)
        if (!picker) {
            pendingDialogRequest = null
            request.cancel()
            console.warn("Yandex form: could not open the file picker")
        }
    }

    Connections {
        target: DialogHandler
        onFileDialogRequested: root.handleFileDialogRequest(dialogRequest)
        onMultiFileDialogRequested: root.handleMultiFileDialogRequest(dialogRequest)
        onFolderDialogRequested: {
            if (dialogRequest && dialogRequest.isValid)
                dialogRequest.cancel()
        }
    }

    Component.onDestruction: {
        if (pendingDialogRequest && pendingDialogRequest.isValid)
            pendingDialogRequest.cancel()
    }

    PageHeader {
        id: header
        title: qsTr("Yandex form")
    }

    WebView {
        id: form
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        url: root.formUrl

        TouchInput { enabled: true }
        KeyboardInput { enabled: true }
        TextSelection { enabled: true }

        onUrlChanged: closeAfterSubmit.restart()
        onLoadError: root.loadError = qsTr("Could not load the Yandex form.")
        onLoadFinished: {
            if (httpStatusCode >= 400)
                root.loadError = qsTr("Could not load the Yandex form.")
            else
                root.loadError = ""
        }
    }

    Label {
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.horizontalPageMargin
        visible: root.loadError.length > 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: root.loadError
        color: palette.highlightColor
    }

    Component {
        id: filePickerPage
        FilePickerPage {
            readonly property var webViewRequest: root.pendingDialogRequest
            property bool requestAnswered: false
            allowedOrientations: Orientation.All
            title: qsTr("Select log file")
            nameFilters: webViewRequest && webViewRequest.extensions.length > 0
                         ? webViewRequest.extensions : [ "*.txt", "*.TXT" ]

            onSelectedContentPropertiesChanged: {
                if (requestAnswered || !selectedContentProperties ||
                    !selectedContentProperties.filePath) return
                requestAnswered = true
                root.finishFileDialogRequest(webViewRequest,
                                             [selectedContentProperties.filePath])
            }

            Component.onDestruction: {
                if (!requestAnswered) {
                    requestAnswered = true
                    root.finishFileDialogRequest(webViewRequest, [])
                }
            }
        }
    }

    Component {
        id: multiFilePickerDialog
        MultiFilePickerDialog {
            id: multiFileDialog
            // The installed SDK picker uses this name when pushing its directory page.
            readonly property var page: multiFileDialog
            readonly property var webViewRequest: root.pendingDialogRequest
            property bool requestAnswered: false
            allowedOrientations: Orientation.All
            title: qsTr("Select log files")
            nameFilters: webViewRequest && webViewRequest.extensions.length > 0
                         ? webViewRequest.extensions : [ "*.txt", "*.TXT" ]

            onAccepted: {
                if (requestAnswered) return
                var paths = []
                if (selectedContent) {
                    for (var i = 0; i < selectedContent.count; ++i) {
                        var content = selectedContent.get(i)
                        if (content.filePath)
                            paths.push(content.filePath)
                    }
                }
                requestAnswered = true
                root.finishFileDialogRequest(webViewRequest, paths)
            }

            onRejected: {
                if (requestAnswered) return
                requestAnswered = true
                root.finishFileDialogRequest(webViewRequest, [])
            }

            Component.onDestruction: {
                if (!requestAnswered) {
                    requestAnswered = true
                    root.finishFileDialogRequest(webViewRequest, [])
                }
            }
        }
    }
}
