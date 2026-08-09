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

#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{
struct vk_bundle;
struct encoder_settings;

// Resamples one quad layer's swapchain image into a YCbCr image of its own, the one
// the quad layer stream is encoded from. Nothing here is shared with the eye images:
// that is the whole point of promoting the layer, its resolution and its quality do
// not follow the world stream's foveation.
//
// One image and one descriptor set per compositor slot, so a frame being encoded is
// never overwritten by the next one.
class quad_converter
{
	struct slot
	{
		image_allocation image;
		vk::raii::ImageView view_y;
		vk::raii::ImageView view_cbcr;
		vk::DescriptorSet descriptor_set;
	};

	const vk::Extent2D size;
	vk::raii::Sampler sampler;
	vk::raii::DescriptorSetLayout ds_layout;
	vk::raii::PipelineLayout layout;
	vk::raii::Pipeline pipeline;
	vk::raii::DescriptorPool descriptor_pool;
	std::vector<slot> slots;

public:
	quad_converter(vk_bundle &, const encoder_settings &, size_t num_slots);

	quad_converter(const quad_converter &) = delete;
	quad_converter & operator=(const quad_converter &) = delete;

	// Records the conversion of src into the image of the given slot. src_rect is
	// the part of the source image to read, in normalized coordinates (a negative
	// height turns a bottom up source the right way up), and aspect
	// the width over height ratio the picture must come out with, which is the
	// quad's ratio in meters so that its pixels are square in the world.
	//
	// Returns the part of the encode image that was filled: the rest is black
	// padding, and the headset is told to sample only what is returned here.
	xrt_rect convert(
	        vk::raii::Device & device,
	        vk::raii::CommandBuffer & cmd,
	        size_t slot,
	        vk::ImageView src,
	        const xrt_normalized_rect & src_rect,
	        float aspect,
	        uint32_t src_width,
	        uint32_t src_height);

	vk::Image image(size_t slot) const
	{
		return slots[slot].image;
	}

	vk::Extent2D extent() const
	{
		return size;
	}
};
} // namespace wivrn
