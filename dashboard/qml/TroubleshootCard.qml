pragma ComponentBehavior: Bound
import org.kde.kirigami as Kirigami
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Templates as T
import io.github.wivrn.wivrn

Control {
    id: card
    property alias title: toggle.text
    property alias details: label_details.text
    property alias expanded: toggle.checked
    property list<T.Action> actions
    padding: DashboardSettings.nx_theme ? 20 : Kirigami.Units.largeSpacing
    leftPadding: padding
    rightPadding: padding
    topPadding: padding
    bottomPadding: padding
    implicitWidth: 480
    implicitHeight: implicitContentHeight + topPadding + bottomPadding

    background: Rectangle {
        radius: DashboardSettings.nx_theme ? 6 : Kirigami.Units.cornerRadius
        border.width: toggle.activeFocus ? 2 : 0
        border.color: DashboardSettings.nx_theme ? "#bc91ff" : Kirigami.Theme.highlightColor
        gradient: Gradient {
            GradientStop { position: 0; color: DashboardSettings.nx_theme ? (toggle.hovered ? "#201432" : "#171022") : Kirigami.Theme.alternateBackgroundColor }
            GradientStop { position: 1; color: DashboardSettings.nx_theme ? "#090610" : Kirigami.Theme.backgroundColor }
        }
    }
    contentItem: ColumnLayout {
        spacing: 16
        AbstractButton {
            id: toggle
            Layout.fillWidth: true
            checkable: true
            focusPolicy: Qt.StrongFocus
            Accessible.name: text
            Accessible.description: checked ? i18n("Expanded. Activate to collapse.") : i18n("Collapsed. Activate to expand.")
            Keys.onReturnPressed: event => { toggle.toggle(); event.accepted = true; }
            Keys.onEnterPressed: event => { toggle.toggle(); event.accepted = true; }
            contentItem: RowLayout {
                spacing: 16
                Label {
                    text: toggle.text
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    color: DashboardSettings.nx_theme ? "#efeaff" : Kirigami.Theme.textColor
                }
                Kirigami.Icon {
                    source: toggle.checked ? "go-up-symbolic" : "go-down-symbolic"
                    implicitWidth: 20
                    implicitHeight: 20
                    color: DashboardSettings.nx_theme ? "#bc91ff" : Kirigami.Theme.textColor
                    Accessible.ignored: true
                }
            }
        }
        BetterLabel {
            id: label_details
            visible: toggle.checked
            Layout.fillWidth: true
            color: DashboardSettings.nx_theme ? "#b6a9d2" : Kirigami.Theme.textColor
        }
        Kirigami.ActionToolBar {
            visible: toggle.checked && card.actions.length > 0
            Layout.fillWidth: true
            actions: card.actions
            position: ToolBar.Footer
            flat: false
        }
    }
}
