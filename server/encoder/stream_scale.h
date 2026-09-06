/*
 * WiVRn VR streaming
 * Copyright (C) 2025  WiVRn contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace wivrn
{

// Alignment of the encoded eye images, in pixels, on both axes.
//
// The foveation shader and the NX Warp encoder both work in 64x64 tiles, so a size that is not a
// multiple of 64 would leave a partial tile at the right and bottom edges. It also covers 4:2:0
// chroma siting, which only needs even, and every hardware encoder's own alignment requirement,
// which is 16 or 32 at worst.
inline constexpr uint16_t encode_alignment = 64;

// The per-eye size the video streams are encoded at.
//
// Derived from the per-eye stream size the headset asked for (its stream_eye_width/height) and two
// independent linear scale factors:
//
//   client_render_scale  the headset's own "reduced resolution" slider, clamped to [0.5, 1] the
//                        way the headset's UI clamps it,
//   server_stream_scale  the server configuration's "stream_scale", a ceiling for the same
//                        quantity, so the operator can trade sharpness for decode time on the
//                        headset (the NX Warp decoder's cost scales with the pixel count and the
//                        headset serialises the two eyes).
//
// The two compose as the smaller of the two, not as a product: both name the same thing — a linear
// fraction of the stream eye size — so a product would make two moderate values multiply into a
// blurry one, and the server value is meant as a cap that never sharpens a headset which asked for
// less than it.
//
// The result is rounded UP to encode_alignment on both axes and floored at one tile, so it is
// always a valid tile grid and never degenerate.
struct encode_size
{
	uint16_t width;
	uint16_t height;
	// The effective linear factor, min(client, server), before alignment. The foveation
	// guardrail reads this: it bounds the peripheral sampling factor against how much the
	// encode was already shrunk, so it needs the scale that was actually applied.
	float scale;
};

constexpr encode_size stream_encode_size(uint16_t stream_eye_width,
                                         uint16_t stream_eye_height,
                                         float client_render_scale,
                                         float server_stream_scale)
{
	const float client = std::clamp(client_render_scale, 0.5f, 1.0f);
	const float server = std::clamp(server_stream_scale, 0.f, 1.0f);
	const float scale = std::min(client, server);

	auto align = [](uint16_t value) -> uint16_t {
		const uint16_t aligned = ((value + encode_alignment - 1) / encode_alignment) * encode_alignment;
		return std::max(encode_alignment, aligned);
	};

	return {
	        .width = align(uint16_t(stream_eye_width * scale)),
	        .height = align(uint16_t(stream_eye_height * scale)),
	        .scale = scale,
	};
}

} // namespace wivrn
