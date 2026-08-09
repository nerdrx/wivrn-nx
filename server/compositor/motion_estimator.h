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

#include "vk/allocation.h"
#include "wivrn_packets.h"
#include "xrt/xrt_defines.h"

#include <array>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

struct vk_bundle;

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

	struct field
	{
		uint16_t width = 0;
		uint16_t height = 0;
		// Longest displacement in the field, as a fraction of the eye image
		float scale = 0;
		// Two int8 components per cell, left eye then right eye
		std::vector<int8_t> vectors;
	};

	// eye_size is the size of one composited eye view, in pixels
	motion_estimator(vk_bundle &, vk::Extent2D eye_size);

	motion_estimator(const motion_estimator &) = delete;
	motion_estimator & operator=(const motion_estimator &) = delete;

	// Records the pyramid build for the frame described by src, and, if a previous
	// frame is available, the block matching against it. src must be readable by a
	// compute shader for the whole submission. Returns whether a field will be
	// available from read_back() once the submission completes.
	bool estimate(
	        vk::raii::Device &,
	        vk::raii::CommandBuffer & cmd,
	        std::array<vk::ImageView, view_count> src,
	        std::array<xrt_rect, view_count> src_rect,
	        bool flip_y);

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
