/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "motion_field.h"
#include "vk/allocation.h"
#include "wivrn_packets.h"
#include <vulkan/vulkan_raii.hpp>
#include <openxr/openxr.h>

class stream_defoveator
{
	struct vertex;
	static const uint32_t view_count = 2;
	// Vertex buffer
	buffer_allocation buffer;
	size_t vertices_size = 0;

	vk::raii::Device & device;
	vk::raii::PhysicalDevice & physical_device;

	// Motion smoothing: the field the server measured between the last two
	// application frames, as a small two layer texture the fragment shader
	// displaces its texture coordinates with.
	//
	// Declared before the pipelines: the descriptor set layouts embed this sampler
	// through pImmutableSamplers, so it has to outlive them and members are
	// destroyed in reverse declaration order.
	vk::raii::Sampler motion_sampler = nullptr;

	// Graphic pipeline
	vk::raii::RenderPass renderpass = nullptr;
	vk::raii::DescriptorPool ds_pool = nullptr;
	struct pipeline_t
	{
		vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
		vk::DescriptorSet ds;
		vk::raii::PipelineLayout layout = nullptr;
		vk::raii::Pipeline pipeline = nullptr;
	};
	pipeline_t pipeline_rgb[view_count];
	pipeline_t pipeline_a[view_count];

	// CAS kernel the currently built pipelines were specialized for. defoveate()
	// rebuilds them if the requested kernel differs, so switching is a rare pipeline
	// reset rather than a per-pixel branch.
	bool cas_full_baked = false;
	// Whether FSR (EASU + RCAS) is compiled into the currently built pipelines, same
	// specialization-constant scheme as cas_full_baked above.
	bool fsr_baked = false;

	// Motion smoothing texture the sampler above is bound with
	image_allocation motion_image;
	std::vector<vk::raii::ImageView> motion_views;
	buffer_allocation motion_staging;
	uint32_t motion_width = 0;
	uint32_t motion_height = 0;
	// frame_idx of the field currently in the texture
	uint64_t motion_frame = uint64_t(-1);
	bool motion_ready = false;

	void ensure_motion_image(vk::raii::CommandBuffer & command_buffer, uint32_t width, uint32_t height);

	// Destination images
	std::vector<vk::Image> output_images;
	std::vector<vk::raii::ImageView> output_image_views;
	std::vector<vk::raii::Framebuffer> framebuffers;
	vk::Extent2D output_extent;
	float out_scale = 1.0f;

	void ensure_vertices(size_t num_vertices);
	vertex * get_vertices(size_t view);

	pipeline_t & ensure_pipeline(size_t view, vk::Sampler rgb, vk::Sampler a);

public:
	struct input
	{
		vk::ImageView rgb;
		vk::Sampler sampler_rgb;
		vk::Rect2D rect_rgb;
		vk::ImageLayout layout_rgb;
		vk::ImageView a;
		vk::Sampler sampler_a;
		vk::Rect2D rect_a;
		vk::ImageLayout layout_a;
		// Frame smoothing: the colour image of the decoded frame this one replaces,
		// from the same decoder (so the same sampler and the same geometry). Null
		// means there is none to blend with, and the pass then binds rgb here and
		// leaves the blend weight at zero.
		vk::ImageView prev_rgb = nullptr;
		vk::ImageLayout layout_prev_rgb = vk::ImageLayout::eGeneral;
	};

	// Post-processing folded into the defoveation pass, all values are neutral by default
	struct post_processing
	{
		// Contrast adaptive sharpening strength, 0 disables the filter
		float sharpness = 0;
		// Peripheral darkening, 0 disables it
		float vignette = 0;
		// Normalized radii, 0 at the center of the eye and 1 at the edge of the image
		float vignette_inner = 0;
		float vignette_outer = 1;
		// Ambient bias lighting: strength of the peripheral colour wash, 0 disables it,
		// and the fraction of the half image, from each edge inward, it covers
		float glow = 0;
		float glow_margin = 0;
		// Debanding: dither strength in units of one 8-bit step (1/255), 0 disables it
		float deband = 0;
	};

	// Motion smoothing. Neutral by default: with a null field, or a zero step, the
	// pass samples exactly where it would without it.
	struct motion_warp
	{
		// Field to warp along, uploaded on the first pass that uses it
		const wivrn::motion_field_data * field = nullptr;
		// How far along the field to move, in units of the interval the field
		// spans. 0 reproduces the decoded frame.
		float step = 0;
	};

	// Frame smoothing (config.frame_smoothing). Share of the previous decoded frame
	// blended into this one, on the refresh that first shows a new decoded frame and
	// on no other. 0 disables it and the pass is then byte identical to not having it.
	struct frame_blend
	{
		float weight = 0;
	};

	stream_defoveator(
	        vk::raii::Device & device,
	        vk::raii::PhysicalDevice & physical_device,
	        std::vector<vk::Image> output_images,
	        vk::Extent2D output_extent,
	        vk::Format format);

	stream_defoveator(const stream_defoveator &) = delete;

	void reset_pipelines();

	void defoveate(
	        vk::raii::CommandBuffer & command_buffer,
	        const std::array<wivrn::to_headset::foveation_parameter, 2> & foveation,
	        const std::array<input, 2> & inputs,
	        std::array<float, 4> scale,
	        std::array<float, 4> bias,
	        const post_processing & post,
	        const motion_warp & motion,
	        const frame_blend & blend,
	        int destination,
	        bool cas_full_kernel = false,
	        bool fsr = false);

	// `scale` < 1 renders the pass into fewer fragments and leaves the last upscale to
	// the runtime's compositor, which resamples the layer during timewarp regardless.
	static XrExtent2Di defoveated_size(const wivrn::to_headset::foveation_parameter &, float scale = 1.0f);

	// The scale defoveate() itself lays its viewport out with. Must match what the
	// caller sized the swapchain and the layer rect with, or the picture is cropped.
	void set_output_scale(float s)
	{
		out_scale = s;
	}
};
