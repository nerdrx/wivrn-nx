pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import io.github.wivrn.wivrn

ColumnLayout {
    id: root
    readonly property var feed: FuseService.tracking || ({})
    readonly property var records: feed.records || []
    readonly property var livePose: {
        const joints = {};
        for (const record of records) {
            if ((Number(record.flags) & 0x11) === 0x11 && record.position && record.position.length === 3)
                joints[record.joint] = {fused: record.position};
        }
        return {joints: joints};
    }
    spacing: 10
    Controls.Label {
        text: i18n("WiVRn tracking feed · read only")
        color: "#efeaff"
        font.pixelSize: 16
        font.weight: Font.DemiBold
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
    Controls.Label {
        Layout.fillWidth: true
        color: root.feed.connected ? "#00e5ff" : "#b6a9d2"
        wrapMode: Text.WordWrap
        text: root.feed.connected ? i18n("Receiving raw headset/tracker poses. No camera correction is applied.")
              : root.feed.enabled ? i18n("Listening for WiVRn. Connect a headset through the tracking lab build to inspect its poses.")
              : i18n("Tracking feed is off. The tracking lab launcher enables this separate read-only connection.")
    }
    Controls.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: "#b6a9d2"
        text: i18n("These observations are separate from the simulation below. Generic trackers keep their indices until you assign body roles. Missing or old poses disappear from this view.")
    }
    RowLayout {
        Layout.fillWidth: true
        Controls.Label { Layout.fillWidth: true; text: i18n("Received tracking positions"); color: "#efeaff" }
        Controls.ComboBox { id: direction; model: [i18n("Front"), i18n("Side")]; Accessible.name: i18n("Live tracking view direction") }
    }
    FuseSkeleton {
        Layout.fillWidth: true
        Layout.preferredHeight: 300
        liveTracking: true
        connected: !!root.feed.connected
        poseData: root.livePose
        sideView: direction.currentIndex === 1
    }
    Controls.Button {
        id: details
        checkable: true
        checked: false
        text: checked ? i18n("Hide raw poses") : i18n("Inspect raw poses (%1)", root.records.length)
    }
    ColumnLayout {
        visible: details.checked
        Layout.fillWidth: true
        spacing: 8
        Repeater {
            model: root.records.length
            delegate: Controls.Label {
                required property int index
                readonly property var modelData: root.records[index] || ({})
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: modelData.position_valid ? "#d4bbff" : "#ffcd80"
                text: modelData.joint.replace(/_/g," ") + " · " + modelData.route + " · "
                      + (modelData.position_valid ? modelData.position.map(function(n) { return Number(n).toFixed(3); }).join(", ") + " m" : i18n("position invalid"))
                      + " · " + Math.round(modelData.age_ms) + " ms"
            }
        }
        Controls.Label {
            text: i18n("No fresh poses")
            visible: !root.records.length
            color: "#b6a9d2"
        }
    }
}
