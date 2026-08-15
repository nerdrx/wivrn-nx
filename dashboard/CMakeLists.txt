include(KDEInstallDirs)
include(KDECMakeSettings)
include(ECMFeatureSummary)
include(ECMFindQmlModule)
include(ECMQmlModule)
include(I18n)

ecm_find_qmlmodule(org.kde.kirigami REQUIRED)
ecm_find_qmlmodule(org.kde.kirigamiaddons.formcard REQUIRED)
ecm_feature_summary(WHAT REQUIRED_PACKAGES_NOT_FOUND FATAL_ON_MISSING_REQUIRED_PACKAGES)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(wivrn-dashboard
    avahi.cpp
    main.cpp
    wivrn_server.cpp
    wivrn_qdbus_types.cpp
    apps.cpp
    dashboard_utils.cpp
    escape_string.cpp
    settings.cpp
    steam.cpp
    adb.cpp
    apk_installer.cpp
    firewall.cpp
    vulkan_info.cpp
    mirror_view.cpp
)

set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../dbus/io.github.wivrn.Server.xml PROPERTIES INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/wivrn_qdbus_types.h")
qt_add_dbus_interface(DBUS_INTERFACE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../dbus/io.github.wivrn.Server.xml wivrn_server_dbus)
target_sources(wivrn-dashboard PRIVATE ${DBUS_INTERFACE_SRC})

set_source_files_properties(../images/wivrn-nx.svg PROPERTIES QT_RESOURCE_ALIAS qml/wivrn.svg)
qt_add_resources(wivrn-dashboard PREFIX "/" BASE .. FILES ../images/wivrn-nx.svg)

# NX design language color scheme, applied app-locally from main.cpp
qt_add_resources(wivrn-dashboard "nx-theme" PREFIX "/" FILES nx.colors)

set_source_files_properties(qml/DashboardSettings.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(wivrn-dashboard
    URI io.github.wivrn.wivrn
    # MirrorView exposes a QVideoSink property and MirrorPage uses VideoOutput
    DEPENDENCIES
        "QtMultimedia auto"
    QML_FILES
        qml/Main.qml
        qml/NxBackground.qml
        qml/About.qml
        qml/ApkInstallPage.qml
        qml/DashboardSettings.qml
        qml/SettingsPage.qml
        qml/HeadsetsPage.qml
        qml/HeadsetStatsPage.qml
        qml/MirrorPage.qml
        qml/WizardPage.qml
        qml/WizardStep.qml
        qml/DeviceCard.qml
        qml/BetterLabel.qml
        qml/ConnectUsbDialog.qml
        qml/SelectGame.qml
        qml/SteamLaunchOptions.qml
        qml/TroubleshootPage.qml
        qml/TroubleshootCard.qml
    RESOURCES
        ../images/wivrn-nx.svg
        qml/Discord.svg
        qml/Github.svg
)

configure_file(gui_config.h.in gui_config.h)
configure_file(io.github.wivrn.wivrn.desktop.in io.github.wivrn.wivrn.nx.desktop)

target_link_libraries(wivrn-dashboard
    PRIVATE
    Qt6::DBus
    Qt6::Quick
    Qt6::Qml
    Qt6::Gui
    Qt6::QuickControls2
    Qt6::Multimedia
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::I18nQml
    KF6::IconThemes
    KF6::Kirigami
    QCoro::Core
    QCoro::Network
    QCoro::Qml
    wivrn-common-base
    wivrn-common-server
    wivrn-external
)

if (WIVRN_USE_PIPEWIRE)
    target_link_libraries(wivrn-dashboard PRIVATE PkgConfig::libpipewire)
endif()

# Optional: NX deep-space color scheme needs KColorScheme; without it the
# dashboard falls back to the system scheme.
find_package(KF6ColorScheme QUIET)
find_package(KF6Config QUIET)
if (TARGET KF6::ColorScheme AND TARGET KF6::ConfigCore)
    target_link_libraries(wivrn-dashboard PRIVATE KF6::ColorScheme KF6::ConfigCore)
    target_compile_definitions(wivrn-dashboard PRIVATE WIVRN_HAVE_KCOLORSCHEME)
endif()

install(TARGETS wivrn-dashboard ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

install(FILES io.github.wivrn.wivrn.metainfo.xml
	DESTINATION share/metainfo
)

install(FILES ../images/wivrn-nx.svg
	DESTINATION share/icons/hicolor/scalable/apps
	RENAME io.github.wivrn.wivrn.nx.svg
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.wivrn.wivrn.nx.desktop
	DESTINATION share/applications
)

create_mo_files(wivrn-dashboard wivrn-dashboard "${CMAKE_CURRENT_SOURCE_DIR}/../locale")
