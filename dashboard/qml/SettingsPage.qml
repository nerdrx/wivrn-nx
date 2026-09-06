pragma ComponentBehavior: Bound

import QtCore as Core
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami

import io.github.wivrn.wivrn

Kirigami.ScrollablePage {
    id: settings
    title: i18n("Settings")

    // transparent over the NX nebula, stock look otherwise
    background: Rectangle {
        visible: !DashboardSettings.nx_theme
        color: Kirigami.Theme.backgroundColor
    }

    flickable.interactive: false // Make sure the Kirigami.ScrollablePage does not eat the vertical mouse dragging events

    property bool allowUpdates: false // ignore onXXX events until document is loaded

    ColumnLayout {
        id: column
        anchors.fill: parent

        Kirigami.FormLayout {

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18n("The current encoder configuration is not supported")
                type: Kirigami.MessageType.Information
                visible: !Settings.simpleConfig
                actions: [
                    Kirigami.Action {
                        text: i18n("Reset")
                        onTriggered: Settings.encoder = Settings.EncoderAuto
                    }
                ]
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Encoder:")
                enabled: Settings.simpleConfig
                Controls.ComboBox {
                    id: encoder_combo
                    model: [
                        {
                            label: i18nc("automatic encoder setup", "Auto"),
                            encoder: Settings.EncoderAuto
                        },
                        {
                            label: i18n("nvenc (NVIDIA GPUs)"),
                            encoder: Settings.Nvenc
                        },
                        {
                            label: i18n("vaapi (AMD and Intel GPUs)"),
                            encoder: Settings.Vaapi
                        },
                        {
                            label: i18n("Vulkan (Any modern GPU)"),
                            encoder: Settings.Vulkan
                        },
                        {
                            label: i18n("x264 (software encoding)"),
                            encoder: Settings.X264
                        },
                        {
                            label: i18n("NX Warp (experimental, headset GPU decode)"),
                            encoder: Settings.Nxwarp
                        }
                    ]
                    onCurrentIndexChanged: if (settings.allowUpdates) {Settings.encoder = model[currentIndex].encoder}
                    textRole: "label"
                    Connections {
                        target: Settings
                        function onEncoderChanged() {
                            var encoder = Settings.encoder;
                            var i = encoder_combo.model.findIndex( item => item.encoder == encoder)
                            if (i > -1)
                                encoder_combo.currentIndex = i
                        }
                    }
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("\"Auto\" will use hardware acceleration if it is available")
                }
            }

            RowLayout {
                Controls.CheckBox {
                    id: bitrate_auto
                    text: i18n("Automatic bitrate")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Adapt the bitrate to the wireless link quality while streaming. The bitrate configured on the headset is the maximum. On a sudden lag spike the bitrate drops sharply to let the connection recover, then climbs back. Applies from the next connection.")
                }
            }

            // ---------------------------------------------------------------------
            // Edge bleed. At a low frame rate the headset reprojects a late frame to the
            // newest head pose, and where the frame's field of view runs out there is
            // nothing to show: a black band sweeps in at the edge of the view. Not gated
            // on the encoder -- the overscan is a property of the rendered field of view
            // and the extension is a headset render pass, so both apply to every codec.
            // ---------------------------------------------------------------------
            Kirigami.Separator {
                Kirigami.FormData.isSection: true
            }

            Kirigami.Heading {
                text: i18n("Edge bleed")
                level: 1
                type: Kirigami.Heading.Type.Primary
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Overscan margin:")
                Layout.fillWidth: true
                Controls.Slider {
                    id: edge_bleed_overscan
                    Layout.fillWidth: true
                    from: 0.0
                    to: 0.20
                    stepSize: 0.01
                    snapMode: Controls.Slider.SnapAlways
                    value: Settings.edgeBleedOverscan
                    // Written live so the percentage readout beside it follows the handle
                    // rather than the last saved value, the same as the stream scale.
                    onValueChanged: if (settings.allowUpdates) { Settings.edgeBleedOverscan = value }
                }
                Controls.Label {
                    text: Math.round(edge_bleed_overscan.value * 100) + "%"
                    Layout.preferredWidth: 45
                    Layout.alignment: Qt.AlignRight
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("How much wider than the headset's own field of view the application renders, per side. Those are real pixels, so a reprojection has picture to move into instead of black. The encoded size does not change, so the same pixels cover a wider angle and the image is correspondingly less sharp: 5%% costs about 4.5%% of the resolution. 0 turns it off and leaves the fallback below to fill the margin.")
                }
            }

            // What the margin actually costs, live, so the trade is on screen next to the
            // control that makes it rather than in the documentation.
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.7
                text: edge_bleed_overscan.value > 0
                      ? i18n("Real pixels in the margin. About %1%% of the encoded area falls outside the panel, and the picture is about %2%% less sharp.",
                             Math.round((1 - 1 / ((1 + edge_bleed_overscan.value) * (1 + edge_bleed_overscan.value))) * 1000) / 10,
                             Math.round((1 - 1 / (1 + edge_bleed_overscan.value)) * 1000) / 10)
                      : i18n("No overscan: the margin, if any, is invented by the headset from the picture's own edge.")
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Edge extension:")
                Controls.ComboBox {
                    id: edge_bleed_extension
                    model: [
                        {
                            label: i18nc("no edge extension", "None"),
                            value: Settings.ExtensionNone
                        },
                        {
                            label: i18n("Clamp (stretch the edge)"),
                            value: Settings.ExtensionClamp
                        },
                        {
                            label: i18n("Fade (stretch, then blend to the edge colour)"),
                            value: Settings.ExtensionFade
                        }
                    ]
                    textRole: "label"
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("What the headset does when the overscan above is 0. It widens its own projection layer and fills the invented margin out of the picture's edge: Clamp stretches the outermost row and column outward, Fade stretches and then decays into that edge's own colour, which reads as the picture continuing off the side of the view. None is the old behaviour and shows a black band. Nothing is decoded for this margin; it is a smear, and it is only ever used where the alternative is black.")
                }
            }

            // ---------------------------------------------------------------------
            // NX Warp encoder. Every control here is a server configuration key that
            // previously had no GUI. The whole section is hidden unless NX Warp is the
            // selected encoder, because none of these mean anything for H.264/HEVC/AV1.
            // ---------------------------------------------------------------------
            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                visible: Settings.nxwarpSelected
            }

            Kirigami.Heading {
                text: i18n("NX Warp encoder")
                level: 1
                type: Kirigami.Heading.Type.Primary
                visible: Settings.nxwarpSelected
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Stream scale:")
                visible: Settings.nxwarpSelected
                Layout.fillWidth: true
                Controls.Slider {
                    id: stream_scale
                    Layout.fillWidth: true
                    from: 0.5
                    to: 1.0
                    stepSize: 0.05
                    snapMode: Controls.Slider.SnapAlways
                    value: Settings.streamScale
                    // The size readout has to follow the handle, not the saved value.
                    onValueChanged: if (settings.allowUpdates) { Settings.streamScale = value }
                }
                Controls.Label {
                    text: stream_scale.value.toFixed(2)
                    Layout.preferredWidth: 35
                    Layout.alignment: Qt.AlignRight
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Fewer pixels per eye, proportionally less decode time on the headset, softer image. Caps what the headset asks for; if it already asks for less, its own setting wins. Applies from the next connection.")
                }
            }

            // What the slider actually produces, live. The size comes from the headset that
            // last connected, so before any connection there is nothing honest to show.
            Controls.Label {
                visible: Settings.nxwarpSelected
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.75
                text: {
                    // Named so the binding actually depends on them: encodedEyeSize and
                    // willPairEyes are Q_INVOKABLE calls, which QML does not track, so
                    // without these two reads the readout would keep showing the size and
                    // the pairing verdict from whenever the label was last built. The
                    // slider and the combo both write their property before this runs.
                    let scale = Settings.streamScale;
                    let stereo = Settings.nxwarpStereoFrame;
                    let s = WivrnServer.streamEyeSize;
                    if (!s || s.width <= 0 || s.height <= 0)
                        return i18n("Connect a headset once to see the resulting encode size.");
                    let e = Settings.encodedEyeSize(s.width, s.height);
                    let tiles = Settings.encodedTiles(s.width, s.height);
                    let base = i18n("Encodes %1x%2 per eye (%3x%4 = %5 tiles), from the %6x%7 the headset asks for.",
                                    e.width, e.height,
                                    e.width / 64, e.height / 64, tiles,
                                    s.width, s.height);
                    // What the stereo setting will actually do with that size. The refusal
                    // case is the warning below rather than a sentence here, because it is
                    // the one outcome where the setting and the result disagree.
                    if (Settings.nxwarpSelected && !Settings.pairingRefused(s.width, s.height)) {
                        if (Settings.willPairEyes()) {
                            let p = Settings.pairedFrameSize(s.width, s.height);
                            base += " " + i18n("Both eyes pair into one %1x%2 frame (%3 tiles) on stream 0.",
                                               p.width, p.height,
                                               Settings.pairedTiles(s.width, s.height));
                        } else {
                            base += " " + i18n("The eyes are encoded as two separate streams.");
                        }
                    }
                    return base;
                }
            }

            // The one case where the stereo setting asks for something this stream scale
            // cannot deliver: the server pairs the eyes only when the per-eye width is a
            // multiple of 64, and it discovers that AFTER deciding to pair, so it logs a
            // warning and quietly runs two streams. stream_encode_size rounds every size
            // up to 64 on both axes, so no position of the slider above reaches this
            // today -- it is here so that an alignment change cannot make the setting lie
            // silently, and the settings test pins that it is currently unreachable.
            Kirigami.InlineMessage {
                id: pairing_refused_warning
                Layout.fillWidth: true
                type: Kirigami.MessageType.Warning
                visible: {
                    let scale = Settings.streamScale;
                    let stereo = Settings.nxwarpStereoFrame;
                    let s = WivrnServer.streamEyeSize;
                    if (!Settings.nxwarpSelected || !s || s.width <= 0 || s.height <= 0)
                        return false;
                    return Settings.pairingRefused(s.width, s.height);
                }
                text: {
                    let s = WivrnServer.streamEyeSize;
                    if (!s || s.width <= 0 || s.height <= 0)
                        return "";
                    let e = Settings.encodedEyeSize(s.width, s.height);
                    return i18n("The eyes will not be paired at this stream scale: it encodes %1 pixels per eye across, and pairing needs a multiple of 64. The server will fall back to one stream per eye and the headset will run two decoders.", e.width);
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Stereo frame:")
                visible: Settings.nxwarpSelected
                Controls.ComboBox {
                    id: nxwarp_stereo
                    model: [
                        {
                            label: i18nc("automatic stereo frame pairing", "Auto"),
                            value: Settings.StereoAuto
                        },
                        {
                            label: i18n("On"),
                            value: Settings.StereoOn
                        },
                        {
                            label: i18n("Off"),
                            value: Settings.StereoOff
                        }
                    ]
                    textRole: "label"
                    onCurrentIndexChanged: if (settings.allowUpdates) {Settings.nxwarpStereoFrame = model[currentIndex].value}
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Both eyes in one frame — the headset decodes once instead of twice, which measured 28% less GPU there. Auto pairs only when both eye streams are NX Warp and the per-eye width is a multiple of 64; the stream scale above always produces such a width, and the readout says which way it went. On forces it wherever auto would allow it, off keeps one stream and one encoder per eye. Costs the server one full-frame copy per frame. Applies from the next connection.")
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Tile mapping:")
                visible: Settings.nxwarpSelected
                Controls.ComboBox {
                    id: nxwarp_tile_map
                    model: [
                        {
                            label: i18nc("automatic tile mapping", "Auto"),
                            value: Settings.TileAuto
                        },
                        {
                            label: i18n("Per-tile spans"),
                            value: Settings.TileSpans
                        },
                        {
                            label: i18n("Fixed chunks (fallback)"),
                            value: Settings.TileChunks
                        }
                    ]
                    textRole: "label"
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Which bytes of a frame travel at which tile position. With per-tile spans a tile's own bytes travel at its own position, so one lost datagram costs the few tiles it was carrying instead of the whole frame. Auto uses them whenever the encoder can and every tile fits a packet, and falls back per frame when one does not — at a low quantiser, tiles outgrow a packet and the whole frame falls back. Fixed chunks never uses them: it is the older behaviour, and the one to pick if a session gets worse after an update. The server log says which was used. Applies from the next connection.")
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Entropy coder:")
                visible: Settings.nxwarpSelected
                Controls.ComboBox {
                    id: nxwarp_entropy
                    model: [
                        {
                            label: i18nc("automatic entropy coder", "Auto"),
                            value: Settings.EntropyAuto
                        },
                        {
                            label: i18n("rANS (smaller stream)"),
                            value: Settings.EntropyRans
                        },
                        {
                            label: i18n("Lite (cheaper to decode)"),
                            value: Settings.EntropyLite
                        }
                    ]
                    textRole: "label"
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("rANS spends headset decode time to make the stream smaller; Lite spends bitrate to make it cheaper to decode. Auto picks from what the headset says it supports.")
                }
            }

            RowLayout {
                visible: Settings.nxwarpSelected
                Controls.CheckBox {
                    id: nxwarp_effort
                    text: i18n("Extra encoder effort")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Let the encoder work a little harder for a smaller stream: it drops a coefficient whose error is worth less than the bits it saves. Measured 1.5%% fewer bytes with rANS and 3.6%% with Lite, for no measurable encode time, and the picture is unchanged in every other way. On unless you are chasing a difference to the byte.")
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Snap still tiles:")
                visible: Settings.nxwarpSelected
                Controls.ComboBox {
                    id: nxwarp_snap
                    model: [
                        { label: i18nc("snap to identity", "Off"), value: Settings.SnapOff },
                        { label: i18n("1 sample"), value: Settings.SnapOneSample },
                        { label: i18n("1.5 samples"), value: Settings.SnapOneAndHalf },
                        { label: i18n("2 samples"), value: Settings.SnapTwo }
                    ]
                    textRole: "label"
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("When the head has barely moved, send the frame as if it had not moved at all. Tiles that did not change then cost the headset a straight copy instead of a filtered warp, which is most of what its decode spends on a still scene. The picture error this can introduce is at most half the setting — half a sample at '1 sample', which is the same rounding the motion search already accepts.\n\nMeasured: below 1 sample nothing is ever snapped, because a head at rest still drifts about half a sample per frame; at 1 sample about a third of still frames qualify, for 0.05 dB and slightly FEWER bytes. Moving scenes are unaffected — nothing snaps at ordinary head speeds. Needs the GPU encoder with inter prediction on.")
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Send pacing:")
                visible: Settings.nxwarpSelected
                Controls.ComboBox {
                    id: nxwarp_pace
                    model: [
                        {
                            label: i18nc("automatic send pacing", "Auto"),
                            value: Settings.PaceAuto
                        },
                        {
                            label: i18n("Off (send every frame)"),
                            value: Settings.PaceOff
                        },
                        {
                            label: i18n("Fixed rate"),
                            value: Settings.PaceFixed
                        }
                    ]
                    textRole: "label"
                }
                Controls.SpinBox {
                    id: nxwarp_pace_fps
                    from: 1
                    to: 1000
                    enabled: nxwarp_pace.model[nxwarp_pace.currentIndex].value === Settings.PaceFixed
                    visible: enabled
                }
                Controls.Label {
                    text: i18nc("frames per second", "fps")
                    visible: nxwarp_pace_fps.visible
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Auto sends at the rate the headset reports it can decode at; Off sends every composited frame, which wastes bitrate the headset cannot keep up with; a fixed rate is held exactly whatever the headset reports.")
                }
            }

            RowLayout {
                visible: Settings.nxwarpSelected
                Controls.CheckBox {
                    id: nxwarp_rc_auto
                    text: i18n("Automatic quantiser")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Maps the bitrate the session allows to a quantiser every frame; turning it off holds one fixed quality and lets the bitrate go where it likes.")
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Quantiser range:")
                visible: Settings.nxwarpSelected && nxwarp_rc_auto.checked
                Controls.SpinBox {
                    id: nxwarp_min_qp
                    from: 0
                    to: 63
                }
                Controls.Label {
                    text: i18nc("range separator between minimum and maximum quantiser", "to")
                }
                Controls.SpinBox {
                    id: nxwarp_max_qp
                    from: 0
                    to: 63
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("The band the automatic quantiser may move in. A lower minimum buys sharpness on an idle link, a lower maximum refuses to degrade quality and drops frames instead.")
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Coded motion vectors:")
                visible: Settings.nxwarpSelected
                Controls.ComboBox {
                    id: nxwarp_coded_vectors
                    model: [
                        {
                            label: i18nc("default coded vectors mode", "Default"),
                            value: Settings.VectorsDefault
                        },
                        {
                            label: i18n("None"),
                            value: Settings.VectorsNone
                        },
                        {
                            label: i18n("Static"),
                            value: Settings.VectorsStatic
                        }
                    ]
                    textRole: "label"
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Coding motion vectors into the stream costs bitrate and encoder time but spares the headset from re-deriving them; None leaves that work to the decoder.")
                }
            }

            RowLayout {
                visible: Settings.nxwarpSelected
                Controls.CheckBox {
                    id: nxwarp_inter
                    text: i18n("Inter prediction")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Predict each frame from the one before: far less bitrate for the same quality, at the cost of a lost frame damaging the ones that follow until the next intra frame.")
                }
            }

            RowLayout {
                visible: Settings.nxwarpSelected
                Controls.CheckBox {
                    id: nxwarp_lens_mask
                    text: i18n("Skip invisible tiles")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("The lens shows a round region and the encoded picture is a rectangle. The 64x64 tiles in the corners are never seen: this fills them with a flat grey so they cost almost nothing, and tells the codec to skip them where it can. A one-tile ring around the visible region is always still coded.")
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Intra period:")
                visible: Settings.nxwarpSelected && nxwarp_inter.checked
                Controls.SpinBox {
                    id: nxwarp_intra_period
                    from: 1
                    to: 100000
                }
                Controls.Label {
                    text: i18nc("unit for the intra period setting", "frames")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("How often a frame is coded without prediction. Shorter recovers from loss sooner and costs bitrate; longer is cheaper and leaves damage on screen for longer.")
                }
            }

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
            }

            SelectGame {
                id: select_game
                Kirigami.FormData.label: i18n("Autostart application:")
            }

            Controls.CheckBox {
                id: auto_connect_usb
                text: i18n("Auto connect from USB")
            }

            RowLayout {
                Controls.CheckBox {
                    id: usb_backup_tunnel
                    text: i18n("USB backup tunnel")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("While a session is running, keep a USB tunnel open to every connected headset. The headset uses it as a backup connection if the setting is enabled there too.")
                }
            }

            RowLayout {
                Controls.CheckBox {
                    id: desktop_mirror
                    text: i18n("Desktop mirror")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Publish the headset view as a PipeWire video source, so that it can be shown on the Mirror page or captured by any other application. It costs a resample and a readback per captured frame. Applies from the next connection.")
                }
            }

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
            }

            Kirigami.Heading {
                text: i18n("Advanced options")
                level: 1
                type: Kirigami.Heading.Type.Primary
            }
            Controls.CheckBox {
                id: show_system_checks
                text: i18n("Check system configuration on start")
            }
            RowLayout {
                Controls.CheckBox {
                    id: nx_look
                    text: i18n("NX look")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Deep-space color scheme with the drifting nebula background. Colors fully apply after the dashboard is restarted.")
                }
            }
            RowLayout {
                visible: Settings.hid_forwarding_supported
                Controls.CheckBox {
                    id: hid_forwarding
                    text: i18n("Expose forwarded input devices via uinput")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Replicate mouse, keyboard and gamepad connected to the headset as virtual devices on PC.\nReplicated devices will appear as if they were plugged to the PC, some keys may be reserved by the headset OS and not be available. Gamepad is also available without virtual devices for applications that access it through OpenXR.")
                }
            }
            Controls.CheckBox {
                id: debug_gui
                text: i18n("Enable debug window")
                visible: Settings.debug_gui_supported
            }
            RowLayout {
                visible: Settings.steamvr_lh_supported
                Controls.CheckBox {
                    id: steamvr_lh
                    text: i18n("Enable SteamVR tracked devices support")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Allows the use of lighthouse-based controllers and trackers.\nRequires SteamVR to be installed.\nDevices must be be powered on before connecting to WiVRn.\nAn external tool such as motoc is needed for calibration.")
                }
            }
            RowLayout {
                visible: Settings.steamvr_lh_supported && steamvr_lh.checked
                Kirigami.FormData.label: i18n("SteamVR joystick deadzone")
                Controls.Slider {
                    id: lh_stick_deadzone
                    Layout.fillWidth: true
                    from: 0.0
                    to: 0.9
                    stepSize: 0.05
                    value: Settings.lhStickDeadzone
                }
                Controls.Label {
                    text: lh_stick_deadzone.value.toFixed(2)
                    Layout.preferredWidth: 35
                    Layout.alignment: Qt.AlignRight
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("Deadzone to apply to joysticks on lighthouse-tracked controllers, such as Index.\nFor standalone controllers, deadzones may be adjusted via the headset's system settings.")
                }
            }

            Controls.CheckBox {
                id: adb_custom
                Layout.row: 0
                Layout.column: 0
                text: i18n("Custom adb location")
            }

            Dialogs.FileDialog {
                id: adb_browse
                onAccepted: {
                    adb_location.text = WivrnServer.host_path(new URL(selectedFile).pathname);
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Adb location:")
                Layout.fillWidth: true
                enabled: adb_custom.checked
                Controls.TextField {
                    id: adb_location
                    placeholderText: DashboardSettings.adb_location
                    Layout.fillWidth: true
                }
                Controls.Button {
                    text: i18nc("browse a file to choose the adb binary", "Browse")
                    onClicked: adb_browse.open()
                }
            }

            ListModel {
                id: openvr_libs

                function init() {
                    openvr_libs.append({
                        "name": i18n("Default"),
                        "value": "",
                        "is_custom": false
                    });
                    var libs = WivrnServer.openVRCompat;
                    for (var i = 0; i < libs.length; i++) {
                        openvr_libs.append({
                            "name": libs[i].name,
                            "value": libs[i].path,
                            "is_custom": false
                        });
                    }

                    openvr_libs.append({
                        "name": i18nc("set a custom OpenVR compatibility library", "Custom"),
                        "is_custom": true
                    });

                    openvr_libs.append({
                        "name": i18n("Disabled"),
                        "value": "-",
                        "is_custom": false
                    });
                }
            }

            Dialogs.FolderDialog{
                id: openvr_browse
                onAccepted: {
                    currentFolder = selectedFolder
                    openvr_text.text = WivrnServer.host_path(new URL(selectedFolder).pathname);
                    openvr_text.text = openvr_text.text.replace(/\/linux64$/, "").replace(/\/bin$/, "")
                }
            }
            RowLayout {
                Kirigami.FormData.label: i18n("OpenVR compatibility library:")
                Layout.fillWidth: true
                Controls.ComboBox {
                    id: openvr_combobox
                    Layout.columnSpan: 2
                    textRole: "name"
                    model: openvr_libs

                    function load() {
                        for(let i=0 ; i < openvr_libs.count; i++) {
                            if (openvr_libs.get(i).value == Settings.openvr) {
                                openvr_combobox.currentIndex = i;
                                return;
                            }
                        }
                        for(let i=0 ; i < openvr_libs.count; i++) {
                            if (openvr_libs.get(i).is_custom) {
                                openvr_text.text = Settings.openvr
                                openvr_combobox.currentIndex = i;
                                return;
                            }
                        }
                    }
                    onActivated: index => {
                            if (openvr_libs.get(index).is_custom && openvr_text.text == "")
                                openvr_browse.open()
                    }
                }
                Controls.TextField {
                    id: openvr_text
                    placeholderText: i18n("Library path, excluding bin/linux64/vrclient.so")
                    visible: !!openvr_combobox.model.get(openvr_combobox.currentIndex)?.is_custom
                    Layout.fillWidth: true
                }
                Controls.Button {
                    text: i18nc("browse to choose the OpenVR compatility to use", "Browse")
                    visible: openvr_text.visible
                    onClicked: openvr_browse.open()
                }
            }

        }

        Item {
            // spacer item
            Layout.fillHeight: true
        }
    }

    footer: Controls.DialogButtonBox {
        standardButtons: Controls.DialogButtonBox.Ok | Controls.DialogButtonBox.Cancel | Controls.DialogButtonBox.Reset

        onAccepted: {
            settings.save();
            Settings.save(WivrnServer);

            applicationWindow().pageStack.pop();
        }
        onReset: {
            Settings.restore_defaults();
            settings.load();
        }
        onRejected: applicationWindow().pageStack.pop()
    }

    Component.onCompleted: {
        openvr_libs.init()
        Settings.load(WivrnServer);
        settings.allowUpdates = true;
        settings.load();
    }

    function save() {
        let openvr = openvr_combobox.model.get(openvr_combobox.currentIndex)
        if (openvr.is_custom) {
            Settings.openvr = openvr_text.text;
        } else {
            Settings.openvr = openvr.value
        }
        DashboardSettings.adb_custom = adb_custom.checked;
        DashboardSettings.adb_location = adb_location.text;
        Adb.setPath(adb_custom.checked ? adb_location.text : "adb");

        DashboardSettings.show_system_checks = show_system_checks.checked;
        DashboardSettings.nx_theme = nx_look.checked;

        Settings.bitrateAuto = bitrate_auto.checked;

        // Edge bleed. The margin is written live by its slider, for the readout; the mode
        // is written here on OK like every other combo box.
        Settings.edgeBleedExtension = edge_bleed_extension.model[edge_bleed_extension.currentIndex].value;

        // NX Warp. streamScale is already written live by the slider so its size readout can
        // follow the handle; the rest are written here, on OK, like every other control.
        if (Settings.nxwarpSelected) {
            Settings.nxwarpEntropy = nxwarp_entropy.model[nxwarp_entropy.currentIndex].value;
            // The pace mode has to be set before the rate: the rate is only stored in the
            // fixed mode, and the setter reads the mode to decide.
            Settings.nxwarpPace = nxwarp_pace.model[nxwarp_pace.currentIndex].value;
            Settings.nxwarpPaceFps = nxwarp_pace_fps.value;
            Settings.nxwarpRcAuto = nxwarp_rc_auto.checked;
            // Minimum first: each setter pushes the other bound out of the way rather than
            // letting an inverted range reach the server, which would refuse the session.
            Settings.nxwarpMinQp = nxwarp_min_qp.value;
            Settings.nxwarpMaxQp = nxwarp_max_qp.value;
            Settings.nxwarpStereoFrame = nxwarp_stereo.model[nxwarp_stereo.currentIndex].value;
            Settings.nxwarpTileMap = nxwarp_tile_map.model[nxwarp_tile_map.currentIndex].value;
            Settings.nxwarpCodedVectors = nxwarp_coded_vectors.model[nxwarp_coded_vectors.currentIndex].value;
            Settings.nxwarpEffort = nxwarp_effort.checked;
            Settings.nxwarpSnapIdentity = nxwarp_snap.model[nxwarp_snap.currentIndex].value;
            Settings.nxwarpInter = nxwarp_inter.checked;
            Settings.nxwarpLensMask = nxwarp_lens_mask.checked;
            Settings.nxwarpIntraPeriod = nxwarp_intra_period.value;
        }
        Settings.mirror = desktop_mirror.checked;
        Settings.debugGui = debug_gui.checked;
        Settings.steamVrLh = steamvr_lh.checked;
        Settings.lhStickDeadzone = lh_stick_deadzone.value;
        Settings.hidForwarding = hid_forwarding.checked;

        DashboardSettings.auto_connect_usb = auto_connect_usb.checked;
        DashboardSettings.usb_backup_tunnel = usb_backup_tunnel.checked;
    }

    function load() {
        select_game.load();
        bitrate_auto.checked = Settings.bitrateAuto;

        stream_scale.value = Settings.streamScale;
        edge_bleed_overscan.value = Settings.edgeBleedOverscan;
        edge_bleed_extension.currentIndex = edge_bleed_extension.model.findIndex(i => i.value === Settings.edgeBleedExtension);
        nxwarp_entropy.currentIndex = nxwarp_entropy.model.findIndex(i => i.value === Settings.nxwarpEntropy);
        nxwarp_pace.currentIndex = nxwarp_pace.model.findIndex(i => i.value === Settings.nxwarpPace);
        nxwarp_pace_fps.value = Settings.nxwarpPaceFps;
        nxwarp_rc_auto.checked = Settings.nxwarpRcAuto;
        nxwarp_min_qp.value = Settings.nxwarpMinQp;
        nxwarp_max_qp.value = Settings.nxwarpMaxQp;
        nxwarp_stereo.currentIndex = nxwarp_stereo.model.findIndex(i => i.value === Settings.nxwarpStereoFrame);
        nxwarp_tile_map.currentIndex = nxwarp_tile_map.model.findIndex(i => i.value === Settings.nxwarpTileMap);
        nxwarp_coded_vectors.currentIndex = nxwarp_coded_vectors.model.findIndex(i => i.value === Settings.nxwarpCodedVectors);
        nxwarp_effort.checked = Settings.nxwarpEffort;
        nxwarp_snap.currentIndex = nxwarp_snap.model.findIndex(i => i.value === Settings.nxwarpSnapIdentity);
        nxwarp_inter.checked = Settings.nxwarpInter;
        nxwarp_lens_mask.checked = Settings.nxwarpLensMask;
        nxwarp_intra_period.value = Settings.nxwarpIntraPeriod;
        desktop_mirror.checked = Settings.mirror;
        debug_gui.checked = Settings.debugGui;
        steamvr_lh.checked = Settings.steamVrLh;
        lh_stick_deadzone.value = Settings.lhStickDeadzone;
        hid_forwarding.checked = Settings.hidForwarding;

        auto_connect_usb.checked = DashboardSettings.auto_connect_usb;
        usb_backup_tunnel.checked = DashboardSettings.usb_backup_tunnel;

        openvr_combobox.load()

        adb_custom.checked = DashboardSettings.adb_custom;
        adb_location.text = DashboardSettings.adb_location;

        show_system_checks.checked = DashboardSettings.show_system_checks;
        nx_look.checked = DashboardSettings.nx_theme;
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: {if (isCurrentPage) applicationWindow().pageStack.pop();}
    }
}
