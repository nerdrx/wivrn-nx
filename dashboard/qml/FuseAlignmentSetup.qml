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
    readonly property var calibration: FuseService.calibration || ({})
    readonly property var alignment: FuseService.alignment || ({})
    readonly property var pending: root.alignment.pending || ({})
    readonly property var profileSet: root.alignment.profiles || ({})
    readonly property var lensProfileSet: root.calibration.profiles || ({})
    readonly property var profile: Object.keys(profileSet).length > 0 ? (profileSet[root.cameraId] || ({}))
        : ((root.alignment.camera === root.cameraId || !root.alignment.camera) ? (root.alignment.profile || ({})) : ({}))
    readonly property var lensProfile: Object.keys(lensProfileSet).length > 0 ? (lensProfileSet[root.cameraId] || ({}))
        : ((root.calibration.camera_id === root.cameraId || !root.calibration.camera_id) ? (root.calibration.profile || ({})) : ({}))
    readonly property bool lensReady: !!root.lensProfile && !!root.lensProfile.validated
                                      && (!root.calibration.camera_id || root.calibration.camera_id === root.cameraId)
                                      && (!root.lensProfile.camera_id || root.lensProfile.camera_id === root.cameraId)
    readonly property bool usable: root.cameraReady && root.lensReady && !!root.cameraId
    readonly property bool autoUsable: root.cameraReady && !!root.cameraId
    readonly property bool manualUsable: root.usable && !root.estimating && !root.automatic.running
    readonly property string anchor: anchorBox.currentValue || "head"
    readonly property int count: Number(root.alignment.count || 0)
    readonly property var automatic: root.alignment.automatic || ({})
    property bool automaticAdvanced: false
    property bool useControllers: false
    spacing: 8

    Controls.Label {
        Layout.fillWidth: true
        text: i18n("Spatial alignment")
        color: "#efeaff"
        font.pixelSize: 16
        font.weight: Font.DemiBold
    }
    Controls.AbstractButton {
        Layout.fillWidth: true
        checkable: true
        checked: root.expanded
        focusPolicy: Qt.StrongFocus
        Accessible.name: i18n("Spatial alignment setup")
        Accessible.description: checked ? i18n("Expanded. Activate to collapse.") : i18n("Collapsed. Activate to expand.")
        onToggled: root.expanded = checked
        Keys.onReturnPressed: (event) => { toggle(); event.accepted = true }
        Keys.onEnterPressed: (event) => { toggle(); event.accepted = true }
        contentItem: RowLayout {
            Controls.Label { Layout.fillWidth: true; text: root.expanded ? i18n("Hide alignment controls") : i18n("Show alignment controls"); color: "#bc91ff" }
            Controls.Label { text: root.expanded ? "⌃" : "⌄"; color: "#bc91ff"; Accessible.ignored: true }
        }
    }
    Controls.Label {
        Layout.fillWidth: true
        text: i18n("Requires a validated lens calibration. Measure the device local offset to the physical reference; the default 0 mm is only an approximation.")
        color: "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    Controls.Label {
        Layout.fillWidth: true
        text: root.lensReady ? i18n("Samples: %1/12 · capture varied width, height, and depth; the last quarter is held out for validation.", root.count) : i18n("Automatic alignment can learn lens geometry from anchors; precision is lower until a lens profile is validated.")
        color: root.lensReady ? "#b6a9d2" : "#ffcd80"
        wrapMode: Text.WordWrap
    }
    RowLayout {
        visible: root.expanded
        Layout.fillWidth: true
        Controls.Label { text: i18n("Reference"); color: "#b6a9d2" }
        Controls.ComboBox {
            id: anchorBox
            model: [
                { text: i18n("Head"), value: "head" },
                { text: i18n("Left controller"), value: "left_hand" },
                { text: i18n("Right controller"), value: "right_hand" }
            ]
            textRole: "text"
            valueRole: "value"
            currentIndex: 0
            enabled: root.manualUsable && !root.pending.token
            Accessible.name: i18n("Physical reference")
        }
    }
    RowLayout {
        id: offsetRow
        visible: root.expanded && root.automaticAdvanced
        Layout.fillWidth: true
        Controls.Label { text: i18n("Local offset (mm)"); color: "#b6a9d2" }
        Controls.TextField {
            id: xOffset
            Layout.fillWidth: true
            placeholderText: "0"
            text: "0"
            selectByMouse: true
            validator: DoubleValidator { locale: "C"; bottom: -500; top: 500; decimals: 2 }
            Accessible.name: i18n("X offset in millimetres")
        }
        Controls.TextField {
            id: yOffset
            Layout.fillWidth: true
            placeholderText: "0"
            text: "0"
            selectByMouse: true
            validator: DoubleValidator { locale: "C"; bottom: -500; top: 500; decimals: 2 }
            Accessible.name: i18n("Y offset in millimetres")
        }
        Controls.TextField {
            id: zOffset
            Layout.fillWidth: true
            placeholderText: "0"
            text: "0"
            selectByMouse: true
            validator: DoubleValidator { locale: "C"; bottom: -500; top: 500; decimals: 2 }
            Accessible.name: i18n("Z offset in millimetres")
        }
    }
    RowLayout {
        visible: root.expanded
        Layout.fillWidth: true
        Controls.Label { Layout.fillWidth: true; text: i18n("Automatic alignment"); color: "#efeaff"; font.weight: Font.DemiBold }
        Controls.CheckBox { text: i18n("Use controllers"); checked: root.useControllers; onToggled: root.useControllers = checked; enabled: root.usable && !root.automatic.running; Accessible.name: i18n("Include controller anchors") }
        Controls.Button {
            text: root.automatic.running ? i18n("Stop") : i18n("Start")
            enabled: root.automatic.running ? FuseService.connected : root.autoUsable && xOffset.acceptableInput && yOffset.acceptableInput && zOffset.acceptableInput
            onClicked: {
                if (root.automatic.running)
                    FuseService.alignmentCommand({action: "auto_stop", id: root.cameraId})
                else
                    root.requestAutomatic()
            }
            Accessible.name: root.automatic.running ? i18n("Stop automatic alignment") : i18n("Start automatic alignment")
        }
    }
    Controls.AbstractButton {
        visible: root.expanded
        Layout.fillWidth: true
        checkable: true
        checked: root.automaticAdvanced
        onToggled: root.automaticAdvanced = checked
        contentItem: Controls.Label { text: root.automaticAdvanced ? i18n("Hide automatic alignment offsets") : i18n("Show automatic alignment offsets"); color: "#bc91ff" }
    }
    Controls.Label {
        visible: root.expanded && !!root.automatic.running
        Layout.fillWidth: true
        text: i18n("Automatic alignment: %1/%2 samples", Number(root.automatic.count || 0), Number(root.automatic.max_samples || 0))
        color: "#73ecda"
    }
    Controls.Label {
        visible: root.expanded && !!root.automatic.error
        Layout.fillWidth: true
        text: root.automatic.error || ""
        color: "#ffcd80"
        wrapMode: Text.WordWrap
    }
    Controls.Label {
        visible: root.expanded && !root.automatic.running
        Layout.fillWidth: true
        text: i18n("Automatic alignment: move your head around the room, then pause for 400 ms; samples are collected without clicking.")
        color: "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    RowLayout {
        visible: root.expanded
        Layout.fillWidth: true
        Controls.Button {
            text: root.pending.token ? i18n("Freeze another frame") : i18n("Freeze while held still")
            enabled: root.manualUsable && xOffset.acceptableInput && yOffset.acceptableInput && zOffset.acceptableInput
            onClicked: FuseService.alignmentCommand({action: "freeze", id: root.cameraId, anchor: root.anchor, offset: root.offset()})
            Accessible.name: i18n("Freeze camera frame for alignment")
        }
        Controls.Button {
            text: i18n("Solve")
            enabled: root.manualUsable && root.count >= 12
            onClicked: FuseService.alignmentCommand({action: "solve", id: root.cameraId, anchor: root.anchor, offset: root.offset()})
        }
        Controls.Button {
            text: i18n("Reset")
            enabled: !!root.cameraId && FuseService.connected && !root.automatic.running
            onClicked: FuseService.alignmentCommand({action: "reset", id: root.cameraId, anchor: root.anchor})
        }
    }
    Controls.Label {
        visible: root.expanded
        Layout.fillWidth: true
        text: i18n("Hold still for 400 ms before freezing. Click the same physical reference in the frozen image; use the reference you chose above, not your wrist.")
        color: "#73ecda"
        wrapMode: Text.WordWrap
    }
    Rectangle {
        visible: root.expanded
        id: frameBox
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(180, Math.min(width * 0.5625, 420))
        color: "#050309"
        radius: 6
        clip: true
        Image {
            id: frame
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            source: root.pending.token ? FuseService.serviceUrl + "/api/alignment/frame?token=" + encodeURIComponent(root.pending.token) : ""
            Accessible.name: i18n("Frozen alignment camera image")
        }
        MouseArea {
            id: clickArea
            anchors.fill: parent
            enabled: !!root.pending.token && frame.status === Image.Ready
            onClicked: (mouse) => root.acceptPixel(mouse.x, mouse.y)
            Keys.onLeftPressed: (event) => { pixelX.value = Math.max(0, pixelX.value - 1); event.accepted = true }
            Keys.onRightPressed: (event) => { pixelX.value = Math.min(Number(root.pending.width || 0) - 1, pixelX.value + 1); event.accepted = true }
            Keys.onUpPressed: (event) => { pixelY.value = Math.max(0, pixelY.value - 1); event.accepted = true }
            Keys.onDownPressed: (event) => { pixelY.value = Math.min(Number(root.pending.height || 0) - 1, pixelY.value + 1); event.accepted = true }
            focus: true
        }
        Controls.Label {
            anchors.centerIn: parent
            width: parent.width - 32
            visible: !root.pending.token || frame.status !== Image.Ready
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#b6a9d2"
            text: root.pending.token ? i18n("Loading frozen frame…") : i18n("Freeze a frame to begin sampling.")
        }
    }
    RowLayout {
        visible: root.expanded && !!root.pending.token
        Layout.fillWidth: true
        Controls.Label { text: i18n("Pixel"); color: "#b6a9d2" }
        Controls.SpinBox { id: pixelX; from: 0; to: Math.max(0, Number(root.pending.width || 1) - 1); editable: true; Accessible.name: i18n("Pixel X") }
        Controls.SpinBox { id: pixelY; from: 0; to: Math.max(0, Number(root.pending.height || 1) - 1); editable: true; Accessible.name: i18n("Pixel Y") }
        Controls.Button {
            text: i18n("Accept sample")
            enabled: !!root.pending.token
            onClicked: root.acceptPixelValue(pixelX.value, pixelY.value)
            Accessible.name: i18n("Accept selected pixel sample")
        }
    }
    Controls.Label {
        visible: root.expanded
        Layout.fillWidth: true
        text: root.alignment.error || ""
        color: root.alignment.error ? "#ffcd80" : "#b6a9d2"
        wrapMode: Text.WordWrap
    }
    Controls.Label {
        Layout.fillWidth: true
        visible: root.expanded && !!root.profile
        text: !root.profile ? "" : root.profile.validated
               ? i18n("Validated · training RMS %1 px · held-out RMS %2 px · max held-out %3 px", Number(root.profile.training_rms_px).toFixed(2), Number(root.profile.held_out_rms_px).toFixed(2), Number(root.profile.max_held_out_px).toFixed(2))
               : i18n("Not validated · training RMS %1 px · held-out RMS %2 px · max held-out %3 px", Number(root.profile.training_rms_px).toFixed(2), Number(root.profile.held_out_rms_px).toFixed(2), Number(root.profile.max_held_out_px).toFixed(2))
        color: root.profile && root.profile.validated ? "#73ecda" : "#ffcd80"
        wrapMode: Text.WordWrap
    }
    Controls.Button {
        visible: root.expanded && !!root.profile && !!root.profile.validated
        text: i18n("Download alignment profile")
        onClicked: Qt.openUrlExternally(FuseService.serviceUrl + "/api/alignment/profile?id=" + encodeURIComponent(root.cameraId))
    }

    function startAutomatic() {
        var anchors = root.useControllers ? ["head", "left_hand", "right_hand"] : [root.anchor]
        var offsets = {head: [0, 0, 0], left_hand: [0, 0, 0], right_hand: [0, 0, 0]}
        offsets[root.anchor] = root.offset()
        FuseService.alignmentCommand({action: "auto_start", id: root.cameraId, anchors: anchors, offsets: offsets})
    }
    function requestAutomatic() {
        if (root.estimating) {
            root.startAutomatic()
            return
        }
        FuseService.setEstimation(root.cameraId, true)
        // Collection waits for estimates; no timer or view-local pending state is needed.
        root.startAutomatic()
    }
    function offset() {
        return [Number(xOffset.text || 0) / 1000, Number(yOffset.text || 0) / 1000, Number(zOffset.text || 0) / 1000]
    }
    function paintedRect() {
        var w = Number(root.pending.width || 1), h = Number(root.pending.height || 1)
        var scale = Math.min(frameBox.width / w, frameBox.height / h)
        var pw = w * scale, ph = h * scale
        return {x: (frameBox.width - pw) / 2, y: (frameBox.height - ph) / 2, width: pw, height: ph}
    }
    function acceptPixel(x, y) {
        var r = root.paintedRect()
        if (x < r.x || x >= r.x + r.width || y < r.y || y >= r.y + r.height) return
        var w = Number(root.pending.width || 1), h = Number(root.pending.height || 1)
        root.acceptPixelValue(Math.min(w - 1, Math.floor((x - r.x) / r.width * w)),
                              Math.min(h - 1, Math.floor((y - r.y) / r.height * h)))
    }
    function acceptPixelValue(x, y) {
        if (!root.pending.token) return
        FuseService.alignmentCommand({action: "sample", id: root.cameraId, anchor: root.anchor, offset: root.offset(), token: root.pending.token, pixel: [Number(x), Number(y)]})
    }
}
