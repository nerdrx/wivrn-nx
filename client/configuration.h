/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_discover.h"
#include "wivrn_packets.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <simdjson.h>
#include <string>
#include <type_traits>
#include <vector>
#include <openxr/openxr.h>

namespace xr
{
class session;
class system;
} // namespace xr

// key <-> member serialization descriptor, defined in configuration.cpp
struct config_field;

enum class feature
{
	microphone,
	hand_tracking,
	eye_gaze,
	face_tracking,
	body_tracking,
};

class configuration
{
public:
	struct server_data
	{
		bool autoconnect;
		bool manual;
		bool visible;
		bool compatible;

		wivrn_discover::service service;
	};

	std::map<std::string, server_data> servers;
	float preferred_refresh_rate = 0;
	std::optional<float> minimum_refresh_rate;
	float resolution_scale = 1.0;
	std::optional<wivrn::video_codec> codec;
	uint32_t bitrate_bps = 50'000'000;
	// Let the server adapt the bitrate to the link quality, using bitrate_bps as the maximum
	bool bitrate_auto = true;
	// Which control law it should use: the original AIMD one (false) or the experimental
	// bandwidth estimating one (true). Empty until the user picks one of the two adaptive
	// entries in the bitrate selector, and while it is empty the server's own configuration
	// decides — a server set up for one law keeps it for everyone who never chose.
	std::optional<bool> bitrate_bbr;
	// What goes on the wire for the above
	std::optional<wivrn::bitrate_mode> bitrate_control() const
	{
		if (not bitrate_bbr)
			return std::nullopt;
		return *bitrate_bbr ? wivrn::bitrate_mode::bbr : wivrn::bitrate_mode::aimd;
	}
	// Report the Wi-Fi radio state (~1 Hz) so that the automatic bitrate can step down on a
	// falling signal, before the packet loss it is about to cause. Needs bitrate_auto.
	bool radio_aware = true;
	uint8_t bit_depth = 10;
	// Ask the server to bias the encoders for fine detail instead of a smooth image
	bool sharp_text = false;

	// Attach a secondary path over the USB cable while streaming. Off means the
	// tunnel is never probed and the server never sees a second path at all, which
	// is why this stayed a plain switch: it is the one state the server cannot be
	// told about, because there is nothing to tell it over.
	bool multipath_usb = true;
	// What that path is for, when there is one: a spare the session falls over to
	// (false, the original meaning of the switch and still the default), or a second
	// link carrying the tail of every frame alongside Wi-Fi (true, experimental).
	// Only meaningful with multipath_usb on, exactly like bitrate_bbr under
	// bitrate_auto and motion_smoothing_server under motion_smoothing.
	bool multipath_combine = false;
	// What goes on the wire for the two above
	wivrn::multipath_mode multipath_mode() const
	{
		if (not multipath_usb)
			return wivrn::multipath_mode::off;
		return multipath_combine ? wivrn::multipath_mode::combine : wivrn::multipath_mode::backup;
	}

	// Ask the server to spread each video frame's packets over a fraction of the
	// frame period instead of bursting them out, so that the access point's
	// buffer is never asked to swallow a whole frame at once
	bool smooth_pacing = true;

	// Ask the server to send a parity shard per group of video shards, so that a
	// datagram the Wi-Fi drops is rebuilt here instead of costing the whole frame
	// and the keyframe round trip that follows it. Costs about 12% of the video
	// bandwidth, which the server takes out of the encoder rather than adding on top.
	bool fec = true;

	// Mark both ends' sockets with a DSCP class, which access points map to the
	// WMM access categories. Off is for the networks that mangle marked traffic.
	bool wifi_qos = true;

	// Let the server hand a stream to its software encoder when the graphics
	// driver's encoder dies or stops answering, instead of that eye freezing for
	// the rest of the session. Costs CPU on the PC while it lasts.
	bool encoder_failover = true;

	// Let a controller that goes to sleep hold its last tracked pose on the PC instead of
	// following whatever pose the runtime keeps reporting for it. On by default: it is what
	// stops a sleeping Pico controller from teleporting across the play space.
	bool standby_freeze = true;

	// Darken the periphery of the streamed image when the application frame rate collapses
	bool comfort_vignette = true;

	// Synthesize the frames the application does not produce, by warping the last one
	// along the motion the server measured between application frames. Off by default:
	// the artefacts it trades judder for depend on the content.
	bool motion_smoothing = false;
	// Which end does the warping when motion_smoothing is on: here (false, the original
	// and the default) or on the PC (true, experimental). Only meaningful with
	// motion_smoothing on, exactly like bitrate_bbr under bitrate_auto.
	bool motion_smoothing_server = false;
	// What goes on the wire for the two above
	wivrn::motion_mode motion_mode() const
	{
		if (not motion_smoothing)
			return wivrn::motion_mode::off;
		return motion_smoothing_server ? wivrn::motion_mode::server : wivrn::motion_mode::headset;
	}

	// Ask the server to stream one overlay panel (wlx-overlay-s, WayVR and the like)
	// as a layer of its own instead of baking it into the eye images, and submit it
	// here as a real quad layer. On by default: it is strictly sharper and the
	// runtime, rather than the server, holds it still when the head moves.
	bool quad_layers = true;

	// Contrast adaptive sharpening, applied to the decoded image by the reprojection pass
	bool cas_sharpening = false;
	float cas_sharpness = 0.5;

	// Ambient bias lighting: bleed the frame's edge colours outward into the black
	// periphery beyond the headset's field of view as a soft glow, softening the hard
	// cutoff at the FOV edge. Applied by the reprojection pass, client side, in stream.
	bool ambient_glow = true;
	float ambient_glow_intensity = 0.4;

	bool passthrough_enabled = false;
	bool mic_unprocessed_audio = false;

	// Send audio on the same loss-tolerant path as the video instead of sharing the
	// control socket with everything else, with a sequence number per packet and
	// concealment of what is lost. A dropped datagram then costs a concealed few
	// milliseconds instead of stalling every later audio packet behind it while TCP
	// retransmits. Applies both ways: the headset's microphone and the PC's speaker.
	bool low_latency_audio = true;

	// Input forwarding, per device. Off by default; only effective if the server permits it.
	bool forward_keyboard = false;
	bool forward_mouse = false;
	bool forward_gamepad = false;

	std::underlying_type_t<wivrn::from_headset::body_part_mask> body_part_mask = ~0;

	bool enable_stream_gui = true;

	// application launcher: list vs grid, and grid icon size, 0 small 1 medium 2 large
	bool app_list_view = false;
	uint32_t app_icon_size = 0;

	// interface theme, defaults match the built-in "OLED" preset and "NX" accent
	std::string theme_preset = "OLED";
	std::string theme_accent = "NX";
	float theme_rounding = 8;
	float theme_card_rounding = 14;
	float theme_font_scale = 1.0;
	float theme_background_alpha = 0.75;

	// XR_FB_composition_layer_settings extension flags
	struct openxr_post_processing_settings
	{
		XrCompositionLayerSettingsFlagsFB super_sampling = 0;
		XrCompositionLayerSettingsFlagsFB sharpening = 0;
	};
	openxr_post_processing_settings openxr_post_processing{};

	std::string virtual_keyboard_layout = "QWERTY";

	std::string environment_model = "assets://ground.glb";

	bool override_foveation_enable = false;
	float override_foveation_pitch = -10 * M_PI / 180;
	float override_foveation_distance = 3;

	bool high_power_mode = true;
	uint32_t fps_divider = 1;

	// Allow unsafe config values
	bool extended_config = false;

	bool first_run = true;

	std::string locale;

	bool check_feature(feature f) const;
	void set_feature(feature f, bool state);

private:
	mutable std::mutex mutex;
	std::map<feature, bool> features;
	std::optional<float> stream_scale;

	// table of scalar settings shared by save()/load(); non-scalar settings are explicit
	static const std::vector<config_field> & config_fields();

public:
	configuration(xr::system &, xr::session &);
	configuration(xr::system &, xr::session &, const std::filesystem::path &);

	void save();

	void set_stream_scale(float);
	float get_stream_scale() const;
	float get_default_stream_scale() const;

	uint32_t max_bitrate(bool extended) const
	{
		return extended ? 800'000'000u : 200'000'000u;
	}

	uint32_t max_bitrate() const
	{
		return max_bitrate(extended_config);
	}
};
