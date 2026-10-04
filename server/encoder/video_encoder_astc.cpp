#include "video_encoder_astc.h"
#include "astc_gpu_timing.h"
#include "nxastc_compact.h"

#include "encoder/encoder_settings.h"
#include "nxastc_motion.h"
#include "nxastc_packet.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <lz4.h>
#include <stdexcept>
#include <string_view>

namespace
{
struct push_constants
{
	uint32_t width, height, fit, quality, direct_rgb;
};

vk::raii::CommandPool make_command_pool(wivrn::vk_bundle & vk)
{
	return vk::raii::CommandPool(vk.device, vk::CommandPoolCreateInfo{
	                                                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
	                                                .queueFamilyIndex = vk.queue.family_index,
	                                        });
}

vk::raii::Sampler make_sampler(wivrn::vk_bundle & vk)
{
	return vk::raii::Sampler(vk.device, vk::SamplerCreateInfo{
	                                            .magFilter = vk::Filter::eNearest,
	                                            .minFilter = vk::Filter::eNearest,
	                                            .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                                            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                                            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                                            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	                                    });
}

vk::raii::DescriptorSetLayout make_ds_layout(wivrn::vk_bundle & vk)
{
	std::array bindings{
	        vk::DescriptorSetLayoutBinding{.binding = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
	        vk::DescriptorSetLayoutBinding{.binding = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
	        vk::DescriptorSetLayoutBinding{.binding = 2, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
	};
	return vk::raii::DescriptorSetLayout(vk.device, vk::DescriptorSetLayoutCreateInfo{.bindingCount = uint32_t(bindings.size()), .pBindings = bindings.data()});
}

vk::raii::PipelineLayout make_pipeline_layout(wivrn::vk_bundle & vk, vk::DescriptorSetLayout ds)
{
	vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(push_constants)};
	return vk::raii::PipelineLayout(vk.device, vk::PipelineLayoutCreateInfo{.setLayoutCount = 1, .pSetLayouts = &ds, .pushConstantRangeCount = 1, .pPushConstantRanges = &range});
}

vk::raii::Pipeline make_pipeline(wivrn::vk_bundle & vk, vk::PipelineLayout layout)
{
	auto shader = vk.load_shader("astc_encode");
	return vk::raii::Pipeline(vk.device, nullptr, vk::ComputePipelineCreateInfo{.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *shader, .pName = "main"}, .layout = layout});
}

vk::raii::DescriptorPool make_ds_pool(wivrn::vk_bundle & vk)
{
	std::array sizes{
	        vk::DescriptorPoolSize{.type = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 2 * wivrn::video_encoder::num_slots},
	        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBuffer, .descriptorCount = wivrn::video_encoder::num_slots},
	};
	return vk::raii::DescriptorPool(vk.device, vk::DescriptorPoolCreateInfo{.maxSets = wivrn::video_encoder::num_slots, .poolSizeCount = uint32_t(sizes.size()), .pPoolSizes = sizes.data()});
}

buffer_allocation make_blocks(wivrn::vk_bundle & vk, vk::DeviceSize size)
{
	return buffer_allocation(vk.device,
	                         {.size = size, .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc},
	                         {.usage = VMA_MEMORY_USAGE_AUTO},
	                         "nxastc block buffer");
}

buffer_allocation make_readback(wivrn::vk_bundle & vk, vk::DeviceSize size)
{
	return buffer_allocation(vk.device,
	                         {.size = size, .usage = vk::BufferUsageFlagBits::eTransferDst},
	                         {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST, .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT},
	                         "nxastc cached readback");
}

class astc_idr_handler : public wivrn::idr_handler
{
	wivrn::nxastc_packet::motion_decode_ack newest_decoded;

public:
	uint64_t newest_decoded_frame() const
	{
		return newest_decoded.frame();
	}
	void on_feedback(const wivrn::from_headset::feedback & feedback) override
	{
		newest_decoded.observe(feedback.frame_index, feedback.received_from_decoder != 0);
	}
	void reset() override { newest_decoded.reset(); }
	bool should_skip(uint64_t) override
	{
		return false;
	}
};
} // namespace

wivrn::video_encoder_astc::video_encoder_astc(vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx) :
        video_encoder(vk, stream_idx, vk.queue.family_index, settings, std::make_unique<astc_idr_handler>(), true),
        vk(vk),
        cmd_pool(make_command_pool(vk)),
        sampler(make_sampler(vk)),
        ds_layout(make_ds_layout(vk)),
        pipeline_layout(make_pipeline_layout(vk, *ds_layout)),
        pipeline(make_pipeline(vk, *pipeline_layout)),
        ds_pool(make_ds_pool(vk)),
        initial_fps(settings.fps),
        direct_rgb_input(settings.options.contains("_wivrn_astc_direct_rgb")),
        motion_delta_enabled(settings.options.contains("_wivrn_astc_motion_delta") &&
                             settings.options.at("_wivrn_astc_motion_delta") == "1"),
        compact_enabled(settings.options.contains("_wivrn_astc_compact") &&
                        settings.options.at("_wivrn_astc_compact") == "1"),
        independent_zstd_level(!motion_delta_enabled &&
                               settings.options.contains("_wivrn_astc_fast_zstd") &&
                               settings.options.at("_wivrn_astc_fast_zstd") == "1" ? 1 : 3)
{
	if (settings.bit_depth != 8 || settings.eyes != 1)
		throw std::runtime_error("NX ASTC requires 8-bit single-eye streams");
	const uint64_t bytes = nxastc_packet::block_bytes(extent.width, extent.height);
	if (bytes > UINT32_MAX)
		throw std::runtime_error("NX ASTC frame is too large");
	U_LOG_I("nxastc: stream %u, %ux%u, ASTC 8x8 fit3 adaptive q0-q6, independent LZ4/Zstd packets",
	        unsigned(stream_idx), unsigned(extent.width), unsigned(extent.height));
	if (motion_delta_enabled)
		U_LOG_I("nxastc: stream %u motion deltas enabled, ACK references up to 8 frames old", unsigned(stream_idx));
	if (compact_enabled && !motion_delta_enabled)
		U_LOG_I("nxastc: stream %u independent compact ASTC packing enabled (requires v4 client)", unsigned(stream_idx));
	U_LOG_I("nxastc: stream %u independent Zstd level %d", unsigned(stream_idx), independent_zstd_level);
	if (const char * option = std::getenv("WIVRN_NX_ASTC_GPU_TIMING"); option and std::string_view{option} == "1")
	{
		try
		{
			auto queue_properties = vk.physical_device.getQueueFamilyProperties();
			const double period_ns = vk.physical_device.getProperties().limits.timestampPeriod;
			const uint32_t valid_bits = vk.queue.family_index < queue_properties.size()
			                                    ? queue_properties[vk.queue.family_index].timestampValidBits
			                                    : 0;
			if (vk.queue.family_index < queue_properties.size() &&
			    valid_bits != 0 && valid_bits <= 64 &&
			    period_ns > 0 && std::isfinite(period_ns))
			{
				gpu_timing_queries = vk::raii::QueryPool(vk.device, vk::QueryPoolCreateInfo{
				                                                            .queryType = vk::QueryType::eTimestamp,
				                                                            .queryCount = 2 * num_slots,
				                                                    });
				gpu_timestamp_valid_bits = valid_bits;
				gpu_timestamp_period_ns = period_ns;
			}
		}
		catch (const std::exception &)
		{
			// Timing is diagnostic only: a missing query pool must not disable encoding.
		}
		if (not *gpu_timing_queries)
			U_LOG_W("nxastc: GPU timing unavailable on stream %u; encoding continues without samples", unsigned(stream_idx));
	}
	auto cmds = vk.device.allocateCommandBuffers({.commandPool = *cmd_pool, .commandBufferCount = num_slots});
	std::array layouts{*ds_layout, *ds_layout};
	auto sets = vk.device.allocateDescriptorSets({.descriptorPool = *ds_pool, .descriptorSetCount = num_slots, .pSetLayouts = layouts.data()});
	for (size_t i = 0; i < num_slots; ++i)
	{
		auto & s = slots[i];
		s.cmd = std::move(cmds[i]);
		s.descriptor_set = sets[i].release();
		s.fence = vk::raii::Fence(vk.device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
		s.blocks = make_blocks(vk, bytes);
		s.readback = make_readback(vk, bytes);
	}
}

wivrn::video_encoder_astc::~video_encoder_astc()
{
	for (auto & s: slots)
		if (*s.fence)
			(void)vk.device.waitForFences(*s.fence, true, 1'000'000'000);
}

void wivrn::video_encoder_astc::reset()
{
	if (!motion_delta_enabled)
	{
		video_encoder::reset();
		return;
	}
	std::lock_guard lock(motion_mutex);
	video_encoder::reset();
	for (auto & reference: motion_references)
		reference.valid = false;
	motion_reference_next = 0;
	motion_probe_cooldown = 0;
}

void wivrn::video_encoder_astc::present_image(vk::Image image, vk::SemaphoreSubmitInfo semaphore, uint8_t slot, uint64_t, const to_headset::video_stream_data_shard::view_info_t &)
{
	auto & s = slots.at(slot);
	if (vk.device.waitForFences(*s.fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		s.valid = false;
		U_LOG_E("nxastc: slot fence timeout on stream %d", int(stream_idx));
		return;
	}
	// Invalidate the previous slot generation before recording or submitting this one.
	s.valid = false;
	auto it = image_views.find(VkImage(image));
	if (it == image_views.end())
	{
		const auto format = direct_rgb_input ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8Unorm;
		const auto aspect = direct_rgb_input ? vk::ImageAspectFlagBits::eColor : vk::ImageAspectFlagBits::ePlane0;
		auto y = vk.device.createImageView({.image = image, .viewType = vk::ImageViewType::e2D, .format = format, .subresourceRange = {.aspectMask = aspect, .levelCount = 1, .baseArrayLayer = src_layer, .layerCount = 1}});
		auto uv = direct_rgb_input
		                  ? vk.device.createImageView({.image = image, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8B8A8Unorm, .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .baseArrayLayer = src_layer, .layerCount = 1}})
		                  : vk.device.createImageView({.image = image, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8Unorm, .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::ePlane1, .levelCount = 1, .baseArrayLayer = src_layer, .layerCount = 1}});
		it = image_views.emplace(VkImage(image), std::array{std::move(y), std::move(uv)}).first;
	}
	std::array images{
	        vk::DescriptorImageInfo{.sampler = *sampler, .imageView = *it->second[0], .imageLayout = vk::ImageLayout::eGeneral},
	        vk::DescriptorImageInfo{.sampler = *sampler, .imageView = *it->second[1], .imageLayout = vk::ImageLayout::eGeneral},
	};
	vk::DescriptorBufferInfo out{.buffer = s.blocks, .offset = 0, .range = s.blocks.info().size};
	std::array writes{
	        vk::WriteDescriptorSet{.dstSet = s.descriptor_set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &images[0]},
	        vk::WriteDescriptorSet{.dstSet = s.descriptor_set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &images[1]},
	        vk::WriteDescriptorSet{.dstSet = s.descriptor_set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &out},
	};
	vk.device.updateDescriptorSets(writes, {});
	auto & cmd = s.cmd;
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, s.descriptor_set, {});
	s.quality = quality.load(std::memory_order_relaxed);
	push_constants pc{extent.width, extent.height, 3, s.quality, direct_rgb_input ? 1u : 0u};
	cmd.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, vk::ArrayProxy<const push_constants>{pc});
	const uint32_t blocks_x = (extent.width + 7) / 8, blocks_y = (extent.height + 7) / 8;
	if (*gpu_timing_queries)
	{
		const uint32_t first_query = 2 * slot;
		cmd.resetQueryPool(*gpu_timing_queries, first_query, 2);
		// The submit's semaphore wait includes ComputeShader, so input is ready here.
		cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eComputeShader, *gpu_timing_queries, first_query);
	}
	cmd.dispatch((blocks_x * blocks_y + 63) / 64, 1, 1);
	vk::BufferMemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = vk::AccessFlagBits2::eShaderWrite, .dstStageMask = vk::PipelineStageFlagBits2::eTransfer, .dstAccessMask = vk::AccessFlagBits2::eTransferRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = s.blocks, .offset = 0, .size = s.blocks.info().size};
	cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
	cmd.copyBuffer(s.blocks, s.readback, vk::BufferCopy{.size = s.blocks.info().size});
	vk::BufferMemoryBarrier2 host_barrier{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = vk::PipelineStageFlagBits2::eHost, .dstAccessMask = vk::AccessFlagBits2::eHostRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = s.readback, .offset = 0, .size = s.readback.info().size};
	cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host_barrier});
	if (*gpu_timing_queries)
		// End after readback transfer; this is compute-through-readback, not kernel-only time.
		cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, *gpu_timing_queries, 2 * slot + 1);
	cmd.end();
	std::unique_lock lock(vk.queue.mutex);
	semaphore.stageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	vk.device.resetFences(*s.fence);
	vk::CommandBufferSubmitInfo ci{.commandBuffer = *cmd};
	vk.queue.queue.submit2(vk::SubmitInfo2{.waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &semaphore, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci}, *s.fence);
	s.valid = true;
}

std::optional<wivrn::video_encoder::data> wivrn::video_encoder_astc::encode(uint8_t slot, uint64_t frame_index)
{
	const auto cpu_begin = std::chrono::steady_clock::now();
	auto & s = slots.at(slot);
	if (vk.device.waitForFences(*s.fence, true, 1'000'000'000) == vk::Result::eTimeout || !s.valid)
		return {};
	std::optional<double> gpu_elapsed_ms;
	if (*gpu_timing_queries)
	{
		try
		{
			auto [result, timestamps] = gpu_timing_queries.getResult<std::array<uint64_t, 2>>(
			        2 * slot, 2, sizeof(uint64_t), vk::QueryResultFlagBits::e64);
			if (result == vk::Result::eSuccess)
				gpu_elapsed_ms = astc_gpu_timing::elapsed_ms(
				        timestamps[0], timestamps[1], gpu_timestamp_valid_bits, gpu_timestamp_period_ns);
		}
		catch (const std::exception &)
		{
			// Ignore diagnostic readback errors; the already-completed ASTC image remains usable.
		}
	}
	vmaInvalidateAllocation(vk_allocator::instance(), s.readback, 0, VK_WHOLE_SIZE);
	const auto fence_invalidate_end = std::chrono::steady_clock::now();
	const uint32_t raw_size = uint32_t(nxastc_packet::block_bytes(extent.width, extent.height));
	const auto * raw = s.readback.data<const uint8_t>();
	double lz4_ms = 0, zstd_ms = 0;
	int lz4_size = 0;
	size_t zstd_size = 0;
	uint64_t reference_frame = nxastc_packet::independent_frame;
	size_t packet_header_size = nxastc_packet::header_size;
	double motion_candidate_ms = 0;
	bool motion_candidate_won = false, motion_reference_missing = false;
	double compact_ms = 0;
	bool compact_won = false;
	bool compact_input = false;
	if (compact_enabled && !motion_delta_enabled && zstd_context)
	{
		const auto begin = std::chrono::steady_clock::now();
		compact_blocks.resize(size_t(raw_size / 16) * 14);
		compact_input = nxastc_packet::compact_blocks(std::span<const uint8_t>(raw, raw_size), compact_blocks);
		compact_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
	}
	auto run_lz4 = [&] {
		const auto begin = std::chrono::steady_clock::now();
		compressed.resize(LZ4_compressBound(raw_size));
		lz4_size = LZ4_compress_default(reinterpret_cast<const char *>(raw), reinterpret_cast<char *>(compressed.data()), raw_size, compressed.size());
		lz4_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
	};
	auto run_zstd = [&] {
		const auto begin = std::chrono::steady_clock::now();
		zstd_compressed.resize(ZSTD_compressBound(raw_size));
		// One Zstd attempt per frame: probing both byte layouts would add another
		// native compression call to the PC critical path for a small wire saving.
		zstd_size = ZSTD_compressCCtx(zstd_context.get(), zstd_compressed.data(), zstd_compressed.size(),
		                             compact_input ? compact_blocks.data() : raw,
		                             compact_input ? compact_blocks.size() : raw_size, independent_zstd_level);
		zstd_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
	};
	uint32_t payload_size = raw_size;
	const uint8_t * payload = raw;
	auto encoding = nxastc_packet::compression::none;
	std::unique_lock motion_lock(motion_mutex, std::defer_lock);
	if (motion_delta_enabled)
	{
		motion_lock.lock();
		auto * handler = dynamic_cast<astc_idr_handler *>(idr.get());
		const uint64_t ack = handler ? handler->newest_decoded_frame() : nxastc_packet::independent_frame;
		motion_reference * reference = nullptr;
		if (nxastc_packet::motion_reference_usable(frame_index, ack))
			for (auto & candidate: motion_references)
				if (candidate.valid && candidate.frame_index == ack)
				{
					reference = &candidate;
					break;
				}
		motion_reference_missing = reference == nullptr;

		// Start with an independent anchor; use raw when Zstd cannot shrink it.
		if (zstd_context)
			run_zstd();
		const bool anchor_zstd = !ZSTD_isError(zstd_size) && zstd_size > 0 && zstd_size < raw_size;
		const uint32_t anchor_size = anchor_zstd ? uint32_t(zstd_size) : raw_size;
		const uint8_t * anchor_payload = anchor_zstd ? zstd_compressed.data() : raw;
		payload_size = anchor_size;
		payload = anchor_payload;
		encoding = anchor_zstd ? nxastc_packet::compression::motion_zstd : nxastc_packet::compression::motion_raw;
		packet_header_size = nxastc_packet::motion_header_size;

		const bool probe_motion = motion_probe_cooldown == 0;
		if (motion_probe_cooldown) --motion_probe_cooldown;
		if (reference && zstd_context && probe_motion)
		{
			const auto candidate_begin = std::chrono::steady_clock::now();
			const uint32_t blocks_x = (extent.width + 7) / 8;
			const uint32_t blocks_y = (extent.height + 7) / 8;
			motion_scratch.resize(size_t(raw_size) + raw_size / 16);
			if (nxastc_packet::encode_motion_blocks(blocks_x, blocks_y,
			                                        std::span<const uint8_t>(reference->blocks),
			                                        std::span<const uint8_t>(raw, raw_size),
			                                        std::span<uint8_t>(motion_scratch)))
			{
				motion_zstd_compressed.resize(ZSTD_compressBound(motion_scratch.size()));
				const size_t candidate_size = ZSTD_compressCCtx(zstd_context.get(), motion_zstd_compressed.data(),
				                                                motion_zstd_compressed.size(), motion_scratch.data(),
				                                                motion_scratch.size(), 3);
				if (!ZSTD_isError(candidate_size) && candidate_size > 0 &&
				    uint64_t(candidate_size) * 100 <= uint64_t(anchor_size) * 85)
				{
					payload_size = uint32_t(candidate_size);
					payload = motion_zstd_compressed.data();
					encoding = nxastc_packet::compression::motion_zstd;
					reference_frame = ack;
					motion_candidate_won = true;
				}
			}
			motion_candidate_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - candidate_begin).count();
			// Avoid paying for a losing representation on every frame. Anchors continue immediately.
			if (!motion_candidate_won) motion_probe_cooldown = 3;
		}

		auto & saved = motion_references[motion_reference_next];
		saved.blocks.assign(raw, raw + raw_size);
		saved.frame_index = frame_index;
		saved.valid = true;
		motion_reference_next = (motion_reference_next + 1) % motion_references.size();
	}
	else
	{
		// Legacy selection is unchanged when both packing experiments are off.
		const bool prefer_zstd = s.quality >= 4 && bool(zstd_context);
		if (prefer_zstd)
			run_zstd();
		if (!prefer_zstd || ZSTD_isError(zstd_size) || zstd_size == 0 || zstd_size > raw_size / 2)
			run_lz4();
		const bool use_lz4 = lz4_size > 0 && uint32_t(lz4_size) < raw_size;
		payload_size = use_lz4 ? uint32_t(lz4_size) : raw_size;
		payload = use_lz4 ? compressed.data() : raw;
		encoding = use_lz4 ? nxastc_packet::compression::lz4 : nxastc_packet::compression::none;
		if (zstd_context)
		{
			if (!prefer_zstd)
				run_zstd();
			if (!ZSTD_isError(zstd_size) && zstd_size > 0 && zstd_size * 100 <= uint64_t(payload_size) * 90 &&
			    (!compact_input || zstd_size <= compact_blocks.size()))
			{
				payload_size = uint32_t(zstd_size);
				payload = zstd_compressed.data();
				encoding = compact_input ? nxastc_packet::compression::compact_zstd : nxastc_packet::compression::zstd;
				compact_won = compact_input;
			}
		}
	}
	const auto pack_begin = std::chrono::steady_clock::now();
	auto packet = std::make_shared<std::vector<uint8_t>>(packet_header_size + payload_size);
	if (motion_delta_enabled)
	{
		auto header = nxastc_packet::make_motion_header(extent.width, extent.height, payload_size, encoding, reference_frame);
		std::memcpy(packet->data(), header.data(), header.size());
	}
	else
	{
		auto header = nxastc_packet::make_header(extent.width, extent.height, payload_size, encoding);
		std::memcpy(packet->data(), header.data(), header.size());
	}
	std::memcpy(packet->data() + packet_header_size, payload, payload_size);
	const auto cpu_end = std::chrono::steady_clock::now();
	sampled_cpu_ms[0] += std::chrono::duration<double, std::milli>(fence_invalidate_end - cpu_begin).count();
	sampled_cpu_ms[1] += lz4_ms;
	sampled_cpu_ms[2] += zstd_ms;
	sampled_cpu_ms[3] += std::chrono::duration<double, std::milli>(cpu_end - pack_begin).count();
	// Choose the next ASTC quality rung from actual bytes; severe overruns can
	// skip unmeasured rungs instead of waiting for the EMA to catch up.
	const uint32_t bitrate = pending_bitrate.load(std::memory_order_relaxed);
	const float live_fps = pending_framerate.load(std::memory_order_relaxed);
	const float fps = live_fps > 0 ? live_fps : initial_fps;
	if (fps > 0 && bitrate > 0)
	{
		const double target_bytes = double(bitrate) / (8.0 * double(fps));
		const double actual_bytes = double(payload_size + packet_header_size);
		quality.store(rate_control.update(s.quality, uint32_t(actual_bytes), uint32_t(target_bytes)), std::memory_order_relaxed);
	}
	sampled_bytes += payload_size + packet_header_size;
	++sampled_quality[s.quality];
	if (motion_delta_enabled)
	{
		sampled_motion_candidate_ms += motion_candidate_ms;
		sampled_worker_ms += std::chrono::duration<double, std::milli>(cpu_end - cpu_begin).count();
		sampled_motion_wins += motion_candidate_won;
		sampled_motion_missing_ref += motion_reference_missing;
	}
	else if (compact_won)
		++sampled_compact_frames;
	else
		++sampled_encoding[uint8_t(encoding)];
	sampled_compact_ms += compact_ms;
	if (gpu_elapsed_ms && sampled_gpu_count < sampled_gpu_ms.size())
		sampled_gpu_ms[sampled_gpu_count++] = *gpu_elapsed_ms;
	if (++sampled_frames == 180)
	{
		if (motion_delta_enabled)
			U_LOG_I("nxastc: stream %u motion mean packet %llu bytes; delta wins %u/180, missing ACK reference %u/180, candidate CPU %.3f ms/frame, worker total %.3f ms/frame",
			        unsigned(stream_idx), static_cast<unsigned long long>(sampled_bytes / sampled_frames),
			        sampled_motion_wins, sampled_motion_missing_ref, sampled_motion_candidate_ms / sampled_frames,
			        sampled_worker_ms / sampled_frames);
		else
			U_LOG_I("nxastc: stream %u mean packet %llu bytes, target %.0f bytes; q0-q6 %u/%u/%u/%u/%u/%u/%u, raw/lz4/zstd %u/%u/%u",
			        unsigned(stream_idx), static_cast<unsigned long long>(sampled_bytes / sampled_frames),
			        fps > 0 ? double(bitrate) / (8.0 * fps) : 0.0,
			        sampled_quality[0], sampled_quality[1], sampled_quality[2], sampled_quality[3],
			        sampled_quality[4], sampled_quality[5], sampled_quality[6],
			        sampled_encoding[0], sampled_encoding[1], sampled_encoding[2]);
		U_LOG_I("nxastc: stream %u CPU ms/frame fence+invalidate %.3f, lz4 %.3f, zstd %.3f, packet %.3f",
		        unsigned(stream_idx), sampled_cpu_ms[0] / sampled_frames, sampled_cpu_ms[1] / sampled_frames,
		        sampled_cpu_ms[2] / sampled_frames, sampled_cpu_ms[3] / sampled_frames);
		if (sampled_gpu_count)
		{
			auto sorted = sampled_gpu_ms;
			std::sort(sorted.begin(), sorted.begin() + sampled_gpu_count);
			double total = 0;
			for (uint32_t i = 0; i < sampled_gpu_count; ++i)
				total += sorted[i] / sampled_gpu_count;
			const uint32_t p50 = (sampled_gpu_count - 1) / 2;
			const uint32_t p95 = (sampled_gpu_count * 95 + 99) / 100 - 1;
			U_LOG_I("nxastc: stream %u GPU compute-through-readback ms, n=%u mean=%.3f p50=%.3f p95=%.3f",
			        unsigned(stream_idx),
			        sampled_gpu_count,
			        total,
			        sorted[p50],
			        sorted[p95]);
		}
		else if (*gpu_timing_queries)
			U_LOG_I("nxastc: stream %u GPU compute-through-readback ms, n=0 (no valid samples)", unsigned(stream_idx));
		if (compact_enabled && !motion_delta_enabled)
			U_LOG_I("nxastc: stream %u compact packets %u/180, input packing CPU %.3f ms/frame",
			        unsigned(stream_idx), sampled_compact_frames, sampled_compact_ms / sampled_frames);
		sampled_cpu_ms.fill(0);
		sampled_gpu_count = 0;
		sampled_bytes = sampled_frames = 0;
		sampled_quality.fill(0);
		sampled_encoding.fill(0);
		sampled_compact_frames = 0;
		sampled_compact_ms = 0;
		sampled_motion_candidate_ms = 0;
		sampled_worker_ms = 0;
		sampled_motion_wins = sampled_motion_missing_ref = 0;
	}
	return data{.encoder = this, .span = std::span<uint8_t>(*packet), .mem = packet};
}
