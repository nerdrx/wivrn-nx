/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
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

#include "raw_dump.h"

#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <format>
#include <mutex>

namespace
{
// Bytes every stream of this process has put on disk, against WIVRN_RAW_DUMP_MAX_MB.
// A per-stream cap alone would not bound a four stream session.
std::atomic<uint64_t> total_bytes = 0;
uint64_t byte_budget = 0;

uint64_t env_u64(const char * name, uint64_t fallback)
{
	const char * v = std::getenv(name);
	if (not v or not *v)
		return fallback;
	char * end = nullptr;
	unsigned long long parsed = std::strtoull(v, &end, 10);
	if (end == v or parsed == 0)
	{
		U_LOG_W("raw dump: ignoring %s=\"%s\"", name, v);
		return fallback;
	}
	return parsed;
}

// The two axes of a foveation_parameter as a JSON array. Run lengths, a handful of
// entries each: the harness needs them to map an encoded pixel back to a render one.
void write_u16_array(std::FILE * f, const std::vector<uint16_t> & v)
{
	std::fputc('[', f);
	for (size_t i = 0; i < v.size(); ++i)
		std::fprintf(f, "%s%u", i ? "," : "", unsigned(v[i]));
	std::fputc(']', f);
}

void write_pose(std::FILE * f, const XrPosef & p)
{
	std::fprintf(f,
	             "{\"orientation\":[%.9g,%.9g,%.9g,%.9g],\"position\":[%.9g,%.9g,%.9g]}",
	             p.orientation.x,
	             p.orientation.y,
	             p.orientation.z,
	             p.orientation.w,
	             p.position.x,
	             p.position.y,
	             p.position.z);
}

// Tangents rather than angles: every projection the harness builds wants them, and the
// half angles are recoverable from them exactly.
void write_fov(std::FILE * f, const XrFovf & fov)
{
	std::fprintf(f,
	             "{\"tan_left\":%.9g,\"tan_right\":%.9g,\"tan_up\":%.9g,\"tan_down\":%.9g}",
	             std::tan(fov.angleLeft),
	             std::tan(fov.angleRight),
	             std::tan(fov.angleUp),
	             std::tan(fov.angleDown));
}
} // namespace

std::unique_ptr<wivrn::raw_dump> wivrn::raw_dump::create(
        vk_bundle & vk,
        uint8_t stream_idx,
        vk::Extent2D extent,
        uint32_t src_layer,
        vk::ImageLayout layout,
        int bit_depth,
        uint32_t queue_family)
{
	const char * dir = std::getenv("WIVRN_RAW_DUMP");
	if (not dir or not *dir)
		return nullptr;

	// Only the eyes. The alpha stream is a luma-only mask and the quad stream is a UI
	// panel at its own resolution; neither is video the codec work needs to measure.
	if (stream_idx >= 2)
		return nullptr;

	if (layout != vk::ImageLayout::eGeneral)
	{
		U_LOG_W("raw dump: stream %d presents in %s, only eGeneral can be copied; not dumping",
		        int(stream_idx),
		        vk::to_string(layout).c_str());
		return nullptr;
	}

	if (queue_family != vk.queue.family_index and
	    not(vk.transfer_queue and queue_family == vk.transfer_queue.family_index))
	{
		U_LOG_W("raw dump: stream %d is read on queue family %u, which the tap has no queue for; not dumping",
		        int(stream_idx),
		        unsigned(queue_family));
		return nullptr;
	}

	if (bit_depth != 8 and bit_depth != 10)
	{
		U_LOG_W("raw dump: unsupported bit depth %d", bit_depth);
		return nullptr;
	}

	try
	{
		return std::unique_ptr<raw_dump>(new raw_dump(vk, stream_idx, extent, src_layer, layout, bit_depth, queue_family, dir));
	}
	catch (const std::exception & e)
	{
		U_LOG_W("raw dump: stream %d not dumping: %s", int(stream_idx), e.what());
		return nullptr;
	}
}

wivrn::raw_dump::raw_dump(
        vk_bundle & vk,
        uint8_t stream_idx,
        vk::Extent2D extent,
        uint32_t src_layer,
        vk::ImageLayout layout,
        int bit_depth,
        uint32_t queue_family,
        const std::string & dir) :
        vk(vk),
        stream_idx(stream_idx),
        extent(extent),
        src_layer(src_layer),
        layout(layout),
        ten_bit(bit_depth == 10),
        queue_family(queue_family),
        cmd_pool(vk.device.createCommandPool({
                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
                .queueFamilyIndex = queue_family,
        })),
        frame_limit(env_u64("WIVRN_RAW_DUMP_FRAMES", 300))
{
	vk.name(cmd_pool, std::format("raw dump {} command pool", stream_idx));

	// Y plane then the interleaved CbCr plane at half resolution in both axes: 1.5
	// samples per pixel, 8 or 16 bits each.
	const vk::DeviceSize samples = vk::DeviceSize(extent.width) * extent.height * 3 / 2;
	frame_bytes = samples * (ten_bit ? 2 : 1);

	byte_budget = env_u64("WIVRN_RAW_DUMP_MAX_MB", 8192) * 1024 * 1024;

	auto command_buffers = vk.device.allocateCommandBuffers({
	        .commandPool = *cmd_pool,
	        .commandBufferCount = num_slots,
	});

	for (size_t i = 0; i < num_slots; ++i)
	{
		slots[i].cmd = std::move(command_buffers[i]);
		slots[i].buffer = buffer_allocation(
		        vk.device,
		        {
		                .size = frame_bytes,
		                .usage = vk::BufferUsageFlagBits::eTransferDst,
		        },
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
		        },
		        std::format("raw dump {} buffer", stream_idx));
		slots[i].fence = vk::raii::Fence(vk.device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
	}

	const auto path = [&](const char * suffix) {
		return std::format("{}/stream{}{}", dir, int(stream_idx), suffix);
	};

	yuv.reset(std::fopen(path(".yuv").c_str(), "wb"));
	if (not yuv)
		throw std::runtime_error(std::format("cannot open {}", path(".yuv")));
	jsonl.reset(std::fopen(path(".jsonl").c_str(), "wb"));
	if (not jsonl)
		throw std::runtime_error(std::format("cannot open {}", path(".jsonl")));

	// Everything needed to read the .yuv without knowing anything about WiVRn.
	std::unique_ptr<std::FILE, file_deleter> info(std::fopen(path("-info.json").c_str(), "wb"));
	if (info)
	{
		std::fprintf(info.get(),
		             "{\"stream\":%d,\"eye\":\"%s\",\"width\":%u,\"height\":%u,"
		             "\"bit_depth\":%d,\"pixel_format\":\"%s\",\"chroma\":\"4:2:0\","
		             "\"planes\":[{\"name\":\"Y\",\"width\":%u,\"height\":%u,\"components\":1},"
		             "{\"name\":\"CbCr\",\"width\":%u,\"height\":%u,\"components\":2}],"
		             "\"frame_bytes\":%llu,\"note\":\"encoder input, foveated; see the "
		             "foveation runs in the .jsonl to map back to render space\"}\n",
		             int(stream_idx),
		             stream_idx == 0 ? "left" : "right",
		             unsigned(extent.width),
		             unsigned(extent.height),
		             ten_bit ? 10 : 8,
		             ten_bit ? "p010le" : "nv12",
		             unsigned(extent.width),
		             unsigned(extent.height),
		             unsigned(extent.width / 2),
		             unsigned(extent.height / 2),
		             (unsigned long long)frame_bytes);
	}

	U_LOG_I("raw dump: stream %d to %s, %llu frames max, %llu bytes per frame",
	        int(stream_idx),
	        path(".yuv").c_str(),
	        (unsigned long long)frame_limit,
	        (unsigned long long)frame_bytes);
}

wivrn::raw_dump::~raw_dump()
{
	// The buffers are freed on the way out and a copy may still be running into them.
	for (auto & slot: slots)
	{
		if (slot.armed)
			(void)vk.device.waitForFences(*slot.fence, true, 1'000'000'000);
	}
}

void wivrn::raw_dump::finish(const char * why)
{
	if (done)
		return;
	done = true;
	yuv.reset();
	jsonl.reset();
	U_LOG_I("raw dump: stream %d stopped after %llu frames (%s)",
	        int(stream_idx),
	        (unsigned long long)frames_written,
	        why);
}

void wivrn::raw_dump::present(vk::Image y_cbcr, vk::SemaphoreSubmitInfo compositor_sem, uint8_t slot_idx)
{
	if (done)
		return;

	auto & slot = slots[slot_idx];
	if (vk.device.waitForFences(*slot.fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_W("raw dump: timeout on stream %d", int(stream_idx));
		return;
	}
	slot.armed = false;

	auto & cmd = slot.cmd;
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	// No queue family ownership transfer: the compositor already released the image to
	// this family for the encoder, and the tap runs on a queue of that same family, so
	// it inherits that ownership. The layout is the encoder's target_layout, which
	// create() has already checked is eGeneral — a legal transfer source.
	const std::array regions{
	        vk::BufferImageCopy{
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::ePlane0,
	                        .baseArrayLayer = src_layer,
	                        .layerCount = 1,
	                },
	                .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1},
	        },
	        vk::BufferImageCopy{
	                .bufferOffset = vk::DeviceSize(extent.width) * extent.height * (ten_bit ? 2 : 1),
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::ePlane1,
	                        .baseArrayLayer = src_layer,
	                        .layerCount = 1,
	                },
	                .imageExtent = {.width = extent.width / 2, .height = extent.height / 2, .depth = 1},
	        },
	};
	cmd.copyImageToBuffer(y_cbcr, layout, slot.buffer, regions);
	cmd.end();

	auto & q = (queue_family == vk.queue.family_index) ? vk.queue : vk.transfer_queue;

	std::unique_lock lock(q.mutex);
	vk::CommandBufferSubmitInfo cmd_info{.commandBuffer = *cmd};
	compositor_sem.stageMask = vk::PipelineStageFlagBits2::eTransfer;

	vk.device.resetFences(*slot.fence);
	q.queue.submit2(vk::SubmitInfo2{
	                        .waitSemaphoreInfoCount = 1,
	                        .pWaitSemaphoreInfos = &compositor_sem,
	                        .commandBufferInfoCount = 1,
	                        .pCommandBufferInfos = &cmd_info,
	                },
	                *slot.fence);
	slot.armed = true;
}

void wivrn::raw_dump::write(uint8_t slot_idx,
                            uint64_t frame_index,
                            const to_headset::video_stream_data_shard::view_info_t & view_info)
{
	if (done)
		return;

	auto & slot = slots[slot_idx];
	if (not slot.armed)
		return;
	if (vk.device.waitForFences(*slot.fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_W("raw dump: timeout on stream %d", int(stream_idx));
		return;
	}
	slot.armed = false;

	if (frames_written >= frame_limit)
		return finish("frame cap reached");
	if (total_bytes.fetch_add(frame_bytes) + frame_bytes > byte_budget)
		return finish("size budget reached");

	if (std::fwrite(slot.buffer.map(), 1, frame_bytes, yuv.get()) != frame_bytes)
		return finish("write failed");

	std::FILE * f = jsonl.get();
	std::fprintf(f,
	             "{\"frame\":%llu,\"stream\":%d,\"display_time_ns\":%lld,"
	             "\"width\":%u,\"height\":%u,\"alpha\":%s,\"pose\":[",
	             (unsigned long long)frame_index,
	             int(stream_idx),
	             (long long)view_info.display_time,
	             unsigned(extent.width),
	             unsigned(extent.height),
	             view_info.alpha ? "true" : "false");
	for (size_t eye = 0; eye < view_info.pose.size(); ++eye)
	{
		if (eye)
			std::fputc(',', f);
		write_pose(f, view_info.pose[eye]);
	}
	std::fprintf(f, "],\"fov\":[");
	for (size_t eye = 0; eye < view_info.fov.size(); ++eye)
	{
		if (eye)
			std::fputc(',', f);
		write_fov(f, view_info.fov[eye]);
	}
	std::fprintf(f, "],\"foveation\":[");
	for (size_t eye = 0; eye < view_info.foveation.size(); ++eye)
	{
		if (eye)
			std::fputc(',', f);
		std::fputs("{\"x\":", f);
		write_u16_array(f, view_info.foveation[eye].x);
		std::fputs(",\"y\":", f);
		write_u16_array(f, view_info.foveation[eye].y);
		std::fputc('}', f);
	}
	std::fprintf(f, "]}\n");

	++frames_written;
}
