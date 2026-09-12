pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import io.github.wivrn.wivrn

ColumnLayout {
    id: root
    spacing: 12
    readonly property var device: selection.currentIndex >= 0 ? (FuseService.devices[selection.currentIndex] || {}) : ({})
    readonly property bool streaming: FuseService.connected && !!device.streaming
    readonly property bool estimating: streaming && !!device.estimation
    property bool settingsExpanded: false
    property bool syncingSettings: false
    property bool settingsDirty: false
    property int draftResolutionIndex: 1
    property int draftFpsIndex: 0
    readonly property var resolutionChoices: [i18n("320 × 240"), i18n("640 × 480"), i18n("1280 × 720")]
    readonly property var resolutionWidths: [320, 640, 1280]
    readonly property var resolutionHeights: [240, 480, 720]
    readonly property var fpsChoices: [15, 30, 60]
    function syncSettings() {
        const s = root.device.settings || ({})
        root.syncingSettings = true
        root.draftResolutionIndex = Math.max(0, root.resolutionWidths.findIndex(function(width, index) {
            return Number(width) === Number(s.width || 640) && root.resolutionHeights[index] === Number(s.height || 480)
        }))
        root.draftFpsIndex = Math.max(0, root.fpsChoices.indexOf(Number(s.fps || 15)))
        root.settingsDirty = false
        root.syncingSettings = false
    }
    Controls.Label {
        text: i18n("Camera preview")
        color: "#efeaff"
        font.pixelSize: 16
        font.weight: Font.DemiBold
    }
    Controls.Label {
        Layout.fillWidth: true
        text: i18n("Start a camera explicitly to view it here. Images stay local and are not recorded. Camera video does not drive the simulated VR skeleton.")
        color: "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    Controls.Label {
        Layout.fillWidth: true
        text: i18n("1 Camera → 2 Automatic alignment → 3 Body preview → 4 Output")
        color: "#73ecda"
        wrapMode: Text.WordWrap
    }
    Controls.Switch {
        text: i18n("Estimate body from this camera")
        checked: root.estimating
        enabled: root.streaming
        onClicked: FuseService.setEstimation(root.device.id, checked)
    }
    RowLayout {
        Layout.fillWidth: true
        Controls.ComboBox {
            id: selection
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            model: FuseService.cameraChoices
            textRole: "name"
            valueRole: "id"
            Accessible.name: i18n("Camera to preview")
        }
        Controls.Button {
            text: root.streaming ? i18n("Stop camera") : i18n("Start camera")
            enabled: FuseService.connected && !!root.device.id
            onClicked: FuseService.setCamera(root.device.id, !root.streaming)
        }
    }
    Controls.AbstractButton {
        Layout.fillWidth: true
        checkable: true
        checked: root.settingsExpanded
        focusPolicy: Qt.StrongFocus
        Accessible.name: i18n("Camera settings")
        Accessible.description: checked ? i18n("Expanded. Activate to collapse.") : i18n("Collapsed. Activate to expand.")
        onToggled: root.settingsExpanded = checked
        contentItem: RowLayout {
            Controls.Label { Layout.fillWidth: true; text: root.settingsExpanded ? i18n("Hide camera settings") : i18n("Show camera settings"); color: "#bc91ff" }
            Controls.Label { text: root.settingsExpanded ? "⌃" : "⌄"; color: "#bc91ff"; Accessible.ignored: true }
        }
    }
    RowLayout {
        visible: root.settingsExpanded
        Layout.fillWidth: true
        Controls.ComboBox {
            id: widthBox
            Layout.fillWidth: true
            model: root.resolutionChoices
            currentIndex: root.draftResolutionIndex
            onCurrentIndexChanged: if (!root.syncingSettings) root.settingsDirty = true
            Accessible.name: i18n("Camera resolution")
        }
        Controls.ComboBox {
            id: fpsBox
            Layout.fillWidth: true
            model: root.fpsChoices
            currentIndex: root.draftFpsIndex
            onCurrentIndexChanged: if (!root.syncingSettings) root.settingsDirty = true
            Accessible.name: i18n("Camera frame rate")
        }
        Controls.Button {
            text: i18n("Apply")
            enabled: FuseService.connected && !!root.device.id && !root.streaming && root.settingsDirty
            onClicked: {
                FuseService.setCameraSettings(root.device.id, root.resolutionWidths[widthBox.currentIndex], root.resolutionHeights[widthBox.currentIndex], root.fpsChoices[fpsBox.currentIndex])
                root.settingsDirty = false
            }
            Accessible.name: i18n("Apply camera settings")
        }
    }
    Controls.Label {
        visible: root.settingsExpanded
        Layout.fillWidth: true
        text: i18n("Settings apply while camera is stopped. Next start may negotiate a different actual size or rate.")
        color: "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(160, Math.min(width * 0.5625, 360))
        color: "#050309"
        radius: 6
        clip: true
        Image {
            id: frame
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            property bool hadFrame: false
            property bool retainsFrames: false
            Component.onCompleted: {
                // Qt 6.8+ can keep the displayed image while the next JPEG loads.
                if ("retainWhileLoading" in frame) {
                    frame["retainWhileLoading"] = true
                    retainsFrames = true
                }
            }
            property string frameCameraId: ""
            onSourceChanged: {
                if (source.toString() === "" || !root.streaming || root.device.id !== frameCameraId) {
                    hadFrame = false
                    frameCameraId = root.device.id || ""
                }
            }
            onStatusChanged: if (status === Image.Ready) hadFrame = true
            source: root.visible && root.streaming && !root.device.frame_stale && Number(root.device.sequence) >= 0
                    ? FuseService.serviceUrl + "/api/camera/frame?id=" + encodeURIComponent(root.device.id) + "&sequence=" + root.device.sequence : ""
            Accessible.name: root.estimating ? i18n("Live local camera image with estimated pose overlay") : i18n("Live local camera image")
        }
        Controls.Label {
            anchors.centerIn: parent
            width: parent.width - 32
            visible: frame.status !== Image.Ready && !(frame.status === Image.Loading && frame.hadFrame && frame.retainsFrames)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#b6a9d2"
            text: root.streaming ? i18n("Waiting for a camera frame…") : i18n("Camera stopped")
        }
    }
    Controls.Label {
        visible: root.streaming && !!root.device.image_size && root.device.image_size.length === 2
        Layout.fillWidth: true
        text: visible ? i18n("Actual output: %1 × %2", root.device.image_size[0], root.device.image_size[1]) : ""
        color: "#b6a9d2"
    }
    Controls.Label {
        Layout.fillWidth: true
        text: root.device.error || root.device.estimate_error || FuseService.cameraError || (root.device.frame_stale ? i18n("Camera stopped delivering fresh frames. Stop and restart it to retry.") : (root.streaming ? (root.estimating ? i18n("Live video · inferred body · no VR output") : i18n("Live video · pose estimation off")) : i18n("No camera image is being displayed.")))
        color: root.device.error || root.device.estimate_error || FuseService.cameraError ? "#ffcd80" : "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    FuseEstimateView {
        Layout.fillWidth: true
        visible: root.estimating
        estimate: root.device.estimate || ({})
    }
    FuseLensSetup {
        Layout.fillWidth: true
        cameraId: root.device.id || ""
        cameraReady: root.streaming && !root.estimating
    }
    FuseAlignmentSetup {
        Layout.fillWidth: true
        cameraId: root.device.id || ""
        cameraReady: root.streaming
        estimating: root.estimating
    }
    FuseShadowView {
        Layout.fillWidth: true
        cameraId: root.device.id || ""
        cameraReady: root.streaming
        estimating: root.estimating
    }
    Connections {
        target: selection
        function onCurrentIndexChanged() { root.syncSettings() }
    }
    Component.onCompleted: root.syncSettings()
    Controls.Label {
        Layout.fillWidth: true
        visible: FuseService.devices.length > 1
        text: i18n("Each camera has its own start/stop control. Switching the preview keeps other started cameras running.")
        color: "#b6a9d2"
        wrapMode: Text.WordWrap
    }
}
