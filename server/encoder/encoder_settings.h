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
	std::optional<std::string> device;
};

// Number of video streams: left, right, alpha, quad
inline constexpr size_t num_streams = 4;
// Stream carrying the promoted quad layer
inline constexpr uint8_t quad_stream_idx = 3;

std::array<encoder_settings, num_streams> get_encoder_settings(wivrn::vk_bundle &, wivrn_session &);

void print_encoders(const std::array<wivrn::encoder_settings, num_streams> & encoders);

} // namespace wivrn
