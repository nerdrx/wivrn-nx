/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "shaders/motion_constants.glsl.inc"

#include "motion_field.h"
#include "vk/allocation.h"
#include "wivrn_packets.h"
#include "xrt/xrt_defines.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

struct vk_bundle;

// Maps one axis of a source rectangle onto a destination of `destination` texels the
// way foveation.cpp maps it onto the encoded image, mirroring included. Returns the
// source coordinate, in texels, of the corner of destination texel 0, and the step of
// one destination texel; the step is negative on a mirrored axis.
//
// Shared by the estimator's pyramid and the server-side warper's retained image so
// that the vectors and the picture they move land in exactly the same space.
inline std::pair<float, float> motion_axis_mapping(int32_t offset, int32_t extent, bool flip, uint32_t destination)
{
	if (extent < 0)
	{
		offset += extent;
		extent = -extent;
		flip = not flip;
	}

	float step = float(extent) / float(destination);
	if (flip)
		return {float(offset + extent), -step};
	return {float(offset), step};
}

// Estimates a coarse motion field between two consecutive application frames, in
// the compositor, from the composited eye views.
//
// The estimator keeps its own downscaled luma pyramid of the last frame it was
// given rather than holding on to a full composited image: the application's
// swapchain image is recycled as soon as the frame is submitted, and a pyramid is
// three orders of magnitude smaller than the image it came from.
//
// Every call records into the caller's command buffer, using the caller's queue.
// The results are read back from host visible memory once that submission has
// completed, which the compositor already waits for.
class motion_estimator
{
public:
	static constexpr size_t view_count = 2;

	// The frame identity of a field is the caller's business; read_back() only
	// fills the geometry and the vectors.
	using field = motion_field_data;

	// eye_size is the size of one composited eye view, in pixels
	motion_estimator(vk_bundle &, vk::Extent2D eye_size);

	motion_estimator(const motion_estimator &) = delete;
	motion_estimator & operator=(const motion_estimator &) = delete;

	// Records the pyramid build for the frame described by src, and, if a previous
	// frame is available, the block matching against it. src must be readable by a
	// compute shader for the whole submission. Returns whether a field was computed.
	//
	// copy_to_host adds the copy into host visible memory that read_back() reads, and
	// is what the headset-side mode needs. The server-side mode consumes vectors()
	// on the GPU instead and asks for no copy; the result is the same either way.
	bool estimate(
	        vk::raii::Device &,
	        vk::raii::CommandBuffer & cmd,
	        std::array<vk::ImageView, view_count> src,
	        std::array<xrt_rect, view_count> src_rect,
	        bool flip_y,
	        bool copy_to_host);

	// The device local buffer the matching pass writes: one vec2 per cell, left eye
	// then right eye, index ((view * grid_height + j) * grid_width + i), each a
	// displacement in normalized coordinates of the defoveated eye image. Valid for a
	// compute read from any submission after the one that estimate() was recorded
	// into; the barrier that makes it so is recorded by estimate() itself.
	vk::Buffer vectors() const
	{
		return result;
	}

	// Quantizes the result of the last estimate() that returned true. Only valid
	// once the submission it was recorded into has completed.
	field read_back();

	// Forget the previous frame: the next estimate() will only build a pyramid.
	void reset()
	{
		have_previous = false;
	}

	uint16_t grid_width() const
	{
		return grid.width;
	}
	uint16_t grid_height() const
	{
		return grid.height;
	}

	// Device memory held by the estimator, for logging
	size_t device_memory() const;

private:
	struct pyramid
	{
		image_allocation image;
		// One view per level, written by the downsample pass
		std::vector<vk::raii::ImageView> levels;
		// All levels, read by the matching pass
		vk::raii::ImageView sampled = nullptr;
	};

	vk_bundle & vk;

	const vk::Extent2D eye_size;
	// Level 0 dimensions, a multiple of 1 << (MOTION_LEVELS - 1) so that every
	// coarser level divides exactly
	const vk::Extent2D level0;
	const vk::Extent2D grid;

	vk::raii::Sampler source_sampler;  // for the composited views
	vk::raii::Sampler pyramid_sampler; // for the pyramids

	vk::raii::DescriptorSetLayout downsample_ds_layout;
	vk::raii::PipelineLayout downsample_layout;
	vk::raii::Pipeline downsample_pipeline;

	vk::raii::DescriptorSetLayout estimate_ds_layout;
	vk::raii::PipelineLayout estimate_layout;
	vk::raii::Pipeline estimate_pipeline;

	vk::raii::DescriptorPool descriptor_pool;
	vk::DescriptorSet downsample_ds;
	vk::DescriptorSet estimate_ds;

	std::array<pyramid, 2> pyramids;
	// Index of the pyramid holding the most recent frame
	size_t current = 0;
	bool have_previous = false;
	bool initialized[2] = {false, false};

	buffer_allocation result;   // device local, written by the matching pass
	buffer_allocation readback; // host visible copy of it
	bool readback_coherent = false;

	pyramid make_pyramid(int index);
};

} // namespace wivrn
