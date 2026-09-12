pragma ComponentBehavior: Bound
import QtQuick

Rectangle {
    id: root
    property var poseData: ({})
    property bool observationsOnly: false
    property bool sideView: false
    property bool connected: false
    readonly property var bones: [["head","neck"],["neck","hip"],["neck","left_shoulder"],["neck","right_shoulder"],["left_shoulder","left_elbow"],["left_elbow","left_hand"],["right_shoulder","right_elbow"],["right_elbow","right_hand"],["hip","left_knee"],["left_knee","left_foot"],["hip","right_knee"],["right_knee","right_foot"]]
    implicitHeight: 350
    implicitWidth: 420
    radius: 4
    color: "#070410"
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: observationsOnly ? "Synthetic camera input projection. No real camera image." : "Simulated body output and baseline skeleton"
    onPoseDataChanged: drawing.requestPaint()
    onObservationsOnlyChanged: drawing.requestPaint()
    onSideViewChanged: drawing.requestPaint()
    onConnectedChanged: drawing.requestPaint()

    Canvas {
        id: drawing
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var c = getContext("2d");
            c.reset();
            var floor = height - 32;
            c.strokeStyle = "#211932";
            c.lineWidth = 1;
            for (var i = -7; i <= 7; ++i) {
                c.beginPath(); c.moveTo(width/2+i*18,floor-42); c.lineTo(width/2+i*80,height); c.stroke();
            }
            for (var y = 0; y < 6; ++y) {
                c.beginPath(); c.moveTo(0,floor-42+y*y*3); c.lineTo(width,floor-42+y*y*3); c.stroke();
            }
            var joints = root.connected ? (root.poseData.joints || {}) : {};
            var raw = root.connected ? (root.poseData.observations || []) : [];
            var data = joints;
            if (root.observationsOnly) {
                data = {};
                for (var o of raw) data[o.joint] = {fused: o.position, weight: o.confidence};
            }
            var names = Object.keys(data);
            if (!names.length) {
                c.fillStyle = "#b6a9d2"; c.font = "13px sans-serif"; c.textAlign = "center";
                c.fillText(root.connected ? "No camera observations" : "Simulator disconnected", width/2,height/2);
                return;
            }
            var scale = Math.min((height-80)/1.85,width/2.3);
            function project(p) { return [width/2+p[root.sideView?2:0]*scale,floor-p[1]*scale]; }
            var fields = root.observationsOnly ? ["fused"] : ["pico","fused"];
            for (var field of fields) {
                c.strokeStyle = field === "pico" ? "#ae82e5" : root.observationsOnly ? "#ffcd80" : "#00e5ff";
                c.fillStyle = c.strokeStyle;
                c.globalAlpha = field === "pico" ? 0.38 : 1;
                c.lineWidth = field === "pico" ? 2 : 3;
                for (var pair of root.bones) {
                    var a=data[pair[0]], b=data[pair[1]];
                    if (!a || !b || !a[field] || !b[field] || (field === "pico" && root.poseData.camera_only)) continue;
                    var pa=project(a[field]), pb=project(b[field]);
                    c.beginPath(); c.moveTo(pa[0],pa[1]); c.lineTo(pb[0],pb[1]); c.stroke();
                }
                for (var name of names) {
                    var p=data[name][field];
                    if (!p || (field === "pico" && root.poseData.camera_only && ["head","left_hand","right_hand"].indexOf(name)<0)) continue;
                    var pt=project(p);
                    c.beginPath(); c.arc(pt[0],pt[1],name==="head"?12:4,0,Math.PI*2);
                    if(name==="head") c.stroke(); else c.fill();
                }
            }
            c.globalAlpha=1;
        }
    }
    Text {
        anchors { left: parent.left; top: parent.top; margins: 16 }
        text: root.observationsOnly ? "SYNTHETIC CAMERA · INPUT" : "SIMULATED BODY · OUTPUT"
        color: "#b6a9d2"
        font.pixelSize: 10
        font.letterSpacing: 1.5
    }
}
