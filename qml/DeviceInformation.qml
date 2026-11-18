import QtQuick 2.0
import QtQuick.Window 2.2
import Sailfish.Silica 1.0

Column {
    id: deviceInformation
    objectName: "deviceInformation"

    Item { width: parent.width; height: Theme.paddingMedium }
    DetailItem {
        objectName: "deviceOperatingSystem"
        label: qsTr("Operating system")
        visible: !!probe.deviceOperatingSystem
        value: probe.deviceOperatingSystem
    }
    DetailItem {
        objectName: "deviceArchitecture"
        label: qsTr("CPU architecture")
        visible: !!probe.deviceCpuArchitecture
        value: probe.deviceCpuArchitecture
    }
    DetailItem {
        objectName: "deviceScreenSize"
        label: qsTr("Screen size")
        visible: deviceInformation.Screen.width > 0 && deviceInformation.Screen.height > 0
        value: qsTr("%1 × %2 logical pixels").arg(deviceInformation.Screen.width).arg(deviceInformation.Screen.height)
    }
}
