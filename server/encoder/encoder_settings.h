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

#include "wivrn_packets.h"

#include <map>
#include <optional>
#include <string>

namespace wivrn
{
struct vk_bundle;
class wivrn_session;

struct encoder_settings
{
	uint16_t width;
	uint16_t height;
	video_codec codec; // left, right, alpha, quad
	float fps;
	// False when the stream is not used at all this session, in which case no
	// encoder is created for it and it takes no share of the bitrate. Only the
	// quad layer stream is ever disabled, when the headset did not ask for it.
	bool enabled = true;
	// Array layer of the compositor image this stream is encoded from. Streams
	// 0 to 2 read the shared eye image, the quad stream has an image of its own.
	uint32_t src_layer = 0;
	// encoder identifier, such as nvenc, vaapi or x264
	std::string encoder_name;
	uint64_t bitrate;                           // bit/s
	double bitrate_multiplier;                  // encoder bitrate / global bitrate
	std::map<std::string, std::string> options; // additional encoder-specific configuration
	int bit_depth;
	// Bias the encoder towards keeping fine detail (text, user interfaces) rather than a
	// smooth image, requested by the headset. Only read when the encoder is created.
	bool sharp_text = false;
	// Foveation v2 lever 3: protect the static foveal rectangle with a per-region QP bias.
	// Only NVENC and x264 have a path for it (still a TODO there); VAAPI and Vulkan log once
	// that they cannot honour it. Only read when the encoder is created.
	bool foveation_foveal_qp = false;
	// Recover from unrecoverable loss with a rolling intra refresh instead of a keyframe.
	// Both switches (server configuration and headset toggle) ANDed. The refresh mechanism
	// is part of the encode session's configuration on every encoder that has one, so this
	// can only be read when the encoder is created; the live half of the switch goes through
	// video_encoder::set_intra_refresh. x264 and NVENC honour it, VAAPI has no such control
	// and the Vulkan encoders already recover without keyframes.
	bool intra_refresh = true;
	// Repair loss by invalidating the reference the headset never got, one rung cheaper than
	// the refresh above. Both switches ANDed, and read only when the encoder is created for
	// the same reason: the deeper reference buffer this needs is part of the encode session's
	// configuration. NVENC honours it (NvEncInvalidateRefFrames); the Vulkan encoders already
	// behave this way with no switch at all; x264 and the FFmpeg VAAPI encoders expose no
	// per-frame reference control and keep to the rungs above.
	bool ref_invalidation = true;
	std::optional<std::string> device;
	// The nxvc tool bits the headset's decoder advertised (headset_info_packet), for
	// video_codec::nxwarp only. Zero means the headset reported none, which is "no
	// information" and not "supports nothing": the NX Warp encoder then keeps to the
	// tools every decoder has and negotiates nothing. See video_encoder_nxwarp.cpp.
	uint64_t nxvc_tools = 0;
	// The linear fraction of the headset's stream eye size this stream is actually encoded
	// at: min(headset render_scale, server "stream_scale"). Same for every eye stream. The
	// foveation guardrail needs the effective value, not the headset's half of it, or a
	// server-side downscale would let the periphery collapse past what the guardrail allows.
	float encode_scale = 1.f;
};

// Number of video streams: left, right, alpha, quad
inline constexpr size_t num_streams = 4;
// Stream carrying the promoted quad layer
inline constexpr uint8_t quad_stream_idx = 3;

std::array<encoder_settings, num_streams> get_encoder_settings(wivrn::vk_bundle &, wivrn_session &);

void print_encoders(const std::array<wivrn::encoder_settings, num_streams> & encoders);

} // namespace wivrn
