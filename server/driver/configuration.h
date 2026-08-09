/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "bitrate_controller.h"
#include "hostname.h"
#include "wivrn_config.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>

#include "wivrn_packets.h"

namespace wivrn
{

enum class service_publication
{
	none,
	avahi,
};

struct configuration
{
	struct encoder
	{
		std::string name;
		std::optional<wivrn::video_codec> codec;
		std::map<std::string, std::string> options;
		std::optional<std::string> device;
	};

	// Desktop mirror of the headset view, published as a PipeWire video source.
	// Disabled by default: it costs a resample and a readback per captured frame.
	struct mirror_config
	{
		bool enabled = false;
		int fps = 30;
		// Fraction of the per-eye render resolution used for the published video
		float scale = 0.5;
	};

	// Multipath (secondary path over the USB tunnel) tuning
	struct multipath_config
	{
		// Ceiling applied on top of the client's while video is carried by the
		// secondary path. adb forward over USB 2.0 tops out well below what a
		// good 5 GHz link does, so the AIMD needs a lower budget to converge in.
		uint32_t usb_max_bitrate_bps = 100'000'000;
	};

	// Separate streaming of overlay quad layers (wlx-overlay-s / WayVR panels and
	// the like), the bandwidth guard for a feature the headset turns on.
	struct quad_layer_config
	{
		// Largest square the promoted layer is encoded into, before alignment.
		// Zero disables the feature server side whatever the headset asks.
		uint16_t max_size = 1024;
		// Promote a quad even when the application asked for its alpha channel
		// to be blended. Off by default: only the colour of the layer is
		// streamed, so a panel that is actually translucent would come out
		// opaque. Turn it on for overlays known to be solid rectangles.
		bool allow_blended = false;
	};

	// Packet pacing of the video shards, the server side switch for a feature
	// the headset also has a toggle for
	struct pacing_config
	{
		bool enabled = true;
		// Fraction of a frame period a frame's shards are spread over. Clamped
		// to shard_pacer::max_window (0.5) where it is applied: the automatic
		// bitrate reads link utilisation as the fraction of a frame period a
		// frame took to arrive, and a window near its 0.60 probe-up threshold
		// would stop it ever raising the bitrate again.
		float window = 0.4f;
	};

	std::array<encoder, 4> encoders; // left, right, alpha, quad
	quad_layer_config quad_layers;
	pacing_config pacing;
	// Automatic bitrate control, the ceiling is always the bitrate requested by the client
	bitrate_controller::config bitrate_auto;
	multipath_config multipath;
	mirror_config mirror;
	std::optional<uint8_t> bit_depth;
	std::optional<std::array<float, 3>> grip_surface;
	std::vector<std::string> application;
	bool debug_gui = false;
	bool use_steamvr_lh = false;
	std::optional<float> lh_stick_deadzone;
	bool hid_forwarding = false;
	bool tcp_only = false;
	int port = wivrn::default_port;
	std::string hostname = wivrn::hostname();
	service_publication publication = service_publication::avahi;

	// monostate: default value, string: user defined, nullptr: disabled
	std::variant<std::monostate, std::string, std::nullptr_t> openvr_compat_path;

	static void set_config_file(const std::filesystem::path &);
	static std::filesystem::path get_config_file();

	static nlohmann::json read_configuration();
	configuration();
};

std::string server_cookie();

struct headset_key
{
	std::string public_key;
	std::string name;
	std::optional<std::chrono::system_clock::time_point> last_connection;
};

std::vector<headset_key> known_keys();
void add_known_key(headset_key key);
void remove_known_key(const std::string & key);
void rename_known_key(headset_key key);
void update_last_connection_timestamp(const std::string & key);

} // namespace wivrn
