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

#include "stream_quad_blitter.h"

#include "application.h"
#include "vk/pipeline.h"
#include "vk/shader.h"
#include "vk/specialization_constants.h"

#include <algorithm>
#include <array>

namespace
{
struct quad_pc
{
	std::array<float, 4> uv_rect;
};
} // namespace

stream_quad_blitter::stream_quad_blitter(
        vk::raii::Device & device,
        const std::vector<vk::Image> & output_images,
        vk::Extent2D output_extent,
        vk::Format format,
        vk::Sampler sampler) :
        device(device),
        output_extent(output_extent),
        sampler_(sampler)
{
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

	renderpass = vk::raii::RenderPass(
	        device,
	        vk::RenderPassCreateInfo{
	                .attachmentCount = 1,
	                .pAttachments = &attachment,
	                .subpassCount = 1,
	                .pSubpasses = &subpass,
	        });

	vk::DescriptorPoolSize pool_size{
	        .type = vk::DescriptorType::eCombinedImageSampler,
	        .descriptorCount = 1,
	};

	ds_pool = device.createDescriptorPool(vk::DescriptorPoolCreateInfo{
	        .maxSets = 1,
	        .poolSizeCount = 1,
	        .pPoolSizes = &pool_size,
	});

	// Immutable: on Android this sampler carries the decoder's YCbCr conversion,
	// which the pipeline is compiled against.
	vk::DescriptorSetLayoutBinding binding{
	        .binding = 0,
	        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	        .descriptorCount = 1,
	        .stageFlags = vk::ShaderStageFlagBits::eFragment,
	        .pImmutableSamplers = &sampler_,
	};

	ds_layout = device.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo{
	        .bindingCount = 1,
	        .pBindings = &binding,
	});

	ds = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	        .descriptorPool = *ds_pool,
	        .descriptorSetCount = 1,
	        .pSetLayouts = &*ds_layout,
	})[0]
	             .release();

	vk::PushConstantRange pc_range{
	        .stageFlags = vk::ShaderStageFlagBits::eVertex,
	        .offset = 0,
	        .size = sizeof(quad_pc),
	};

	layout = vk::raii::PipelineLayout(
	        device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &*ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &pc_range,
	        });

	auto vertex_shader = load_shader(device, "quad_blit.vert");
	auto fragment_shader = load_shader(device, "quad_blit.frag");

	auto specialization = make_specialization_constants(
	        VkBool32(application::get_hmd_traits().needs_srgb_conversion));

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
	        .VertexBindingDescriptions = {},
	        .VertexAttributeDescriptions = {},
	        .InputAssemblyState = {{
	                .topology = vk::PrimitiveTopology::eTriangleList,
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
	        .layout = *layout,
	        .renderPass = *renderpass,
	        .subpass = 0,
	};

	pipeline = device.createGraphicsPipeline(application::get_pipeline_cache(), pipeline_info);

	output_image_views.reserve(output_images.size());
	framebuffers.reserve(output_images.size());
	for (vk::Image image: output_images)
	{
		output_image_views.emplace_back(
		        device,
		        vk::ImageViewCreateInfo{
		                .image = image,
		                .viewType = vk::ImageViewType::e2D,
		                .format = format,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 1,
		                },
		        });

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

void stream_quad_blitter::blit(
        vk::raii::CommandBuffer & command_buffer,
        vk::ImageView source,
        vk::ImageLayout source_layout,
        vk::Rect2D src_rect,
        vk::Extent2D src_size,
        vk::Extent2D dst_extent,
        int destination)
{
	if (destination < 0 or destination >= (int)framebuffers.size())
		throw std::runtime_error("Invalid destination image index");
	if (src_size.width == 0 or src_size.height == 0 or dst_extent.width == 0 or dst_extent.height == 0)
		return;

	dst_extent.width = std::min(dst_extent.width, output_extent.width);
	dst_extent.height = std::min(dst_extent.height, output_extent.height);

	vk::DescriptorImageInfo image_info{
	        .sampler = sampler_,
	        .imageView = source,
	        .imageLayout = source_layout,
	};

	vk::WriteDescriptorSet write{
	        .dstSet = ds,
	        .dstBinding = 0,
	        .descriptorCount = 1,
	        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	        .pImageInfo = &image_info,
	};
	device.updateDescriptorSets(write, {});

	quad_pc pc{
	        .uv_rect = {
	                float(src_rect.offset.x) / src_size.width,
	                float(src_rect.offset.y) / src_size.height,
	                float(src_rect.extent.width) / src_size.width,
	                float(src_rect.extent.height) / src_size.height,
	        },
	};

	command_buffer.setScissor(0, vk::Rect2D{.extent = dst_extent});
	command_buffer.setViewport(
	        0,
	        vk::Viewport{
	                .x = 0,
	                .y = 0,
	                .width = float(dst_extent.width),
	                .height = float(dst_extent.height),
	                .minDepth = 0,
	                .maxDepth = 1,
	        });

	command_buffer.beginRenderPass(
	        vk::RenderPassBeginInfo{
	                .renderPass = *renderpass,
	                .framebuffer = *framebuffers[destination],
	                .renderArea = {
	                        .offset = {0, 0},
	                        .extent = output_extent,
	                },
	        },
	        vk::SubpassContents::eInline);
	command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
	command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, ds, {});
	command_buffer.pushConstants<quad_pc>(*layout, vk::ShaderStageFlagBits::eVertex, 0, pc);
	command_buffer.draw(3, 1, 0, 0);
	command_buffer.endRenderPass();
}
