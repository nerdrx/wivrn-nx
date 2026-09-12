pragma ComponentBehavior: Bound
import QtQml
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import io.github.wivrn.wivrn

Kirigami.ScrollablePage {
    id: root
    title: i18n("NX Body Tracking")
    Component.onCompleted: FuseService.connectService()
    Shortcut { sequence: "Ctrl+D"; enabled: root.isCurrentPage; onActivated: { debugWindow.show(); debugWindow.raise(); debugWindow.requestActivate(); } }
    readonly property var pose: FuseService.connected ? FuseService.state : ({})
    readonly property var observations: pose.observations || []
    readonly property bool cameraOnly: !!pose.camera_only
    readonly property bool wide: width > 850
    readonly property color ink: "#efeaff"
    readonly property color muted: "#b6a9d2"
    background: Rectangle { color: "#000000" }

    function sourceName() {
        if (!FuseService.connected) return i18n("Disconnected");
        var body=Object.keys(root.pose.joints || {}).filter(function(j) { return ["head","left_hand","right_hand"].indexOf(j)<0; });
        if(root.cameraOnly) return body.length ? i18n("Camera only") : i18n("Body unavailable");
        var assisting=Object.values(root.pose.joints || {}).some(function(j) { return j.weight > 0.01; });
        return assisting ? i18n("Pico + camera assistance") : i18n("Pico baseline");
    }
    function availability(o) {
        if (["head","left_hand","right_hand"].indexOf(o.joint)>=0) return i18n("VR anchor");
        return !root.cameraOnly && !root.pose.enabled ? i18n("Assistance off") : i18n("Observed");
    }
    component Copy: Controls.Label {
        color: root.muted
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        font.pixelSize: 12
    }
    component Panel: Controls.Frame {
        padding: 20
        background: Rectangle {
            radius: 6
            gradient: Gradient {
                GradientStop { position: 0; color: "#171022" }
                GradientStop { position: 1; color: "#090610" }
            }
        }
    }
    component SectionTitle: Controls.Label {
        color: root.ink
        font.pixelSize: 16
        font.weight: Font.DemiBold
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
    component DebugContent: ColumnLayout {
        spacing: 14
        FuseCameraView { Layout.fillWidth: true }
        SectionTitle { text: i18n("Synthetic camera input") }
        Copy { text: i18n("This projection is synthetic and independent of the camera estimates above.") }
        FuseSkeleton { Layout.fillWidth: true; Layout.preferredHeight: 300; poseData: root.pose; connected: FuseService.connected; observationsOnly: true }
        Copy { text: i18n("Input observations → final body output. Z is a synthetic world coordinate in meters, not measured camera depth. Age uses the simulation clock.") }
        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 16
            rowSpacing: 8
            Repeater {
                model: [i18n("Joint / input"),i18n("Z (m)"),i18n("Confidence / age"),i18n("Availability")]
                delegate: Controls.Label { required property string modelData; text: modelData; color: root.muted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            }
            Repeater {
                model: root.observations
                delegate: RowLayout {
                    id: observationRow
                    required property var modelData
                    Layout.columnSpan: 4
                    Layout.fillWidth: true
                    spacing: 16
                    Controls.Label { Layout.fillWidth: true; Layout.preferredWidth: 140; text: observationRow.modelData.joint.replace(/_/g," "); color: root.ink; font.pixelSize: 11; wrapMode: Text.WordWrap }
                    Controls.Label { Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 60; text: Number(observationRow.modelData.position[2]).toFixed(3); color: root.muted; font.pixelSize: 11 }
                    Controls.Label { Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 125; text: Math.round(observationRow.modelData.confidence*100)+"% · "+Math.round((root.pose.time-observationRow.modelData.timestamp)*1000)+" ms"; color: root.muted; font.pixelSize: 11 }
                    Controls.Label { Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 110; text: root.availability(observationRow.modelData); color: "#00e5ff"; font.pixelSize: 11; wrapMode: Text.WordWrap }
                }
            }
        }
        Copy { visible: !root.observations.length; text: i18n("No input observations. Hidden body joints are unavailable in camera-only mode.") }
        Copy { text: i18n("Availability describes received inputs, not fusion acceptance. Real camera capture, calibrated pose estimation, and VR output are separate milestones.") }
    }

    ColumnLayout {
        spacing: 20
        Controls.Label { text: i18n("BODY TRACKING LAB / OBSERVATION + SIMULATION"); color: "#bc91ff"; font.pixelSize: 11; font.letterSpacing: 1.6; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Controls.Label { text: i18n("A little more grounded."); color: root.ink; font.pixelSize: 30; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Copy { text: i18n("Inspect cameras and WiVRn tracking, calibrate camera lenses, and test assisted or camera-only fusion in a separate simulation.") }
        Flow {
            Layout.fillWidth: true
            spacing: 10
            Controls.Button { text: i18n("Connect to Fuse"); enabled: !FuseService.busy; onClicked: FuseService.connectService() }
            Controls.Button { text: FuseService.running ? i18n("Worker running") : i18n("Start local worker"); enabled: FuseService.workerAvailable && !FuseService.running && !FuseService.busy; onClicked: FuseService.startWorker() }
            Controls.Button { text: FuseService.running ? i18n("Stop worker") : i18n("Disconnect from Fuse"); enabled: (FuseService.running || FuseService.connected) && !FuseService.busy; onClicked: FuseService.stopWorker() }
        }
        Copy { text: FuseService.connected ? i18n("Connected to Fuse · observation tools ready · fusion output remains simulated") : i18n("Disconnected. Start the local worker or connect to an existing instance."); color: FuseService.connected ? "#00e5ff" : "#ffcd80" }
        Kirigami.InlineMessage { Layout.fillWidth: true; visible: FuseService.error.length > 0; text: FuseService.error; type: Kirigami.MessageType.Error }
        Panel {
            Layout.fillWidth: true
            contentItem: FuseTrackingView {}
        }
        GridLayout {
            Layout.fillWidth: true
            columns: root.wide ? 2 : 1
            columnSpacing: 20
            rowSpacing: 20
            Panel {
                Layout.fillWidth: true
                Layout.preferredWidth: 620
                Layout.alignment: Qt.AlignTop
                contentItem: ColumnLayout {
                    spacing: 14
                    RowLayout {
                        SectionTitle { text: i18n("Body observatory") }
                        Controls.ComboBox { id: view; model: [i18n("Front"),i18n("Side")]; Accessible.name: i18n("Skeleton viewing direction") }
                    }
                    FuseSkeleton { Layout.fillWidth: true; Layout.preferredHeight: 370; poseData: root.pose; connected: FuseService.connected; sideView: view.currentIndex===1 }
                    Copy { text: root.cameraOnly ? i18n("Cyan: camera body + VR anchors · Violet: head and hand baseline") : i18n("Cyan: assisted output · Violet: Pico baseline") }
                    SectionTitle { text: root.sourceName(); color: "#00e5ff" }
                }
            }
            Panel {
                Layout.fillWidth: true
                Layout.preferredWidth: 310
                Layout.alignment: Qt.AlignTop
                contentItem: ColumnLayout {
                    spacing: 14
                    SectionTitle { text: i18n("Tracking foundation") }
                    Controls.Switch { text: i18n("Camera-only simulation"); checked: root.cameraOnly; enabled: FuseService.connected && !FuseService.busy; onClicked: FuseService.setControl("camera_only",checked) }
                    Copy { text: i18n("No wearable body trackers. Head and hands remain simulated VR anchors.") }
                    Controls.Switch { text: i18n("Camera assistance"); checked: !!root.pose.enabled; enabled: FuseService.connected && !FuseService.busy && !root.cameraOnly; onClicked: FuseService.setControl("enabled",checked) }
                    Copy { text: i18n("Correct the synthetic Pico body estimate where camera observations are available.") }
                    Controls.Switch { text: i18n("Simulate occlusion"); checked: !!root.pose.occluded; enabled: FuseService.connected && !FuseService.busy; onClicked: FuseService.setControl("occluded",checked) }
                    Copy { text: root.cameraOnly ? i18n("No body tracker fallback. Hidden body joints become unavailable; head and hand anchors remain.") : i18n("Camera loss gradually releases corrections. Disabling assistance immediately restores the Pico baseline.") }
                    SectionTitle { text: i18n("Camera devices") }
                    Copy { text: i18n("Capture starts only from the camera preview controls below. Video nodes may include metadata endpoints.") }
                    Repeater {
                        model: FuseService.devices || []
                        delegate: Copy { required property var modelData; text: (modelData.name || modelData.id)+" · "+(modelData.streaming ? i18n("streaming") : i18n("stopped")) }
                    }
                    Copy { visible: !(FuseService.devices || []).length; text: i18n("No discovered video devices. Simulation needs no camera.") }
                }
            }
        }
        Panel {
            Layout.fillWidth: true
            contentItem: ColumnLayout {
                spacing: 14
                RowLayout {
                    SectionTitle { text: i18n("Debug views") }
                    Controls.Button { text: i18n("Open debug window"); onClicked: { debugWindow.show(); debugWindow.raise(); debugWindow.requestActivate(); } }
                }
                DebugContent { Layout.fillWidth: true }
            }
        }
    }
    Window {
        id: debugWindow
        title: i18n("NX Body Tracking · camera and pose debug")
        width: 760
        height: 820
        minimumWidth: 520
        minimumHeight: 400
        color: "#000000"
        Controls.ScrollView {
            id: debugScroll
            anchors.fill: parent
            anchors.margins: 24
            contentWidth: availableWidth
            DebugContent { width: debugScroll.availableWidth }
        }
    }
}
