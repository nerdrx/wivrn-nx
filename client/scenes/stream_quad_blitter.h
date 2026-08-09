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

#include <vector>
#include <vulkan/vulkan_raii.hpp>

// Copies the picture of the promoted quad layer stream into the swapchain that is
// submitted as an XrCompositionLayerQuad.
//
// It is a render pass rather than an image copy because on Android the decoder hands
// out YCbCr images that can only be read through the sampler it created, with its
// conversion attached: the same reason the defoveation pass takes its sampler from
// the decoder. The sampler is immutable in the descriptor set layout, so a new
// decoder means a new blitter.
class stream_quad_blitter
{
	vk::raii::Device & device;

	vk::raii::RenderPass renderpass = nullptr;
	vk::raii::DescriptorPool ds_pool = nullptr;
	vk::raii::DescriptorSetLayout ds_layout = nullptr;
	vk::DescriptorSet ds;
	vk::raii::PipelineLayout layout = nullptr;
	vk::raii::Pipeline pipeline = nullptr;

	std::vector<vk::raii::ImageView> output_image_views;
	std::vector<vk::raii::Framebuffer> framebuffers;
	vk::Extent2D output_extent;

	const vk::Sampler sampler_;

public:
	stream_quad_blitter(
	        vk::raii::Device & device,
	        const std::vector<vk::Image> & output_images,
	        vk::Extent2D output_extent,
	        vk::Format format,
	        vk::Sampler sampler);

	stream_quad_blitter(const stream_quad_blitter &) = delete;
	stream_quad_blitter & operator=(const stream_quad_blitter &) = delete;

	// The sampler this was built for, to notice when the decoder was replaced
	vk::Sampler sampler() const
	{
		return sampler_;
	}

	vk::Extent2D extent() const
	{
		return output_extent;
	}

	// src_rect is the part of the decoded image that holds picture, in pixels, and
	// src_size the size of the whole decoded image. dst_extent is the part of the
	// swapchain image to fill, which is also what is submitted as the layer's
	// imageRect: the swapchain is allocated once at the size the server encodes at
	// and a panel that does not fill it uses a corner of it.
	void blit(
	        vk::raii::CommandBuffer & command_buffer,
	        vk::ImageView source,
	        vk::ImageLayout source_layout,
	        vk::Rect2D src_rect,
	        vk::Extent2D src_size,
	        vk::Extent2D dst_extent,
	        int destination);
};
