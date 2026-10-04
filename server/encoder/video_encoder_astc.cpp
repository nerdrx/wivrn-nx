#include "video_encoder_astc.h"

#include "encoder/encoder_settings.h"
#include "nxastc_packet.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <lz4.h>
#include <stdexcept>

namespace
{
struct push_constants
{
	uint32_t width, height, fit, quality, direct_rgb, block;
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
public:
	void on_feedback(const wivrn::from_headset::feedback &) override {}
	void reset() override {}
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
        direct_rgb_input(settings.options.contains("_wivrn_astc_direct_rgb"))
{
	if (settings.bit_depth != 8 || settings.eyes != 1)
		throw std::runtime_error("NX ASTC requires 8-bit single-eye streams");
	const uint64_t bytes = nxastc_packet::block_bytes(extent.width, extent.height, 4);
	if (bytes > UINT32_MAX)
		throw std::runtime_error("NX ASTC frame is too large");
	U_LOG_I("nxastc: stream %u, %ux%u, ASTC 8x8/6x6/4x4 fit3 adaptive q0-q8, independent LZ4/Zstd packets",
	        unsigned(stream_idx), unsigned(extent.width), unsigned(extent.height));
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

void wivrn::video_encoder_astc::present_image(vk::Image image, vk::SemaphoreSubmitInfo semaphore, uint8_t slot, uint64_t, const to_headset::video_stream_data_shard::view_info_t &)
{
	auto & s = slots.at(slot);
	if (vk.device.waitForFences(*s.fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		s.valid = false;
		U_LOG_E("nxastc: slot fence timeout on stream %d", int(stream_idx));
		return;
	}
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
	s.block = astc_rate_control::block(s.quality);
	push_constants pc{extent.width, extent.height, 3, std::min(s.quality, 6u), direct_rgb_input ? 1u : 0u, s.block};
	cmd.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, vk::ArrayProxy<const push_constants>{pc});
	const uint32_t blocks_x = (extent.width + s.block - 1) / s.block, blocks_y = (extent.height + s.block - 1) / s.block;
	cmd.dispatch((blocks_x * blocks_y + 63) / 64, 1, 1);
	vk::BufferMemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = vk::AccessFlagBits2::eShaderWrite, .dstStageMask = vk::PipelineStageFlagBits2::eTransfer, .dstAccessMask = vk::AccessFlagBits2::eTransferRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = s.blocks, .offset = 0, .size = s.blocks.info().size};
	cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
	cmd.copyBuffer(s.blocks, s.readback, vk::BufferCopy{.size = nxastc_packet::block_bytes(extent.width, extent.height, s.block)});
	vk::BufferMemoryBarrier2 host_barrier{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = vk::PipelineStageFlagBits2::eHost, .dstAccessMask = vk::AccessFlagBits2::eHostRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = s.readback, .offset = 0, .size = s.readback.info().size};
	cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host_barrier});
	cmd.end();
	std::unique_lock lock(vk.queue.mutex);
	semaphore.stageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	vk.device.resetFences(*s.fence);
	vk::CommandBufferSubmitInfo ci{.commandBuffer = *cmd};
	vk.queue.queue.submit2(vk::SubmitInfo2{.waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &semaphore, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci}, *s.fence);
	s.valid = true;
}

std::optional<wivrn::video_encoder::data> wivrn::video_encoder_astc::encode(uint8_t slot, uint64_t)
{
	const auto cpu_begin = std::chrono::steady_clock::now();
	auto & s = slots.at(slot);
	if (vk.device.waitForFences(*s.fence, true, 1'000'000'000) == vk::Result::eTimeout || !s.valid)
		return {};
	vmaInvalidateAllocation(vk_allocator::instance(), s.readback, 0, VK_WHOLE_SIZE);
	const uint32_t raw_size = uint32_t(nxastc_packet::block_bytes(extent.width, extent.height, s.block));
	const auto * raw = s.readback.data<const uint8_t>();
	const auto lz4_begin = std::chrono::steady_clock::now();
	compressed.resize(LZ4_compressBound(raw_size));
	const int n = LZ4_compress_default(reinterpret_cast<const char *>(raw), reinterpret_cast<char *>(compressed.data()), raw_size, compressed.size());
	const bool use_lz4 = n > 0 && uint32_t(n) < raw_size;
	uint32_t payload_size = use_lz4 ? uint32_t(n) : raw_size;
	const uint8_t * payload = use_lz4 ? compressed.data() : raw;
	const auto zstd_begin = std::chrono::steady_clock::now();
	auto encoding = use_lz4 ? nxastc_packet::compression::lz4 : nxastc_packet::compression::none;
	// Keep lossless packing independent of other frames. Reuse the context and
	// output buffer; Zstd must save at least 10% to justify its CPU decode cost.
	if (zstd_context)
	{
		zstd_compressed.resize(ZSTD_compressBound(raw_size));
		const size_t packed = ZSTD_compressCCtx(zstd_context.get(), zstd_compressed.data(), zstd_compressed.size(), raw, raw_size, 3);
		if (!ZSTD_isError(packed) && packed > 0 && packed * 100 <= uint64_t(payload_size) * 90)
		{
			payload_size = uint32_t(packed);
			payload = zstd_compressed.data();
			encoding = nxastc_packet::compression::zstd;
		}
	}
	const auto pack_begin = std::chrono::steady_clock::now();
	auto packet = std::make_shared<std::vector<uint8_t>>(nxastc_packet::header_size + payload_size);
	auto header = nxastc_packet::make_header(extent.width, extent.height, payload_size, encoding, uint8_t(s.block));
	std::memcpy(packet->data(), header.data(), header.size());
	std::memcpy(packet->data() + header.size(), payload, payload_size);
	const auto cpu_end = std::chrono::steady_clock::now();
	sampled_cpu_ms[0] += std::chrono::duration<double, std::milli>(lz4_begin - cpu_begin).count();
	sampled_cpu_ms[1] += std::chrono::duration<double, std::milli>(zstd_begin - lz4_begin).count();
	sampled_cpu_ms[2] += std::chrono::duration<double, std::milli>(pack_begin - zstd_begin).count();
	sampled_cpu_ms[3] += std::chrono::duration<double, std::milli>(cpu_end - pack_begin).count();
	// Choose the next ASTC quality rung from actual bytes; severe overruns can
	// skip unmeasured rungs instead of waiting for the EMA to catch up.
	const uint32_t bitrate = pending_bitrate.load(std::memory_order_relaxed);
	const float live_fps = pending_framerate.load(std::memory_order_relaxed);
	const float fps = live_fps > 0 ? live_fps : initial_fps;
	bool expansion_over_budget = false;
	if (fps > 0 && bitrate > 0)
	{
		const double target_bytes = double(bitrate) / (8.0 * double(fps));
		const double actual_bytes = double(payload_size + nxastc_packet::header_size);
		quality.store(rate_control.update(s.quality, uint32_t(actual_bytes), uint32_t(target_bytes)), std::memory_order_relaxed);
		// A failed footprint probe must not turn into a large network burst.
		expansion_over_budget = s.quality >= 7 && actual_bytes > target_bytes;
	}
	sampled_bytes += payload_size + nxastc_packet::header_size;
	sampled_expansion_drops += expansion_over_budget;
	++sampled_quality[s.quality];
	++sampled_encoding[uint8_t(encoding)];
	if (++sampled_frames == 180)
	{
		U_LOG_I("nxastc: stream %u mean packet %llu bytes, target %.0f bytes; q0-q8 %u/%u/%u/%u/%u/%u/%u/%u/%u, raw/lz4/zstd %u/%u/%u; over-budget expansion drops %u",
		        unsigned(stream_idx), static_cast<unsigned long long>(sampled_bytes / sampled_frames),
		        fps > 0 ? double(bitrate) / (8.0 * fps) : 0.0,
		        sampled_quality[0], sampled_quality[1], sampled_quality[2], sampled_quality[3],
		        sampled_quality[4], sampled_quality[5], sampled_quality[6], sampled_quality[7], sampled_quality[8],
		        sampled_encoding[0], sampled_encoding[1], sampled_encoding[2], sampled_expansion_drops);
		U_LOG_I("nxastc: stream %u CPU ms/frame fence+invalidate %.3f, lz4 %.3f, zstd %.3f, packet %.3f",
		        unsigned(stream_idx), sampled_cpu_ms[0] / sampled_frames, sampled_cpu_ms[1] / sampled_frames,
		        sampled_cpu_ms[2] / sampled_frames, sampled_cpu_ms[3] / sampled_frames);
		sampled_cpu_ms.fill(0);
		sampled_bytes = sampled_frames = sampled_expansion_drops = 0;
		sampled_quality.fill(0);
		sampled_encoding.fill(0);
	}
	if (expansion_over_budget)
		return {};
	return data{.encoder = this, .span = std::span<uint8_t>(*packet), .mem = packet};
}
