/*
 * WiVRn NX
 * Copyright (C) 2026  nerdrx
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace wivrn
{

// The transport's tile grid over an eye PAIR.
//
// nxvc reports a stream's geometry with a deliberate asymmetry ([SYN] 3.3 -- "a picture is
// one eye"): `tiles_x` and `tiles_y` are PER EYE, while `tile_count` spans the pair, which
// is also the length of every per-tile array in the ABI and the transport's linear tile
// index. The transport's grid is over the pair, so
//
//     cols = eyes * tiles_x        rows = tiles_y
//
// This lives in one place because it was written twice -- once in the client decoder's
// on_stream_header and once in the reference codec's tile_grid -- and both had it as
//
//     cols = tiles_x               rows = tile_count / tiles_x
//
// which is the SAME NUMBERS WITH THE AXES SWAPPED: at eyes == 2, 17x34 where the truth is
// 34x17. The tile count is identical either way, so nothing overflows, nothing asserts,
// and every buffer is the right size. What moves is where the band boundaries fall, and
// therefore which nonce every datagram is sealed with; the receiver derives its own from
// its own copy of the same struct, the two ends disagree silently, and the symptom is an
// authentication failure on every datagram, which names nothing.
//
// The two forms coincide exactly when eyes == 1, which is every stream this fork has sent
// so far. tests/nxwarp_stereo_grid_test.cpp pins both the pair and that coincidence.
struct nxwarp_grid
{
	uint16_t cols = 0; // over the eye pair
	uint16_t rows = 0; // per eye, and NOT doubled by pairing

	constexpr uint32_t tiles() const
	{
		return uint32_t(cols) * rows;
	}
};

// `tiles_x`/`tiles_y` per eye, `eyes` 1 or 2. Takes the two numbers rather than a
// stream_info so that both ends can call it: the server has an nxvc_tile_layout, the
// client an nxvc_vkd_stream_info, and neither header is visible to the other.
constexpr nxwarp_grid nxwarp_tile_grid(uint32_t tiles_x, uint32_t tiles_y, uint32_t eyes)
{
	return {
	        .cols = uint16_t((eyes ? eyes : 1) * tiles_x),
	        .rows = uint16_t(tiles_y),
	};
}

// Band height, clamped to the grid. 6 is the contract's default; the server lets an option
// move it and then sends nothing about it, so both ends must clamp the same way.
constexpr uint16_t nxwarp_band_rows(uint16_t rows, uint32_t requested = 6)
{
	return uint16_t(std::min<uint32_t>(rows, requested ? requested : 6));
}

} // namespace wivrn
