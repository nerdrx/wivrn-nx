pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

import io.github.wivrn.wivrn

Kirigami.ScrollablePage {
    id: headsets
    title: i18n("Headset statistics")

    // transparent over the NX nebula, stock look otherwise
    background: Rectangle {
        visible: !DashboardSettings.nx_theme
        color: Kirigami.Theme.backgroundColor
    }

    Connections {
        target: WivrnServer
        function onHeadsetConnectedChanged(value) {
            if (!value)
                applicationWindow().pageStack.pop();
        }
    }

    // The NX Warp encoder's two-second report, which until now existed only in the server log.
    // One card per stream; the eyes are streams 0 and 1.
    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: WivrnServer.nxwarpStats.length === 0
            type: Kirigami.MessageType.Information
            text: i18n("No NX Warp stream is running. These statistics appear about two seconds after a session starts with the NX Warp encoder selected. With the eyes paired there is one card for both of them, not two.")
        }

        Repeater {
            model: WivrnServer.nxwarpStats

            delegate: Kirigami.AbstractCard {
                required property var modelData

                Layout.fillWidth: true

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    RowLayout {
                        Kirigami.Heading {
                            // A paired stream is not stream 0 with a broken stream 1: the
                            // right eye has no encoder and will never report, so naming the
                            // pair here is what stops the page reading as half-dead.
                            text: modelData.paired
                                  ? i18n("Stream %1 · both eyes paired", modelData.streamIndex)
                                  : i18n("Stream %1", modelData.streamIndex)
                            level: 2
                        }
                        Item { Layout.fillWidth: true }
                        // The one line here that is a fault rather than a measurement.
                        Kirigami.Chip {
                            visible: modelData.rcUnreachable
                            text: i18n("ceiling unreachable")
                            closable: false
                            checkable: false
                        }
                    }

                    Kirigami.FormLayout {
                        Layout.fillWidth: true

                        RowLayout {
                            Kirigami.FormData.label: i18n("Sending:")
                            Controls.Label {
                                text: modelData.paceModeText === "off"
                                      ? i18n("%1 fps, every composited frame", modelData.fpsSent.toFixed(1))
                                      : i18n("%1 fps, paced to %2 (%3)",
                                             modelData.fpsSent.toFixed(1),
                                             modelData.pacedFps.toFixed(1),
                                             modelData.paceModeText === "fixed"
                                                 ? i18n("fixed by configuration")
                                                 : (modelData.clientDecodeKnown
                                                    ? i18n("headset decodes in %1 ms", modelData.clientDecodeMs.toFixed(1))
                                                    : i18n("headset has not reported a decode time yet")))
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("The server composites faster than most headsets can decode, so the encoder sends at the rate the headset reports it can keep up with; frames above that rate are dropped before they cost bandwidth.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Frames not sent:")
                            Controls.Label {
                                text: i18n("%1 in the last %2 s", modelData.framesNotSent, modelData.windowSeconds.toFixed(1))
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("Composited frames the pacer held back. Not a fault: it is the gap between what the server drew and what the headset could take, and sending them anyway would only make the decode slower.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Encode time:")
                            Controls.Label {
                                text: i18n("%1 ms per frame, worst %2 ms", modelData.encodeMsMean.toFixed(1), modelData.encodeMsMax.toFixed(1))
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("What the server's own encoder costs per frame. On the CPU reference backend this is what caps the frame rate; on the Vulkan backend it rarely is.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Frame size:")
                            Controls.Label {
                                text: modelData.hasTarget
                                      ? i18n("%1 B, %2% off the %3 B the controller aimed at",
                                             Math.round(modelData.bytesPerFrame),
                                             modelData.bytesOffTargetPercent.toFixed(0),
                                             Math.round(modelData.targetBytesPerFrame))
                                      : i18n("%1 B at a fixed quantiser", Math.round(modelData.bytesPerFrame))
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("What each frame actually cost, against the budget the bitrate controller set. Persistently over target with the quantiser pinned means the configured quantiser band cannot reach the bitrate.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Quantiser:")
                            Controls.Label {
                                text: modelData.rcAuto
                                      ? i18n("%1 on average, ranged %2 to %3", modelData.qpMean.toFixed(1), modelData.qpMin, modelData.qpMax)
                                      : i18n("%1, fixed", modelData.qpMean.toFixed(0))
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("Lower is sharper and larger. A band one value wide is a settled controller; a quantiser sitting at either end of its range is one that has run out of room.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Controller allows:")
                            Controls.Label {
                                text: i18n("%1 Mbit/s", modelData.controllerMbps.toFixed(1))
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("The bitrate the session's controller currently grants this stream. Every number above is derived from it, and it is the one that is missing when the picture is worse than the link should carry.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Headset dropped:")
                            Controls.Label {
                                text: modelData.notReconstructed === 0
                                      ? i18n("nothing")
                                      : i18n("%1 frame(s), %2 of them costly, mostly %3",
                                             modelData.notReconstructed,
                                             modelData.notReconstructedCostly,
                                             modelData.dominantReasonText)
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("Frames the headset received but could not reconstruct. A hole is the network's fault, the decode stride is the pacer working as intended, and a codec refusal is the stream's; each costly one makes the server send a full intra frame.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Encodes:")
                            Controls.Label {
                                text: modelData.paired
                                      ? i18n("%1x%2 per eye, paired into one %3x%2 frame of %4 tiles, scale %5",
                                             modelData.encodedWidth, modelData.encodedHeight,
                                             modelData.codedFrameWidth, modelData.codedFrameTiles,
                                             modelData.encodeScale.toFixed(2))
                                      : i18n("%1x%2 per eye, %3 tiles, scale %4",
                                             modelData.encodedWidth, modelData.encodedHeight,
                                             modelData.tiles, modelData.encodeScale.toFixed(2))
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("The size actually encoded, after the headset's own resolution setting and the server's stream scale. The headset's decode cost follows the tile count almost exactly; when the eyes are paired it decodes that one frame instead of two.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Tile mapping:")
                            visible: modelData.spanFrames + modelData.chunkFrames > 0
                            Controls.Label {
                                text: modelData.chunkFrames === 0
                                      ? i18n("per-tile spans, all %1 frames", modelData.spanFrames)
                                      : (modelData.spanFrames === 0
                                         ? i18n("fixed chunks, all %1 frames", modelData.chunkFrames)
                                         : i18n("%1 frames on per-tile spans, %2 on fixed chunks",
                                                modelData.spanFrames, modelData.chunkFrames))
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("Which bytes of a frame travel at which tile position. With per-tile spans one lost datagram costs the few tiles it was carrying instead of the whole frame. The choice is made per frame, so a mix here is normal: a frame with any tile too big for a packet falls back on its own, which happens at a low quantiser. All frames on fixed chunks when the setting is Auto means every frame is falling back — raise the quantiser, or the MTU.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Encoder effort:")
                            Controls.Label {
                                text: modelData.effort >= 1
                                      ? i18n("1 — integer requantiser")
                                      : i18n("0 — dead-zone quantiser")
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("How hard the encoder looks for the cheapest way to say each frame. Level 1 drops a coefficient whose error is worth less than the bits it saves: measured 1.5%% fewer bytes on rANS and 3.6%% on Lite, for no measurable encode time. The level leaves no trace in the stream, so this line is the only place it can be read back.")
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: i18n("Entropy coder:")
                            Controls.Label {
                                text: modelData.entropyWasAuto
                                      ? i18n("%1, chosen automatically (headset tools %2)", modelData.entropy, modelData.toolsText)
                                      : i18n("%1, set in the configuration", modelData.entropy)
                            }
                            Kirigami.ContextualHelpButton {
                                toolTipText: i18n("rANS spends headset decode time to make the stream smaller; Lite spends bitrate to make it cheaper to decode. Auto picks from the tools the headset advertises.")
                            }
                        }
                    }
                }
            }
        }

        Item {
            // spacer item
            Layout.fillHeight: true
        }
    }

    footer: Controls.DialogButtonBox {
        standardButtons: Controls.DialogButtonBox.NoButton
        onAccepted: applicationWindow().pageStack.pop()

        Controls.Button {
            text: i18nc("go back to the home page", "Back")
            icon.name: "go-previous"
            Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.AcceptRole
        }
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: {if (isCurrentPage) applicationWindow().pageStack.pop();}
    }
}
