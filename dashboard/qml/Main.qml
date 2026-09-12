import QtCore as Core
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import Qt.labs.platform
import org.kde.kirigami as Kirigami

import io.github.wivrn.wivrn

Kirigami.ApplicationWindow {
    id: root
    title: i18n("WiVRn NX")
    property int destination: 0
    Shortcut { sequence: "Alt+1"; onActivated: root.navigate("", 0) }
    Shortcut { sequence: "Alt+2"; onActivated: root.navigate("SettingsPage.qml", 1) }
    Shortcut { sequence: "Alt+3"; onActivated: root.navigate("FusePage.qml", 2) }
    Shortcut { sequence: "Alt+4"; onActivated: root.navigate("TroubleshootPage.qml", 3) }
    function navigate(page, index) {
        root.pageStack.pop(null);
        if (page) root.pageStack.push(Qt.resolvedUrl(page));
        destination = index;
    }

    // NX design language: the deep-space field lives on the window, pages are
    // transparent so content floats on the nebula (glass stays on chrome).
    background: Rectangle {
        color: Kirigami.Theme.backgroundColor

    }

    ConnectUsbDialog {
        id: select_usb_device
    }

    // USB backup path: the reverse tunnel is only useful while a session runs
    Binding {
        target: Adb
        property: "sessionActive"
        value: !nxPreview && WivrnServer.headsetConnected
    }

    Binding {
        target: Adb
        property: "usbTunnelEnabled"
        value: !nxPreview && DashboardSettings.usb_backup_tunnel
    }

    SystemTrayIcon {
        id: systray
        visible: true
        icon.source: "qrc:/qml/wivrn-tray.svg"
        onActivated: root.visible = !root.visible
    }

    Component{
        id: error_template
        Kirigami.InlineMessage{
            visible: true
            property string where
            property string message

            Layout.fillWidth: true
            text: where + (where ? "\n" : "") + message
            type: Kirigami.MessageType.Error
            showCloseButton: true
        }
    }

    onClosing: (close) => {
        if (WivrnServer.ownServer && WivrnServer.sessionRunning)
        {
            close.accepted = false;
            confirm_close.open();
        } else {
            Qt.quit();
        }
    }

    Kirigami.PromptDialog {
        id: confirm_close
        title: i18n("Quit WiVRn")
        subtitle: i18n("The WiVRn server is active.\nClosing the window will terminate it.")
        iconName: "dialog-warning"
        popupType: Controls.Popup.Native
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
        onAccepted: Qt.quit()
    }

    width: 1280
    height: 900
    minimumWidth: 720
    minimumHeight: 600

    property bool server_started: WivrnServer.serverStatus == WivrnServer.Started
    property bool json_loaded: false
    property bool prev_headset_connected: false

    Connections {
        target: WivrnServer
        function onServerStatusChanged(value) {
            var started = value == WivrnServer.Started;

            // Only set the switch value if it has changed, to avoid loops
            if (switch_running.checked != started)
                switch_running.checked = started;

            // Set the visible property here because dismissing the message removes the binding
            if (value == WivrnServer.FailedToStart)
                message_failed_to_start.visible = true;

            if (!nxPreview && DashboardSettings.first_run && started) {
                console.log("First run");
                root.pageStack.push(Qt.createComponent("WizardPage.qml").createObject());
                DashboardSettings.first_run = false;
            }
        }

        function onPairingEnabledChanged(value) {
            if (switch_pairing.checked != WivrnServer.pairingEnabled)
                switch_pairing.checked = WivrnServer.pairingEnabled;
        }

        function onServerError(info) {
            console.log(info.where);
            console.log(info.message);
            messages.append(error_template.createObject(
                null,
                {
                    where: info.where,
                    message: info.message
                }
            ));
        }

        function onJsonConfigurationChanged(value) {
            Settings.load(WivrnServer);
        }
    }

    Connections {
        target: Avahi
        function onRunningChanged(value) {
            if (value && !nxPreview) {
                if (WivrnServer.serverStatus != WivrnServer.Started) {
                    WivrnServer.start_server();
                    message_failed_to_start.visible = false;
                }
            }
        }
    }

    Component.onCompleted: {
        if (!nxPreview && WivrnServer.serverStatus == WivrnServer.Stopped)
            WivrnServer.start_server();

        if (DashboardSettings.last_run_version != ApkInstaller.currentVersion) {
            DashboardSettings.last_run_version = ApkInstaller.currentVersion;
        }
    }

    globalDrawer: Kirigami.GlobalDrawer {
        title: "WiVRn NX"
        titleIcon: "qrc:/qml/wivrn-small.svg"
        modal: root.width < 1000
        drawerOpen: !modal
        width: 236
        actions: [
            Kirigami.Action { text: i18n("Overview"); icon.name: "go-home-symbolic"; checkable: true; checked: root.destination === 0; onTriggered: root.navigate("", 0) },
            Kirigami.Action { text: i18n("Streaming"); icon.name: "configure"; checkable: true; checked: root.destination === 1; onTriggered: root.navigate("SettingsPage.qml", 1) },
            Kirigami.Action { text: i18n("Body Tracking"); icon.name: "camera-web-symbolic"; checkable: true; checked: root.destination === 2; onTriggered: root.navigate("FusePage.qml", 2) },
            Kirigami.Action { text: i18n("Diagnostics"); icon.name: "help-contents"; checkable: true; checked: root.destination === 3; onTriggered: root.navigate("TroubleshootPage.qml", 3) },
            Kirigami.Action { separator: true },
            Kirigami.Action { text: i18n("Headsets"); icon.name: "network-wireless-symbolic"; enabled: root.server_started; onTriggered: root.navigate("HeadsetsPage.qml", 4) },
            Kirigami.Action { text: i18n("Headset mirror"); icon.name: "video-display-symbolic"; enabled: root.server_started; onTriggered: root.navigate("MirrorPage.qml", 5) },
            Kirigami.Action { text: i18n("Install headset app"); icon.name: "install-symbolic"; enabled: root.server_started && Adb.adbInstalled; onTriggered: root.navigate("ApkInstallPage.qml", 6) },
            Kirigami.Action { text: i18n("Setup assistant"); icon.name: "tools-wizard-symbolic"; enabled: root.server_started; onTriggered: root.navigate("WizardPage.qml", 7) },
            Kirigami.Action { text: i18n("About"); icon.name: "help-about"; onTriggered: root.navigate("About.qml", 8) }
        ]
        footer: ColumnLayout {
            spacing: 8
            Controls.Label { text: nxPreview ? i18n("DASHBOARD PREVIEW") : (root.server_started ? i18n("SERVER RUNNING") : i18n("SERVER OFFLINE")); color: nxPreview ? "#ffb300" : Kirigami.Theme.activeTextColor; font.pixelSize: 11; font.bold: true }
            Controls.Label { text: WivrnServer.headsetConnected ? WivrnServer.systemName : i18n("No headset connected"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor }
            Controls.Label { text: i18n("Fuse · research framework"); font.pixelSize: 11; color: Kirigami.Theme.disabledTextColor }
        }
    }

    pageStack.globalToolBar.showNavigationButtons: Kirigami.ApplicationHeaderStyle.NoNavigationButtons
    pageStack.defaultColumnWidth: width // Force non-wide mode
    Connections {
        target: root.pageStack
        function onDepthChanged() { if (root.pageStack.depth === 1) root.destination = 0; }
    }
    pageStack.interactive: false // Don't let the back/forward mouse button handle the pagestack

    pageStack.initialPage: Kirigami.ScrollablePage {
        title: i18n("Overview")
        // transparent over the NX nebula, stock look otherwise
        background: Rectangle {
            visible: !DashboardSettings.nx_theme
            color: Kirigami.Theme.backgroundColor
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 22
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 20
                Image { source: Qt.resolvedUrl("wivrn.svg"); Layout.preferredWidth: 72; Layout.preferredHeight: 72; fillMode: Image.PreserveAspectFit; Accessible.name: "WiVRn NX" }
                ColumnLayout {
                    Layout.fillWidth: true
                    Kirigami.Heading { text: i18n("Your space. Connected."); font.pixelSize: 30; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Controls.Label { text: i18n("Streaming, devices and body tracking in one place."); color: Kirigami.Theme.disabledTextColor; Layout.fillWidth: true; wrapMode: Text.Wrap }
                }
            }
            Kirigami.InlineMessage { visible: nxPreview; Layout.fillWidth: true; type: Kirigami.MessageType.Information; text: i18n("Preview mode — VR server startup and attachment are disabled. Fuse simulation remains available.") }
            Repeater{
                model: ObjectModel {
                    id: messages

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: i18n("Avahi daemon is not installed")
                        type: Kirigami.MessageType.Warning
                        showCloseButton: true
                        visible: DashboardSettings.show_system_checks && !Avahi.installed && !Avahi.running
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: i18n("Avahi daemon is not started")
                        type: Kirigami.MessageType.Warning
                        showCloseButton: true
                        visible: DashboardSettings.show_system_checks && Avahi.installed && !Avahi.running
                        actions: [
                            Kirigami.Action {
                                visible: Avahi.canStart
                                text: i18n("Fix it")
                                onTriggered: Avahi.start()
                            }
                        ]
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: i18n("Firewall may not allow port 9757")
                        type: Kirigami.MessageType.Warning
                        showCloseButton: true
                        visible: DashboardSettings.show_system_checks && Firewall.needSetup && Settings.port == Settings.default_port
                        actions: [
                            Kirigami.Action {
                                text: i18n("Fix it")
                                onTriggered: Firewall.doSetup()
                            }
                        ]
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: i18n("NVIDIA GPUs require at least driver version 565.77, current version is %1", VulkanInfo.driverVersion)
                        type: Kirigami.MessageType.Error
                        showCloseButton: true
                        visible: VulkanInfo.driverId == "NvidiaProprietary" && VulkanInfo.driverVersionCode < 2371043328 /* Version 565.77 */
                        actions: [
                            Kirigami.Action {
                                text: i18n("More info")
                                onTriggered: Qt.openUrlExternally("https://github.com/WiVRn/WiVRn/issues/180")
                            }
                        ]
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: Settings.flatpak ? i18n("Vulkan drivers cannot be found, you may need to run \"flatpak update\"") : i18n("Vulkan drivers cannot be found")
                        type: Kirigami.MessageType.Warning
                        showCloseButton: true
                        visible: DashboardSettings.show_system_checks && (VulkanInfo.type == VulkanInfo.SoftGPU || VulkanInfo.type == VulkanInfo.NoGPU)
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: i18n("Use the Mesa (radv) driver for AMD GPUs\nHardware encoding with vaapi does not work with AMDVLK and AMDGPU-PRO")
                        type: Kirigami.MessageType.Warning
                        showCloseButton: true
                        visible: VulkanInfo.driverId == "AmdProprietary" || VulkanInfo.driverId == "AmdOpenSource"
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: i18n("No OpenVR compatibility detected, Steam games won't be able to load VR.\nInstall xrizer or OpenComposite.")
                        type: Kirigami.MessageType.Warning
                        showCloseButton: true
                        visible: WivrnServer.openVRCompat.length == 0 && Settings.openvr == ""
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: i18n("Steam is installed as a snap. Snaps are not compatible with WiVRn.")
                        type: Kirigami.MessageType.Warning
                        showCloseButton: true
                        visible: DashboardSettings.show_system_checks && Steam.snap
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: i18n("Steam is installed as a flatpak but does not have sufficient permissions.")
                        type: Kirigami.MessageType.Warning
                        showCloseButton: true
                        visible: DashboardSettings.show_system_checks && Steam.flatpakNeedPerm
                        actions: [
                            Kirigami.Action {
                                text: i18n("Fix it")
                                onTriggered: Steam.fixFlatpakPerm()
                            }
                        ]
                    }

                    Kirigami.InlineMessage {
                        id: config_restart
                        Layout.fillWidth: true
                        text: i18n("Restart the server for configuration changes to take effect")
                        type: Kirigami.MessageType.Information
                        showCloseButton: true
                        visible: false
                        actions: [
                            Kirigami.Action {
                                text: i18nc("restart the server", "Restart now")
                                onTriggered: {
                                    WivrnServer.restart_server();
                                    config_restart.visible = false;
                                }
                            }
                        ]
                        Connections {
                            target: Settings
                            function onSettingsChanged() {
                                if (WivrnServer.sessionRunning)
                                    config_restart.visible = true;
                            }
                        }
                        Connections {
                            target: WivrnServer
                            function onServerStatusChanged(value) {
                                config_restart.visible = false;
                            }
                        }
                    }

                    Kirigami.InlineMessage {
                        id: message_failed_to_start
                        visible: false
                        Layout.fillWidth: true
                        text: i18n("Server failed to start")
                        type: Kirigami.MessageType.Error
                        showCloseButton: true
                        // visible: WivrnServer.serverStatus == WivrnServer.FailedToStart
                        actions: [
                            Kirigami.Action {
                                text: i18n("Open server logs")
                                onTriggered: WivrnServer.open_server_logs()
                            }
                        ]
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width > 720 ? 2 : 1
                columnSpacing: 20
                rowSpacing: 20
                Controls.Frame {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 450
                    Layout.alignment: Qt.AlignTop
                    padding: 22
                    background: Rectangle { radius: 6; color: DashboardSettings.nx_theme ? "#0c0818" : Kirigami.Theme.alternateBackgroundColor }
                    contentItem: ColumnLayout {
                        spacing: 14
                        Controls.Label { text: i18n("STREAMING SERVICE"); font.pixelSize: 11; font.bold: true; color: Kirigami.Theme.disabledTextColor }
                        Kirigami.Heading { text: root.server_started ? i18n("Ready for your headset") : i18n("Start your next session"); level: 2; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        Controls.Label { text: nxPreview ? i18n("Server controls are disabled in this isolated preview.") : i18n("Run the service, then connect from WiVRn on your headset."); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor }
                        Controls.Switch {
                            id: switch_running
                            text: i18n("VR streaming service")
                            checked: root.server_started
                            enabled: !nxPreview
                            onClicked: {
                                if (checked && !root.server_started) WivrnServer.start_server();
                                else if (!checked && root.server_started) WivrnServer.stop_server();
                            }
                        }
                        RowLayout {
                            Controls.Switch {
                                id: switch_pairing
                                text: i18n("Allow headset pairing")
                                enabled: root.server_started
                                checked: WivrnServer.pairingEnabled
                                onClicked: {
                                    if (checked && !WivrnServer.pairingEnabled) WivrnServer.enable_pairing();
                                    else if (!checked && WivrnServer.pairingEnabled) WivrnServer.disable_pairing();
                                }
                            }
                            Controls.Label { text: WivrnServer.pin; visible: WivrnServer.pairingEnabled; font.bold: true; color: Kirigami.Theme.activeTextColor }
                        }
                    }
                }
                Controls.Frame {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 450
                    Layout.alignment: Qt.AlignTop
                    padding: 22
                    background: Rectangle { radius: 6; color: DashboardSettings.nx_theme ? "#0c0818" : Kirigami.Theme.alternateBackgroundColor }
                    contentItem: ColumnLayout {
                        spacing: 14
                        Controls.Label { text: i18n("HEADSET CONNECTION"); font.pixelSize: 11; font.bold: true; color: Kirigami.Theme.disabledTextColor }
                        Kirigami.Heading { text: WivrnServer.headsetConnected ? WivrnServer.systemName : i18n("Your headset goes here"); level: 2; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        Controls.Label { text: WivrnServer.headsetConnected ? i18n("Connected to WiVRn NX.") : i18n("Connect over Wi-Fi, or use a wired USB connection."); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor }
                        Controls.Label {
                            text: !Adb.adbInstalled ? i18n("USB setup needs ADB.") : select_usb_device.connected_headset_count > 0 ? i18n("USB headset detected") : i18n("No USB headset detected")
                            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor
                        }
                        Controls.Button {
                            text: i18n("Connect with USB")
                            enabled: root.server_started && Adb.adbInstalled && select_usb_device.connected_headset_count > 0 && !WivrnServer.headsetConnected
                            onClicked: select_usb_device.connect()
                        }
                        Controls.Button { text: i18n("Disconnect headset"); visible: WivrnServer.headsetConnected; onClicked: WivrnServer.disconnect_headset() }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: fuseTeaser.implicitHeight + 40
                color: DashboardSettings.nx_theme ? "#120c22" : Kirigami.Theme.alternateBackgroundColor
                radius: 6
                border.color: DashboardSettings.nx_theme ? "#392257" : Kirigami.Theme.disabledTextColor
                ColumnLayout {
                    id: fuseTeaser
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 10
                    Controls.Label { text: i18n("BODY TRACKING / NX FUSE"); color: "#b78aff"; font.pixelSize: 11; font.bold: true }
                    Kirigami.Heading { text: i18n("A second perspective."); level: 2 }
                    Controls.Label { text: i18n("Explore camera assistance and camera-only body tracking in the native simulation lab. Live camera estimation and VR output are still in development."); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor }
                    Controls.Button { text: i18n("Open Body Tracking"); highlighted: true; onClicked: root.navigate("FusePage.qml", 2) }
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                visible: steam_info.visible
            }

            Kirigami.Heading {
                level: 1
                type: Kirigami.Heading.Type.Primary
                wrapMode: Text.WordWrap
                text: i18n("Steam information")
                visible: steam_info.visible
            }
            SteamLaunchOptions {
                id: steam_info
                visible: root.server_started && WivrnServer.steamCommand != ""
            }

            Item {
                // spacer item
                Layout.fillHeight: true
            }
        }

        footer: Controls.Label {
            text: i18n("Version %1", ApkInstaller.currentVersion)
            horizontalAlignment: Text.AlignHCenter
            opacity: 0.6
            padding: Kirigami.Units.smallSpacing
        }
    }
}
