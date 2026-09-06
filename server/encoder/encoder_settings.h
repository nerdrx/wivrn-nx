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

#include <array>
#include <format>
#include <ranges>
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
	// encoder is created for it and it takes no share of the bitrate. The quad
	// layer stream is disabled when the headset did not ask for it, and the RIGHT
	// EYE stream is disabled when NX Warp pairs the eyes into one stereo frame --
	// see `eyes` below.
	bool enabled = true;
	// Array layer of the compositor image this stream is encoded from. Streams
	// 0 to 2 read the shared eye image, the quad stream has an image of its own.
	uint32_t src_layer = 0;
	// Eyes this ONE stream carries, 1 or 2. Only the NX Warp encoder ever sets 2:
	// nxvc can code both eyes of a frame as a single stereo frame, which is worth
	// -28.6 % of the headset's decode GPU (the two eyes' decoders serialise there,
	// and Pass A's cost is a step function of workgroup count that is starved at
	// one eye's 289 tiles) and -45 % of the encoder's E-stages for the same
	// reason. At 2 this stream reads BOTH `src_layer` and `src_layer_right`, the
	// right-eye stream is disabled, and no encoder is created for it.
	//
	// This is the eye PAIRING and not the STEREO tool: the cross-eye predictor
	// (nxvc tool bit 12) stays off and each eye is still coded independently, so
	// nothing here depends on inter-view prediction being implemented.
	uint32_t eyes = 1;
	// The second eye's array layer, read only when `eyes` is 2.
	uint32_t src_layer_right = 1;
	// What this stream IS, and what the client is told on the wire. Set from
	// default_stream_role() for every stream in get_encoder_settings(); only the
	// hybrid base layer changes it afterwards, taking stream 1's slot when the eye
	// pairing vacates it. See docs/NXWARP-HYBRID.md §10 and wivrn_packets.h's
	// `stream_role`.
	//
	// This member deliberately has NO default initialiser that is a real role. It
	// used to default to `view`, and because get_encoder_settings() never assigned
	// it, every stream went on the wire labelled `view` -- including the alpha
	// plane, which the server enables but never sends a frame for. The client's
	// views_ready() gate then waited forever for a picture on a stream that is not
	// a picture, never reached state::streaming, and the lobby never pushed the
	// stream scene: a connected, decoding, feedback-sending session that renders
	// the lobby at 90 fps and calls scenes::stream::render() exactly never.
	//
	// `unset` makes that failure impossible to repeat quietly: it is not a valid
	// wire value, check_stream_roles() rejects it, and anything that forgets to
	// assign a role is caught at the point the description is built rather than by
	// a headset that will not start.
	stream_role role = stream_role::unset;
	// For a `base` stream, the index of the stream whose atlas it fills. 0xff
	// elsewhere. The base names its enhancement stream rather than the client
	// inferring the pairing from the indices.
	uint8_t serves_stream = 0xff;
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

// THE positional convention, in one place. Streams 0 and 1 are the eyes, 2 is the
// passthrough alpha plane, 3 is the promoted quad layer. Everything that needs to know
// what a stream slot means by default reads it from here, so the server's idea of a
// stream's role and the packet's cannot drift apart -- which is precisely what happened
// when the settings defaulted every slot to `view` while the packet defaulted them
// positionally, and the settings won.
constexpr stream_role default_stream_role(size_t stream_index)
{
	switch (stream_index)
	{
		case 0:
		case 1:
			return stream_role::view;
		case 2:
			return stream_role::alpha;
		case quad_stream_idx:
			return stream_role::quad;
		default:
			return stream_role::view;
	}
}

// The two definitions agree, checked by the compiler rather than by a session that fails
// to start. to_headset::video_stream_description::role is the wire's own default array;
// if either side is reordered or extended without the other, this stops the build.
static_assert(num_streams == std::tuple_size_v<decltype(to_headset::video_stream_description{}.role)>);
static_assert([] {
	to_headset::video_stream_description d{};
	for (size_t i = 0; i < num_streams; ++i)
		if (d.role[i] != default_stream_role(i))
			return false;
	return true;
}(),
              "encoder_settings' positional roles and the wire description's default roles disagree");

// The role's name, for messages. Not magic_enum: this header is included by a test that
// links nothing, and a mislabelled stream is exactly the situation where the reader needs
// a word rather than an integer.
constexpr const char * role_name(stream_role r)
{
	switch (r)
	{
		case stream_role::view:
			return "view";
		case stream_role::alpha:
			return "alpha";
		case stream_role::quad:
			return "quad";
		case stream_role::base:
			return "base";
		case stream_role::unset:
			return "unset";
	}
	return "unknown";
}

// Every enabled stream carries a role that the client can route on, and it is either the
// positional default or a documented override. Returns an empty string when the settings
// are consistent, and otherwise says which stream is wrong -- the caller logs it.
//
// The one legitimate override is the hybrid base layer on the stream-1 slot, which the
// eye pairing vacates (docs/NXWARP-HYBRID.md §10).
inline std::string check_stream_roles(const std::array<encoder_settings, num_streams> & encoders)
{
	for (auto [i, e]: std::ranges::enumerate_view(encoders))
	{
		// A disabled stream is never advertised: the compositor skips it and the
		// packet keeps its own positional default, so its role does not matter.
		if (not e.enabled)
			continue;

		const size_t idx = size_t(i);
		if (e.role == stream_role::unset)
			return std::format("stream {} is enabled but no role was assigned to it", idx);

		if (e.role == default_stream_role(idx))
			continue;

		// The one documented override: the hybrid base layer moves into the
		// stream-1 slot after the eye pairing vacates it, and names the stream
		// whose atlas it fills.
		if (idx == 1 and e.role == stream_role::base and e.serves_stream == 0)
			continue;

		return std::format("stream {} is labelled '{}' but that slot is the '{}' stream, and this is not the base-layer override",
		                   idx,
		                   role_name(e.role),
		                   role_name(default_stream_role(idx)));
	}
	return {};
}


std::array<encoder_settings, num_streams> get_encoder_settings(wivrn::vk_bundle &, wivrn_session &);

void print_encoders(const std::array<wivrn::encoder_settings, num_streams> & encoders);

} // namespace wivrn
