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

#include <cmath>
#include "stream_defoveator.h"
#include "application.h"
#include "utils/ranges.h"
#include "vk/allocation.h"
#include "vk/pipeline.h"
#include "vk/shader.h"
#include "vk/specialization_constants.h"
#include <array>
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>

struct stream_defoveator::vertex
{
	// output image position
	alignas(8) glm::vec2 position;
	// input texture coordinates
	alignas(8) glm::uvec2 uv;
};

struct vert_pc
{
	glm::ivec4 rgb_rect;
	glm::ivec4 a_rect;
	std::array<float, 4> scale;
	std::array<float, 4> bias;
	std::array<float, 4> post;
	std::array<float, 4> motion;
	std::array<float, 4> glow;
	std::array<float, 4> deband;
};

// The motion field is stored as one signed byte per axis, scaled by the longest
// vector in the field. R8G8Snorm is one of the formats every implementation must
// support as a linearly filtered sampled image.
static const vk::Format motion_format = vk::Format::eR8G8Snorm;

void stream_defoveator::ensure_vertices(size_t num_vertices)
{
	vk::BufferCreateInfo create_info{
	        .size = num_vertices * sizeof(vertex) * view_count,
	        .usage = vk::BufferUsageFlagBits::eVertexBuffer,
	        .sharingMode = vk::SharingMode::eExclusive,
	};

	if (create_info.size <= buffer.info().size)
		return;

	VmaAllocationCreateInfo alloc_info{
	        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	};

	buffer = buffer_allocation(device, create_info, alloc_info);
	vertices_size = num_vertices * sizeof(vertex);
}

stream_defoveator::vertex * stream_defoveator::get_vertices(size_t view)
{
	assert(buffer);
	return reinterpret_cast<vertex *>(reinterpret_cast<uintptr_t>(buffer.map()) + view * vertices_size);
}

stream_defoveator::pipeline_t & stream_defoveator::ensure_pipeline(size_t view, vk::Sampler rgb, vk::Sampler a)
{
	auto & target = a ? pipeline_a[view] : pipeline_rgb[view];
	if (*target.pipeline)
		return target;

	std::array samplers{rgb, a};
	int32_t alpha = a ? 1 : 0;

	// Create VkDescriptorSetLayout
	std::array layout_binding{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = uint32_t(alpha + 1),
	                .stageFlags = vk::ShaderStageFlagBits::eFragment,
	                .pImmutableSamplers = samplers.data(),
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eFragment,
	                .pImmutableSamplers = &*motion_sampler,
	        },
	        // Frame smoothing: the previous decoded colour frame. Same immutable sampler
	        // as rgb[0] (it comes from the same decoder, so it needs the same, possibly
	        // YCbCr, conversion). It is always bound, to rgb[0] itself when there is no
	        // previous frame to blend with.
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eFragment,
	                .pImmutableSamplers = samplers.data(),
	        },
	};

	target.descriptor_set_layout = device.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo{
	        .bindingCount = layout_binding.size(),
	        .pBindings = layout_binding.data(),
	});

	target.ds = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	        .descriptorPool = *ds_pool,
	        .descriptorSetCount = 1,
	        .pSetLayouts = &*target.descriptor_set_layout,
	})[0]
	                    .release();

	// pipeline layout
	vk::PushConstantRange pc_range{
	        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
	        .size = sizeof(vert_pc),
	};

	vk::PipelineLayoutCreateInfo pipeline_layout_info{
	        .setLayoutCount = 1,
	        .pSetLayouts = &*target.descriptor_set_layout,
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &pc_range,
	};

	target.layout = vk::raii::PipelineLayout(device, pipeline_layout_info);

	const auto & vk_device_extensions = application::get_vk_device_extensions();

	// Vertex shader
	auto vertex_shader = load_shader(device, "reprojection.vert");

	// Fragment shader
	auto specialization = make_specialization_constants(
	        int32_t(alpha),
	        VkBool32(application::get_hmd_traits().needs_srgb_conversion),
	        VkBool32(cas_full_baked),
	        VkBool32(fsr_baked));
	auto fragment_shader = load_shader(device, "reprojection.frag");

	vk::pipeline_builder pipeline_info{
	        .flags = {},
	        .Stages = {
	                {
	                        .stage = vk::ShaderStageFlagBits::eVertex,
	                        .module = *vertex_shader,
	                        .pName = "main",
	                },
	                {
	                        .stage = vk::ShaderStageFlagBits::eFragment,
	                        .module = *fragment_shader,
	                        .pName = "main",
	                        .pSpecializationInfo = specialization,
	                },
	        },
	        .VertexBindingDescriptions = {
	                {
	                        .binding = 0,
	                        .stride = sizeof(vertex),
	                        .inputRate = vk::VertexInputRate::eVertex,
	                },
	        },
	        .VertexAttributeDescriptions = {
	                {
	                        .location = 0,
	                        .binding = 0,
	                        .format = vk::Format::eR32G32Sfloat,
	                        .offset = offsetof(vertex, position),
	                },
	                {
	                        .location = 1,
	                        .binding = 0,
	                        .format = vk::Format::eR32G32Uint,
	                        .offset = offsetof(vertex, uv),
	                },
	        },
	        .InputAssemblyState = {{
	                .topology = vk::PrimitiveTopology::eTriangleStrip,
	        }},
	        .Viewports = {{}},
	        .Scissors = {{}},
	        .RasterizationState = {{
	                .polygonMode = vk::PolygonMode::eFill,
	                .lineWidth = 1,
	        }},
	        .MultisampleState = {{
	                .rasterizationSamples = vk::SampleCountFlagBits::e1,
	        }},
	        .ColorBlendState = {.flags = {}},
	        .ColorBlendAttachments = {{
	                .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
	        }},
	        .DynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor},
	        .layout = *target.layout,
	        .renderPass = *renderpass,
	        .subpass = 0,
	};

	target.pipeline = device.createGraphicsPipeline(application::get_pipeline_cache(), pipeline_info);
	return target;
}

stream_defoveator::stream_defoveator(
        vk::raii::Device & device,
        vk::raii::PhysicalDevice & physical_device,
        std::vector<vk::Image> output_images_,
        vk::Extent2D output_extent,
        vk::Format format) :
        device(device),
        physical_device(physical_device),
        output_images(std::move(output_images_)),
        output_extent(output_extent)
{
	// Create renderpass
	vk::AttachmentDescription attachment{
	        .format = format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eDontCare,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
	};

	vk::AttachmentReference color_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::SubpassDescription subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &color_ref,
	};

	vk::StructureChain renderpass_info{
	        vk::RenderPassCreateInfo{
	                .attachmentCount = 1,
	                .pAttachments = &attachment,
	                .subpassCount = 1,
	                .pSubpasses = &subpass,
	        },
	};

	renderpass = vk::raii::RenderPass(device, renderpass_info.get());

	vk::DescriptorPoolSize pool_size{
	        .type = vk::DescriptorType::eCombinedImageSampler,
	        // rgb, alpha, the motion field and the previous frame, for both variants
	        .descriptorCount = view_count * 8,
	};

	ds_pool = device.createDescriptorPool(vk::DescriptorPoolCreateInfo{
	        .maxSets = view_count * 2,
	        .poolSizeCount = 1,
	        .pPoolSizes = &pool_size,
	});

	motion_sampler = device.createSampler(vk::SamplerCreateInfo{
	        .magFilter = vk::Filter::eLinear,
	        .minFilter = vk::Filter::eLinear,
	        .mipmapMode = vk::SamplerMipmapMode::eNearest,
	        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	});

	// Create image views and framebuffers
	output_image_views.reserve(output_images.size() * view_count);
	framebuffers.reserve(output_images.size() * view_count);
	for (vk::Image image: output_images)
	{
		for (uint32_t view = 0; view < view_count; ++view)
		{
			vk::ImageViewCreateInfo iv_info{
			        .image = image,
			        .viewType = vk::ImageViewType::e2DArray,
			        .format = format,
			        .components = {
			                .r = vk::ComponentSwizzle::eIdentity,
			                .g = vk::ComponentSwizzle::eIdentity,
			                .b = vk::ComponentSwizzle::eIdentity,
			                .a = vk::ComponentSwizzle::eIdentity,
			        },
			        .subresourceRange = {
			                .aspectMask = vk::ImageAspectFlagBits::eColor,
			                .baseMipLevel = 0,
			                .levelCount = 1,
			                .baseArrayLayer = view,
			                .layerCount = 1,
			        },
			};

			output_image_views.emplace_back(device, iv_info);

			vk::FramebufferCreateInfo fb_create_info{
			        .renderPass = *renderpass,
			        .width = output_extent.width,
			        .height = output_extent.height,
			        .layers = 1,
			};
			fb_create_info.setAttachments(*output_image_views.back());

			framebuffers.emplace_back(device, fb_create_info);
		}
	}
}

void stream_defoveator::reset_pipelines()
{
	for (auto & p: pipeline_rgb)
		p = {};
	for (auto & p: pipeline_a)
		p = {};
	// Reclaim the descriptor sets; the pool is sized for one generation and has
	// no free-flag, so without this a rebuild (resolution/codec change, or the
	// CAS full-kernel toggle) exhausts it and allocateDescriptorSets throws.
	ds_pool.reset();
}

// Makes sure the motion texture exists at the requested size and is readable by the
// fragment shader. The texture is bound by every pass, whether motion smoothing is
// in use or not, so it must be valid even when there is no field: a 1x1 texture of
// zeroes then, which the shader never reads because the step is zero.
void stream_defoveator::ensure_motion_image(vk::raii::CommandBuffer & command_buffer, uint32_t width, uint32_t height)
{
	if (motion_image and motion_width == width and motion_height == height)
		return;

	// Nothing can still be reading it: the caller waits on the frame fence before
	// recording anything.
	motion_views.clear();
	motion_image = image_allocation(
	        device,
	        vk::ImageCreateInfo{
	                .imageType = vk::ImageType::e2D,
	                .format = motion_format,
	                .extent = {.width = width, .height = height, .depth = 1},
	                .mipLevels = 1,
	                .arrayLayers = view_count,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
	        },
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO});

	motion_views.reserve(view_count);
	for (uint32_t view = 0; view < view_count; ++view)
	{
		motion_views.emplace_back(
		        device,
		        vk::ImageViewCreateInfo{
		                .image = motion_image,
		                .viewType = vk::ImageViewType::e2D,
		                .format = motion_format,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .baseArrayLayer = view,
		                        .layerCount = 1,
		                },
		        });
	}

	motion_staging = buffer_allocation(
	        device,
	        vk::BufferCreateInfo{
	                .size = vk::DeviceSize(width) * height * view_count * 2,
	                .usage = vk::BufferUsageFlagBits::eTransferSrc,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
	        });

	motion_width = width;
	motion_height = height;
	motion_frame = uint64_t(-1);
	motion_ready = false;
}

static size_t required_vertices(const wivrn::to_headset::foveation_parameter & p)
{
	// strips are constructed like this:
	// 0 2 4
	// 1 3 5 5*
	// there is one such line per value in y
	// the last element is repeated to break the line
	return (2 * (p.x.size() + 1) + 1) * p.y.size();
}

void stream_defoveator::defoveate(vk::raii::CommandBuffer & command_buffer,
                                  const std::array<wivrn::to_headset::foveation_parameter, 2> & foveation,
                                  const std::array<input, 2> & inputs,
                                  std::array<float, 4> scale,
                                  std::array<float, 4> bias,
                                  const post_processing & post,
                                  const motion_warp & motion,
                                  const frame_blend & blend,
                                  int destination,
                                  bool cas_full_kernel,
                                  bool fsr)
{
	if (destination < 0 || destination >= (int)output_images.size())
		throw std::runtime_error("Invalid destination image index");

	// The CAS kernel and the FSR path are specialization constants, so a change means
	// rebuilding the pipelines. The caller has waited on the frame fence before recording,
	// so nothing is still reading the old pipelines. They only ever flip from a settings
	// toggle, a rare event.
	if (cas_full_kernel != cas_full_baked or fsr != fsr_baked)
	{
		reset_pipelines();
		cas_full_baked = cas_full_kernel;
		fsr_baked = fsr;
	}

	ensure_vertices(std::max(required_vertices(foveation[0]), required_vertices(foveation[1])));

	// Motion field: keep whatever is already in the texture unless a new one came in
	const wivrn::motion_field_data * field = motion.field;
	if (field and (field->width == 0 or field->height == 0 or
	               field->vectors.size() != size_t(field->width) * field->height * view_count * 2))
		field = nullptr;

	float motion_step = field ? motion.step : 0.f;
	float motion_scale = field ? field->scale : 0.f;

	// Without a field the texture is left as it is rather than dropped: the shader
	// does not read it when the step is zero, and a field usually comes back within
	// a frame or two. Reallocating on every gap would be pointless churn.
	ensure_motion_image(
	        command_buffer,
	        field ? field->width : std::max(motion_width, 1u),
	        field ? field->height : std::max(motion_height, 1u));

	if (not motion_ready or (field and field->frame_idx != motion_frame))
	{
		const size_t size = size_t(motion_width) * motion_height * view_count * 2;
		if (field and size_t(field->width) * field->height * view_count * 2 == size)
			vmaCopyMemoryToAllocation(vk_allocator::instance(), field->vectors.data(), motion_staging, 0, size);
		else
		{
			std::vector<int8_t> zeroes(size, 0);
			vmaCopyMemoryToAllocation(vk_allocator::instance(), zeroes.data(), motion_staging, 0, size);
		}

		vk::ImageMemoryBarrier to_transfer{
		        .srcAccessMask = vk::AccessFlagBits::eNone,
		        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
		        .oldLayout = vk::ImageLayout::eUndefined,
		        .newLayout = vk::ImageLayout::eTransferDstOptimal,
		        .image = motion_image,
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .layerCount = view_count,
		        },
		};
		command_buffer.pipelineBarrier(
		        vk::PipelineStageFlagBits::eTopOfPipe,
		        vk::PipelineStageFlagBits::eTransfer,
		        {},
		        {},
		        {},
		        to_transfer);

		// The two layers are consecutive in the packet, which is exactly how a
		// multi layer copy expects them
		command_buffer.copyBufferToImage(
		        motion_staging,
		        motion_image,
		        vk::ImageLayout::eTransferDstOptimal,
		        vk::BufferImageCopy{
		                .imageSubresource = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .layerCount = view_count,
		                },
		                .imageExtent = {.width = motion_width, .height = motion_height, .depth = 1},
		        });

		vk::ImageMemoryBarrier to_read{
		        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
		        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
		        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
		        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        .image = motion_image,
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .layerCount = view_count,
		        },
		};
		command_buffer.pipelineBarrier(
		        vk::PipelineStageFlagBits::eTransfer,
		        vk::PipelineStageFlagBits::eFragmentShader,
		        {},
		        {},
		        {},
		        to_read);

		motion_ready = true;
		motion_frame = field ? field->frame_idx : uint64_t(-1);
	}

	for (size_t view = 0; view < view_count; ++view)
	{
		const auto out_size = defoveated_size(foveation[view], out_scale);
		auto vertices = get_vertices(view);
		const auto & [px, py] = foveation[view];
		assert(px.size() % 2 == 1);
		assert(py.size() % 2 == 1);
		const int n_ratio_y = (py.size() - 1) / 2;
		const int n_ratio_x = (px.size() - 1) / 2;

		command_buffer.setScissor(
		        0,
		        vk::Rect2D{
		                .extent = {.width = uint32_t(out_size.width), .height = uint32_t(out_size.height)},
		        });
		command_buffer.setViewport(
		        0,
		        vk::Viewport{
		                .x = 0,
		                .y = 0,
		                .width = float(out_size.width),
		                .height = float(out_size.height),
		                .minDepth = 0,
		                .maxDepth = 1,
		        });

		glm::uvec2 in(0);
		glm::vec2 out(-0.5 * out_size.width, -0.5 * out_size.height); // pixel coordinates
		glm::vec2 out_pixel_size(2. / out_size.width,
		                         2. / out_size.height);
		for (auto [iy, n_out_y]: utils::enumerate_range(py))
		{
			// number of output pixels per source pixels
			const int ratio_y = std::abs(n_ratio_y - int(iy)) + 1;
			in.x = 0;
			out.x = -0.5 * out_size.width;
			for (auto [ix, n_out_x]: utils::enumerate_range(px))
			{
				const int ratio_x = std::abs(n_ratio_x - int(ix)) + 1;
				*vertices++ = {
				        .position = out * out_pixel_size,
				        .uv = in,
				};
				*vertices++ = {
				        .position = (out + glm::vec2(0, n_out_y * ratio_y)) * out_pixel_size,
				        .uv = (in + glm::uvec2(0, n_out_y)),
				};
				in.x += n_out_x;
				out.x += n_out_x * ratio_x;
			}
			*vertices++ = {
			        .position = out * out_pixel_size,
			        .uv = in,
			};
			in.y += n_out_y;
			out.y += n_out_y * ratio_y;
			*vertices++ = {
			        .position = out * out_pixel_size,
			        .uv = in,
			};
			*vertices++ = {
			        .position = out * out_pixel_size,
			        .uv = in,
			};
		}
	}

	for (size_t view = 0; view < view_count; ++view)
	{
		vk::RenderPassBeginInfo begin_info{
		        .renderPass = *renderpass,
		        .framebuffer = *framebuffers[destination * view_count + view],
		        .renderArea = {
		                .offset = {0, 0},
		                .extent = output_extent,
		        },
		};

		const auto & input = inputs[view];
		auto & pipeline = ensure_pipeline(view, input.sampler_rgb, input.sampler_a);

		std::array image_info{
		        vk::DescriptorImageInfo{
		                .sampler = input.sampler_rgb,
		                .imageView = input.rgb,
		                .imageLayout = input.layout_rgb,
		        },
		        vk::DescriptorImageInfo{
		                .sampler = input.sampler_a,
		                .imageView = input.a,
		                .imageLayout = input.layout_a,
		        },
		};

		vk::DescriptorImageInfo motion_info{
		        .sampler = *motion_sampler,
		        .imageView = *motion_views[view],
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};

		// Frame smoothing. Without a previous frame the binding still has to be a valid
		// image, so it points at this frame's own colour image and the weight is forced
		// to zero: the shader then never reads it and the result is unchanged.
		const bool blend_on = blend.weight > 0 and input.prev_rgb;
		vk::DescriptorImageInfo prev_info{
		        .sampler = input.sampler_rgb,
		        .imageView = blend_on ? input.prev_rgb : input.rgb,
		        .imageLayout = blend_on ? input.layout_prev_rgb : input.layout_rgb,
		};

		std::array descriptor_writes{
		        vk::WriteDescriptorSet{
		                .dstSet = pipeline.ds,
		                .dstBinding = 0,
		                .descriptorCount = input.sampler_a ? 2u : 1u,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = image_info.data(),
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = pipeline.ds,
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &motion_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = pipeline.ds,
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &prev_info,
		        },
		};

		vert_pc pc{
		        .rgb_rect = glm::ivec4(input.rect_rgb.offset.x,
		                               input.rect_rgb.offset.y,
		                               input.rect_rgb.extent.width,
		                               input.rect_rgb.extent.height),
		        .a_rect = glm::ivec4(input.rect_a.offset.x,
		                             input.rect_a.offset.y,
		                             input.rect_a.extent.width,
		                             input.rect_a.extent.height),
		        .scale = scale,
		        .bias = bias,
		        .post = {post.sharpness, post.vignette, post.vignette_inner, post.vignette_outer},
		        .motion = {motion_step, motion_scale, blend_on ? blend.weight : 0.f, 0},
		        .glow = {post.glow, post.glow_margin, 0, 0},
		        .deband = {post.deband, 0, 0, 0},
		};

		device.updateDescriptorSets(descriptor_writes, {});

		command_buffer.beginRenderPass(begin_info, vk::SubpassContents::eInline);
		command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline.pipeline);
		command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline.layout, 0, pipeline.ds, {});
		command_buffer.pushConstants<vert_pc>(*pipeline.layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, pc);
		command_buffer.bindVertexBuffers(0, vk::Buffer(buffer), vertices_size * view);
		command_buffer.draw(required_vertices(foveation[view]), 1, 0, 0);
		command_buffer.endRenderPass();
	}
}

static uint16_t count_pixels(const std::vector<uint16_t> & param)
{
	uint16_t res = 0;
	const int n_ratio = (param.size() - 1) / 2;
	for (auto [i, n_out]: utils::enumerate_range(param))
	{
		// number of output pixels per source pixels
		const int ratio = std::abs(n_ratio - int(i)) + 1;
		res += ratio * n_out;
	}
	return res;
}

XrExtent2Di stream_defoveator::defoveated_size(const wivrn::to_headset::foveation_parameter & view, float scale)
{
	// `scale` shrinks the pass's OUTPUT only. The geometry is untouched: the vertex
	// positions below are normalised by the same out_size they are laid out in
	// (out_pixel_size = 2 / out_size), so the drawn picture is identical and only the
	// number of fragments changes -- which is the whole cost of this pass.
	//
	// Measured on a Pico 4: the pass renders 2160x2160 PER EYE, invariant across
	// stream_scale, because the defoveated size is what the panel wants and not what
	// the stream carries. At stream_scale 0.7 that is a 768x768 source blown up to
	// 2160x2160, 9.3 Mpixel a frame for 8.4 ms, 30-49% of the frame budget. The
	// runtime's compositor resamples this image anyway when it timewarps it, so
	// rendering fewer fragments and letting it do the last step is not a loss of a
	// resampling, it is the removal of a duplicated one.
	const auto apply = [scale](uint16_t v) -> int32_t {
		if (scale >= 1.0f)
			return v;
		// Even, and never zero: the foveation grid is laid out in whole pixels and a
		// zero-sized viewport is not a legal draw.
		const int32_t r = int32_t(std::lround(double(v) * double(scale)));
		return std::max<int32_t>(2, r & ~1);
	};
	return {
	        apply(count_pixels(view.x)),
	        apply(count_pixels(view.y)),
	};
}
