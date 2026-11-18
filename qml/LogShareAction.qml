import QtQuick 2.0
import Sailfish.Share 1.0

ShareAction {
    objectName: "logShareAction"
    title: qsTr("Share log")
    mimeType: "text/plain"

    function share(text) {
        resources = [{ "name": "megler-log.txt", "data": text,
                       "type": "text/plain", "status": text }]
        trigger()
    }
}
