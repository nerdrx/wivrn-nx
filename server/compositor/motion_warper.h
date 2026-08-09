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

#include "vk/allocation.h"
#include "xrt/xrt_defines.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

struct vk_bundle;

// Server side motion smoothing: keeps the last real application frame and warps it
// forward along the estimator's motion field, so that the commits the application
// produced nothing new for carry a synthesized picture rather than a repeat.
//
// Two images, both R8G8B8A8_SRGB with one array layer per eye and both the size of a
// composited eye view:
//
// - `retained`, written on an application frame by resampling the composited views
//   through the same mapping the estimator uses, so it holds the defoveated eye image
//   in the orientation the headset displays;
// - `output`, written on a duplicate commit by the warp and read by the foveation
//   pass in place of the live composited image.
//
// Both are written through UNORM views and read through sRGB ones, which is what the
// layer squasher's render target does: linear in, linear out, eight bits of sRGB in
// between — the precision the encoder is going to get anyway.
//
// Everything is recorded into the caller's command buffer. The barriers each call
// needs are recorded with it, including the ones that reach into later submissions:
// the image written on an application frame is read several commits later.
class motion_warper
{
public:
	static constexpr size_t view_count = 2;

	// eye_size is the size of one composited eye view, grid the estimator's cell
	// count per eye.
	motion_warper(vk_bundle &, vk::Extent2D eye_size, vk::Extent2D grid);

	motion_warper(const motion_warper &) = delete;
	motion_warper & operator=(const motion_warper &) = delete;

	// Records the copy of the composited views into the retained image. src must be
	// readable by a compute shader for the whole submission; src_rect and flip_y are
	// the ones the foveation pass would have been given for the same frame.
	void retain(
	        vk::raii::Device &,
	        vk::raii::CommandBuffer & cmd,
	        std::array<vk::ImageView, view_count> src,
	        std::array<xrt_rect, view_count> src_rect,
	        bool flip_y);

	// Records the warp of the retained image along `vectors`, t intervals past the
	// frame it holds. Only meaningful after a retain() in an earlier submission.
	void warp(
	        vk::raii::Device &,
	        vk::raii::CommandBuffer & cmd,
	        vk::Buffer vectors,
	        float t);

	// What the foveation pass must be given to read the warp output instead of the
	// live composited image: the views, an identity source rectangle, and flip_y
	// false — the retained image is already in display orientation.
	std::array<vk::ImageView, view_count> output_views() const
	{
		return {*output.view_srgb[0], *output.view_srgb[1]};
	}

	std::array<xrt_rect, view_count> output_rect() const
	{
		const xrt_rect r{
		        .offset = {},
		        .extent = {.w = int32_t(eye_size.width), .h = int32_t(eye_size.height)},
		};
		return {r, r};
	}

	// Device memory held by the warper, for logging
	size_t device_memory() const;

private:
	struct target
	{
		image_allocation image;
		// Both layers at once, UNORM, written by a compute pass
		vk::raii::ImageView storage = nullptr;
		// Both layers at once, sRGB, sampled by the warp pass
		vk::raii::ImageView sampled = nullptr;
		// One layer each, sRGB, sampled by the foveation pass
		std::array<vk::raii::ImageView, view_count> view_srgb{nullptr, nullptr};
	};

	vk_bundle & vk;

	const vk::Extent2D eye_size;
	const vk::Extent2D grid;

	vk::raii::Sampler sampler;

	vk::raii::DescriptorSetLayout retain_ds_layout;
	vk::raii::PipelineLayout retain_layout;
	vk::raii::Pipeline retain_pipeline;

	vk::raii::DescriptorSetLayout warp_ds_layout;
	vk::raii::PipelineLayout warp_layout;
	vk::raii::Pipeline warp_pipeline;

	vk::raii::DescriptorPool descriptor_pool;
	vk::DescriptorSet retain_ds;
	vk::DescriptorSet warp_ds;

	target retained;
	target output;

	target make_target(const char * name);
};

} // namespace wivrn
