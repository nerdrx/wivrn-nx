/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Guillaume Meunier <guillaume.meunier@centraliens.net>
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

#include "image_writer.h"

#include "utils/thread_safe.h"
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <format>
#include <utility>
#include <string_view>
#include <stdexcept>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

std::optional<std::string> image_capture_request()
{
	std::string value;
#ifdef __ANDROID__
	char property[PROP_VALUE_MAX] = {};
	if (__system_property_get("debug.wivrn.nx.capture", property) > 0)
		value = property;
#else
	if (const char * property = std::getenv("WIVRN_NX_CAPTURE"))
		value = property;
#endif
	if (value.empty() || value == "0")
		return std::nullopt;
	for (char & c: value)
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
			c = '_';
	return value;
}

static std::string safe_capture_name(std::string_view request)
{
	std::string result;
	for (const char c: request)
		result += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
	return result.empty() ? "capture" : result;
}

void write_image_layers(
		vk::raii::Device & device,
		thread_safe<vk::raii::Queue> & queue,
		uint32_t queue_family_index,
		const std::filesystem::path & path,
		vk::Image image,
		vk::Format format,
		vk::Extent2D extent,
		uint32_t array_layers)
{
	if (!array_layers || (format != vk::Format::eR8G8B8A8Srgb && format != vk::Format::eB8G8R8A8Srgb))
		throw std::runtime_error("NX capture requires RGBA/BGRA SRGB layers");
	const vk::DeviceSize layer_size = vk::DeviceSize(extent.width) * extent.height * 4;
	buffer_allocation output_buffer{
	        device,
	        vk::BufferCreateInfo{.size = layer_size * array_layers, .usage = vk::BufferUsageFlagBits::eTransferDst},
	        VmaAllocationCreateInfo{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
	                                 .usage = VmaMemoryUsage::VMA_MEMORY_USAGE_AUTO},
	        "Saved image buffer"};
	vk::raii::CommandPool cp{device, vk::CommandPoolCreateInfo{.queueFamilyIndex = queue_family_index}};
	vk::raii::CommandBuffer command_buffer = std::move(device.allocateCommandBuffers(
	        {.commandPool = *cp, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1})[0]);
	command_buffer.begin(vk::CommandBufferBeginInfo{});
	const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, array_layers};
	command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
	                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
	                               vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
	                                                      .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	                                                      .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
	                                                      .newLayout = vk::ImageLayout::eTransferSrcOptimal,
	                                                      .image = image, .subresourceRange = range});
	for (uint32_t layer = 0; layer < array_layers; ++layer)
		command_buffer.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, output_buffer,
		                                 vk::BufferImageCopy{.bufferOffset = layer_size * layer,
		                                                      .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, layer, 1},
		                                                      .imageExtent = {extent.width, extent.height, 1}});
	command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                               vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {},
	                               vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferRead,
	                                                      .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
	                                                      .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
	                                                      .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
	                                                      .image = image, .subresourceRange = range});
	command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                               vk::PipelineStageFlagBits::eHost, {},
		                       vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
		                                          .dstAccessMask = vk::AccessFlagBits::eHostRead},
		                       {}, {});
	command_buffer.end();
	// A diagnostic may stall the queue. Keep submission and completion under
	// its lock, and avoid creating a short-lived sync-fd-backed capture fence.
	{
		auto capture_queue = queue.lock();
		capture_queue->submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &*command_buffer});
		capture_queue->waitIdle();
	}
	auto * output_pixels = output_buffer.data();
	vmaInvalidateAllocation(vk_allocator::instance(), output_buffer, 0, VK_WHOLE_SIZE);
	const auto request = safe_capture_name(path.stem().string());
	for (uint32_t layer = 0; layer < array_layers; ++layer)
	{
		auto * pixels = output_pixels + layer_size * layer;
		if (format == vk::Format::eB8G8R8A8Srgb)
			for (size_t i = 0; i < size_t(extent.width) * extent.height; ++i)
				std::swap(pixels[4 * i], pixels[4 * i + 2]);
		if (!stbi_write_png((path.parent_path() / std::format("{}_{}.png", request, layer)).c_str(),
		                    extent.width, extent.height, 4, pixels, 0))
			throw std::runtime_error("NX capture PNG write failed");
	}
}

void write_image(
        vk::raii::Device & device,
        thread_safe<vk::raii::Queue> & queue,
        uint32_t queue_family_index,
        const std::filesystem::path & path,
        vk::Image image,
        const vk::ImageCreateInfo & info)
{
	assert(info.usage & vk::ImageUsageFlagBits::eTransferSrc);
	assert(info.extent.depth == 1);
	assert(info.format == vk::Format::eR8G8B8A8Srgb);

	buffer_allocation output_buffer{
	        device,
	        vk::BufferCreateInfo{
	                .size = info.extent.height * info.extent.width * 4,
	                .usage = vk::BufferUsageFlagBits::eTransferDst,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
	                .usage = VmaMemoryUsage::VMA_MEMORY_USAGE_AUTO,
	        },
	        "Saved image buffer"};

	vk::raii::CommandPool cp{
	        device,
	        vk::CommandPoolCreateInfo{
	                .queueFamilyIndex = queue_family_index,
	        }};

	vk::raii::CommandBuffer command_buffer = std::move(device.allocateCommandBuffers(
	        {
	                .commandPool = *cp,
	                .level = vk::CommandBufferLevel::ePrimary,
	                .commandBufferCount = 1,
	        })[0]);

	vk::raii::Fence fence = device.createFence({});

	command_buffer.begin(vk::CommandBufferBeginInfo{});

	command_buffer.pipelineBarrier(
	        vk::PipelineStageFlagBits::eColorAttachmentOutput,
	        vk::PipelineStageFlagBits::eTransfer,
	        vk::DependencyFlags{},
	        {},
	        {},
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
	                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
	                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
	                .image = image,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .baseMipLevel = 0,
	                        .levelCount = 1,
	                        .baseArrayLayer = 0,
	                        .layerCount = 1,
	                },
	        });

	command_buffer.copyImageToBuffer(
	        image,
	        vk::ImageLayout::eTransferSrcOptimal,
	        output_buffer,
	        vk::BufferImageCopy{
	                .bufferOffset = 0,
	                .bufferRowLength = 0,
	                .bufferImageHeight = 0,
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .mipLevel = 0,
	                        .baseArrayLayer = 0,
	                        .layerCount = 1,
	                },
	                .imageOffset = {0, 0, 0},
	                .imageExtent = info.extent,
	        });

	command_buffer.end();
	queue.lock()->submit(vk::SubmitInfo{
	                             .commandBufferCount = 1,
	                             .pCommandBuffers = &*command_buffer,
	                     },
	                     *fence);

	if (auto result = device.waitForFences(*fence, true, 1'000'000'000); result == vk::Result::eSuccess)
		stbi_write_png(path.c_str(), info.extent.width, info.extent.height, 4, output_buffer.data(), 0);
}
