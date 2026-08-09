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

#include "motion_estimator.h"

#include "utils/wivrn_vk_bundle.h"
#include "vk/allocation.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace
{

struct downsample_push_constants
{
	// Per view: base.xy then step.xy
	float mapping[2][4];
	int32_t l0_size[2];
};

struct estimate_push_constants
{
	int32_t grid[2];
	int32_t l0_size[2];
};

const uint32_t coarsest = 1u << (MOTION_LEVELS - 1);

uint32_t divide_and_round_up(uint32_t a, uint32_t b)
{
	return (a + b - 1) / b;
}

// Level 0 dimensions: the source divided by the level 0 scale, rounded up to a
// multiple of the coarsest scale so that every level divides exactly and the
// downsample shader never writes outside a level.
vk::Extent2D level0_size(vk::Extent2D eye)
{
	auto round = [](uint32_t x) {
		uint32_t n = divide_and_round_up(std::max<uint32_t>(x, 1), 1u << MOTION_L0_SHIFT);
		return std::max(coarsest, divide_and_round_up(n, coarsest) * coarsest);
	};
	return {round(eye.width), round(eye.height)};
}

vk::Extent2D grid_size(vk::Extent2D eye)
{
	auto round = [](uint32_t x) {
		return std::max<uint32_t>(1, (x + MOTION_BLOCK_PX / 2) / MOTION_BLOCK_PX);
	};
	return {round(eye.width), round(eye.height)};
}

// Maps the source rectangle onto level 0 the way foveation.cpp maps it onto the
// encoded image, so that the pyramid, and therefore the motion vectors, are in the
// orientation of the image the headset displays.
std::pair<float, float> axis_mapping(int32_t offset, int32_t extent, bool flip, uint32_t destination)
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

} // namespace

namespace wivrn
{

motion_estimator::pyramid motion_estimator::make_pyramid(int index)
{
	pyramid res;
	res.image = image_allocation{
	        vk.device,
	        vk::ImageCreateInfo{
	                .imageType = vk::ImageType::e2D,
	                .format = vk::Format::eR8Unorm,
	                .extent = {.width = level0.width, .height = level0.height, .depth = 1},
	                .mipLevels = MOTION_LEVELS,
	                .arrayLayers = view_count,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
	        },
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
	        std::format("motion pyramid {}", index),
	};

	res.levels.reserve(MOTION_LEVELS);
	for (uint32_t level = 0; level < MOTION_LEVELS; ++level)
	{
		res.levels.emplace_back(
		        vk.device,
		        vk::ImageViewCreateInfo{
		                .image = res.image,
		                .viewType = vk::ImageViewType::e2DArray,
		                .format = vk::Format::eR8Unorm,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .baseMipLevel = level,
		                        .levelCount = 1,
		                        .layerCount = view_count,
		                },
		        });
	}

	res.sampled = vk::raii::ImageView{
	        vk.device,
	        vk::ImageViewCreateInfo{
	                .image = res.image,
	                .viewType = vk::ImageViewType::e2DArray,
	                .format = vk::Format::eR8Unorm,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = MOTION_LEVELS,
	                        .layerCount = view_count,
	                },
	        },
	};

	return res;
}

motion_estimator::motion_estimator(vk_bundle & bundle, vk::Extent2D eye) :
        vk(bundle),
        eye_size(eye),
        level0(level0_size(eye)),
        grid(grid_size(eye)),
        source_sampler(nullptr),
        pyramid_sampler(nullptr),
        downsample_ds_layout(nullptr),
        downsample_layout(nullptr),
        downsample_pipeline(nullptr),
        estimate_ds_layout(nullptr),
        estimate_layout(nullptr),
        estimate_pipeline(nullptr),
        descriptor_pool(nullptr)
{
	// The composited views are resampled with a linear filter, the pyramids are
	// only ever read with texelFetch
	source_sampler = vk::raii::Sampler{
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eLinear,
	                .minFilter = vk::Filter::eLinear,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	        }};
	vk.name(*source_sampler, "motion source sampler");

	pyramid_sampler = vk::raii::Sampler{
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eNearest,
	                .minFilter = vk::Filter::eNearest,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	        }};
	vk.name(*pyramid_sampler, "motion pyramid sampler");

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
		                .descriptorCount = MOTION_LEVELS,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		};
		downsample_ds_layout = vk::raii::DescriptorSetLayout{
		        vk.device,
		        vk::DescriptorSetLayoutCreateInfo{
		                .bindingCount = bindings.size(),
		                .pBindings = bindings.data(),
		        }};
		vk.name(*downsample_ds_layout, "motion downsample descriptor set layout");

		vk::PushConstantRange range{
		        .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        .size = sizeof(downsample_push_constants),
		};
		downsample_layout = vk::raii::PipelineLayout{
		        vk.device,
		        vk::PipelineLayoutCreateInfo{
		                .setLayoutCount = 1,
		                .pSetLayouts = &*downsample_ds_layout,
		                .pushConstantRangeCount = 1,
		                .pPushConstantRanges = &range,
		        }};

		auto shader = vk.load_shader("motion_downsample");
		downsample_pipeline = vk::raii::Pipeline{
		        vk.device,
		        nullptr,
		        vk::ComputePipelineCreateInfo{
		                .stage = {
		                        .stage = vk::ShaderStageFlagBits::eCompute,
		                        .module = *shader,
		                        .pName = "main",
		                },
		                .layout = *downsample_layout,
		        }};
		vk.name(*downsample_pipeline, "motion downsample pipeline");
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
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 2,
		                .descriptorType = vk::DescriptorType::eStorageBuffer,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		};
		estimate_ds_layout = vk::raii::DescriptorSetLayout{
		        vk.device,
		        vk::DescriptorSetLayoutCreateInfo{
		                .bindingCount = bindings.size(),
		                .pBindings = bindings.data(),
		        }};
		vk.name(*estimate_ds_layout, "motion estimate descriptor set layout");

		vk::PushConstantRange range{
		        .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        .size = sizeof(estimate_push_constants),
		};
		estimate_layout = vk::raii::PipelineLayout{
		        vk.device,
		        vk::PipelineLayoutCreateInfo{
		                .setLayoutCount = 1,
		                .pSetLayouts = &*estimate_ds_layout,
		                .pushConstantRangeCount = 1,
		                .pPushConstantRanges = &range,
		        }};

		auto shader = vk.load_shader("motion_estimate");
		estimate_pipeline = vk::raii::Pipeline{
		        vk.device,
		        nullptr,
		        vk::ComputePipelineCreateInfo{
		                .stage = {
		                        .stage = vk::ShaderStageFlagBits::eCompute,
		                        .module = *shader,
		                        .pName = "main",
		                },
		                .layout = *estimate_layout,
		        }};
		vk.name(*estimate_pipeline, "motion estimate pipeline");
	}

	{
		std::array pool_sizes{
		        vk::DescriptorPoolSize{
		                .type = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = uint32_t(view_count) + 2,
		        },
		        vk::DescriptorPoolSize{
		                .type = vk::DescriptorType::eStorageImage,
		                .descriptorCount = MOTION_LEVELS,
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
		vk.name(*descriptor_pool, "motion descriptor pool");

		std::array layouts{*downsample_ds_layout, *estimate_ds_layout};
		auto sets = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
		        .descriptorPool = *descriptor_pool,
		        .descriptorSetCount = uint32_t(layouts.size()),
		        .pSetLayouts = layouts.data(),
		});
		downsample_ds = sets[0].release();
		estimate_ds = sets[1].release();
	}

	pyramids = {make_pyramid(0), make_pyramid(1)};

	const vk::DeviceSize result_size = vk::DeviceSize(grid.width) * grid.height * view_count * 2 * sizeof(float);

	result = buffer_allocation{
	        vk.device,
	        vk::BufferCreateInfo{
	                .size = result_size,
	                .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc,
	        },
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE},
	        "motion result buffer",
	};

	readback = buffer_allocation{
	        vk.device,
	        vk::BufferCreateInfo{
	                .size = result_size,
	                .usage = vk::BufferUsageFlagBits::eTransferDst,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
	        },
	        "motion readback buffer",
	};
	readback_coherent = readback.properties() & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	readback.map();
}

size_t motion_estimator::device_memory() const
{
	// One byte per texel and level, the mip chain adds a third at these ratios
	size_t pyramid_texels = 0;
	for (uint32_t level = 0; level < MOTION_LEVELS; ++level)
		pyramid_texels += size_t(std::max<uint32_t>(level0.width >> level, 1)) *
		                  std::max<uint32_t>(level0.height >> level, 1);

	return pyramid_texels * view_count * pyramids.size() + 2 * result.info().size;
}

bool motion_estimator::estimate(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        std::array<vk::ImageView, view_count> src,
        std::array<xrt_rect, view_count> src_rect,
        bool flip_y)
{
	const size_t previous = 1 - current;
	const size_t destination = have_previous ? previous : current;

	downsample_push_constants pc{
	        .mapping = {},
	        .l0_size = {int32_t(level0.width), int32_t(level0.height)},
	};
	for (size_t view = 0; view < view_count; ++view)
	{
		auto [base_x, step_x] = axis_mapping(src_rect[view].offset.w, src_rect[view].extent.w, false, level0.width);
		auto [base_y, step_y] = axis_mapping(src_rect[view].offset.h, src_rect[view].extent.h, flip_y, level0.height);
		pc.mapping[view][0] = base_x;
		pc.mapping[view][1] = base_y;
		pc.mapping[view][2] = step_x;
		pc.mapping[view][3] = step_y;
	}

	std::array source_info{
	        vk::DescriptorImageInfo{
	                .sampler = *source_sampler,
	                .imageView = src[0],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	        vk::DescriptorImageInfo{
	                .sampler = *source_sampler,
	                .imageView = src[1],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	};

	std::array<vk::DescriptorImageInfo, MOTION_LEVELS> level_info;
	for (uint32_t level = 0; level < MOTION_LEVELS; ++level)
		level_info[level] = {
		        .imageView = *pyramids[destination].levels[level],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};

	std::array writes{
	        vk::WriteDescriptorSet{
	                .dstSet = downsample_ds,
	                .dstBinding = 0,
	                .descriptorCount = source_info.size(),
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = source_info.data(),
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = downsample_ds,
	                .dstBinding = 1,
	                .descriptorCount = level_info.size(),
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = level_info.data(),
	        },
	};
	device.updateDescriptorSets(writes, {});

	// The pyramid is fully rewritten; its previous contents only matter for the
	// slot that is being kept, which is not this one.
	vk::ImageMemoryBarrier2 to_general{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .oldLayout = initialized[destination] ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = pyramids[destination].image,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = MOTION_LEVELS,
	                .layerCount = view_count,
	        },
	};
	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &to_general,
	});

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *downsample_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *downsample_layout, 0, downsample_ds, {});
	cmd.pushConstants<downsample_push_constants>(*downsample_layout, vk::ShaderStageFlagBits::eCompute, 0, pc);
	cmd.dispatch(divide_and_round_up(level0.width, 8), divide_and_round_up(level0.height, 8), view_count);

	initialized[destination] = true;

	// The estimate pass below samples the pyramid that was just written, and so does
	// the next call, whichever of the two runs first. Make those writes visible
	// unconditionally: on the first call after a (re)start there is nothing to match
	// against yet and the function returns early, but the pyramid it just built is
	// exactly what the next call reads.
	vk::ImageMemoryBarrier2 to_read{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	        .oldLayout = vk::ImageLayout::eGeneral,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = pyramids[destination].image,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = MOTION_LEVELS,
	                .layerCount = view_count,
	        },
	};
	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &to_read,
	});

	current = destination;

	if (not have_previous)
	{
		// Nothing to match against yet, the pyramid just built becomes the
		// previous frame for the next call.
		have_previous = true;
		return false;
	}

	std::array match_images{
	        vk::DescriptorImageInfo{
	                .sampler = *pyramid_sampler,
	                .imageView = *pyramids[current].sampled,
	                .imageLayout = vk::ImageLayout::eGeneral,
	        },
	        vk::DescriptorImageInfo{
	                .sampler = *pyramid_sampler,
	                .imageView = *pyramids[1 - current].sampled,
	                .imageLayout = vk::ImageLayout::eGeneral,
	        },
	};
	vk::DescriptorBufferInfo result_info{
	        .buffer = result,
	        .range = vk::WholeSize,
	};

	std::array match_writes{
	        vk::WriteDescriptorSet{
	                .dstSet = estimate_ds,
	                .dstBinding = 0,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &match_images[0],
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = estimate_ds,
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &match_images[1],
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = estimate_ds,
	                .dstBinding = 2,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageBuffer,
	                .pBufferInfo = &result_info,
	        },
	};
	device.updateDescriptorSets(match_writes, {});

	estimate_push_constants epc{
	        .grid = {int32_t(grid.width), int32_t(grid.height)},
	        .l0_size = {int32_t(level0.width), int32_t(level0.height)},
	};

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *estimate_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *estimate_layout, 0, estimate_ds, {});
	cmd.pushConstants<estimate_push_constants>(*estimate_layout, vk::ShaderStageFlagBits::eCompute, 0, epc);
	cmd.dispatch(grid.width, grid.height, view_count);

	vk::BufferMemoryBarrier2 to_transfer{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
	        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
	        .buffer = result,
	        .offset = 0,
	        .size = vk::WholeSize,
	};
	cmd.pipelineBarrier2({
	        .bufferMemoryBarrierCount = 1,
	        .pBufferMemoryBarriers = &to_transfer,
	});

	cmd.copyBuffer(result, readback, vk::BufferCopy{.size = result.info().size});

	vk::BufferMemoryBarrier2 to_host{
	        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
	        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
	        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
	        .buffer = readback,
	        .offset = 0,
	        .size = vk::WholeSize,
	};
	cmd.pipelineBarrier2({
	        .bufferMemoryBarrierCount = 1,
	        .pBufferMemoryBarriers = &to_host,
	});

	return true;
}

motion_estimator::field motion_estimator::read_back()
{
	if (not readback_coherent)
		vmaInvalidateAllocation(vk_allocator::instance(), readback, 0, VK_WHOLE_SIZE);

	const size_t count = size_t(grid.width) * grid.height * view_count * 2;
	const float * values = readback.data<float>();

	field res{
	        .width = uint16_t(grid.width),
	        .height = uint16_t(grid.height),
	};

	float peak = 0;
	for (size_t i = 0; i < count; ++i)
	{
		float v = values[i];
		if (not std::isfinite(v))
			v = 0;
		peak = std::max(peak, std::abs(v));
	}
	peak = std::min<float>(peak, MOTION_MAX_DISPLACEMENT);

	res.scale = peak;
	res.vectors.resize(count);
	if (peak <= 0)
		return res;

	// The whole field is quantized against its own longest vector, so the step is
	// as fine as the frame allows and no scale needs to be guessed anywhere else.
	const float inv = 127.f / peak;
	for (size_t i = 0; i < count; ++i)
	{
		float v = values[i];
		if (not std::isfinite(v))
			v = 0;
		res.vectors[i] = int8_t(std::clamp<int>(std::lround(v * inv), -127, 127));
	}

	return res;
}

} // namespace wivrn
