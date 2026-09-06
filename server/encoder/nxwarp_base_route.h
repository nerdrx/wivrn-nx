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

// nxwarp_base_route -- the road from the base layer's encoded bytes to
// base-sourced tiles in the nxwarp encoder's atlas.
//
// One access unit in, one patched atlas out:
//
//   stream 1's HEVC encoder emits the AU for frame F
//     -> nxwarp_base_shadow decodes it (conforming, one in one out)
//       -> base_patcher stages the picture into the atlas layout
//         -> nxvc_vk_encoder_atlas_write_tiles(src_frame = F)
//           -> frame F+1's mode decision sees those tiles as already covered
//
// WHY THE FRAME NUMBER IS THE WHOLE PROBLEM.  A base-sourced tile records the
// frame its pixels came FROM, and SYNTAX 13.12.9's ordering rule compares that
// number against what the position already holds: a write that does not ADVANCE
// the position is dropped.  So the AU for frame F must patch with src_frame F
// -- not with "now", not with a counter of patches -- or the rule compares two
// unrelated sequences and either drops every patch or accepts a stale one over
// a fresh coded tile.  The encoder is not able to detect the mistake: both are
// well-formed numbers.
//
// AND WHY IT MUST LAND BEFORE THE NEXT ENCODE.  The patch is for frame F's
// pixels, and the frame that can benefit is F+1 -- the next mode decision.
// nxvc records the copy on the encoder's own command buffer at the top of the
// next encode, so "before frame F+1's encode" is a real deadline on the calling
// order, not a formality.  submit() is therefore called from the base encoder's
// own encode(), which by construction runs inside frame F.
//
// WHEN THE BASE FALLS BEHIND.  It will: the base rides an ordinary HEVC
// encoder, and nothing couples its latency to stream 0's.  Two distinct
// failures, and neither may become a wrong picture:
//
//   * LATE.  The AU for frame F arrives after frame G > F has already been
//     coded and its tiles are in the atlas at src_frame G.  13.12.9 drops
//     those writes and the call succeeds -- which is correct, and is why the
//     supersede count is not an error path.  Counted, not warned about.
//   * STALE / OUT OF ORDER.  An AU whose frame number does not advance the one
//     this route last accepted.  Refused HERE, before the decoder sees it,
//     because feeding a conforming decoder pictures out of order corrupts its
//     reference state for every later frame -- a fault that outlives the frame
//     that caused it.  The decoder is opened one-in-one-out precisely so that
//     this is the only ordering this class has to enforce.
//
// A refusal is never fatal: the atlas simply keeps coded tiles where base ones
// would have gone, which costs bitrate and not correctness.  That asymmetry is
// the reason every path here degrades rather than throws.

#include "nxwarp_base_patch.h"
#include "nxwarp_base_shadow.h"
#include "nxwarp_codec.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <vulkan/vulkan.h>

namespace wivrn
{

class base_route
{
public:
	// Throws std::runtime_error if the codec has no atlas to patch or libavcodec
	// has no HEVC decoder. Both are configuration faults, caught at stream start.
	base_route(nxwarp_codec & codec, VkPhysicalDevice phys, VkDevice dev, uint32_t eyes);

	struct stats
	{
		uint64_t submitted = 0;  // access units handed in
		uint64_t decoded = 0;    // pictures the shadow decoder produced
		uint64_t patched = 0;    // AUs that reached the atlas
		uint64_t applied = 0;    // tiles written
		uint64_t superseded = 0; // tiles dropped by the ordering rule
		uint64_t refused_order = 0;
		uint64_t refused_decode = 0;
		uint64_t refused_geometry = 0;
		uint64_t refused_colour = 0;
	};

	// One access unit for frame `frame_index`, Annex-B, as the base encoder
	// produced it. Returns true if it reached the atlas.
	bool submit(uint64_t frame_index, std::span<const uint8_t> au);

	const stats & get_stats() const
	{
		return st;
	}

	// The colour guard's complaint, empty while the base is configured right.
	// Sampled after the first decode: the decoder does not know the stream's
	// range until it has read a picture out of it.
	const std::string & colour_fault() const
	{
		return colour_bad;
	}

	const nxwarp_codec::atlas_slot_layout & layout() const
	{
		return patcher->layout();
	}

private:
	base_shadow_decoder shadow;
	std::unique_ptr<base_patcher> patcher;
	stats st;
	std::string colour_bad;
	bool colour_checked = false;
	bool warned_colour = false;
	bool warned_geometry = false;
	// The frame number of the last AU this route accepted. A submission must
	// advance it.
	std::optional<uint64_t> last_frame;
};

} // namespace wivrn
