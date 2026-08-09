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

#include "motion_warper.h"

#include "motion_estimator.h"
#include "shaders/motion_constants.glsl.inc"
#include "utils/wivrn_vk_bundle.h"
#include "vk/allocation.h"

#include <algorithm>
#include <format>

namespace
{

struct retain_push_constants
{
	// Per view: base.xy then step.xy
	float mapping[2][4];
	int32_t size[2];
};

struct warp_push_constants
{
	int32_t size[2];
	int32_t grid[2];
	float t;
};

// The same two aliases the layer squasher's render target uses: written through the
// UNORM view, read through the sRGB one.
const std::array formats{
        vk::Format::eR8G8B8A8Srgb,
        vk::Format::eR8G8B8A8Unorm,
};

uint32_t divide_and_round_up(uint32_t a, uint32_t b)
{
	return (a + b - 1) / b;
}

} // namespace

namespace wivrn
{

motion_warper::target motion_warper::make_target(const char * name)
{
	target res;

	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat,
	                .imageType = vk::ImageType::e2D,
	                .format = formats[0],
	                .extent = {.width = eye_size.width, .height = eye_size.height, .depth = 1},
	                .mipLevels = 1,
	                .arrayLayers = view_count,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = formats.size(),
	                .pViewFormats = formats.data(),
	        },
	};

	res.image = image_allocation{
	        vk.device,
	        image_info.get(),
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
	        std::format("motion {} image", name),
	};

	vk::Image image{res.image};

	// An sRGB image cannot be a storage image, so the writing view has to be the
	// UNORM alias and the shader does the encoding itself. eExtendedUsage plus a
	// per-view usage is how the squasher gets both out of one image.
	vk::ImageViewUsageCreateInfo storage_usage{.usage = vk::ImageUsageFlagBits::eStorage};
	vk::ImageViewUsageCreateInfo sampled_usage{.usage = vk::ImageUsageFlagBits::eSampled};

	res.storage = vk::raii::ImageView{
	        vk.device,
	        vk::ImageViewCreateInfo{
	                .pNext = &storage_usage,
	                .image = image,
	                .viewType = vk::ImageViewType::e2DArray,
	                .format = formats[1],
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = view_count,
	                },
	        }};

	res.sampled = vk::raii::ImageView{
	        vk.device,
	        vk::ImageViewCreateInfo{
	                .pNext = &sampled_usage,
	                .image = image,
	                .viewType = vk::ImageViewType::e2DArray,
	                .format = formats[0],
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = view_count,
	                },
	        }};

	for (uint32_t view = 0; view < view_count; ++view)
		res.view_srgb[view] = vk::raii::ImageView{
		        vk.device,
		        vk::ImageViewCreateInfo{
		                .pNext = &sampled_usage,
		                .image = image,
		                .viewType = vk::ImageViewType::e2D,
		                .format = formats[0],
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .baseArrayLayer = view,
		                        .layerCount = 1,
		                },
		        }};

	return res;
}

motion_warper::motion_warper(vk_bundle & bundle, vk::Extent2D eye, vk::Extent2D cells) :
        vk(bundle),
        eye_size(eye),
        grid(cells),
        sampler(nullptr),
        retain_ds_layout(nullptr),
        retain_layout(nullptr),
        retain_pipeline(nullptr),
        warp_ds_layout(nullptr),
        warp_layout(nullptr),
        warp_pipeline(nullptr),
        descriptor_pool(nullptr)
{
	// One sampler for both passes: the retained image is resampled with a linear
	// filter and so is the composited view it comes from, and both clamp to edge —
	// which is what fills the disocclusions the warp opens up.
	sampler = vk::raii::Sampler{
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eLinear,
	                .minFilter = vk::Filter::eLinear,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	        }};
	vk.name(*sampler, "motion warp sampler");

	{
		std::array bindings{
		        vk::DescriptorSetLayoutBinding{
		                .binding = 0,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = view_count,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		};
		retain_ds_layout = vk::raii::DescriptorSetLayout{
		        vk.device,
		        vk::DescriptorSetLayoutCreateInfo{
		                .bindingCount = bindings.size(),
		                .pBindings = bindings.data(),
		        }};
		vk.name(*retain_ds_layout, "motion retain descriptor set layout");

		vk::PushConstantRange range{
		        .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        .size = sizeof(retain_push_constants),
		};
		retain_layout = vk::raii::PipelineLayout{
		        vk.device,
		        vk::PipelineLayoutCreateInfo{
		                .setLayoutCount = 1,
		                .pSetLayouts = &*retain_ds_layout,
		                .pushConstantRangeCount = 1,
		                .pPushConstantRanges = &range,
		        }};

		auto shader = vk.load_shader("motion_retain");
		retain_pipeline = vk::raii::Pipeline{
		        vk.device,
		        nullptr,
		        vk::ComputePipelineCreateInfo{
		                .stage = {
		                        .stage = vk::ShaderStageFlagBits::eCompute,
		                        .module = *shader,
		                        .pName = "main",
		                },
		                .layout = *retain_layout,
		        }};
		vk.name(*retain_pipeline, "motion retain pipeline");
	}

	{
		std::array bindings{
		        vk::DescriptorSetLayoutBinding{
		                .binding = 0,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 1,
		                .descriptorType = vk::DescriptorType::eStorageBuffer,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 2,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		};
		warp_ds_layout = vk::raii::DescriptorSetLayout{
		        vk.device,
		        vk::DescriptorSetLayoutCreateInfo{
		                .bindingCount = bindings.size(),
		                .pBindings = bindings.data(),
		        }};
		vk.name(*warp_ds_layout, "motion warp descriptor set layout");

		vk::PushConstantRange range{
		        .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        .size = sizeof(warp_push_constants),
		};
		warp_layout = vk::raii::PipelineLayout{
		        vk.device,
		        vk::PipelineLayoutCreateInfo{
		                .setLayoutCount = 1,
		                .pSetLayouts = &*warp_ds_layout,
		                .pushConstantRangeCount = 1,
		                .pPushConstantRanges = &range,
		        }};

		auto shader = vk.load_shader("motion_warp");
		warp_pipeline = vk::raii::Pipeline{
		        vk.device,
		        nullptr,
		        vk::ComputePipelineCreateInfo{
		                .stage = {
		                        .stage = vk::ShaderStageFlagBits::eCompute,
		                        .module = *shader,
		                        .pName = "main",
		                },
		                .layout = *warp_layout,
		        }};
		vk.name(*warp_pipeline, "motion warp pipeline");
	}

	{
		std::array pool_sizes{
		        vk::DescriptorPoolSize{
		                .type = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = uint32_t(view_count) + 1,
		        },
		        vk::DescriptorPoolSize{
		                .type = vk::DescriptorType::eStorageImage,
		                .descriptorCount = 2,
		        },
		        vk::DescriptorPoolSize{
		                .type = vk::DescriptorType::eStorageBuffer,
		                .descriptorCount = 1,
		        },
		};
		descriptor_pool = vk::raii::DescriptorPool{
		        vk.device,
		        vk::DescriptorPoolCreateInfo{
		                .maxSets = 2,
		                .poolSizeCount = pool_sizes.size(),
		                .pPoolSizes = pool_sizes.data(),
		        }};
		vk.name(*descriptor_pool, "motion warp descriptor pool");

		std::array layouts{*retain_ds_layout, *warp_ds_layout};
		auto sets = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
		        .descriptorPool = *descriptor_pool,
		        .descriptorSetCount = uint32_t(layouts.size()),
		        .pSetLayouts = layouts.data(),
		});
		retain_ds = sets[0].release();
		warp_ds = sets[1].release();
	}

	retained = make_target("retained");
	output = make_target("warped");
}

size_t motion_warper::device_memory() const
{
	return size_t(eye_size.width) * eye_size.height * 4 * view_count * 2;
}

void motion_warper::retain(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        std::array<vk::ImageView, view_count> src,
        std::array<xrt_rect, view_count> src_rect,
        bool flip_y)
{
	retain_push_constants pc{
	        .mapping = {},
	        .size = {int32_t(eye_size.width), int32_t(eye_size.height)},
	};
	for (size_t view = 0; view < view_count; ++view)
	{
		auto [base_x, step_x] = motion_axis_mapping(src_rect[view].offset.w, src_rect[view].extent.w, false, eye_size.width);
		auto [base_y, step_y] = motion_axis_mapping(src_rect[view].offset.h, src_rect[view].extent.h, flip_y, eye_size.height);
		pc.mapping[view][0] = base_x;
		pc.mapping[view][1] = base_y;
		pc.mapping[view][2] = step_x;
		pc.mapping[view][3] = step_y;
	}

	std::array source_info{
	        vk::DescriptorImageInfo{
	                .sampler = *sampler,
	                .imageView = src[0],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	        vk::DescriptorImageInfo{
	                .sampler = *sampler,
	                .imageView = src[1],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	};
	vk::DescriptorImageInfo dest_info{
	        .imageView = *retained.storage,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};

	std::array writes{
	        vk::WriteDescriptorSet{
	                .dstSet = retain_ds,
	                .dstBinding = 0,
	                .descriptorCount = source_info.size(),
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = source_info.data(),
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = retain_ds,
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &dest_info,
	        },
	};
	device.updateDescriptorSets(writes, {});

	// The image is fully rewritten, so nothing in it needs preserving; the source
	// scope is what keeps this write behind the warp pass of the previous commit,
	// which sampled the frame being replaced.
	vk::ImageMemoryBarrier2 to_general{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .oldLayout = vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = retained.image,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = view_count,
	        },
	};
	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &to_general,
	});

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *retain_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *retain_layout, 0, retain_ds, {});
	cmd.pushConstants<retain_push_constants>(*retain_layout, vk::ShaderStageFlagBits::eCompute, 0, pc);
	cmd.dispatch(divide_and_round_up(eye_size.width, MOTION_WARP_GROUP),
	             divide_and_round_up(eye_size.height, MOTION_WARP_GROUP),
	             view_count);

	// The reader is the warp pass of a *later* commit, in a later submission: a
	// barrier's second synchronisation scope covers everything after it in submission
	// order, so this one release is what makes every warp until the next retain()
	// see the frame, and it also puts the image in the layout they sample it in.
	vk::ImageMemoryBarrier2 to_read{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	        .oldLayout = vk::ImageLayout::eGeneral,
	        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        .image = retained.image,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = view_count,
	        },
	};
	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &to_read,
	});
}

void motion_warper::warp(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        vk::Buffer vectors,
        float t)
{
	vk::DescriptorImageInfo retained_info{
	        .sampler = *sampler,
	        .imageView = *retained.sampled,
	        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorBufferInfo field_info{
	        .buffer = vectors,
	        .range = vk::WholeSize,
	};
	vk::DescriptorImageInfo dest_info{
	        .imageView = *output.storage,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};

	std::array writes{
	        vk::WriteDescriptorSet{
	                .dstSet = warp_ds,
	                .dstBinding = 0,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &retained_info,
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = warp_ds,
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageBuffer,
	                .pBufferInfo = &field_info,
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = warp_ds,
	                .dstBinding = 2,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &dest_info,
	        },
	};
	device.updateDescriptorSets(writes, {});

	// Fully rewritten again, and again the source scope is only there to keep the
	// write behind the foveation pass of the previous commit, which sampled it.
	vk::ImageMemoryBarrier2 to_general{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .oldLayout = vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = output.image,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = view_count,
	        },
	};
	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &to_general,
	});

	warp_push_constants pc{
	        .size = {int32_t(eye_size.width), int32_t(eye_size.height)},
	        .grid = {int32_t(grid.width), int32_t(grid.height)},
	        .t = std::clamp<float>(t, 0, MOTION_MAX_STEPS),
	};

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *warp_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *warp_layout, 0, warp_ds, {});
	cmd.pushConstants<warp_push_constants>(*warp_layout, vk::ShaderStageFlagBits::eCompute, 0, pc);
	cmd.dispatch(divide_and_round_up(eye_size.width, MOTION_WARP_GROUP),
	             divide_and_round_up(eye_size.height, MOTION_WARP_GROUP),
	             view_count);

	// Read by the foveation pass, in this same command buffer.
	vk::ImageMemoryBarrier2 to_read{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	        .oldLayout = vk::ImageLayout::eGeneral,
	        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        .image = output.image,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = view_count,
	        },
	};
	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &to_read,
	});
}

} // namespace wivrn
