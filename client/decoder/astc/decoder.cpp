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
#include "nxastc_packet_decode.h"
#include "scenes/stream.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

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
	                 std::atomic_bool & free) :
	        wivrn::decoder::blit_handle{feedback, view_info, view, image, extent, layout, nullptr, nullptr}, free(free)
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
#ifdef __ANDROID__
	char sync_upload[PROP_VALUE_MAX] = {};
	const bool force_sync_upload = __system_property_get("debug.wivrn.nx.astc_sync_upload", sync_upload) > 0 && sync_upload[0] == '1';
	if (force_sync_upload)
	{
		async_upload_enabled = false;
		spdlog::info("ASTC synchronous uploads forced by debug.wivrn.nx.astc_sync_upload");
	}
	char queue_timing[PROP_VALUE_MAX] = {};
	// Diagnostic only: no steady-clock reads or queue counters unless exactly enabled.
	queue_timing_enabled = __system_property_get("debug.wivrn.nx.astc_queue_timing", queue_timing) == 1 && queue_timing[0] == '1';
#endif
	// LZ4/Zstd read backward references while writing. Pico measurements favour
	// ordinary CPU memory plus one forward copy, even when VMA reports HOST_CACHED.
	cpu_scratch.resize(size_t(raw_bytes));

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
	}
	spdlog::info("ASTC upload handoff: {}", async_upload_enabled ? "same-queue async" : "synchronous fence");
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
		use_recycled_packet_locked();
		assembling_frame = frame_index;
		have_frame = true;
		invalid_frame = false;
	}
	const size_t max_packet = size_t(raw_bytes) + nxastc_packet::motion_header_size;
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

void astc_decoder::recycle_packet_locked(std::vector<uint8_t> & packet)
{
	const size_t max_packet = size_t(raw_bytes) + nxastc_packet::motion_header_size;
	if (exiting)
		return;
	astc_detail::recycle_packet_buffer_locked(recycled_packet, packet, max_packet);
}

void astc_decoder::recycle_packet(std::vector<uint8_t> & packet)
{
	std::lock_guard lock(mutex);
	recycle_packet_locked(packet);
}

void astc_decoder::use_recycled_packet_locked()
{
	astc_detail::use_recycled_packet_buffer_locked(assembling, recycled_packet);
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
		recycle_packet_locked(pending.front().packet);
		pending.pop_front();
		if (queue_timing_enabled)
			++pending_drop_count;
		spdlog::debug("ASTC decoder drops oldest queued frame to keep latency bounded");
	}
	frame queued{std::move(assembling), feedback, view_info};
	if (queue_timing_enabled)
		queued.queued_at = std::chrono::steady_clock::now();
	pending.push_back(std::move(queued));
	assembling.clear();
	use_recycled_packet_locked();
	have_frame = false;
	invalid_frame = false;
	wake.notify_one();
}

astc_decoder::image * astc_decoder::get_free()
{
	for (auto & item: images)
		if (item.upload_complete && item.free.exchange(false))
			return &item;
	return nullptr;
}

void astc_decoder::worker_function(uint32_t queue_family_index)
{
	using steady_clock = std::chrono::steady_clock;
	uint32_t timing_frames = 0;
	uint64_t decode_copy_ns = 0, prewait_ns = 0, syncwait_ns = 0, handoff_ns = 0;
	vk::raii::CommandPool command_pool(device, vk::CommandPoolCreateInfo{
	                                                   .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
	                                                   .queueFamilyIndex = queue_family_index,
	                                           });
	vk::raii::CommandBuffer cmd(std::move(device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
	        .commandPool = *command_pool,
	        .commandBufferCount = 1,
	})[0]));
	vk::raii::Fence fence(device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
	image * last_submitted = nullptr;
	auto wait_upload_fence = [&]() noexcept {
		try
		{
			return device.waitForFences(*fence, true, UINT64_MAX) == vk::Result::eSuccess;
		}
		catch (const std::exception & e)
		{
			spdlog::warn("ASTC upload fence wait failed: {}", e.what());
		}
		catch (...)
		{
			spdlog::warn("ASTC upload fence wait failed with an unknown Vulkan error");
		}
		return false;
	};
	auto drain_upload = [&]() noexcept {
		if (!last_submitted)
			return true;
		if (!wait_upload_fence())
		{
			// A lost Vulkan device cannot guarantee completion; never mark the image reusable.
			spdlog::warn("ASTC shutdown upload drain failed; device may be lost");
			return false;
		}
		last_submitted->upload_complete = true;
		last_submitted = nullptr;
		return true;
	};
	while (true)
	{
		frame current;
		bool stopping = false;
		{
			std::unique_lock lock(mutex);
			wake.wait(lock, [&] { return exiting || !pending.empty(); });
			if (exiting)
				stopping = true;
			else
			{
				current = std::move(pending.front());
				pending.pop_front();
				if (queue_timing_enabled)
				{
					const uint64_t dwell_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
					        std::chrono::steady_clock::now() - current.queued_at).count());
					pending_dwell_ns += dwell_ns;
					pending_max_dwell_ns = std::max(pending_max_dwell_ns, dwell_ns);
					++pending_dequeue_count;
				}
			}
		}
		if (stopping)
		{
			// Release the mutex before waiting so teardown and packet delivery never
			// block behind GPU completion. Drain before cmd/fence/pool are destroyed.
			drain_upload();
			return;
		}

		image * item = nullptr;
		bool handed_off = false;
		bool submitted = false;
		bool fence_reset = false;
		bool stop_worker = false;
		uint64_t frame_decode_copy_ns = 0, frame_prewait_ns = 0, frame_syncwait_ns = 0, frame_handoff_ns = 0;
		try
		{
			const auto parsed = nxastc_packet::parse_packet(current.packet);
			if (!parsed || parsed->width != extent.width || parsed->height != extent.height || parsed->raw_bytes != raw_bytes)
				throw std::runtime_error("invalid ASTC packet dimensions or length");
			if (last_submitted && fence.getStatus() == vk::Result::eSuccess)
			{
				last_submitted->upload_complete = true;
				last_submitted = nullptr;
			}
			item = get_free();
			if (!item)
			{
				spdlog::debug("ASTC image pool exhausted; dropping complete frame");
				recycle_packet(current.packet);
				continue;
			}
			const auto payload = std::span<const uint8_t>(current.packet).subspan(parsed->header_bytes);
			const bool motion_packet = parsed->encoding == nxastc_packet::compression::motion_zstd ||
			                           parsed->encoding == nxastc_packet::compression::motion_raw;
			std::span<const uint8_t> reference;
			if (motion_packet && parsed->reference_frame != UINT64_MAX)
			{
				if (!nxastc_packet::motion_reference_usable(current.feedback.frame_index, parsed->reference_frame))
					throw std::runtime_error("ASTC motion reference is outside the allowed frame window");
				for (const auto & cached: references)
					if (cached.frame_index == parsed->reference_frame) reference = cached.blocks;
				if (reference.empty())
					throw std::runtime_error("ASTC motion reference unavailable; await independent frame");
			}
			auto stage_start = steady_clock::now();
			auto output = parsed->encoding == nxastc_packet::compression::none ?
			                      std::span<uint8_t>(item->mapped, parsed->raw_bytes) :
			                      std::span<uint8_t>(cpu_scratch);
			if (motion_packet && parsed->reference_frame != UINT64_MAX)
				motion_scratch.resize(size_t(parsed->raw_bytes / 16) * 17);
			const auto decode = motion_packet
			                  ? nxastc_packet::decode_motion_payload(*parsed, payload, reference, output, motion_scratch)
			                  : nxastc_packet::decode_payload(*parsed, payload, output);
			if (decode != nxastc_packet::decode_status::ok)
			{
				spdlog::warn("ASTC payload decode failed frame={} encoding={} raw={} payload={} packet={} reason={}",
				             current.feedback.frame_index,
				             unsigned(parsed->encoding),
				             parsed->raw_bytes,
				             parsed->payload_bytes,
				             current.packet.size(),
				             nxastc_packet::decode_status_message(decode));
				throw std::runtime_error(nxastc_packet::decode_status_message(decode));
			}
			if (parsed->encoding != nxastc_packet::compression::none)
				std::memcpy(item->mapped, cpu_scratch.data(), parsed->raw_bytes);
			if (motion_packet)
			{
				// Rotate reusable CPU buffers; do not copy the decoded frame a second time.
				auto cached = std::min_element(references.begin(), references.end(), [](const auto & a, const auto & b) {
					if (a.blocks.empty() != b.blocks.empty()) return a.blocks.empty();
					return a.frame_index < b.frame_index;
				});
				const bool duplicate = std::any_of(references.begin(), references.end(), [&](const auto & entry) {
					return !entry.blocks.empty() && entry.frame_index == current.feedback.frame_index;
				});
				if (!duplicate && (cached->blocks.empty() || current.feedback.frame_index > cached->frame_index))
				{
					cached->blocks.swap(cpu_scratch);
					cached->frame_index = current.feedback.frame_index;
					cpu_scratch.resize(raw_bytes);
				}
			}
			frame_decode_copy_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now() - stage_start).count();
			if (vmaFlushAllocation(vk_allocator::instance(), static_cast<VmaAllocation>(item->staging), 0, parsed->raw_bytes) != VK_SUCCESS)
				throw std::runtime_error("failed to flush ASTC staging buffer");
			stage_start = steady_clock::now();
			vk::Result waited = device.waitForFences(*fence, true, UINT64_MAX);
			frame_prewait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now() - stage_start).count();
			if (waited != vk::Result::eSuccess)
				throw std::runtime_error("failed waiting for ASTC upload fence");
			if (last_submitted)
			{
				last_submitted->upload_complete = true;
				last_submitted = nullptr;
			}
			cmd.reset();
			cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
			const auto previous = item->layout;
			// Upload and scene rendering submit to the same application queue. This barrier
			// carries the transfer write into later fragment sampling submissions on that queue.
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
			fence_reset = true;
			item->upload_complete = false;
			application::get_queue().lock()->submit(
			        vk::SubmitInfo{
			                .commandBufferCount = 1,
			                .pCommandBuffers = &*cmd,
			        },
			        *fence);
			submitted = true;
			fence_reset = false;
			last_submitted = item;
			stage_start = steady_clock::now();
			if (!async_upload_enabled)
			{
				auto sync_start = steady_clock::now();
				waited = device.waitForFences(*fence, true, UINT64_MAX);
				frame_syncwait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now() - sync_start).count();
				if (waited != vk::Result::eSuccess)
					throw std::runtime_error("failed waiting for ASTC upload fence");
				item->upload_complete = true;
				last_submitted = nullptr;
			}
			// On async uploads, later scene submissions on this same queue are ordered by
			// the transfer-to-fragment image barrier above; no timeline semaphore is needed.
			current.feedback.received_from_decoder = application::get_xr_instance().now();

			auto handle = std::make_shared<astc_blit_handle>(current.feedback,
			                                                current.view_info,
		                                                *item->view,
		                                                item->pixels,
		                                                extent,
		                                                item->layout,
		                                                item->free);
			if (auto scene = weak_scene.lock())
			{
				scene->push_blit_handle(accumulator, std::move(handle));
				frame_handoff_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now() - stage_start).count();
				handed_off = true;
			}

		}
		catch (const std::exception & e)
		{
			if (fence_reset && !submitted)
			{
				// No submission owns the reset fence. Do not let the next frame wait
				// forever on it; stop this worker rather than reuse an invalid upload path.
				stop_worker = true;
				if (item)
				{
					item->upload_complete = true;
					item->free = true;
				}
			}
			if (item && !handed_off)
			{
				if (submitted)
				{
					if (drain_upload())
					{
						item->upload_complete = true;
						item->free = true;
					}
					else
						stop_worker = true;
				}
				else
					item->free = true;
			}
			spdlog::warn("ASTC decoder exception: {}", e.what());
		}
		recycle_packet(current.packet);
		if (handed_off && ++timing_frames == 180)
		{
			decode_copy_ns += frame_decode_copy_ns; prewait_ns += frame_prewait_ns; syncwait_ns += frame_syncwait_ns; handoff_ns += frame_handoff_ns;
			if (queue_timing_enabled)
			{
				uint64_t drops = 0, dwell_ns = 0, max_dwell_ns = 0, dequeues = 0;
				{
					std::lock_guard lock(mutex);
					drops = std::exchange(pending_drop_count, 0);
					dwell_ns = std::exchange(pending_dwell_ns, 0);
					max_dwell_ns = std::exchange(pending_max_dwell_ns, 0);
					dequeues = std::exchange(pending_dequeue_count, 0);
				}
				const double dwell_us = dequeues ? double(dwell_ns) / double(dequeues) / 1000.0 : 0.0;
				spdlog::info("ASTC worker 180-frame mean us/frame: decode+staging-copy {:.1f}, prior-upload fence {:.1f}, sync post-submit fence {:.1f}, host submit-to-handoff {:.1f} ({}; async ends at host handoff, not GPU completion); pending queue since prior summary: mean/max dwell {:.1f}/{:.1f} us over {} dequeues, oldest-pending drops {}",
				             decode_copy_ns / 180000.0, prewait_ns / 180000.0, syncwait_ns / 180000.0, handoff_ns / 180000.0,
				             async_upload_enabled ? "same-queue async" : "sync", dwell_us, double(max_dwell_ns) / 1000.0, dequeues, drops);
			}
			else
				spdlog::info("ASTC worker 180-frame mean us/frame: decode+staging-copy {:.1f}, prior-upload fence {:.1f}, sync post-submit fence {:.1f}, host submit-to-handoff {:.1f} ({}; async ends at host handoff, not GPU completion)", decode_copy_ns / 180000.0, prewait_ns / 180000.0, syncwait_ns / 180000.0, handoff_ns / 180000.0, async_upload_enabled ? "same-queue async" : "sync");
			timing_frames = 0; decode_copy_ns = prewait_ns = syncwait_ns = handoff_ns = 0;
		}
		else if (handed_off)
		{
			decode_copy_ns += frame_decode_copy_ns; prewait_ns += frame_prewait_ns; syncwait_ns += frame_syncwait_ns; handoff_ns += frame_handoff_ns;
		}
		if (stop_worker)
			break;
	}
	drain_upload();
}

std::vector<video_codec> astc_decoder::supported_codecs()
{
	return {video_codec::nxastc};
}
} // namespace wivrn
