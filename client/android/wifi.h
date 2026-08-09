/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

// State of the headset's own Wi-Fi radio, read from android.net.wifi.WifiManager.
//
// Permissions: reading the RSSI and the negotiated PHY rate needs ACCESS_WIFI_STATE, which
// the manifest already declares (it is a normal permission, granted at install time, never
// prompted). ACCESS_FINE_LOCATION is *not* needed and deliberately not requested: since
// Android 10 the location redaction of WifiInfo covers the identifiers (SSID, BSSID,
// network id, passpoint names), not the radio measurements, so getRssi() and getLinkSpeed()
// keep answering for real without it. A runtime location prompt in a headset would be a
// terrible trade for a bitrate hint.
//
// Vendor OSes are allowed to be stricter than AOSP, so every reading is validated against
// the documented sentinels (WifiInfo.INVALID_RSSI == -127, LINK_SPEED_UNKNOWN == -1) and
// against plain physical plausibility. A rejected reading is reported as invalid, never
// forwarded as a number, because the server turns these into a *trend* and a single
// injected -127 would look exactly like walking through a wall.
struct wifi_status
{
	// False when there is no usable reading at all: Wi-Fi off, another transport, a
	// sentinel, or a JNI failure. The other fields are then meaningless.
	bool valid = false;
	// Negative dBm, e.g. -50 next to the access point, -75 across the flat
	int rssi_dbm = 0;
	// Negotiated PHY rate in Mbit/s, 0 when the platform would not say
	int link_speed_mbps = 0;
};

// Reads the radio once. Cheap enough for the ~1 Hz the tracking thread calls it at; must be
// called from a thread attached to the JVM (application::setup_jni). Never throws.
wifi_status get_wifi_status();
