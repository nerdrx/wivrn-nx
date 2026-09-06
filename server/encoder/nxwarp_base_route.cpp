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

#include "nxwarp_base_route.h"

#include "util/u_logging.h"

#include <stdexcept>

namespace wivrn
{

base_route::base_route(nxwarp_codec & codec, VkPhysicalDevice phys, VkDevice dev, uint32_t eyes)
{
	patcher = std::make_unique<base_patcher>(codec, phys, dev, eyes);
}

bool base_route::submit(uint64_t frame_index, std::span<const uint8_t> au)
{
	++st.submitted;

	// ORDER, before the decoder sees anything. A conforming decoder fed
	// pictures out of order does not fail: it produces a picture predicted from
	// the wrong reference, and every frame after it inherits that. Refusing
	// here costs one frame of base coverage; letting it through costs the
	// stream.
	if (last_frame and frame_index <= *last_frame)
	{
		++st.refused_order;
		return false;
	}

	if (au.empty())
	{
		++st.refused_decode;
		return false;
	}

	const base_shadow_frame pic = shadow.decode(au.data(), au.size(), int64_t(frame_index));
	if (not pic.valid)
	{
		// One in, one out is what LOW_DELAY plus thread_count 1 buys, so a
		// miss here is a real decode failure and not buffering.
		++st.refused_decode;
		return false;
	}
	++st.decoded;
	// Accepted by the decoder, so the ordering cursor moves whatever the patch
	// does next: the decoder's reference state has advanced either way, and a
	// later AU for this frame must not be fed to it again.
	last_frame = frame_index;

	// COLOUR. Sampled once, after the first picture, because the range and the
	// matrix are properties of the stream the decoder only knows once it has
	// read one. A limited-range base is a fixed offset on every patched sample
	// -- NXWARP-HYBRID.md 9 -- so the patches are refused rather than applied
	// wrong: a washed-out picture that decodes cleanly is a far worse failure
	// than no base coverage at all, because it looks like a gamma bug
	// somewhere else entirely.
	if (not colour_checked)
	{
		colour_checked = true;
		colour_bad = shadow.check_colour();
	}
	if (not colour_bad.empty())
	{
		++st.refused_colour;
		if (not warned_colour)
		{
			warned_colour = true;
			U_LOG_W("nxwarp: the base layer's colour is wrong for atlas patching "
			        "(%s) -- base-sourced tiles are disabled for this stream. "
			        "The base must be full-range BT.709; see NXWARP-HYBRID.md 9.",
			        colour_bad.c_str());
		}
		return false;
	}

	const base_picture bp{
	        .y = pic.y.data(),
	        .cb = pic.cb.data(),
	        .cr = pic.cr.data(),
	        .y_stride = size_t(pic.width),
	        .c_stride = size_t(pic.cw),
	        .width = uint32_t(pic.width),
	        .height = uint32_t(pic.height),
	};
	if (not patcher->stage(bp))
	{
		// The base picture is not the geometry the atlas was built for. A
		// configuration fault (the base encoder and the nxwarp encoder were
		// given different sizes), so it will be true of every frame -- say it
		// once and stop.
		++st.refused_geometry;
		if (not warned_geometry)
		{
			warned_geometry = true;
			const auto & L = patcher->layout();
			U_LOG_W("nxwarp: the base picture is %dx%d but the atlas wants %ux%u "
			        "-- base-sourced tiles are disabled for this stream",
			        pic.width, pic.height, L.plane_w[0], L.plane_h[0]);
		}
		return false;
	}

	// src_frame IS the frame number. See the header: this is the one value the
	// encoder cannot check for us.
	uint32_t applied = 0, superseded = 0;
	patcher->patch_all(uint32_t(frame_index), applied, superseded);
	st.applied += applied;
	st.superseded += superseded;
	++st.patched;
	return true;
}

} // namespace wivrn
