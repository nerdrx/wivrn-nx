pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    property var estimate: ({})
    readonly property var points: estimate.inferred3d || []
    spacing: 10
    RowLayout {
        Layout.fillWidth: true
        Controls.Label {
            Layout.fillWidth: true
            text: i18n("Inferred body · camera coordinates")
            wrapMode: Text.WordWrap
            color: "#efeaff"
            font.pixelSize: 16
        }
        Controls.ComboBox {
            id: direction
            model: [i18n("Front"), i18n("Side")]
            Accessible.name: i18n("Estimated body viewing direction")
            onCurrentIndexChanged: skeleton.requestPaint()
        }
    }
    Canvas {
        id: skeleton
        Layout.fillWidth: true
        Layout.preferredHeight: 280
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Connections { target: root; function onPointsChanged() { skeleton.requestPaint(); } }
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = "#050309";
            ctx.fillRect(0, 0, width, height);
            ctx.strokeStyle = "#261837";
            ctx.lineWidth = 1;
            ctx.beginPath(); ctx.moveTo(width/2, 12); ctx.lineTo(width/2, height-12); ctx.stroke();
            if (root.points.length !== 33) return;
            const scale = Math.min(width * 0.4, height * 0.43);
            function project(index) {
                const p = root.points[index];
                return [width/2 + p[direction.currentIndex === 1 ? 2 : 0]*scale, height*0.52+p[1]*scale];
            }
            function visible(index) {
                const p = (root.estimate.landmarks || [])[index];
                return p && p.visibility >= 0.5;
            }
            const edges = [[11,12],[11,13],[13,15],[12,14],[14,16],[11,23],[12,24],[23,24],[23,25],[25,27],[27,29],[29,31],[27,31],[24,26],[26,28],[28,30],[30,32],[28,32]];
            ctx.strokeStyle = "#00e5ff"; ctx.lineWidth = 3;
            edges.forEach(function(edge) {
                if (!visible(edge[0]) || !visible(edge[1])) return;
                const a = project(edge[0]), b = project(edge[1]);
                ctx.beginPath(); ctx.moveTo(a[0],a[1]); ctx.lineTo(b[0],b[1]); ctx.stroke();
            });
            [0,11,12,13,14,15,16,23,24,25,26,27,28,29,30,31,32].forEach(function(index) {
                if (!visible(index)) return;
                const p=project(index); ctx.fillStyle="#bc91ff";
                ctx.beginPath(); ctx.arc(p[0],p[1],4,0,2*Math.PI); ctx.fill();
            });
        }
        Controls.Label {
            anchors.centerIn: parent
            width: parent.width - 32
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            visible: root.points.length !== 33
            text: i18n("No unique, valid body estimate")
            color: "#b6a9d2"
        }
    }
    Controls.Label {
        Layout.fillWidth: true
        text: i18n("Single-camera depth is inferred by the model. Hip-relative scale and depth can be wrong. This view is not calibrated to your VR space and sends no poses to VR.")
        wrapMode: Text.WordWrap
        color: "#b6a9d2"
    }
    RowLayout {
        Layout.fillWidth: true
        Controls.Label {
            Layout.fillWidth: true
            text: i18n("Joint inspector")
            color: "#efeaff"
            font.pixelSize: 14
        }
        Controls.Label {
            text: root.estimate.processing_ms !== undefined
                  ? i18n("%1 ms model time", Number(root.estimate.processing_ms).toFixed(1))
                  : i18n("Model time —")
            color: "#b6a9d2"
            Accessible.name: i18n("Pose model processing time")
        }
    }
    Controls.ScrollView {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(220, inspectorColumn.implicitHeight)
        Layout.minimumHeight: 0
        clip: true
        Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded
        Column {
            id: inspectorColumn
            width: parent.width
            spacing: 2
            Repeater {
                model: root.points.length === 33 ? root.points.length : 0
                delegate: Rectangle {
                    required property int index
                    width: inspectorColumn.width
                    height: detailButton.checked ? 72 : 34
                    color: index % 2 ? "#0c0912" : "#100b18"
                    radius: 3
                    border.color: "#261837"
                    Controls.ToolButton {
                        id: detailButton
                        width: parent.width
                        height: 34
                        checkable: true
                        text: {
                            const landmark = (root.estimate.landmarks || [])[index] || {};
                            const confidence = landmark.visibility === undefined ? "—" : Number(landmark.visibility).toFixed(2);
                            return (landmark.name || i18n("Joint %1", index)) + "  ·  " + confidence;
                        }
                        horizontalPadding: 10
                        Accessible.name: text
                        Accessible.description: i18n("Expand joint coordinates")
                    }
                    Controls.Label {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.top: detailButton.bottom
                        width: parent.width - 24
                        height: 34
                        visible: detailButton.checked
                        verticalAlignment: Text.AlignVCenter
                        color: "#b6a9d2"
                        text: {
                            const point = root.points[index] || [0, 0, 0];
                            return i18n("Model metres · X %1  Y %2  Z %3", Number(point[0]).toFixed(3), Number(point[1]).toFixed(3), Number(point[2]).toFixed(3));
                        }
                    }
                }
            }
            Controls.Label {
                visible: root.points.length !== 33
                text: i18n("No joint estimate to inspect")
                color: "#b6a9d2"
                padding: 10
            }
        }
    }
}
