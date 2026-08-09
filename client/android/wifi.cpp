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

#include "wifi.h"

#include "application.h"
#include "jnipp.h"

#include <exception>
#include <mutex>
#include <spdlog/spdlog.h>
#include <utility>

namespace
{
// android.net.wifi.WifiInfo.INVALID_RSSI: "the platform will not tell you"
constexpr int invalid_rssi = -127;
// A real 2.4/5/6 GHz link lives between roughly -30 and -95 dBm; anything outside that is a
// sentinel or a vendor placeholder, not a measurement.
constexpr int min_plausible_rssi = -100;
constexpr int max_plausible_rssi = -20;
// android.net.wifi.WifiInfo.LINK_SPEED_UNKNOWN
constexpr int link_speed_unknown = -1;

// The WifiManager is a process singleton; looking it up costs three JNI calls, which is not
// much at 1 Hz but is entirely avoidable. The global ref is intentionally never released: it
// lives as long as the process and releasing it from an arbitrary thread at exit is a
// worse problem than leaking one object.
jobject wifi_manager()
{
	static std::once_flag once;
	static jobject cached = nullptr;

	std::call_once(once, [] {
		jni::object<""> act(application::native_app()->activity->clazz);
		auto app = act.call<jni::object<"android/app/Application">>("getApplication");
		auto ctx = app.call<jni::object<"android/content/Context">>("getApplicationContext");

		// Context.getSystemService is declared to return Object; the Java side cast to
		// WifiManager is a no-op through JNI. Context.WIFI_SERVICE is "wifi".
		jni::string key("wifi");
		auto service = ctx.call<jni::object<"java/lang/Object">>("getSystemService", key);
		if (jobject handle = service.handle())
			cached = jni::jni_thread::env().NewGlobalRef(handle);

		if (not cached)
			spdlog::info("No WifiManager available, radio-aware bitrate will report nothing");
	});

	return cached;
}
} // namespace

wifi_status get_wifi_status()
{
	try
	{
		jobject manager = wifi_manager();
		if (not manager)
			return {};

		jni::object<"android/net/wifi/WifiManager"> wifi(manager);

		// Not "is there a connection" but "is the radio on at all": on a headset docked
		// to USB tethering or Ethernet the stale WifiInfo would otherwise be reported as
		// a live measurement.
		if (not wifi.call<jni::Bool>("isWifiEnabled"))
			return {};

		auto info = wifi.call<jni::object<"android/net/wifi/WifiInfo">>("getConnectionInfo");
		if (not info.handle())
			return {};

		const int rssi = info.call<jni::Int>("getRssi");
		const int speed = info.call<jni::Int>("getLinkSpeed");

		if (rssi == invalid_rssi or rssi < min_plausible_rssi or rssi > max_plausible_rssi)
		{
			// Once, not every second: a platform that redacts this redacts it forever.
			static bool warned = false;
			if (not std::exchange(warned, true))
				spdlog::info("Wi-Fi RSSI unavailable ({} dBm), radio-aware bitrate will stay idle", rssi);
			return {};
		}

		return {
		        .valid = true,
		        .rssi_dbm = rssi,
		        .link_speed_mbps = speed == link_speed_unknown or speed < 0 ? 0 : speed,
		};
	}
	catch (const std::exception & e)
	{
		static bool warned = false;
		if (not std::exchange(warned, true))
			spdlog::warn("Failed to read the Wi-Fi state: {}", e.what());
		return {};
	}
}
