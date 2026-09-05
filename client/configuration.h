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

#include <algorithm>
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

	// Reduced resolution streaming: ask the server to encode the eye images at a fraction
	// of the size it otherwise would, so the stream is encoded, transmitted and decoded at
	// a lower resolution to save bitrate, encode and decode cost. The headset's display /
	// defoveated resolution is unchanged; the smaller decoded image is reconstructed when
	// it is sampled (bilinear on its own, sharp when FSR upscaling is on). Off by default;
	// render_scale is the slider value that applies when reduced_resolution is on. Changing
	// the encoded size cannot be done live, so it takes effect on the next connection.
	bool reduced_resolution = false;
	float render_scale = 0.75;
	// The factor actually sent to the server: 1.0 unless reduced resolution is on, clamped
	// to a sane range so a bad config value can never ask for a degenerate encode size.
	float effective_render_scale() const
	{
		if (not reduced_resolution)
			return 1.0f;
		return std::clamp(render_scale, 0.5f, 1.0f);
	}

	// Fixed-foveation "sharper center" (foveation v2). Reshapes the server's foveation curve at
	// a fixed encode size so the centre stays 1:1 over a wider plateau and the periphery falls
	// off more steeply — more central sharpness for the same encoded size. Off by default so
	// behaviour is unchanged; foveation_strength is the slider that applies when it is on.
	// Applied live by the server (recomputed per frame), so it takes effect without a reconnect.
	bool sharper_center = false;
	float foveation_strength = 0.5;
	// The factor actually sent to the server: 0 (neutral) unless sharper center is on.
	float effective_foveation_strength() const
	{
		if (not sharper_center)
			return 0.0f;
		return std::clamp(foveation_strength, 0.0f, 1.0f);
	}
	// Let the server steepen that curve on its own as the link degrades (needs the automatic
	// bitrate). Off by default.
	bool foveation_adaptive = false;
	// Ask the server to protect foveal quality with a per-region QP bias where the encoder can
	// (NVENC, x264). VAAPI/Vulkan have no such path and ignore it. Off by default.
	bool foveation_foveal_qp = false;

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

	// Let the server size that redundancy to the loss it is measuring rather than
	// always sending the same amount, and spread each group's shards out so that a
	// burst of consecutive packets lost on the air costs one recoverable hole in
	// several groups instead of several in one. Extends the switch above: with error
	// correction off this does nothing at all.
	bool fec_adaptive = true;

	// Ask the server to send a video packet again when one goes missing that the
	// redundancy cannot rebuild, instead of losing the frame. On a LAN the round trip
	// is a couple of milliseconds against an 11 ms frame, so the answer still arrives
	// in time; off, the server keeps no history and nothing is ever asked for.
	bool shard_retransmit = true;

	// Mark both ends' sockets with a DSCP class, which access points map to the
	// WMM access categories. Off is for the networks that mangle marked traffic.
	bool wifi_qos = true;

	// Let the server hand a stream to its software encoder when the graphics
	// driver's encoder dies or stops answering, instead of that eye freezing for
	// the rest of the session. Costs CPU on the PC while it lasts.
	bool encoder_failover = true;

	// Repair loss the error correction and the retransmissions could not with a rolling
	// sweep of intra coded blocks over the next half second, instead of a full keyframe.
	// A keyframe is the largest frame there is and it is asked for exactly when the link
	// is worst, which is how one lost frame turns into several.
	bool intra_refresh = true;

	// Before that sweep, try the cheaper repair: tell the PC's encoder not to predict from the
	// frame that never arrived, so the next one it sends is built on an older frame this
	// headset still holds. Costs one ordinary frame and lands immediately. Falls back to the
	// sweep when the loss is too old to reach or the repair is itself lost. Needs an encoder
	// that can do it (NVIDIA can; the Vulkan encoders always have); the PC also has its own
	// switch.
	bool ref_invalidation = true;

	// Hold decoded video frames for a short adaptive delay and release them on schedule,
	// instead of showing whichever one is nearest the moment it is ready. Trades a little
	// latency for even pacing on a link whose frames arrive in clumps; on a calm link the
	// delay settles at zero and nothing changes at all. Off by default — the latency is real
	// and only worth paying when the link is erratic. Headset-side only, the PC never hears
	// about it.
	bool dejitter = false;

	// Last automatic resort below the bitrate floor: when the server's automatic bitrate is
	// already pinned at its minimum and the link is still losing frames, let it halve the
	// stream framerate to instantly halve bandwidth, restoring the full rate once the link
	// recovers. Independent of the manual "Half framerate mode". The server also has its own
	// switch; both must be on. On by default.
	bool emergency_framerate = true;

	// Let a controller that goes to sleep hold its last tracked pose on the PC instead of
	// following whatever pose the runtime keeps reporting for it. On by default: it is what
	// stops a sleeping Pico controller from teleporting across the play space.
	bool standby_freeze = true;

	// Ride out a short network outage without dropping to the lobby: the stream scene is
	// held alive with the last frame frozen while the client re-handshakes to the same
	// server in the background (the server already pauses and re-accepts on its side). On
	// by default. Off restores the old behaviour of returning to the lobby on any network
	// error. Client-local: the server needs no knowledge of it.
	bool seamless_reconnect = true;

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
	// NX Warp: ask the server for the nxvc compute codec instead of a hardware one.
	//
	// Off by default while it is experimental. What the switch actually does is re-order
	// the codec list in from_headset::headset_info — nothing else — so the negotiation
	// that already exists is what picks the codec; see scenes::stream. That means no
	// protocol field, and a server that has never heard of NX Warp simply picks the next
	// codec in the list.
	bool nxwarp = true; // the side-by-side test build exists for this codec: on unless turned off

	// Frame smoothing: on the first display refresh that shows a newly decoded frame,
	// blend it half and half with the frame it replaces, so the step between two decoded
	// frames is split into two smaller ones. It hides a low decoded frame rate; it does
	// not raise it and it synthesizes nothing. Off by default, and off is bit identical
	// to not having it: the blend weight is zero and the pass samples exactly what it
	// sampled before. Applies live.
	bool frame_smoothing = false;

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

	// AMD FSR1 (EASU + RCAS) spatial upscaling, applied to the decoded image by the
	// reprojection pass in place of the plain bilinear tap. The decoded image is usually
	// smaller than the defoveated/display size (foveation, and more so with reduced
	// resolution streaming below), so EASU reconstructs the edges and RCAS sharpens.
	// Off by default: EASU costs about a dozen texture taps and this pass already runs on
	// every opaque pixel every vsync on weak GPUs (Adreno/Pico). When on it REPLACES the
	// contrast adaptive sharpening below (EASU+RCAS instead of a plain CAS pass), so the
	// CAS controls are ignored while FSR is enabled. fsr_sharpness drives the RCAS lobe,
	// 0 disables the sharpen and leaves a pure EASU upscale (a mild edge-preserving AA).
	bool fsr = false;
	float fsr_sharpness = 0.5;

	// Contrast adaptive sharpening, applied to the decoded image by the reprojection pass
	bool cas_sharpening = false;
	float cas_sharpness = 0.5;
	// Sharpening kernel: false (default) is a 5-tap cross, half the texture taps and
	// barely distinguishable on a compressed streamed image; true is the full 3x3 with
	// the diagonal taps. Only meaningful with cas_sharpening on.
	bool cas_full_kernel = false;

	// Ambient bias lighting: bleed the frame's edge colours outward into the black
	// periphery beyond the headset's field of view as a soft glow, softening the hard
	// cutoff at the FOV edge. Applied by the reprojection pass, client side, in stream.
	bool ambient_glow = true;
	float ambient_glow_intensity = 0.4;

	// Debanding: dither the decoded image just before it is quantized to the 8-bit panel,
	// breaking the colour contours 8-bit output plus video compression leaves in smooth
	// gradients (skyboxes, fog, dark rooms) into imperceptible noise. Cheap (pure ALU, no
	// extra texture taps) and broadly beneficial, especially on OLED, so on by default.
	// Applied by the reprojection pass, client side, in stream. deband_strength is in
	// units of one 8-bit step (1/255) of triangular-PDF dither; 1.0 is +/- one LSB.
	bool deband = true;
	float deband_strength = 1.0;

	// Skip re-running the in-stream defoveation pass on the refreshes where nothing it
	// draws has changed, re-presenting the last result instead. Off by default. Meant
	// for headsets that never discard a refresh (Pico), where the pass would otherwise
	// run at the full display rate even while the application sits at a low frame rate.
	// Experimental: guarded by a conservative dirty check, and it falls straight back
	// to a normal render on anything it does not recognise.
	bool reduce_gpu_load = false;

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

	// interface theme, defaults match the built-in "NX" preset and "NX" accent
	std::string theme_preset = "NX";
	std::string theme_accent = "NX";
	float theme_rounding = 5;
	float theme_card_rounding = 6;
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

	// Subtle ambient animation of the space lobby (sky drift, star twinkle,
	// gas-giant self-rotation, occasional comet). Purely client-local visual.
	bool animated_lobby = true;

	// Hyperspace star-streak transition when connecting to / returning from a
	// server. Purely client-local visual.
	bool warp_transition = true;

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
