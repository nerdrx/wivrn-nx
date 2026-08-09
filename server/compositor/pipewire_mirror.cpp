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
#include <atomic>
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

	std::thread reader;

	static void on_state_changed(void * self_v, pw_stream_state old, pw_stream_state state, const char * error);
	static void on_param_changed(void * self_v, uint32_t id, const spa_pod * param);

	void read_frames();
	void push_frame();

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

	// DRIVER: nothing else paces this node, frames are produced by the compositor,
	// so the graph is triggered once a frame has been pushed.
	if (pw_stream_connect(
	            stream.get(),
	            PW_DIRECTION_OUTPUT,
	            PW_ID_ANY,
	            pw_stream_flags(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DRIVER),
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

	const spa_pod * params[1];
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

	pw_stream_update_params(self->stream.get(), params, 1);
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
			push_frame();
		}

		lock.lock();
		armed = false;
		pending = false;
	}
}

void mirror_impl::push_frame()
{
	pw_thread_loop_lock(loop.get());

	if (pw_stream_get_state(stream.get(), nullptr) == PW_STREAM_STATE_STREAMING)
	{
		if (pw_buffer * buffer = pw_stream_dequeue_buffer(stream.get()))
		{
			if (buffer->buffer->n_datas > 0 and buffer->buffer->datas[0].data)
			{
				spa_data & data = buffer->buffer->datas[0];
				size_t size = std::min<size_t>(data.maxsize, size_t(stride) * height);
				memcpy(data.data, readback.data(), size);
				data.chunk->offset = 0;
				data.chunk->stride = stride;
				data.chunk->size = size;
				data.chunk->flags = 0;
			}
			pw_stream_queue_buffer(stream.get(), buffer);

			if (pw_stream_is_driving(stream.get()))
				pw_stream_trigger_process(stream.get());
		}
	}

	pw_thread_loop_unlock(loop.get());
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
