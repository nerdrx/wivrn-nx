pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import io.github.wivrn.wivrn

ColumnLayout {
    id: root
    property string cameraId: ""
    property bool cameraReady: false
    property bool estimating: false
    property bool expanded: false
    readonly property var shadow: FuseService.shadow || ({})
    readonly property var calibration: FuseService.calibration || ({})
    readonly property var alignment: FuseService.alignment || ({})
    readonly property var joints: shadow.joints || []
    readonly property var profile: alignment.profile || ({})
    readonly property var lensProfile: calibration.profile || ({})
    readonly property bool profileReady: !!profile.validated && !!lensProfile.validated &&
        (!profile.camera_id || profile.camera_id === root.cameraId) &&
        (!lensProfile.camera_id || lensProfile.camera_id === root.cameraId)
    readonly property bool usable: root.cameraReady && root.estimating && root.profileReady && !!root.cameraId
    spacing: 8

    Controls.Label {
        Layout.fillWidth: true
        text: i18n("Shadow body preview")
        color: "#efeaff"
        font.pixelSize: 16
        font.weight: Font.DemiBold
    }
    Controls.AbstractButton {
        Layout.fillWidth: true
        checkable: true
        checked: root.expanded
        Accessible.name: i18n("Shadow body preview")
        Accessible.description: checked ? i18n("Expanded. Activate to collapse.") : i18n("Collapsed. Activate to expand.")
        onToggled: root.expanded = checked
        focusPolicy: Qt.StrongFocus
        Keys.onReturnPressed: (event) => { toggle(); event.accepted = true }
        Keys.onEnterPressed: (event) => { toggle(); event.accepted = true }
        contentItem: RowLayout {
            Controls.Label { Layout.fillWidth: true; text: root.expanded ? i18n("Hide shadow preview") : i18n("Show shadow preview"); color: "#bc91ff" }
            Controls.Label { text: root.expanded ? "⌃" : "⌄"; color: "#bc91ff"; Accessible.ignored: true }
        }
    }
    Controls.Switch {
        visible: root.expanded
        text: i18n("Preview anchored body · no VR output")
        checked: !!root.shadow.enabled
        enabled: (root.shadow.enabled || root.usable) && !FuseService.busy
        onClicked: FuseService.setShadow(root.shadow.camera || root.cameraId, checked)
        Accessible.name: i18n("Enable anchored shadow body preview")
    }
    Controls.Label {
        visible: root.expanded
        Layout.fillWidth: true
        text: !root.cameraReady ? i18n("Start this camera to enable the preview.")
              : !root.estimating ? i18n("Enable camera body estimation to enable the preview.")
              : !root.profileReady ? i18n("Validate alignment for this camera before enabling the preview.")
              : root.shadow.reason || (root.shadow.note || i18n("Shadow preview is ready."))
        color: root.shadow.reason ? "#ffcd80" : "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    RowLayout {
        visible: root.expanded
        Layout.fillWidth: true
        Controls.Label { Layout.fillWidth: true; text: i18n("Front / side projection · same aligned VR space"); color: "#b6a9d2"; wrapMode: Text.WordWrap }
        Controls.ComboBox { id: direction; model: [i18n("Front"), i18n("Side")]; Accessible.name: i18n("Shadow body viewing direction"); onCurrentIndexChanged: drawing.requestPaint() }
    }
    Canvas {
        id: drawing
        visible: root.expanded
        Layout.fillWidth: true
        Layout.preferredHeight: 260
        onWidthChanged: requestPaint(); onHeightChanged: requestPaint()
        Connections { target: root; function onJointsChanged() { drawing.requestPaint() } }
        onPaint: {
            const c = getContext("2d"); c.reset(); c.fillStyle = "#050309"; c.fillRect(0, 0, width, height);
            const valid = root.joints.filter(function(j) { return j.position && j.position.length === 3; });
            if (!valid.length) return;
            let lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
            valid.forEach(function(j) { [j.position, j.baseline].forEach(function(p) { if (!p || p.length !== 3) return; for (let k=0;k<3;k++) { lo[k]=Math.min(lo[k],p[k]); hi[k]=Math.max(hi[k],p[k]); } }); });
            const axis = direction.currentIndex === 1 ? 2 : 0;
            const span = Math.max(0.5, hi[axis]-lo[axis], hi[1]-lo[1]);
            const scale = Math.min(width * 0.72, height * 0.78) / span;
            function point(p) { return [width/2 + (p[axis]-(lo[axis]+hi[axis])/2)*scale, height*0.84-(p[1]-lo[1])*scale]; }
            const links = [["hip","left_knee"],["left_knee","left_foot"],["hip","right_knee"],["right_knee","right_foot"],["hip","left_elbow"],["hip","right_elbow"]];
            function find(name) { return valid.find(function(j) { return j.joint === name; }); }
            function draw(field, color, dashed) {
                c.strokeStyle=color; c.fillStyle=color; c.lineWidth=field === "position" ? 3 : 2;
                links.forEach(function(link) { const a=find(link[0]),b=find(link[1]); if (!a||!b||!a[field]||!b[field]) return; const pa=point(a[field]),pb=point(b[field]); c.beginPath(); c.moveTo(pa[0],pa[1]); c.lineTo(pb[0],pb[1]); c.stroke(); });
                valid.forEach(function(j) { if (!j[field]) return; const p=point(j[field]); c.beginPath(); c.arc(p[0],p[1],field === "position" ? 4 : 3,0,Math.PI*2); c.fill(); });
            }
            draw("baseline", "#ffcd80", true); draw("position", "#00e5ff", false);
        }
        Controls.Label { anchors.centerIn: parent; visible: !root.joints.length; text: i18n("No fresh shadow joints"); color: "#b6a9d2" }
        RowLayout {
            anchors { left: parent.left; top: parent.top; margins: 8 }
            spacing: 12
            Controls.Label { text: i18n("Estimated"); color: "#00e5ff" }
            Controls.Label { text: i18n("Raw tracked"); color: "#ffcd80" }
        }
    }
    Controls.Label {
        visible: root.expanded
        Layout.fillWidth: true
        text: root.shadow.available && root.shadow.metrics ? i18n("RMS %1 px · anchor shift %2 cm · age %3 ms", Number(root.shadow.metrics.rms_px).toFixed(1), (Number(root.shadow.metrics.anchor_shift_m)*100).toFixed(1), Math.round(Number(root.shadow.metrics.age_ms))) : i18n("Metrics unavailable")
        color: "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    Controls.Label { visible: root.expanded; Layout.fillWidth: true; text: i18n("Inferred depth is uncertain. Ear midpoint gives approximate HMD translation."); color: "#ffcd80"; wrapMode: Text.WordWrap }
    RowLayout {
        visible: root.expanded
        Layout.fillWidth: true
        Controls.Button {
            text: root.shadow.recording ? i18n("Stop pose log") : i18n("Start pose log")
            enabled: (root.shadow.recording || (!!root.shadow.enabled && Number(root.shadow.recorded_frames || 0) < 600)) && !FuseService.busy
            onClicked: FuseService.shadowRecordingCommand(root.shadow.recording ? "stop" : "start")
            Accessible.name: i18n("Shadow pose recording")
        }
        Controls.Button {
            text: i18n("Clear")
            enabled: Number(root.shadow.recorded_frames || 0) > 0 && !FuseService.busy
            onClicked: FuseService.shadowRecordingCommand("clear")
        }
        Controls.Button {
            text: i18n("Download pose log")
            enabled: Number(root.shadow.recorded_frames || 0) > 0
            onClicked: Qt.openUrlExternally(FuseService.serviceUrl + "/api/shadow/recording")
        }
        Controls.Label {
            Layout.fillWidth: true
            text: i18n("%1/600 frames · no camera images", Number(root.shadow.recorded_frames || 0))
            color: "#b6a9d2"
            wrapMode: Text.WordWrap
        }
    }
    Controls.Label { visible: root.expanded; Layout.fillWidth: true; text: i18n("Pose log stores memory-only landmarks. Residual is a difference, not an accuracy score."); color: "#b6a9d2"; wrapMode: Text.WordWrap }
    ColumnLayout {
        visible: root.expanded
        Layout.fillWidth: true
        RowLayout {
            Layout.fillWidth: true
            Controls.Label { Layout.preferredWidth: root.width * .44; text: i18n("Joint"); color: "#b6a9d2" }
            Controls.Label { Layout.preferredWidth: root.width * .27; text: i18n("Residual (cm)"); color: "#b6a9d2" }
            Controls.Label { Layout.fillWidth: true; text: i18n("Visibility"); color: "#b6a9d2" }
        }
        Repeater {
            model: root.joints
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                Controls.Label { Layout.preferredWidth: root.width * .44; text: (modelData.joint || "").replace(/_/g," "); color: "#efeaff" }
                Controls.Label { Layout.preferredWidth: root.width * .27; text: modelData.residual_m == null ? i18n("—") : Number(modelData.residual_m*100).toFixed(1); color: "#00e5ff" }
                Controls.Label { Layout.fillWidth: true; text: modelData.confidence == null ? i18n("—") : i18n("%1%", Math.round(Number(modelData.confidence)*100)); color: "#b6a9d2" }
            }
        }
    }
}
