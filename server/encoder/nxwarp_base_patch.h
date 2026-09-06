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

// nxwarp_base_patch -- staging a decoded base picture into the shape
// nxvc_vk_encoder_atlas_write_tiles() demands, and issuing the runs.
//
// WHAT THIS IS FOR.  ADR-0029 cheat 7 option B: the encoder decodes its own
// HEVC base layer so that its model of the atlas holds the same base-sourced
// patches the headset's does.  Without it the mode decision has no idea which
// tiles the base already covers, so it codes them again -- correct picture,
// wasted bitrate, and none of the saving the hybrid gate measured.
//
// THE TWO REPRESENTATION GAPS, which is all this file really is.  The encoder
// API takes DEVICE memory "already in the ATLAS's own plane layout"; FFmpeg
// hands over three 8-bit HOST planes at the coded geometry.  So:
//
//   1. u8 -> u16.  A ring slot holds u16 samples; the base picture is 8-bit.
//      The atlas's coded sample domain under CT_NONE is BT.709 FULL-RANGE
//      YCbCr, which is why encoder_settings.cpp pins `color_range = pc` on the
//      base encoder: the widening is then a plain zero-extend and not a range
//      conversion.  A limited-range base makes every patch wrong by a fixed
//      offset -- a failure that reads as a gamma bug.
//   2. the coded picture is the eye PAIR side by side; the atlas is planar
//      with each eye's sub-picture at column `eye * plane_w` of a padded row.
//      Same pixels, different address arithmetic, and the API's contract that
//      "a tile's source address is its destination address plus offset" is
//      only true once this has been done.
//
// A REPORTED GAP.  nxvc publishes no query for the ring-slot layout, so
// nxwarp_codec_vk::atlas_layout() reproduces nxvw_ring_layout() from nx-warp's
// private vk/decoder/inter/inter_layout.h -- the same duplication
// hybrid-proto/src/base_patch_layout.h carries, with the same rule: if the two
// disagree, nx-warp's header is right and this is the bug.  It is duplicated
// rather than included so the server does not take a build dependency on the
// decoder's private headers.  A public accessor on the encoder would delete
// this whole hazard and is worth asking for.
//
// WHAT IS NOT HERE.  Nothing routes an encoded base access unit from stream 1
// to stream 0 yet, and there is no HEVC decode on the server at all: the
// shadow decoder lives in hybrid-proto/src/base_shadow.h and has never been
// built into the server.  So this stages and patches, and the thing that will
// eventually call it every frame does not exist.  `stage()` takes the decoded
// planes directly for exactly that reason.

#include "nxwarp_codec.h"

#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

namespace wivrn
{

// A decoded base picture as FFmpeg hands it over: three 8-bit planes at the
// CODED geometry, which for a paired stream is the eye pair side by side with
// eye 0 first.  Strides are in samples.
struct base_picture
{
	const uint8_t * y = nullptr;
	const uint8_t * cb = nullptr;
	const uint8_t * cr = nullptr;
	size_t y_stride = 0;
	size_t c_stride = 0;
	uint32_t width = 0;  // the WHOLE coded picture, both eyes
	uint32_t height = 0;
};

// Converts a decoded base picture into one slot-shaped u16 staging buffer on
// the codec's device, and fills tile runs from it.
//
// One instance per stream; the device buffer is allocated once and rewritten
// per frame, because a base picture arrives at frame rate and allocating there
// would be the expensive part of a 1.9 us-a-tile operation.
class base_patcher
{
public:
	// Throws std::runtime_error if the codec has no atlas, or if no host-visible
	// memory type fits.
	base_patcher(nxwarp_codec & codec,
	             VkPhysicalDevice phys,
	             VkDevice dev,
	             uint32_t eyes);
	~base_patcher();

	base_patcher(const base_patcher &) = delete;
	base_patcher & operator=(const base_patcher &) = delete;

	// Rewrite the staging buffer from one decoded base picture.  Returns false
	// if the picture is not the pair geometry the atlas was built for -- which
	// is a configuration mistake, not a runtime condition.
	bool stage(const base_picture & pic);

	// Patch every tile of both eyes from what stage() last wrote, one run per
	// tile row: a full-frame base refresh, which is the policy the hybrid gate
	// recommends since it passes 100 % of tiles.
	//
	// `applied` and `superseded` accumulate across the runs and always sum to
	// the tiles attempted.  A superseded tile is NOT a failure: it means a
	// coded tile already carried that position at a src_frame this patch does
	// not advance, and 13.12.9 says the write is dropped.  Returns false only
	// if a run was actually refused.
	bool patch_all(uint32_t src_frame, uint32_t & applied, uint32_t & superseded);

	// One contiguous run, for a partial or foveated refresh.
	bool patch_run(uint32_t eye,
	               uint32_t first_tile,
	               uint32_t count,
	               uint32_t src_frame,
	               uint32_t & applied,
	               uint32_t & superseded);

	const nxwarp_codec::atlas_slot_layout & layout() const
	{
		return L;
	}

	// The staging buffer, for a caller that wants to check what was uploaded.
	VkBuffer buffer() const
	{
		return buf;
	}
	std::span<const uint16_t> host_slot() const
	{
		return {mapped, L.slot_u16};
	}

private:
	nxwarp_codec & codec;
	VkDevice dev = VK_NULL_HANDLE;
	uint32_t eyes = 1;
	nxwarp_codec::atlas_slot_layout L{};
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	uint16_t * mapped = nullptr;
};

} // namespace wivrn
