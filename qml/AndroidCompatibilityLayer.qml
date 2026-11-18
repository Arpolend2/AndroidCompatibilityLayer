import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import Nemo.Notifications 1.0

ApplicationWindow {
    id: window
    palette.colorScheme: probe.androidTheme === "light" ? Theme.DarkOnLight
                         : probe.androidTheme === "dark" ? Theme.LightOnDark : Theme.colorScheme
    readonly property color pageBackground: probe.androidTheme === "" ? "transparent" : palette.overlayBackgroundColor
    allowedOrientations: probe.installConfirmationVisible ? Orientation.All : defaultAllowedOrientations
    Component.onCompleted: probe.setInstallUiReady(true)
    Component.onDestruction: probe.setInstallUiReady(false)
    DownloadNotification { id: microgDownloadNotification; title: qsTr("microG") }
    DownloadNotification { id: storeDownloadNotification; title: qsTr("Aurora Store") }
    Notification {
        id: rpmErrorBanner
        objectName: "rpmErrorBanner"
        appName: qsTr("Android App Manager")
        appIcon: "ru.androidcompatibilitylayer.AndroidCompatibilityLayer"
        previewSummary: qsTr("Android App Manager")
    }
    Notification {
        id: signatureBanner
        objectName: "signatureBanner"
        appName: qsTr("Android App Manager")
        appIcon: "ru.androidcompatibilitylayer.AndroidCompatibilityLayer"
        previewSummary: qsTr("Android App Manager")
        previewBody: qsTr("Only add apps you trust. APK signatures are not verified.")
    }
    Connections {
        target: probe
        onMicrogDownloadNotification: microgDownloadNotification.publishUpdate(message, percent, complete)
        onOfficialStoreDownloadNotification: storeDownloadNotification.publishUpdate(message, percent, complete)
        onSignatureReminder: {
            signatureBanner.replacesId = 0
            signatureBanner.publish()
        }
        onStandaloneRpmError: {
            var current = pageStack.currentPage
            if (current && current.objectName === "appDetailsPage" && current.appPackage === appPackage)
                return
            rpmErrorBanner.previewBody = message
            rpmErrorBanner.replacesId = 0
            rpmErrorBanner.publish()
        }
    }
    Rectangle {
        objectName: "installConfirmation"
        parent: pageStack.currentPage ? pageStack.currentPage : window.contentItem
        anchors.fill: parent
        z: 10000
        visible: probe.installConfirmationVisible
        color: window.palette.overlayBackgroundColor
        MouseArea { anchors.fill: parent }
        SilicaFlickable {
            id: installScroll
            objectName: "installConfirmationScroll"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: installActions.top
            clip: true
            contentHeight: installContent.height + 2 * Theme.paddingLarge
            VerticalScrollDecorator {}
            Column {
                id: installContent
                x: Theme.horizontalPageMargin
                y: Theme.paddingLarge
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Label { width: parent.width; text: probe.installConfirmationIsUpdate ? qsTr("Update this app?") : qsTr("Install requested APK?"); wrapMode: Text.Wrap }
                Label { width: parent.width; text: probe.installConfirmationLabel; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                Label { width: parent.width; text: probe.installConfirmationPackage; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                Label { width: parent.width; text: qsTr("Version: %1").arg(probe.installConfirmationVersion) }
                Label {
                    objectName: "installWarning"
                    width: parent.width
                    text: probe.installConfirmationIsBundle
                          ? qsTr("Only install apps you trust. Signatures are not verified, and some incomplete bundles may not be detected. Android apps are not isolated from this app's data.")
                          : qsTr("Only install apps you trust. Signatures are not verified; split APKs are not supported, and some incomplete APKs may not be detected. Android apps are not isolated from this app's data.")
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
            }
        }
        Row {
            id: installActions
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.paddingMedium
            spacing: Theme.paddingMedium
            Button {
                objectName: "cancelInstall"
                width: (installActions.width - installActions.spacing) / 2
                text: qsTr("Cancel")
                onClicked: probe.answerInstall(probe.installConfirmationToken, false)
            }
            Button {
                objectName: "confirmInstall"
                width: (installActions.width - installActions.spacing) / 2
                text: probe.installConfirmationIsUpdate ? qsTr("Update") : qsTr("Install")
                onClicked: probe.answerInstall(probe.installConfirmationToken, true)
            }
        }
        onVisibleChanged: if (visible) installScroll.contentY = 0
    }
    initialPage: Component {
        Page {
            id: launcher
            backgroundColor: window.pageBackground
            allowedOrientations: Orientation.All
            property bool busy: probe.externalInstallBusy
            property int section: 0
            property var apps: probe.installedApps

            SideMouseNavigation {
                stack: pageStack
                rootPage: launcher
                section: launcher.section
                sectionCount: 4
                blocked: probe.installConfirmationVisible
                immediateAction: PageStackAction.Immediate
                onSectionRequested: {
                    launcher.section = section
                    scroll.contentY = 0
                    if (section === 2 && logContent.following) logScrollTimer.restart()
                }
            }

            Connections {
                target: probe
                onExternalInstallNotice: {
                    launcher.section = 0
                    if (pageStack.currentPage !== launcher)
                        pageStack.pop(launcher, PageStackAction.Immediate)
                }
                onControllerCloseBlocked: {
                    launcher.section = 0
                    if (pageStack.currentPage !== launcher)
                        pageStack.pop(launcher, PageStackAction.Immediate)
                }
            }

            Component {
                id: apkPicker
                FilePickerPage {
                    backgroundColor: window.pageBackground
                    title: qsTr("Select APK or bundle")
                    nameFilters: [ "*.apk", "*.APK", "*.apks", "*.APKS", "*.xapk", "*.XAPK" ]
                    onSelectedContentPropertiesChanged: {
                        if (selectedContentProperties && selectedContentProperties.filePath)
                            probe.installApk(selectedContentProperties.filePath)
                    }
                }
            }

            Component {
                id: localeDialog
                Dialog {
                    backgroundColor: window.pageBackground
                    objectName: "androidLocaleDialog"
                    allowedOrientations: Orientation.All
                    canAccept: localeInput.text.trim().length > 0
                    onAccepted: probe.setAndroidLocale(localeInput.text)
                    Column {
                        width: parent.width
                        DialogHeader { title: qsTr("Language and region"); acceptText: qsTr("Use this") }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            text: qsTr("Enter a language and region tag, such as de-DE, uk-UA, pt-BR or zh-Hant-TW.")
                            wrapMode: Text.Wrap
                        }
                        TextField {
                            id: localeInput
                            objectName: "androidLocaleInput"
                            width: parent.width
                            placeholderText: "de-DE"
                            text: probe.androidLocale || probe.systemAndroidLocale
                        }
                    }
                }
            }

            Component {
                id: locationDialog
                Dialog {
                    backgroundColor: window.pageBackground
                    objectName: "androidLocationDialog"
                    allowedOrientations: Orientation.All
                    canAccept: probe.validAndroidLocation(locationInput.text)
                    onAccepted: probe.setAndroidLocation(locationInput.text)
                    Column {
                        width: parent.width
                        DialogHeader { title: qsTr("A place for apps"); acceptText: qsTr("Use this place") }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            text: qsTr("Enter latitude from -90 to 90 and longitude from -180 to 180, separated by a comma. Use a dot for decimals, for example 55.751244, 37.618423.")
                            wrapMode: Text.Wrap
                        }
                        TextField {
                            id: locationInput
                            objectName: "androidLocationInput"
                            width: parent.width
                            placeholderText: "55.751244, 37.618423"
                            text: probe.androidLocation || ""
                        }
                        Label {
                            objectName: "androidLocationValidationError"
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            visible: locationInput.text.trim().length > 0 && !probe.validAndroidLocation(locationInput.text)
                            color: palette.highlightColor
                            wrapMode: Text.Wrap
                            text: qsTr("Enter valid latitude and longitude.")
                        }
                    }
                }
            }

            Component {
                id: removeFromLibrary
                Dialog {
                    backgroundColor: window.pageBackground
                    property string appName
                    property string appPackage
                    property var detailsPage
                    allowedOrientations: Orientation.All
                    onAccepted: if (probe.removeApp(appPackage)) detailsPage.removed = true
                    Column {
                        width: parent.width
                        DialogHeader { title: qsTr("Remove from library?"); acceptText: qsTr("Remove") }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            text: qsTr("Remove the saved APK, cached RPMs and manager data for %1? Installed standalone RPMs are not removed. This cannot be undone.").arg(appName)
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                    }
                }
            }

            Component {
                id: libraryApp
                ListItem {
                    id: appItem
                    objectName: "installedAppItem"
                    width: (libraryGrid.width - (libraryGrid.columns - 1) * libraryGrid.spacing) / libraryGrid.columns
                    contentHeight: Theme.itemSizeLarge
                    onClicked: pageStack.push(appPage, {
                        "appName": modelData.label || modelData.package,
                        "appPackage": modelData.package,
                        "appInfo": modelData
                    })
                    Image {
                        id: appIcon
                        objectName: "libraryAppIcon"
                        x: Theme.paddingSmall
                        width: Theme.iconSizeMedium
                        height: width
                        anchors.verticalCenter: parent.verticalCenter
                        fillMode: Image.PreserveAspectFit
                        source: modelData.icon || "image://theme/icon-m-file-apk"
                        onStatusChanged: {
                            if (status === Image.Error && source.toString() !== "image://theme/icon-m-file-apk")
                                source = "image://theme/icon-m-file-apk"
                        }
                    }
                    Column {
                        x: appIcon.x + appIcon.width + Theme.paddingMedium
                        width: parent.width - x - Theme.paddingSmall
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.paddingSmall
                        Label {
                            width: parent.width
                            text: modelData.label || modelData.package
                            textFormat: Text.PlainText
                            truncationMode: TruncationMode.Fade
                            color: appItem.highlighted ? palette.highlightColor : palette.primaryColor
                        }
                        Label {
                            width: parent.width
                            text: modelData.package
                            textFormat: Text.PlainText
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: palette.secondaryColor
                        }
                        Label {
                            width: parent.width
                            text: qsTr("In library")
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: palette.secondaryColor
                        }
                    }
                }
            }

            Component {
                id: appPage
                Page {
                    id: detailsPage
                    objectName: "appDetailsPage"
                    backgroundColor: window.pageBackground
                    property string appName
                    property string appPackage
                    property var appInfo: ({})
                    property var permissionGroups: []
                    property bool removed: false
                    allowedOrientations: Orientation.All
                    Component.onCompleted: permissionGroups = probe.appPermissionGroups(appPackage, appInfo)
                    onStatusChanged: if (status === PageStatus.Active && removed)
                                         pageStack.pop(launcher, PageStackAction.Immediate)

                    SilicaFlickable {
                        objectName: "appDetailsScroll"
                        anchors.fill: parent
                        contentHeight: appContent.height + Theme.paddingLarge
                        VerticalScrollDecorator {}
                        PullDownMenu {
                            objectName: "appDetailsPulleyMenu"
                            MenuItem {
                                objectName: "installAsRpm"
                                text: qsTr("Install as RPM")
                                enabled: !probe.standaloneRpmBusy
                                onClicked: probe.installStandaloneRpm(detailsPage.appPackage)
                            }
                            MenuItem {
                                objectName: "removeFromLibrary"
                                text: qsTr("Remove from library")
                                enabled: !probe.standaloneRpmBusy && (!probe.previewRunning || !probe.isAppRunning(detailsPage.appPackage))
                                onClicked: pageStack.push(removeFromLibrary, {
                                    "appName": detailsPage.appName,
                                    "appPackage": detailsPage.appPackage,
                                    "detailsPage": detailsPage
                                })
                            }
                        }
                        Column {
                            id: appContent
                            width: parent.width
                            spacing: Theme.paddingMedium

                            PageHeader { title: qsTr("App") }
                            Row {
                                x: Theme.horizontalPageMargin
                                width: parent.width - 2 * x
                                spacing: Theme.paddingMedium
                                Image {
                                    width: Theme.iconSizeLarge
                                    height: width
                                    fillMode: Image.PreserveAspectFit
                                    source: appInfo.icon || "image://theme/icon-m-file-apk"
                                    onStatusChanged: {
                                        if (status === Image.Error && source.toString() !== "image://theme/icon-m-file-apk")
                                            source = "image://theme/icon-m-file-apk"
                                    }
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Column {
                                    width: parent.width - Theme.iconSizeLarge - Theme.paddingMedium
                                    Label {
                                        width: parent.width
                                        text: appName
                                        font.pixelSize: Theme.fontSizeLarge
                                        textFormat: Text.PlainText
                                        truncationMode: TruncationMode.Fade
                                    }
                                    Label {
                                        width: parent.width
                                        text: appPackage
                                        textFormat: Text.PlainText
                                        truncationMode: TruncationMode.Fade
                                        font.pixelSize: Theme.fontSizeExtraSmall
                                        color: palette.secondaryColor
                                    }
                                    Label {
                                        text: qsTr("In library")
                                        font.pixelSize: Theme.fontSizeExtraSmall
                                        color: palette.highlightColor
                                    }
                                }
                            }
                            ProgressBar {
                                objectName: "detailRpmProgress"
                                width: parent.width
                                visible: probe.standaloneRpmBusy && probe.standaloneRpmPackage === detailsPage.appPackage
                                indeterminate: true
                            }
                            Label {
                                objectName: "detailRpmStatus"
                                x: Theme.horizontalPageMargin
                                width: parent.width - 2 * x
                                visible: probe.standaloneRpmPackage === detailsPage.appPackage && probe.standaloneRpmStatus.length > 0
                                text: probe.standaloneRpmStatus
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                color: palette.secondaryColor
                            }
                            SectionHeader { text: qsTr("App permissions") }
                            Label {
                                x: Theme.horizontalPageMargin
                                width: parent.width - 2 * x
                                wrapMode: Text.Wrap
                                color: palette.secondaryColor
                                text: qsTr("These choices apply to newly created RPMs, not apps already installed. They control supported Android permission checks and selected device bridges; they are not a security sandbox.")
                            }
                            Repeater {
                                objectName: "permissionsRepeater"
                                model: detailsPage.permissionGroups
                                TextSwitch {
                                    objectName: "permissionSwitch"
                                    width: parent.width
                                    text: modelData.title
                                    checked: modelData.allowed
                                    automaticCheck: false
                                    enabled: !probe.standaloneRpmBusy
                                    onClicked: {
                                        var allowed = !checked
                                        if (probe.setAppPermission(detailsPage.appPackage, modelData.group, allowed))
                                            checked = allowed
                                    }
                                }
                            }
                            Label {
                                x: Theme.horizontalPageMargin
                                width: parent.width - 2 * x
                                visible: detailsPage.permissionGroups.length === 0 && !probe.appResult.length
                                text: qsTr("No supported dangerous permissions requested.")
                                wrapMode: Text.Wrap
                            }
                            SectionHeader { text: qsTr("About") }
                            DetailItem {
                                label: qsTr("Version")
                                visible: !!appInfo.versionName
                                value: appInfo.versionName || ""
                            }
                            DetailItem {
                                label: qsTr("Version code")
                                visible: appInfo.versionCode > 0
                                value: String(appInfo.versionCode || "")
                            }
                            DetailItem {
                                label: qsTr("Minimum Android SDK")
                                visible: appInfo.minSdk > 0
                                value: String(appInfo.minSdk || "")
                            }
                            DetailItem {
                                label: qsTr("Target Android SDK")
                                visible: appInfo.targetSdk > 0
                                value: String(appInfo.targetSdk || "")
                            }
                            Label {
                                x: Theme.horizontalPageMargin
                                width: parent.width - 2 * x
                                color: palette.secondaryColor
                                font.pixelSize: Theme.fontSizeExtraSmall
                                wrapMode: Text.Wrap
                                text: qsTr("Removing the saved APK does not remove installed standalone RPMs or their data.")
                            }
                            Label {
                                x: Theme.horizontalPageMargin
                                width: parent.width - 2 * x
                                visible: probe.appResult.length > 0
                                text: probe.appResult
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                color: palette.secondaryColor
                            }
                        }
                    }
                }
            }

            SilicaFlickable {
                id: scroll
                objectName: "mainScroll"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: navigation.top
                clip: true
                contentHeight: content.height + Theme.paddingLarge
                VerticalScrollDecorator {}
                PullDownMenu {
                    objectName: "mainPulleyMenu"
                    visible: launcher.section === 0 || launcher.section === 2
                    MenuItem {
                        objectName: "installApk"
                        visible: launcher.section === 0
                        enabled: !launcher.busy
                        text: qsTr("Add APK or bundle")
                        onClicked: pageStack.push(apkPicker)
                    }
                    MenuItem {
                        objectName: "logFollow"
                        visible: launcher.section === 2
                        enabled: probe.coreLogAvailable
                        text: logContent.following ? qsTr("Hold") : qsTr("Follow")
                        onClicked: {
                            logContent.following = !logContent.following
                            if (logContent.following) logScrollTimer.restart()
                        }
                    }
                    MenuItem {
                        objectName: "copyLog"
                        visible: launcher.section === 2
                        enabled: logActions.canAct
                        text: qsTr("Copy")
                        onClicked: logActions.copyLog()
                    }
                    MenuItem {
                        objectName: "saveLog"
                        visible: launcher.section === 2
                        enabled: logActions.canAct
                        text: qsTr("Save to file")
                        onClicked: logActions.saveLog()
                    }
                    MenuItem {
                        objectName: "shareLog"
                        visible: launcher.section === 2
                        enabled: logActions.canAct
                        text: qsTr("Share")
                        onClicked: logActions.shareLog()
                    }
                }

                Column {
                    id: content
                    width: parent.width
                    spacing: Theme.paddingMedium
                    Column {
                        width: parent.width
                        spacing: Theme.paddingMedium
                        visible: launcher.section === 0
                        Item { width: parent.width; height: Theme.paddingMedium }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            visible: launcher.apps.length === 0
                            wrapMode: Text.Wrap
                            color: palette.secondaryColor
                            text: qsTr("No Apps Yet\nPick an Android app's APK to add it to the library.")
                        }
                        Grid {
                            id: libraryGrid
                            objectName: "libraryGrid"
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            columns: Screen.sizeCategory >= Screen.Large ? 2 : 1
                            spacing: Theme.paddingMedium
                            Repeater {
                                objectName: "installedAppsRepeater"
                                model: launcher.apps
                                delegate: libraryApp
                            }
                        }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            visible: launcher.busy
                            wrapMode: Text.Wrap
                            color: palette.secondaryColor
                            text: qsTr("An external installation is in progress.")
                        }
                    }

                    DeviceInformation {
                        width: parent.width
                        visible: launcher.section === 1
                    }
                    Column {
                        id: logContent
                        objectName: "logContent"
                        width: parent.width
                        visible: launcher.section === 2
                        property bool following: true
                        function newestFirst(text) {
                            var lines = text.split("\n")
                            if (lines[lines.length - 1] === "") lines.pop()
                            return lines.reverse().join("\n")
                        }
                        Item { width: parent.width; height: Theme.paddingMedium }
                        LogActions {
                            id: logActions
                            width: parent.width
                            logAvailable: probe.coreLogAvailable
                            logText: probe.coreLog
                        }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            visible: !probe.coreLogAvailable
                            wrapMode: Text.Wrap
                            color: palette.highlightColor
                            text: qsTr("Log capture could not be started.")
                        }
                        Label {
                            objectName: "coreLogText"
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            wrapMode: Text.WrapAnywhere
                            textFormat: Text.PlainText
                            font.pixelSize: Theme.fontSizeExtraSmall
                            text: probe.coreLog ? logContent.newestFirst(probe.coreLog) : qsTr("No core messages yet.")
                        }
                        Connections {
                            target: probe
                            onCoreLogChanged: if (launcher.section === 2 && logContent.following) logScrollTimer.restart()
                        }
                        Timer {
                            id: logScrollTimer
                            interval: 0
                            onTriggered: {
                                if (scroll.contentY > 0 && !scroll.dragging && !scroll.moving)
                                    scroll.contentY = 0
                            }
                        }
                    }
                    Column {
                        objectName: "settingsContent"
                        width: parent.width
                        visible: launcher.section === 3
                        Item { width: parent.width; height: Theme.paddingMedium }
                        ComboBox {
                            objectName: "renderSize"
                            label: qsTr("Resolution")
                            currentIndex: [0, 960, 1280, 1600].indexOf(probe.renderSizeCap)
                            menu: ContextMenu {
                                MenuItem { text: qsTr("Native"); onClicked: probe.setRenderSizeCap(0) }
                                MenuItem { objectName: "renderSize960"; text: "960"; onClicked: probe.setRenderSizeCap(960) }
                                MenuItem { text: "1280"; onClicked: probe.setRenderSizeCap(1280) }
                                MenuItem { text: "1600"; onClicked: probe.setRenderSizeCap(1600) }
                            }
                        }
                        ComboBox {
                            objectName: "androidLocale"
                            label: qsTr("Language and region")
                            automaticSelection: false
                            currentIndex: !probe.androidLocale ? 0 : probe.androidLocale === "en-US" ? 1 : 2
                            menu: ContextMenu {
                                MenuItem { objectName: "localeSystem"; text: qsTr("The phone's (%1)").arg(probe.systemAndroidLocale); onClicked: probe.setAndroidLocale("") }
                                MenuItem { objectName: "localeEnglish"; text: qsTr("English (United States)"); onClicked: probe.setAndroidLocale("en-US") }
                                MenuItem {
                                    objectName: "localeOther"
                                    text: probe.androidLocale && probe.androidLocale !== "en-US" ? probe.androidLocale : qsTr("Another")
                                    onClicked: pageStack.push(localeDialog)
                                }
                            }
                        }
                        ComboBox {
                            objectName: "androidTheme"
                            label: qsTr("Theme")
                            automaticSelection: false
                            currentIndex: ["", "light", "dark"].indexOf(probe.androidTheme)
                            menu: ContextMenu {
                                MenuItem { objectName: "themeSystem"; text: qsTr("As the device"); onClicked: probe.setAndroidTheme("") }
                                MenuItem { objectName: "themeLight"; text: qsTr("Light"); onClicked: probe.setAndroidTheme("light") }
                                MenuItem { objectName: "themeDark"; text: qsTr("Dark"); onClicked: probe.setAndroidTheme("dark") }
                            }
                        }
                        ComboBox {
                            objectName: "androidLocation"
                            label: qsTr("Location")
                            automaticSelection: false
                            currentIndex: probe.androidLocation ? 1 : 0
                            menu: ContextMenu {
                                MenuItem { objectName: "locationDevice"; text: qsTr("The phone's location"); onClicked: probe.setAndroidLocation("") }
                                MenuItem {
                                    objectName: "locationFixed"
                                    text: probe.androidLocation || qsTr("A place set here")
                                    onClicked: pageStack.push(locationDialog)
                                }
                            }
                        }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            color: palette.secondaryColor
                            wrapMode: Text.Wrap
                            font.pixelSize: Theme.fontSizeExtraSmall
                            text: qsTr("Android apps may receive the phone's location, or the coordinates you set here. Device location requires Aurora's Location permission.")
                        }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            color: palette.secondaryColor
                            wrapMode: Text.Wrap
                            font.pixelSize: Theme.fontSizeExtraSmall
                            text: qsTr("Resolution is the longest side in pixels. Changes apply when an app is next opened.")
                        }
                        GoogleServicesSettings {
                            width: parent.width
                            pageStack: window.pageStack
                            pageBackground: window.pageBackground
                        }
                        OfficialStoreSettings {
                            width: parent.width
                            pageStack: window.pageStack
                            pageBackground: window.pageBackground
                        }
                        Label {
                            x: Theme.horizontalPageMargin
                            width: parent.width - 2 * x
                            visible: probe.runtimeSettingsError.length > 0
                            text: probe.runtimeSettingsError
                            textFormat: Text.PlainText
                            color: palette.highlightColor
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            Rectangle {
                objectName: "navigationBackground"
                anchors.fill: navigation
                color: window.palette.overlayBackgroundColor
            }
            Row {
                id: navigation
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.itemSizeLarge
                Repeater {
                    model: [
                        { "title": qsTr("Library"), "icon": "icon-m-file-apk" },
                        { "title": qsTr("Device"), "icon": "icon-m-device" },
                        { "title": qsTr("Log"), "icon": "icon-m-document" },
                        { "title": qsTr("Settings"), "icon": "icon-m-setting" }
                    ]
                    BackgroundItem {
                        width: navigation.width / 4
                        height: navigation.height
                        onClicked: {
                            launcher.section = index
                            scroll.contentY = 0
                            if (index === 2 && logContent.following) logScrollTimer.restart()
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: Theme.paddingSmall
                            Icon {
                                anchors.horizontalCenter: parent.horizontalCenter
                                source: "image://theme/" + modelData.icon
                                opacity: launcher.section === index ? 1 : 0.6
                            }
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.title
                                font.pixelSize: Theme.fontSizeExtraSmall
                                color: launcher.section === index ? palette.highlightColor : palette.secondaryColor
                            }
                        }
                    }
                }
            }
        }
    }
}
