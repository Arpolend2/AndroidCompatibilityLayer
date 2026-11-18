import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

Column {
    id: root
    property var pageStack
    property color pageBackground: "transparent"

    SectionHeader { text: qsTr("Google services") }
    ComboBox {
        objectName: "googleServicesProvider"
        label: qsTr("Provider")
        currentIndex: ["none", "microg"].indexOf(probe.googleServicesProvider)
        menu: ContextMenu {
            MenuItem {
                text: qsTr("None")
                onClicked: probe.setGoogleServicesProvider("none")
            }
            MenuItem {
                objectName: "microgProviderOption"
                text: qsTr("microG")
                onClicked: probe.setGoogleServicesProvider("microg")
            }
        }
    }
    Column {
        objectName: "microgSection"
        width: parent.width
        visible: probe.googleServicesProvider === "microg"
        SectionHeader { text: qsTr("microG") }
        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * x
            wrapMode: Text.Wrap
            color: palette.secondaryColor
            text: qsTr("microG is an open-source compatibility implementation, not Google's software; it supports only some Google APIs.")
        }
        Item {
            id: actions
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * x
            height: Theme.itemSizeExtraSmall
            property real availableWidth: width - 2 * Theme.paddingMedium
            Button {
                objectName: "openGoogleServices"
                anchors.left: parent.left
                width: parent.availableWidth / 3
                preferredWidth: Theme.buttonWidthExtraSmall
                enabled: !probe.microgBusy
                Accessible.name: configureLabel.text
                onClicked: probe.openMicrog()
                Label {
                    id: configureLabel
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
                    text: qsTr("Configure")
                }
            }
            Button {
                objectName: "downloadGoogleServices"
                x: actions.availableWidth / 3 + Theme.paddingMedium
                width: parent.availableWidth / 3
                preferredWidth: Theme.buttonWidthExtraSmall
                enabled: !probe.microgBusy
                Accessible.name: downloadLabel.text
                onClicked: probe.updateMicrog("")
                Label {
                    id: downloadLabel
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
                    text: qsTr("Download from GitHub")
                }
            }
            Button {
                objectName: "selectGoogleServicesApk"
                anchors.right: parent.right
                width: parent.availableWidth / 3
                preferredWidth: Theme.buttonWidthExtraSmall
                enabled: !probe.microgBusy
                Accessible.name: apkLabel.text
                onClicked: if (pageStack) pageStack.push(apkPicker)
                Label {
                    id: apkLabel
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
            text: qsTr("GitHub downloads are checked against release metadata. APK signatures are not verified.")
        }
        ProgressBar {
            objectName: "microgDownloadProgress"
            width: parent.width
            visible: probe.microgDownloadActive
            indeterminate: probe.microgDownloadProgress < 0
            value: Math.max(0, probe.microgDownloadProgress) / 100
            valueText: indeterminate ? "" : qsTr("%1%").arg(probe.microgDownloadProgress)
        }
        Label {
            objectName: "microgStatus"
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * x
            wrapMode: Text.Wrap
            color: palette.secondaryColor
            textFormat: Text.PlainText
            text: probe.microgStatus
            visible: text.length > 0
        }
    }

    Component {
        id: apkPicker
        FilePickerPage {
            backgroundColor: root.pageBackground
            property bool selected: false
            title: qsTr("Select microG APK")
            nameFilters: [ "*.apk", "*.APK" ]
            onSelectedContentPropertiesChanged: {
                if (!selected && selectedContentProperties && selectedContentProperties.filePath) {
                    selected = true
                    pageStack.push(localConfirmation, { "apkPath": selectedContentProperties.filePath })
                }
            }
        }
    }
    Component {
        id: localConfirmation
        Dialog {
            backgroundColor: root.pageBackground
            property string apkPath
            canAccept: apkPath.length > 0
            onAccepted: probe.updateMicrog(apkPath)
            Column {
                width: parent.width
                DialogHeader { title: qsTr("Update microG"); acceptText: qsTr("Update") }
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    text: qsTr("The APK signature is not verified. Only install a file you trust. Continue?")
                }
            }
        }
    }
}
