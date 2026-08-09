pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtMultimedia
import org.kde.kirigami as Kirigami

import io.github.wivrn.wivrn

Kirigami.Page {
    id: mirror_page
    title: i18n("Mirror")

    padding: 0

    // The server only captures while somebody is connected to its PipeWire node,
    // so the stream is dropped as soon as the page is not on screen.
    readonly property bool should_run: mirror_page.visible
                                       && mirror_page.isCurrentPage
                                       && applicationWindow().visible

    onShould_runChanged: {
        if (mirror_page.should_run)
            mirror.start();
        else
            mirror.stop();
    }

    Component.onCompleted: {
        if (mirror_page.should_run)
            mirror.start();
    }

    Component.onDestruction: mirror.stop()

    MirrorView {
        id: mirror
        videoSink: video_output.videoSink
    }

    Kirigami.Action {
        id: enable_mirror_action
        text: i18n("Enable the mirror")
        icon.name: "media-playback-start-symbolic"
        onTriggered: {
            // Reload first: the settings page may not have been opened yet
            Settings.load(WivrnServer);
            Settings.mirror = true;
            Settings.save(WivrnServer);
        }
    }

    Item {
        anchors.fill: parent

        VideoOutput {
            id: video_output
            anchors.fill: parent
            anchors.margins: Kirigami.Units.smallSpacing
            fillMode: VideoOutput.PreserveAspectFit
            visible: mirror.active
        }

        Kirigami.PlaceholderMessage {
            id: placeholder
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: !mirror.active

            // 0: no PipeWire, 1: no session, 2: no node, 3: node found, no frame yet
            readonly property int mode: {
                if (!mirror.supported)
                    return 0;
                if (!WivrnServer.sessionRunning)
                    return 1;
                if (!mirror.available)
                    return 2;
                return 3;
            }

            icon.name: {
                switch (placeholder.mode) {
                case 0:
                    return "dialog-error";
                case 1:
                    return "network-disconnect-symbolic";
                case 2:
                    return "video-display-symbolic";
                default:
                    return "view-refresh-symbolic";
                }
            }

            text: {
                switch (placeholder.mode) {
                case 0:
                    return i18n("The desktop mirror is not available");
                case 1:
                    return i18n("Connect the headset to see the mirror");
                case 2:
                    return i18n("The mirror is disabled in the server settings");
                default:
                    return i18n("Waiting for the headset view…");
                }
            }

            explanation: {
                switch (placeholder.mode) {
                case 0:
                    return mirror.errorString;
                case 1:
                    return i18n("The headset view is published while a session is running.");
                case 2:
                    return i18n("The server publishes the headset view as a PipeWire source only when the mirror is enabled. Enabling it here applies the next time the headset connects.");
                default:
                    return "";
                }
            }

            helpfulAction: placeholder.mode == 2 ? enable_mirror_action : null
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
        onActivated: {if (mirror_page.isCurrentPage) applicationWindow().pageStack.pop();}
    }
}
