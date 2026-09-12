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
    Controls.Switch {
        text: i18n("Estimate body from this camera")
        checked: root.estimating
        enabled: root.streaming && !FuseService.busy
        onClicked: FuseService.setEstimation(root.device.id, checked)
    }
    RowLayout {
        Layout.fillWidth: true
        Controls.ComboBox {
            id: selection
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            model: FuseService.devices
            textRole: "name"
            valueRole: "id"
            Accessible.name: i18n("Camera to preview")
        }
        Controls.Button {
            text: root.streaming ? i18n("Stop camera") : i18n("Start camera")
            enabled: FuseService.connected && !!root.device.id && !FuseService.busy
            onClicked: FuseService.setCamera(root.device.id, !root.streaming)
        }
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
            source: root.visible && root.streaming && !root.device.frame_stale && root.device.sequence > 0
                    ? FuseService.serviceUrl + "/api/camera/frame?id=" + encodeURIComponent(root.device.id) + "&sequence=" + root.device.sequence : ""
            Accessible.name: root.estimating ? i18n("Live local camera image with estimated pose overlay") : i18n("Live local camera image")
        }
        Controls.Label {
            anchors.centerIn: parent
            width: parent.width - 32
            visible: frame.status !== Image.Ready
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#b6a9d2"
            text: root.streaming ? i18n("Waiting for a camera frame…") : i18n("Camera stopped")
        }
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
    Controls.Label {
        Layout.fillWidth: true
        visible: FuseService.devices.length > 1
        text: i18n("Each camera has its own start/stop control. Switching the preview keeps other started cameras running.")
        color: "#b6a9d2"
        wrapMode: Text.WordWrap
    }
}
