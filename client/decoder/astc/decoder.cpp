/*
 * WiVRn VR streaming
 * Copyright (C) 2025 Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "decoder.h"

#include "application.h"
#include "nxastc_packet.h"
#include "scenes/stream.h"
#include <lz4.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace
{
struct astc_blit_handle final : wivrn::decoder::blit_handle
{
	std::atomic_bool & free;

	astc_blit_handle(const wivrn::from_headset::feedback & feedback,
	                 const wivrn::to_headset::video_stream_data_shard::view_info_t & view_info,
	                 vk::ImageView view,
	                 vk::Image image,
	                 vk::Extent2D extent,
	                 vk::ImageLayout & layout,
	                 vk::Semaphore semaphore,
	                 uint64_t & semaphore_value,
	                 std::atomic_bool & free) :
	        wivrn::decoder::blit_handle{feedback, view_info, view, image, extent, layout, semaphore, &semaphore_value}, free(free)
	{}
	~astc_blit_handle() { free = true; }
};
}

namespace wivrn
{
astc_decoder::astc_decoder(vk::raii::Device & device,
                           vk::raii::PhysicalDevice & physical_device,
                           uint32_t queue_family_index,
                           const to_headset::video_stream_description & description,
                           uint8_t stream_index,
                           std::weak_ptr<scenes::stream> scene,
                           shard_accumulator * accumulator) :
        device(device),
        sampler_(device, vk::SamplerCreateInfo{
                                 .magFilter = vk::Filter::eLinear,
                                 .minFilter = vk::Filter::eLinear,
                                 .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                 .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                 .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                 .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                 .maxAnisotropy = 1,
                         }),
        extent{description.stream_size(stream_index).first, description.stream_size(stream_index).second},
        raw_bytes(nxastc_packet::block_bytes(extent.width, extent.height)),
        weak_scene(scene),
        accumulator(accumulator)
{
	const auto features = physical_device.getFeatures();
	if (!features.textureCompressionASTC_LDR)
		throw std::runtime_error("ASTC LDR texture compression is unsupported by this Vulkan device");
	const auto format = vk::Format::eAstc8x8UnormBlock;
	const auto support = physical_device.getFormatProperties(format).optimalTilingFeatures;
	const auto needed = vk::FormatFeatureFlagBits::eSampledImage |
	                    vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	                    vk::FormatFeatureFlagBits::eTransferDst;
	if ((support & needed) != needed)
		throw std::runtime_error("ASTC 8x8 lacks optimal sampled, linear-filter, or transfer-destination support");
	if (!extent.width || !extent.height || raw_bytes > std::numeric_limits<uint32_t>::max() || raw_bytes > INT_MAX)
		throw std::runtime_error("invalid ASTC stream dimensions");

	for (size_t i = 0; i < images.size(); ++i)
	{
		auto & item = images[i];
		item.pixels = image_allocation(
		        device,
		        vk::ImageCreateInfo{
		                .imageType = vk::ImageType::e2D,
		                .format = format,
		                .extent = {extent.width, extent.height, 1},
		                .mipLevels = 1,
		                .arrayLayers = 1,
		                .tiling = vk::ImageTiling::eOptimal,
		                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
		        },
		        {.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("ASTC decoder {} image {}", stream_index, i));
		item.view = vk::raii::ImageView(device, vk::ImageViewCreateInfo{
		                                               .image = item.pixels,
		                                               .viewType = vk::ImageViewType::e2D,
		                                               .format = format,
		                                               .subresourceRange = {
		                                                       .aspectMask = vk::ImageAspectFlagBits::eColor,
		                                                       .levelCount = 1,
		                                                       .layerCount = 1,
		                                               },
		                                       });
		item.staging = buffer_allocation(
		        device,
		        vk::BufferCreateInfo{
		                .size = raw_bytes,
		                .usage = vk::BufferUsageFlagBits::eTransferSrc,
		        },
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        },
		        "ASTC packet staging");
		item.mapped = item.staging.data<uint8_t>();
		item.semaphore = vk::raii::Semaphore(device, vk::StructureChain{
		                                                      vk::SemaphoreCreateInfo{},
		                                                      vk::SemaphoreTypeCreateInfo{.semaphoreType = vk::SemaphoreType::eTimeline},
		                                              }.get());
	}
	worker = std::thread([this, queue_family_index] { worker_function(queue_family_index); });
}

astc_decoder::~astc_decoder()
{
	{
		std::lock_guard lock(mutex);
		exiting = true;
		pending.clear();
	}
	wake.notify_all();
	if (worker.joinable())
		worker.join();
	try
	{
		device.waitIdle();
	}
	catch (const std::exception & e)
	{
		spdlog::warn("ASTC decoder shutdown wait failed: {}", e.what());
	}
}

void astc_decoder::push_data(std::span<std::span<const uint8_t>> data, uint64_t frame_index, bool)
{
	std::lock_guard lock(mutex);
	if (!have_frame || frame_index != assembling_frame)
	{
		if (have_frame && !assembling.empty())
			spdlog::debug("ASTC decoder drops incomplete packet for frame {}", assembling_frame);
		assembling.clear();
		assembling_frame = frame_index;
		have_frame = true;
		invalid_frame = false;
	}
	const size_t max_packet = size_t(raw_bytes) + nxastc_packet::header_size;
	for (auto part: data)
	{
		if (part.size() > max_packet - std::min(max_packet, assembling.size()))
		{
			invalid_frame = true;
			assembling.clear();
			continue;
		}
		if (!invalid_frame)
			assembling.insert(assembling.end(), part.begin(), part.end());
	}
}

void astc_decoder::frame_completed(const from_headset::feedback & feedback,
                                   const to_headset::video_stream_data_shard::view_info_t & view_info)
{
	std::lock_guard lock(mutex);
	if (!have_frame || invalid_frame)
	{
		assembling.clear();
		have_frame = false;
		invalid_frame = false;
		return;
	}
	auto header = nxastc_packet::parse_packet(assembling);
	if (!header || header->width != extent.width || header->height != extent.height || header->raw_bytes != raw_bytes)
	{
		spdlog::warn("ASTC decoder drops invalid packet for frame {}", assembling_frame);
		assembling.clear();
		have_frame = false;
		return;
	}
	if (pending.size() == pending_limit)
	{
		pending.pop_front();
		spdlog::debug("ASTC decoder drops oldest queued frame to keep latency bounded");
	}
	pending.push_back({std::move(assembling), feedback, view_info});
	assembling.clear();
	have_frame = false;
	invalid_frame = false;
	wake.notify_one();
}

astc_decoder::image * astc_decoder::get_free()
{
	for (auto & item: images)
		if (item.free.exchange(false))
			return &item;
	return nullptr;
}

void astc_decoder::worker_function(uint32_t queue_family_index)
{
	vk::raii::CommandPool command_pool(device, vk::CommandPoolCreateInfo{
	                                                   .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
	                                                   .queueFamilyIndex = queue_family_index,
	                                           });
	vk::raii::CommandBuffer cmd(std::move(device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
	        .commandPool = *command_pool,
	        .commandBufferCount = 1,
	})[0]));
	vk::raii::Fence fence(device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
	while (true)
	{
		frame current;
		{
			std::unique_lock lock(mutex);
			wake.wait(lock, [&] { return exiting || !pending.empty(); });
			if (exiting)
				return;
			current = std::move(pending.front());
			pending.pop_front();
		}

		image * item = nullptr;
		bool handed_off = false;
		bool submitted = false;
		try
		{
			const auto parsed = nxastc_packet::parse_packet(current.packet);
			if (!parsed || parsed->width != extent.width || parsed->height != extent.height || parsed->raw_bytes != raw_bytes)
				throw std::runtime_error("invalid ASTC packet dimensions or length");
			item = get_free();
			if (!item)
			{
				spdlog::debug("ASTC image pool exhausted; dropping complete frame");
				continue;
			}
			auto * payload = current.packet.data() + nxastc_packet::header_size;
			if (parsed->compressed)
			{
				const int written = LZ4_decompress_safe(reinterpret_cast<const char *>(payload),
				                                        reinterpret_cast<char *>(item->mapped),
				                                        int(parsed->payload_bytes),
				                                        int(parsed->raw_bytes));
				if (written != int(parsed->raw_bytes))
					throw std::runtime_error("ASTC LZ4 payload did not decode to exact block length");
			}
			else
				std::memcpy(item->mapped, payload, parsed->raw_bytes);
			if (vmaFlushAllocation(vk_allocator::instance(), static_cast<VmaAllocation>(item->staging), 0, parsed->raw_bytes) != VK_SUCCESS)
				throw std::runtime_error("failed to flush ASTC staging buffer");
			vk::Result waited = device.waitForFences(*fence, true, UINT64_MAX);
			if (waited != vk::Result::eSuccess)
				throw std::runtime_error("failed waiting for ASTC upload fence");
			cmd.reset();
			cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
			const auto previous = item->layout;
			cmd.pipelineBarrier(
			        previous == vk::ImageLayout::eUndefined ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eFragmentShader,
			        vk::PipelineStageFlagBits::eTransfer,
			        {},
			        {},
			        {},
			        vk::ImageMemoryBarrier{
			                .srcAccessMask = previous == vk::ImageLayout::eUndefined ? vk::AccessFlags{} : vk::AccessFlagBits::eShaderRead,
			                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
			                .oldLayout = previous,
			                .newLayout = vk::ImageLayout::eTransferDstOptimal,
			                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			                .image = item->pixels,
			                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1},
			        });
			item->layout = vk::ImageLayout::eTransferDstOptimal;
			cmd.copyBufferToImage(item->staging, item->pixels, item->layout, vk::BufferImageCopy{
			                                                                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
			                                                                    .imageExtent = {extent.width, extent.height, 1},
		                                                            });
			cmd.pipelineBarrier(
			        vk::PipelineStageFlagBits::eTransfer,
			        vk::PipelineStageFlagBits::eFragmentShader,
			        {},
			        {},
			        {},
			        vk::ImageMemoryBarrier{
			                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
			                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
			                .oldLayout = item->layout,
			                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			                .image = item->pixels,
			                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1},
			        });
			item->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
			cmd.end();
			device.resetFences(*fence);
			application::get_queue().lock()->submit(
			        vk::StructureChain{
			                vk::SubmitInfo{
			                        .commandBufferCount = 1,
			                        .pCommandBuffers = &*cmd,
			                        .signalSemaphoreCount = 1,
			                        .pSignalSemaphores = &*item->semaphore,
			                },
			                vk::TimelineSemaphoreSubmitInfo{
			                        .signalSemaphoreValueCount = 1,
			                        .pSignalSemaphoreValues = &++item->semaphore_value,
			                },
			        }.get(),
			        *fence);
			submitted = true;
			waited = device.waitForFences(*fence, true, UINT64_MAX);
			if (waited != vk::Result::eSuccess)
				throw std::runtime_error("failed waiting for ASTC upload fence");
			current.feedback.received_from_decoder = application::get_xr_instance().now();

			auto handle = std::make_shared<astc_blit_handle>(current.feedback,
			                                                current.view_info,
			                                                *item->view,
			                                                item->pixels,
			                                                extent,
			                                                item->layout,
			                                                *item->semaphore,
			                                                item->semaphore_value,
			                                                item->free);
			if (auto scene = weak_scene.lock())
			{
				scene->push_blit_handle(accumulator, std::move(handle));
				handed_off = true;
			}

		}
		catch (const std::exception & e)
		{
			if (item && !handed_off && !submitted)
				item->free = true;
			spdlog::warn("ASTC decoder exception: {}", e.what());
		}
	}
}

std::vector<video_codec> astc_decoder::supported_codecs()
{
	return {video_codec::nxastc};
}
} // namespace wivrn
