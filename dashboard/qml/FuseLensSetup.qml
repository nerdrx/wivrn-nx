pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import io.github.wivrn.wivrn

ColumnLayout {
    id: root
    property string cameraId: ""
    property bool cameraReady: false
    readonly property var calibration: FuseService.calibration || ({})
    spacing: 8

    Controls.Label {
        Layout.fillWidth: true
        text: i18n("Lens setup")
        color: "#efeaff"
        font.pixelSize: 16
        font.weight: Font.DemiBold
    }
    Controls.Label {
        Layout.fillWidth: true
        text: i18n("Intrinsic camera calibration only. Results do not define a VR-space transform or body tracking.")
        color: "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    Controls.AbstractButton {
        id: toggle
        Layout.fillWidth: true
        checkable: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: i18n("Lens calibration setup")
        Accessible.description: checked ? i18n("Expanded. Activate to collapse.") : i18n("Collapsed. Activate to expand.")
        Keys.onReturnPressed: event => { toggle.toggle(); event.accepted = true }
        Keys.onEnterPressed: event => { toggle.toggle(); event.accepted = true }
        contentItem: RowLayout {
            Controls.Label {
                Layout.fillWidth: true
                text: toggle.checked ? i18n("Hide calibration controls") : i18n("Show calibration controls")
                color: "#bc91ff"
            }
            Controls.Label { text: toggle.checked ? "⌃" : "⌄"; color: "#bc91ff"; Accessible.ignored: true }
        }
    }
    ColumnLayout {
        visible: toggle.checked
        Layout.fillWidth: true
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Controls.Label { text: i18n("Board"); color: "#b6a9d2" }
            Controls.SpinBox { id: innerCols; from: 2; to: 15; value: 9; editable: true; Accessible.name: i18n("Inner corners across") }
            Controls.SpinBox { id: innerRows; from: 2; to: 15; value: 6; editable: true; Accessible.name: i18n("Inner corners down") }
            Controls.SpinBox { id: square; from: 5; to: 100; value: 25; editable: true; Accessible.name: i18n("Square size in millimetres") }
            Controls.Label { text: i18n("mm"); color: "#b6a9d2" }
        }
        Controls.Label {
            Layout.fillWidth: true
            text: i18n("Print landscape at 100% scale and measure the 25 mm squares. The supplied board has 9 × 6 inner corners; match these fields if using another board.")
            color: "#b6a9d2"
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Controls.Button {
                text: i18n("Open printable board")
                onClicked: Qt.openUrlExternally(FuseService.serviceUrl + "/api/calibration/board")
                Accessible.name: i18n("Open printable calibration board")
            }
            Controls.Button {
                text: i18n("Capture board view")
                enabled: root.cameraReady && !root.calibration.busy && !FuseService.busy
                onClicked: FuseService.calibrationCommand("capture", root.cameraId, innerCols.value, innerRows.value, square.value)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Controls.Button {
                text: i18n("Solve (%1/6)", Number(root.calibration.count || 0))
                enabled: root.cameraReady && Number(root.calibration.count || 0) >= 6 && !root.calibration.busy && !FuseService.busy
                onClicked: FuseService.calibrationCommand("solve", root.cameraId, innerCols.value, innerRows.value, square.value)
            }
            Controls.Button {
                text: i18n("Reset")
                enabled: !root.calibration.busy && !FuseService.busy
                onClicked: FuseService.calibrationCommand("reset", root.cameraId, innerCols.value, innerRows.value, square.value)
            }
            Controls.Label {
                Layout.fillWidth: true
                text: root.calibration.busy ? i18n("Working…") : (root.calibration.error || "")
                color: root.calibration.error ? "#ffcd80" : "#b6a9d2"
                wrapMode: Text.WordWrap
            }
        }
        Controls.Label {
            Layout.fillWidth: true
            visible: !!root.calibration.profile
            text: root.calibration.profile
                   ? i18n("Held-out RMS: %1 px · intrinsics only · review held-out error before use", Number(root.calibration.profile.held_out_rms_px || 0).toFixed(2))
                   : ""
            color: root.calibration.profile && root.calibration.profile.validated ? "#73ecda" : "#ffcd80"
            wrapMode: Text.WordWrap
        }
        Controls.Button {
            visible: !!root.calibration.profile && !!root.calibration.profile.validated
            text: i18n("Download calibration profile")
            onClicked: Qt.openUrlExternally(FuseService.serviceUrl + "/api/calibration/profile")
        }
    }
}
