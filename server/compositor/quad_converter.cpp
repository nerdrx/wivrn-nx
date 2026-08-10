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

#include "quad_converter.h"

#include "encoder/encoder_settings.h"
#include "encoder/video_encoder.h"
#include "is_finite.h"
#include "utils/wivrn_vk_bundle.h"
#include "wivrn_config.h"

#include <algorithm>
#include <format>

namespace
{
// Same three formats the eye image uses: a single plane view for luma, a two
// channel view for chroma, and the two plane 4:2:0 format they alias.
std::array<vk::Format, 3> image_formats(int bit_depth)
{
	switch (bit_depth)
	{
		case 8:
			return {
			        vk::Format::eR8Unorm,
			        vk::Format::eR8G8Unorm,
			        vk::Format::eG8B8R82Plane420Unorm,
			};
		case 10:
			return {
			        vk::Format::eR16Unorm,
			        vk::Format::eR16G16Unorm,
			        vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16,
			};
	}
	throw std::runtime_error(std::format("Unsupported bit depth {}", bit_depth));
}

struct push_constants
{
	std::array<float, 4> src_rect;
	std::array<int32_t, 2> dst_size;
	std::array<int32_t, 2> image_size;
};

// Rounds down to a multiple of two: the encode image is 4:2:0, a filled region of
// odd size would leave a chroma sample half in the picture and half in the padding.
uint32_t even(uint32_t v)
{
	return v & ~1u;
}
} // namespace

namespace wivrn
{

quad_converter::quad_converter(vk_bundle & vk, const encoder_settings & settings, size_t num_slots) :
        size{.width = settings.width, .height = settings.height},
        sampler{
                vk.device,
                vk::SamplerCreateInfo{
                        .magFilter = vk::Filter::eLinear,
                        .minFilter = vk::Filter::eLinear,
                        .mipmapMode = vk::SamplerMipmapMode::eLinear,
                        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                        .borderColor = vk::BorderColor::eFloatOpaqueBlack,
                }},
        ds_layout{nullptr},
        layout{nullptr},
        pipeline{nullptr},
        descriptor_pool{nullptr}
{
	vk.name(sampler, "quad converter sampler");

	std::array bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
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

	ds_layout = vk::raii::DescriptorSetLayout(
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = bindings.size(),
	                .pBindings = bindings.data(),
	        });
	vk.name(ds_layout, "quad converter descriptor set layout");

	vk::PushConstantRange pc_range{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(push_constants),
	};
	layout = vk::raii::PipelineLayout(
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &*ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &pc_range,
	        });
	vk.name(layout, "quad converter pipeline layout");

	pipeline = vk::raii::Pipeline(
	        vk.device,
	        nullptr,
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *vk.load_shader("quad_convert"),
	                        .pName = "main",
	                },
	                .layout = *layout,
	        });
	vk.name(pipeline, "quad converter pipeline");

	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = uint32_t(num_slots),
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = uint32_t(2 * num_slots),
	        },
	};
	descriptor_pool = vk::raii::DescriptorPool(
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = uint32_t(num_slots),
	                .poolSizeCount = pool_sizes.size(),
	                .pPoolSizes = pool_sizes.data(),
	        });
	vk.name(descriptor_pool, "quad converter descriptor pool");

	auto formats = image_formats(settings.bit_depth);

	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat,
	                .imageType = vk::ImageType::e2D,
	                .format = formats.back(),
	                .extent = {
	                        .width = size.width,
	                        .height = size.height,
	                        .depth = 1,
	                },
	                .mipLevels = 1,
	                .arrayLayers = 1,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = formats.size(),
	                .pViewFormats = formats.data(),
	        },
	};
#if WIVRN_USE_VULKAN_ENCODE
	if (std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(vk.feat).videoMaintenance1 and
	    settings.encoder_name == wivrn::encoder_vulkan)
	{
		image_info.get().flags |= vk::ImageCreateFlagBits::eVideoProfileIndependentKHR;
		image_info.get().usage |= vk::ImageUsageFlagBits::eVideoEncodeSrcKHR;
	}
#endif

	std::vector<vk::DescriptorSetLayout> layouts(num_slots, *ds_layout);
	auto sets = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	        .descriptorPool = *descriptor_pool,
	        .descriptorSetCount = uint32_t(num_slots),
	        .pSetLayouts = layouts.data(),
	});

	vk::ImageViewUsageCreateInfo usage{
	        .usage = vk::ImageUsageFlagBits::eStorage,
	};

	slots.reserve(num_slots);
	for (size_t i = 0; i < num_slots; ++i)
	{
		image_allocation image{
		        vk.device,
		        image_info.get(),
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("compositor quad YCbCr image {}", i),
		};
		vk::Image vk_image{image};

		slots.push_back({
		        .image = std::move(image),
		        .view_y{
		                vk.device,
		                vk::ImageViewCreateInfo{
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2D,
		                        .format = formats[0],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane0,
		                                .levelCount = 1,
		                                .layerCount = 1,
		                        },
		                },
		        },
		        .view_cbcr{
		                vk.device,
		                vk::ImageViewCreateInfo{
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2D,
		                        .format = formats[1],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane1,
		                                .levelCount = 1,
		                                .layerCount = 1,
		                        },
		                },
		        },
		        .descriptor_set = sets[i].release(),
		});
	}
}

xrt_rect quad_converter::convert(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        size_t slot_index,
        vk::ImageView src,
        const xrt_normalized_rect & src_rect,
        float aspect,
        uint32_t src_width,
        uint32_t src_height)
{
	auto & slot = slots[slot_index];

	// Largest region of the encode image that has the quad's aspect ratio and does
	// not upscale the source. Whatever is left of the image stays black.
	uint32_t w = std::min<uint32_t>(size.width, std::max(2u, uint32_t(std::abs(src_rect.w) * src_width)));
	uint32_t h = std::min<uint32_t>(size.height, std::max(2u, uint32_t(std::abs(src_rect.h) * src_height)));
	if (wivrn::is_finite(aspect) and aspect > 0)
	{
		if (float(w) / aspect > float(h))
			w = uint32_t(h * aspect);
		else
			h = uint32_t(w / aspect);
	}
	w = std::clamp<uint32_t>(even(w), 2, size.width);
	h = std::clamp<uint32_t>(even(h), 2, size.height);

	std::array image_infos{
	        vk::DescriptorImageInfo{
	                .sampler = *sampler,
	                .imageView = src,
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	        vk::DescriptorImageInfo{
	                .imageView = *slot.view_y,
	                .imageLayout = vk::ImageLayout::eGeneral,
	        },
	        vk::DescriptorImageInfo{
	                .imageView = *slot.view_cbcr,
	                .imageLayout = vk::ImageLayout::eGeneral,
	        },
	};

	std::array writes{
	        vk::WriteDescriptorSet{
	                .dstSet = slot.descriptor_set,
	                .dstBinding = 0,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &image_infos[0],
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = slot.descriptor_set,
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &image_infos[1],
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = slot.descriptor_set,
	                .dstBinding = 2,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &image_infos[2],
	        },
	};
	device.updateDescriptorSets(writes, {});

	push_constants pc{
	        .src_rect = {src_rect.x, src_rect.y, src_rect.w, src_rect.h},
	        .dst_size = {int32_t(w), int32_t(h)},
	        .image_size = {int32_t(size.width), int32_t(size.height)},
	};

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, slot.descriptor_set, {});
	cmd.pushConstants<push_constants>(*layout, vk::ShaderStageFlagBits::eCompute, 0, pc);
	// One invocation per chroma sample, so the whole image including the padding
	cmd.dispatch((size.width / 2 + 7) / 8, (size.height / 2 + 7) / 8, 1);

	return {
	        .offset = {},
	        .extent = {.w = int(w), .h = int(h)},
	};
}

} // namespace wivrn
