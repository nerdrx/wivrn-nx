#include "video_encoder_pyrowave.h"

#include "encoder_settings.h"
#include "idr_handler.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <stdexcept>

namespace
{
class pyrowave_idr_handler : public wivrn::idr_handler
{
public:
	void on_feedback(const wivrn::from_headset::feedback &) override {}
	void reset() override {}
	bool should_skip(uint64_t) override { return false; }
};

vk::raii::CommandPool make_command_pool(wivrn::vk_bundle & vk, uint8_t)
{
	return vk::raii::CommandPool(vk.device, vk::CommandPoolCreateInfo{
		.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
		.queueFamilyIndex = vk.queue.family_index,
	});
}

buffer_allocation make_buffer(wivrn::vk_bundle & vk, vk::DeviceSize size, const char * name)
{
	return buffer_allocation(vk.device,
		{.size = size, .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc},
		{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT,
		 .usage = VMA_MEMORY_USAGE_AUTO},
		name);
}

buffer_allocation make_staging(wivrn::vk_bundle & vk, vk::DeviceSize size, const char * name)
{
	return buffer_allocation(vk.device,
		{.size = size, .usage = vk::BufferUsageFlagBits::eTransferDst},
		{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage = VMA_MEMORY_USAGE_AUTO},
		name);
}
} // namespace

wivrn::video_encoder_pyrowave::video_encoder_pyrowave(
		vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx) :
	video_encoder(vk, stream_idx, vk.queue.family_index, settings, std::make_unique<pyrowave_idr_handler>(), true),
	vk(vk),
	cmd_pool(make_command_pool(vk, stream_idx)),
	encoder(vk.physical_device, vk.device, extent.width, extent.height, PyroWave::ChromaSubsampling::Chroma420),
	target_size(std::clamp<size_t>(size_t(settings.bitrate / std::max(settings.fps, 1.f) / 8), 1, max_target_size)),
	current_fps(settings.fps)
{
	if (settings.bit_depth != 8 || settings.eyes != 1)
		throw std::runtime_error("PyroWave requires 8-bit single-eye streams");
	auto cmds = vk.device.allocateCommandBuffers({.commandPool = *cmd_pool, .commandBufferCount = num_slots});
	const auto meta_size = encoder.get_meta_required_size();
	const auto bitstream_size = max_target_size + 2 * meta_size;
	for (size_t i = 0; i < num_slots; ++i)
	{
		auto & s = slots[i];
		s.cmd = std::move(cmds[i]);
		s.fence = vk::raii::Fence(vk.device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
		s.meta = make_buffer(vk, meta_size, "pyrowave meta buffer");
		s.bitstream = make_buffer(vk, bitstream_size, "pyrowave bitstream buffer");
		if (!(s.meta.properties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			s.meta_staging = make_staging(vk, meta_size, "pyrowave meta staging buffer");
		if (!(s.bitstream.properties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			s.bitstream_staging = make_staging(vk, bitstream_size, "pyrowave bitstream staging buffer");
	}
}

wivrn::video_encoder_pyrowave::~video_encoder_pyrowave()
{
	for (auto & s : slots)
		if (*s.fence)
			(void)vk.device.waitForFences(*s.fence, true, 1'000'000'000);
}

void wivrn::video_encoder_pyrowave::present_image(
		vk::Image image, vk::SemaphoreSubmitInfo semaphore, uint8_t slot, uint64_t,
		const to_headset::video_stream_data_shard::view_info_t &)
{
	auto & s = slots[slot];
	if (const float fps = pending_framerate.load(); fps > 0)
		current_fps = fps;
	if (const uint32_t bps = pending_bitrate.load(); bps > 0)
		target_size = std::clamp<size_t>(size_t(double(bps) / std::max(current_fps, 1.f) / 8), 1, max_target_size);
	if (vk.device.waitForFences(*s.fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_E("pyrowave: timeout on stream %d", int(stream_idx));
		s.valid = false;
		return;
	}
	std::array<vk::ImageView, 3> views;
	if (auto it = image_views.find(VkImage(image)); it != image_views.end())
	{
		for (size_t i = 0; i < views.size(); ++i)
			views[i] = *it->second[i];
	}
	else
	{
		auto y = vk.device.createImageView({.image = image, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8Unorm,
			.subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::ePlane0, .levelCount = 1, .baseArrayLayer = src_layer, .layerCount = 1}});
		auto cb = vk.device.createImageView({.image = image, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8Unorm,
			.subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::ePlane1, .levelCount = 1, .baseArrayLayer = src_layer, .layerCount = 1}});
		auto cr = vk.device.createImageView({.image = image, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8Unorm,
			.components = {.r = vk::ComponentSwizzle::eG},
			.subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::ePlane1, .levelCount = 1, .baseArrayLayer = src_layer, .layerCount = 1}});
		views = {*y, *cb, *cr};
		image_views.emplace(VkImage(image), std::array{std::move(y), std::move(cb), std::move(cr)});
	}
	PyroWave::Encoder::BitstreamBuffers buffers{
		.meta = {.buffer = s.meta, .size = s.meta.info().size},
		.bitstream = {.buffer = s.bitstream, .size = s.bitstream.info().size},
		.target_size = target_size,
	};
	auto & cmd = s.cmd;
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	s.valid = encoder.encode(cmd, views, buffers);
	if (s.meta_staging)
		cmd.copyBuffer(s.meta, s.meta_staging, vk::BufferCopy{.size = buffers.meta.size});
	if (s.bitstream_staging)
		cmd.copyBuffer(s.bitstream, s.bitstream_staging, vk::BufferCopy{.size = buffers.bitstream.size});
	cmd.end();
	std::unique_lock lock(vk.queue.mutex);
	semaphore.stageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	vk.device.resetFences(*s.fence);
	vk::CommandBufferSubmitInfo cmd_info{.commandBuffer = *cmd};
	vk.queue.queue.submit2(vk::SubmitInfo2{.waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &semaphore,
		.commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_info}, *s.fence);
}

std::optional<wivrn::video_encoder::data> wivrn::video_encoder_pyrowave::encode(uint8_t slot, uint64_t)
{
	auto & s = slots[slot];
	if (vk.device.waitForFences(*s.fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_W("pyrowave: timeout on stream %d", int(stream_idx));
		return {};
	}
	if (!s.valid)
		return {};
	// The GPU may have written non-coherent host-visible memory, including the
	// transfer staging buffers. The fence only establishes completion.
	auto & readable_meta = s.meta_staging ? s.meta_staging : s.meta;
	auto & readable_bitstream = s.bitstream_staging ? s.bitstream_staging : s.bitstream;
	vmaInvalidateAllocation(vk_allocator::instance(), readable_meta, 0, VK_WHOLE_SIZE);
	vmaInvalidateAllocation(vk_allocator::instance(), readable_bitstream, 0, VK_WHOLE_SIZE);
	const void * meta = readable_meta.map();
	const void * bitstream = readable_bitstream.map();
	const size_t count = encoder.compute_num_packets(meta, 8 * 1024);
	packets.resize(count);
	packet_buffer.resize(8 * 1024 * 1024);
	const size_t written = encoder.packetize(packets.data(), 8 * 1024, packet_buffer.data(), packet_buffer.size(), meta, bitstream);
	if (!written || written > packets.size())
		return {};
	const auto & last = packets[written - 1];
	return data{.encoder = this, .span = std::span<uint8_t>(packet_buffer.data(), last.offset + last.size)};
}
