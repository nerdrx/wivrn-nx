pragma Singleton

import QtCore

Settings {
	property bool first_run: true
	property bool show_system_checks: true

	// NX design language: deep-space color scheme + nebula background.
	// The color scheme part is applied from main.cpp before QML loads,
	// so turning it off only fully applies after a restart.
	property bool nx_theme: true
	property string last_run_version: ""

	property bool adb_custom: false
	property string adb_location: ""

	property bool auto_connect_usb: false

	// Arm `adb reverse` while a session is running so that the headset can use
	// the cable as a backup path
	property bool usb_backup_tunnel: true
}

