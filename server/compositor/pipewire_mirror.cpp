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

#include "pipewire_mirror.h"

#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_time.h"
#include "utils/wivrn_vk_bundle.h"
#include "vk/allocation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <magic_enum.hpp>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

namespace
{

const uint32_t bytes_per_pixel = 4;
const char * node_name = "wivrn-headset-view";
const char * node_description = "WiVRn Headset View";
//! Period of the delivery counter log
const int64_t delivery_log_interval_ns = int64_t(5) * U_TIME_1S_IN_NS;

struct deleter
{
	void operator()(pw_thread_loop * loop)
	{
		pw_thread_loop_destroy(loop);
	}
	void operator()(pw_stream * stream)
	{
		pw_stream_destroy(stream);
	}
};

struct push_constants
{
	int32_t src_offset[2];
	int32_t src_extent[2];
	int32_t dst_size[2];
};

vk::raii::Sampler make_sampler(wivrn::vk_bundle & vk)
{
	vk::raii::Sampler res{
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eLinear,
	                .minFilter = vk::Filter::eLinear,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	                .borderColor = vk::BorderColor::eFloatOpaqueBlack,
	        },
	};
	vk.name(*res, "mirror sampler");
	return res;
}

vk::raii::DescriptorSetLayout make_ds_layout(wivrn::vk_bundle & vk)
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
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	vk::raii::DescriptorSetLayout res{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = bindings.size(),
	                .pBindings = bindings.data(),
	        },
	};
	vk.name(*res, "mirror descriptor set layout");
	return res;
}

vk::raii::PipelineLayout make_layout(wivrn::vk_bundle & vk, vk::DescriptorSetLayout ds_layout)
{
	vk::PushConstantRange range{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(push_constants),
	};
	vk::raii::PipelineLayout res{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &range,
	        },
	};
	vk.name(*res, "mirror pipeline layout");
	return res;
}

vk::raii::Pipeline make_pipeline(wivrn::vk_bundle & vk, vk::PipelineLayout layout)
{
	auto shader = vk.load_shader("mirror");
	vk::raii::Pipeline res{
	        vk.device,
	        nullptr,
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *shader,
	                        .pName = "main",
	                },
	                .layout = layout,
	        },
	};
	vk.name(*res, "mirror pipeline");
	return res;
}

vk::raii::DescriptorPool make_ds_pool(wivrn::vk_bundle & vk)
{
	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	        },
	};
	vk::raii::DescriptorPool res{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 1,
	                .poolSizeCount = pool_sizes.size(),
	                .pPoolSizes = pool_sizes.data(),
	        },
	};
	vk.name(*res, "mirror descriptor pool");
	return res;
}

uint32_t divide_and_round_up(uint32_t a, uint32_t b)
{
	return (a + b - 1) / b;
}

class mirror_impl : public wivrn::pipewire_mirror
{
	wivrn::vk_bundle & vk;

	const uint32_t width;
	const uint32_t height;
	const uint32_t stride;
	const int64_t frame_interval_ns;

	// GPU side, only used from the compositor thread
	vk::raii::Sampler sampler;
	vk::raii::DescriptorSetLayout ds_layout;
	vk::raii::PipelineLayout layout;
	vk::raii::Pipeline pipeline;
	vk::raii::DescriptorPool ds_pool;
	vk::DescriptorSet descriptor_set;
	image_allocation dest;
	vk::raii::ImageView dest_view = nullptr;
	buffer_allocation readback;
	bool readback_coherent = false;
	int64_t next_capture_ns = 0;

	// pipewire side
	std::unique_ptr<pw_thread_loop, deleter> loop;
	std::unique_ptr<pw_stream, deleter> stream;
	pw_stream_events events{
	        .version = PW_VERSION_STREAM_EVENTS,
	        .state_changed = &mirror_impl::on_state_changed,
	        .param_changed = &mirror_impl::on_param_changed,
	        .process = &mirror_impl::on_process,
	};
	std::atomic<std::underlying_type_t<pw_stream_state>> stream_state{PW_STREAM_STATE_UNCONNECTED};
	std::atomic<bool> negotiated{false};

	// Capture in flight, shared with the reader thread
	std::mutex mutex;
	std::condition_variable cv;
	bool pending = false; // commands recorded, result not consumed yet
	bool armed = false;   // submission info available
	bool quit = false;
	vk::Semaphore sem;
	uint64_t sem_value = 0;

	/*!
	 * Latest captured frame: written by the reader thread, read by the PipeWire
	 * loop thread in process().
	 *
	 * Two slots so that the reader thread can fill one while the loop thread
	 * copies the other out; frame_mutex only protects the slot indices and is
	 * held by the loop thread for the duration of its copy, so that the reader
	 * thread cannot start overwriting the slot being read. The reader thread
	 * writes frame_slots[frame_write] without the mutex: it is the only writer,
	 * and frame_write is never the slot process() reads.
	 */
	std::mutex frame_mutex;
	std::array<std::vector<uint8_t>, 2> frame_slots;
	int frame_write = 0;   // slot the reader thread may write to
	int frame_latest = -1; // slot holding the most recent frame, -1 if none yet
	int64_t frame_pts = 0;
	uint64_t frame_seq = 0;

	// PipeWire loop thread only
	uint64_t delivered = 0;    // buffers queued with a frame in them
	uint64_t empty_cycles = 0; // graph cycles with no frame to deliver
	int64_t next_log_ns = 0;   // next delivery counter log
	bool warned_short_buffer = false;

	std::thread reader;

	static void on_state_changed(void * self_v, pw_stream_state old, pw_stream_state state, const char * error);
	static void on_param_changed(void * self_v, uint32_t id, const spa_pod * param);
	static void on_process(void * self_v);

	void read_frames();
	void store_frame();
	void process();

public:
	mirror_impl(wivrn::vk_bundle & vk, vk::Extent2D size, int fps);
	~mirror_impl();

	bool capture(vk::raii::CommandBuffer & cmd, const source & src) override;
	void submitted(vk::Semaphore, uint64_t value) override;
};

mirror_impl::mirror_impl(wivrn::vk_bundle & vk, vk::Extent2D size, int fps) :
        vk(vk),
        width(size.width),
        height(size.height),
        stride(size.width * bytes_per_pixel),
        frame_interval_ns(U_TIME_1S_IN_NS / std::max(1, fps)),
        sampler(make_sampler(vk)),
        ds_layout(make_ds_layout(vk)),
        layout(make_layout(vk, *ds_layout)),
        pipeline(make_pipeline(vk, *layout)),
        ds_pool(make_ds_pool(vk))
{
	descriptor_set = vk.device.allocateDescriptorSets(
	                                  {
	                                          .descriptorPool = *ds_pool,
	                                          .descriptorSetCount = 1,
	                                          .pSetLayouts = &*ds_layout,
	                                  })[0]
	                         .release();

	dest = image_allocation{
	        vk.device,
	        vk::ImageCreateInfo{
	                .imageType = vk::ImageType::e2D,
	                // R8G8B8A8Unorm is guaranteed to support storage and blit/transfer
	                // source, and its memory layout is what SPA_VIDEO_FORMAT_RGBx expects
	                .format = vk::Format::eR8G8B8A8Unorm,
	                .extent = {.width = width, .height = height, .depth = 1},
	                .mipLevels = 1,
	                .arrayLayers = 1,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
	        },
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
	        "mirror image",
	};

	dest_view = vk::raii::ImageView{
	        vk.device,
	        vk::ImageViewCreateInfo{
	                .image = dest,
	                .viewType = vk::ImageViewType::e2D,
	                .format = vk::Format::eR8G8B8A8Unorm,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 1,
	                },
	        },
	};

	readback = buffer_allocation{
	        vk.device,
	        vk::BufferCreateInfo{
	                .size = vk::DeviceSize(stride) * height,
	                .usage = vk::BufferUsageFlagBits::eTransferDst,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
	        },
	        "mirror readback buffer",
	};
	readback_coherent = readback.properties() & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	// Keep it mapped for the whole lifetime
	readback.map();

	for (auto & slot: frame_slots)
		slot.resize(size_t(stride) * height);

	// The descriptor set only changes on binding 0, binding 1 is written once
	vk::DescriptorImageInfo dest_info{
	        .imageView = *dest_view,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	vk.device.updateDescriptorSets(
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_set,
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &dest_info,
	        },
	        {});

	int argc = 0;
	pw_init(&argc, nullptr);

	loop.reset(pw_thread_loop_new("wivrn-mirror", nullptr));
	if (not loop)
		throw std::runtime_error("failed to create pipewire loop");

	std::string framerate_str = std::format("1/{}", fps);
	stream.reset(pw_stream_new_simple(
	        pw_thread_loop_get_loop(loop.get()),
	        node_description,
	        pw_properties_new(
	                PW_KEY_NODE_NAME,
	                node_name,
	                PW_KEY_NODE_DESCRIPTION,
	                node_description,
	                PW_KEY_MEDIA_TYPE,
	                "Video",
	                PW_KEY_MEDIA_CATEGORY,
	                "Capture",
	                PW_KEY_MEDIA_CLASS,
	                "Video/Source",
	                PW_KEY_MEDIA_ROLE,
	                "Camera",
	                PW_KEY_NODE_RATE,
	                framerate_str.c_str(),
	                NULL),
	        &events,
	        this));
	if (not stream)
		throw std::runtime_error("failed to create pipewire stream");

	std::vector<uint8_t> buffer(1024);
	spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer.data(), uint32_t(buffer.size()));

	spa_video_info_raw video_info{};
	video_info.format = SPA_VIDEO_FORMAT_RGBx;
	video_info.size.width = width;
	video_info.size.height = height;
	video_info.framerate.num = uint32_t(fps);
	video_info.framerate.denom = 1;

	const spa_pod * params[1];
	params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &video_info);

	// No PW_STREAM_FLAG_DRIVER: this node behaves like a camera, the graph drives
	// it and process() hands out whatever the compositor last captured. Driving the
	// graph ourselves depends on what the session manager picks as the driver, and
	// in the topology this ends up in, buffers never reach the peer.
	if (pw_stream_connect(
	            stream.get(),
	            PW_DIRECTION_OUTPUT,
	            PW_ID_ANY,
	            pw_stream_flags(PW_STREAM_FLAG_MAP_BUFFERS),
	            params,
	            1) < 0)
		throw std::runtime_error("failed to connect mirror stream");

	pw_thread_loop_start(loop.get());

	reader = std::thread([this]() { read_frames(); });

	U_LOG_I("Desktop mirror: pipewire node \"%s\" %ux%u@%d", node_description, width, height, fps);
}

mirror_impl::~mirror_impl()
{
	{
		std::lock_guard lock(mutex);
		quit = true;
	}
	cv.notify_all();
	if (reader.joinable())
		reader.join();

	if (loop)
		pw_thread_loop_stop(loop.get());
	stream.reset();
	loop.reset();
}

void mirror_impl::on_state_changed(void * self_v, pw_stream_state old, pw_stream_state state, const char * error)
{
	auto self = (mirror_impl *)self_v;
	self->stream_state = state;
	if (state == PW_STREAM_STATE_UNCONNECTED or state == PW_STREAM_STATE_ERROR)
		self->negotiated = false;

	// Counters are per streaming session, they are only touched from this thread
	if (state == PW_STREAM_STATE_STREAMING)
	{
		self->delivered = 0;
		self->empty_cycles = 0;
		self->next_log_ns = os_monotonic_get_ns() + delivery_log_interval_ns;
		self->warned_short_buffer = false;
	}

	if (error)
		U_LOG_I("Mirror stream state changed from %s to %s (error: %s)",
		        magic_enum::enum_name(old).data(),
		        magic_enum::enum_name(state).data(),
		        error);
	else
		U_LOG_I("Mirror stream state changed from %s to %s",
		        magic_enum::enum_name(old).data(),
		        magic_enum::enum_name(state).data());
}

void mirror_impl::on_param_changed(void * self_v, uint32_t id, const spa_pod * param)
{
	auto self = (mirror_impl *)self_v;
	if (id != SPA_PARAM_Format)
		return;
	if (param == nullptr)
	{
		self->negotiated = false;
		return;
	}

	spa_video_info info{};
	if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
		return;
	if (info.media_type != SPA_MEDIA_TYPE_video or info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;
	if (spa_format_video_raw_parse(param, &info.info.raw) < 0)
		return;

	if (info.info.raw.size.width != self->width or info.info.raw.size.height != self->height)
	{
		U_LOG_W("Mirror: consumer negotiated an unexpected size %ux%u",
		        info.info.raw.size.width,
		        info.info.raw.size.height);
		return;
	}

	std::vector<uint8_t> buffer(1024);
	spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer.data(), uint32_t(buffer.size()));

	const spa_pod * params[2];
	params[0] = (const spa_pod *)spa_pod_builder_add_object(
	        &b,
	        SPA_TYPE_OBJECT_ParamBuffers,
	        SPA_PARAM_Buffers,
	        SPA_PARAM_BUFFERS_buffers,
	        SPA_POD_CHOICE_RANGE_Int(3, 2, 8),
	        SPA_PARAM_BUFFERS_blocks,
	        SPA_POD_Int(1),
	        SPA_PARAM_BUFFERS_size,
	        SPA_POD_Int(int(self->stride * self->height)),
	        SPA_PARAM_BUFFERS_stride,
	        SPA_POD_Int(int(self->stride)),
	        SPA_PARAM_BUFFERS_dataType,
	        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	// Optional: process() timestamps the buffers when the peer allocates it
	params[1] = (const spa_pod *)spa_pod_builder_add_object(
	        &b,
	        SPA_TYPE_OBJECT_ParamMeta,
	        SPA_PARAM_Meta,
	        SPA_PARAM_META_type,
	        SPA_POD_Id(SPA_META_Header),
	        SPA_PARAM_META_size,
	        SPA_POD_Int(int(sizeof(spa_meta_header))));

	pw_stream_update_params(self->stream.get(), params, 2);
	self->negotiated = true;
}

bool mirror_impl::capture(vk::raii::CommandBuffer & cmd, const source & src)
{
	// Zero cost when nobody is listening
	if (not src.view or not negotiated or stream_state != PW_STREAM_STATE_STREAMING)
		return false;

	if (src.width == 0 or src.height == 0)
		return false;

	int64_t now = os_monotonic_get_ns();
	if (now < next_capture_ns)
		return false;

	{
		// Previous capture still in flight, skip this frame rather than stall
		std::lock_guard lock(mutex);
		if (pending)
			return false;
		pending = true;
	}

	if (now > next_capture_ns + frame_interval_ns)
		next_capture_ns = now + frame_interval_ns;
	else
		next_capture_ns += frame_interval_ns;

	vk::DescriptorImageInfo src_info{
	        .sampler = *sampler,
	        .imageView = src.view,
	        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk.device.updateDescriptorSets(
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_set,
	                .dstBinding = 0,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &src_info,
	        },
	        {});

	push_constants pc{
	        .src_offset = {src.x, src.y},
	        .src_extent = {src.width, src.height},
	        .dst_size = {int32_t(width), int32_t(height)},
	};
	if (src.flip_y)
	{
		pc.src_offset[1] += pc.src_extent[1];
		pc.src_extent[1] = -pc.src_extent[1];
	}

	// The contents of dest are fully overwritten, no need to preserve them; the
	// barrier is still needed to order the dispatch after the previous frame's
	// copy out of dest.
	vk::ImageMemoryBarrier2 to_general{
	        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .oldLayout = vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = dest,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = 1,
	        },
	};
	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &to_general,
	});

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, descriptor_set, {});
	cmd.pushConstants<push_constants>(*layout, vk::ShaderStageFlagBits::eCompute, 0, pc);
	cmd.dispatch(divide_and_round_up(width, 8), divide_and_round_up(height, 8), 1);

	vk::ImageMemoryBarrier2 to_transfer{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
	        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
	        .oldLayout = vk::ImageLayout::eGeneral,
	        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
	        .image = dest,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = 1,
	        },
	};
	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &to_transfer,
	});

	cmd.copyImageToBuffer(
	        dest,
	        vk::ImageLayout::eTransferSrcOptimal,
	        readback,
	        vk::BufferImageCopy{
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .layerCount = 1,
	                },
	                .imageExtent = {.width = width, .height = height, .depth = 1},
	        });

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

void mirror_impl::submitted(vk::Semaphore semaphore, uint64_t value)
{
	{
		std::lock_guard lock(mutex);
		if (not pending or armed)
			return;
		sem = semaphore;
		sem_value = value;
		armed = true;
	}
	cv.notify_one();
}

void mirror_impl::read_frames()
{
	std::unique_lock lock(mutex);
	while (true)
	{
		cv.wait(lock, [this]() { return quit or armed; });
		if (quit)
			return;

		vk::Semaphore semaphore = sem;
		uint64_t value = sem_value;
		lock.unlock();

		bool ok = true;
		try
		{
			if (vk.device.waitSemaphores(
			            vk::SemaphoreWaitInfo{
			                    .semaphoreCount = 1,
			                    .pSemaphores = &semaphore,
			                    .pValues = &value,
			            },
			            U_TIME_1S_IN_NS) == vk::Result::eTimeout)
			{
				U_LOG_W("Mirror: timeout waiting for capture");
				ok = false;
			}
		}
		catch (std::exception & e)
		{
			U_LOG_W("Mirror: error waiting for capture: %s", e.what());
			ok = false;
		}

		if (ok)
		{
			if (not readback_coherent)
				vmaInvalidateAllocation(vk_allocator::instance(), readback, 0, VK_WHOLE_SIZE);
			store_frame();
		}

		lock.lock();
		armed = false;
		pending = false;
	}
}

void mirror_impl::store_frame()
{
	// Only the reader thread writes this slot, and process() never reads it
	std::vector<uint8_t> & slot = frame_slots[frame_write];
	memcpy(slot.data(), readback.data(), slot.size());
	const int64_t pts = os_monotonic_get_ns();

	{
		// Swapping the indices is all that has to be serialised with process()
		std::lock_guard lock(frame_mutex);
		frame_latest = frame_write;
		frame_write ^= 1;
		frame_pts = pts;
		++frame_seq;
	}

	// The graph normally drives this node and picks the frame up on its next
	// cycle. Nudging is only meaningful, and only allowed, when the session
	// manager did make us the driver; frame_mutex must be released first, as
	// process() takes it while holding the loop lock.
	pw_thread_loop_lock(loop.get());
	if (pw_stream_get_state(stream.get(), nullptr) == PW_STREAM_STATE_STREAMING and
	    pw_stream_is_driving(stream.get()))
		pw_stream_trigger_process(stream.get());
	pw_thread_loop_unlock(loop.get());
}

void mirror_impl::on_process(void * self_v)
{
	((mirror_impl *)self_v)->process();
}

void mirror_impl::process()
{
	pw_buffer * buffer = pw_stream_dequeue_buffer(stream.get());
	if (not buffer)
		return;

	spa_buffer * buf = buffer->buffer;
	const size_t frame_size = size_t(stride) * height;

	bool filled = false;
	int64_t pts = 0;
	uint64_t seq = 0;

	if (buf->n_datas > 0 and buf->datas[0].data and buf->datas[0].chunk)
	{
		spa_data & data = buf->datas[0];

		if (data.maxsize >= frame_size)
		{
			// Held for the copy so that the reader thread cannot flip this
			// slot back into its write position while it is being read
			std::lock_guard lock(frame_mutex);
			if (frame_latest >= 0)
			{
				memcpy(data.data, frame_slots[frame_latest].data(), frame_size);
				pts = frame_pts;
				seq = frame_seq;
				filled = true;
			}
		}
		else if (not warned_short_buffer)
		{
			warned_short_buffer = true;
			U_LOG_W("Mirror: buffer of %u bytes is too small for %ux%u",
			        data.maxsize,
			        width,
			        height);
		}

		data.chunk->offset = 0;
		data.chunk->stride = int32_t(stride);
		data.chunk->size = filled ? uint32_t(frame_size) : 0;
		data.chunk->flags = filled ? SPA_CHUNK_FLAG_NONE : SPA_CHUNK_FLAG_EMPTY;
	}

	if (auto * header = (spa_meta_header *)spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header)))
	{
		header->pts = filled ? pts : -1;
		header->dts_offset = 0;
		header->seq = seq;
		header->flags = 0;
	}

	pw_stream_queue_buffer(stream.get(), buffer);

	// The graph may cycle faster than the capture rate; repeating the last frame
	// is expected, only count what actually carried pixels.
	if (filled)
		++delivered;
	else
		++empty_cycles;

	const int64_t now = os_monotonic_get_ns();
	if (now >= next_log_ns)
	{
		next_log_ns = now + delivery_log_interval_ns;
		U_LOG_I("Mirror: delivered %" PRIu64 " frames (%" PRIu64 " empty cycles)", delivered, empty_cycles);
	}
}

} // namespace

namespace wivrn
{

pipewire_mirror::~pipewire_mirror() = default;

std::unique_ptr<pipewire_mirror> pipewire_mirror::create(vk_bundle & vk, vk::Extent2D size, int fps)
{
	try
	{
		return std::make_unique<mirror_impl>(vk, size, fps);
	}
	catch (std::exception & e)
	{
		U_LOG_W("Desktop mirror creation failed: %s", e.what());
		return nullptr;
	}
}

} // namespace wivrn
