import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

Column {
    id: root
    property var pageStack
    property color pageBackground: "transparent"
    SectionHeader { text: qsTr("Aurora Store") }
    Item {
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * x
        height: Theme.itemSizeExtraSmall
        Button {
            objectName: "updateOfficialStoreOnline"
            anchors.left: parent.left
            width: (parent.width - Theme.paddingMedium) / 2
            enabled: !probe.officialStoreBusy
            Accessible.name: storeDownloadLabel.text
            onClicked: probe.updateOfficialStore()
            Label {
                id: storeDownloadLabel
                objectName: "actionLabel"
                anchors.fill: parent
                anchors.leftMargin: Theme.paddingSmall
                anchors.rightMargin: Theme.paddingSmall
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: Theme.fontSizeSmall
                minimumPixelSize: 10
                fontSizeMode: Text.Fit
                wrapMode: Text.WordWrap
                color: parent.highlighted ? parent.highlightColor : parent.color
                opacity: parent.enabled ? 1 : Theme.opacityLow
                text: qsTr("Download from GitLab")
            }
        }
        Button {
            objectName: "updateOfficialStoreLocal"
            anchors.right: parent.right
            width: (parent.width - Theme.paddingMedium) / 2
            enabled: !probe.officialStoreBusy
            Accessible.name: storeApkLabel.text
            onClicked: if (pageStack) pageStack.push(apkPicker)
            Label {
                id: storeApkLabel
                objectName: "actionLabel"
                anchors.fill: parent
                anchors.leftMargin: Theme.paddingSmall
                anchors.rightMargin: Theme.paddingSmall
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: Theme.fontSizeSmall
                minimumPixelSize: 10
                fontSizeMode: Text.Fit
                wrapMode: Text.WordWrap
                color: parent.highlighted ? parent.highlightColor : parent.color
                opacity: parent.enabled ? 1 : Theme.opacityLow
                text: qsTr("Choose APK file")
            }
        }
    }
    Label {
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * x
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSizeExtraSmall
        color: palette.secondaryColor
        text: qsTr("Local APK signatures are not checked. Only choose files you trust.")
    }
    ProgressBar {
        objectName: "officialStoreDownloadProgress"
        width: parent.width
        visible: probe.officialStoreDownloadActive
        indeterminate: probe.officialStoreDownloadProgress < 0
        value: Math.max(0, probe.officialStoreDownloadProgress) / 100
        valueText: indeterminate ? "" : qsTr("%1%").arg(probe.officialStoreDownloadProgress)
    }
    Label {
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * x
        wrapMode: Text.Wrap
        color: palette.secondaryColor
        textFormat: Text.PlainText
        text: probe.officialStoreStatus
        visible: text.length > 0
    }
    Component {
        id: apkPicker
        FilePickerPage {
            backgroundColor: root.pageBackground
            property bool selected: false
            title: qsTr("Select Aurora Store APK")
            nameFilters: [ "*.apk", "*.APK" ]
            onSelectedContentPropertiesChanged: {
                if (!selected && selectedContentProperties && selectedContentProperties.filePath) {
                    selected = true
                    probe.updateOfficialStoreFromLocal(selectedContentProperties.filePath)
                }
            }
        }
    }
}
