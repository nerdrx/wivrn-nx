/*
 * WiVRn VR streaming
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

#include "pair_compose.h"

#include "util/u_logging.h"

#include <array>
#include <iterator>

namespace wivrn
{

namespace
{
// The live instance, so two encoders asking for the same geometry get the same
// object and the copy is paid once. Weak, so it dies with the last encoder that
// held it and a reconnection at a different resolution builds a new one.
std::mutex registry_mutex;
std::weak_ptr<pair_compose> live;
uint32_t live_w = 0, live_h = 0, live_eyes = 0;
VkDevice live_dev = VK_NULL_HANDLE;
} // namespace

std::shared_ptr<pair_compose> pair_compose::get(VkPhysicalDevice physical_device,
                                                VkDevice device,
                                                VkQueue queue,
                                                uint32_t queue_family,
                                                uint32_t width,
                                                uint32_t height,
                                                uint32_t eyes)
{
	std::lock_guard lock(registry_mutex);
	if (auto existing = live.lock();
	    existing and live_dev == device and live_w == width and live_h == height and live_eyes == eyes)
		return existing;

	auto created = std::make_shared<pair_compose>(
	        physical_device, device, queue, queue_family, width, height, eyes);
	live = created;
	live_dev = device;
	live_w = width;
	live_h = height;
	live_eyes = eyes;
	return created;
}

pair_compose::pair_compose(VkPhysicalDevice physical_device,
                           VkDevice device,
                           VkQueue queue,
                           uint32_t queue_family,
                           uint32_t width,
                           uint32_t height,
                           uint32_t eyes) :
        vk_phys(physical_device),
        vk_dev(device),
        vk_queue(queue),
        vk_family(queue_family),
        width(width),
        height(height),
        eyes(eyes ? eyes : 1)
{
}

pair_compose::~pair_compose()
{
	if (compose_qpool)
		vkDestroyQueryPool(vk_dev, compose_qpool, nullptr);
	if (compose_fence)
		vkDestroyFence(vk_dev, compose_fence, nullptr);
	if (compose_pool)
		vkDestroyCommandPool(vk_dev, compose_pool, nullptr);
	if (compose_image)
		vkDestroyImage(vk_dev, compose_image, nullptr);
	if (compose_mem)
		vkFreeMemory(vk_dev, compose_mem, nullptr);
}

bool pair_compose::fail(const char * why)
{
	if (not warned)
	{
		U_LOG_W("pair compose unavailable: %s", why);
		warned = true;
	}
	return false;
}

bool pair_compose::ensure()
{
	if (ready)
		return true;

	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(vk_phys, &props);
	// A timestamp period of 0 means the device does not support them; the
	// compose still runs, it just is not timed.
	timestamp_period_ns = props.limits.timestampPeriod;

	// Exactly the image nxvc_vk_enc.h asks for: the two-plane 4:2:0 format,
	// MUTABLE_FORMAT with a format list that names the UINT plane views (a list
	// that omits them makes those views invalid and a driver may refuse them),
	// and STORAGE reached through EXTENDED_USAGE because the planar format has
	// no storage feature of its own.
	//
	// TRANSFER_DST is the copy's own. TRANSFER_SRC and SAMPLED are for the
	// SECOND consumer: a video encoder reads its source by copying or sampling
	// rather than as a storage image, so an image created only for nxvc's E0
	// would be unusable by the base layer's HEVC encoder.
	static const VkFormat view_formats[] = {
	        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	        VK_FORMAT_R8_UNORM,
	        VK_FORMAT_R8G8_UNORM,
	        VK_FORMAT_R8_UINT,
	        VK_FORMAT_R8G8_UINT,
	};
	VkImageFormatListCreateInfo fmt_list{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
	        .viewFormatCount = uint32_t(std::size(view_formats)),
	        .pViewFormats = view_formats,
	};
	VkImageCreateInfo ici{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	        .pNext = &fmt_list,
	        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
	        .imageType = VK_IMAGE_TYPE_2D,
	        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	        .extent = {pair_width(), height, 1},
	        .mipLevels = 1,
	        .arrayLayers = 1,
	        .samples = VK_SAMPLE_COUNT_1_BIT,
	        .tiling = VK_IMAGE_TILING_OPTIMAL,
	        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(vk_dev, &ici, nullptr, &compose_image) != VK_SUCCESS)
		return fail("could not create the compose image");

	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(vk_dev, compose_image, &req);
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(vk_phys, &mem);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
		if ((req.memoryTypeBits & (1u << i)) and
		    (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
		{
			type = i;
			break;
		}
	if (type == UINT32_MAX)
		return fail("no device-local memory type for the compose image");

	VkMemoryAllocateInfo mai{
	        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	        .allocationSize = req.size,
	        .memoryTypeIndex = type,
	};
	if (vkAllocateMemory(vk_dev, &mai, nullptr, &compose_mem) != VK_SUCCESS)
		return fail("could not allocate the compose image");
	if (vkBindImageMemory(vk_dev, compose_image, compose_mem, 0) != VK_SUCCESS)
		return fail("could not bind the compose image");

	VkCommandPoolCreateInfo pci{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	        .queueFamilyIndex = vk_family,
	};
	if (vkCreateCommandPool(vk_dev, &pci, nullptr, &compose_pool) != VK_SUCCESS)
		return fail("could not create the compose command pool");

	VkCommandBufferAllocateInfo cbi{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	        .commandPool = compose_pool,
	        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	        .commandBufferCount = 1,
	};
	if (vkAllocateCommandBuffers(vk_dev, &cbi, &compose_cmd) != VK_SUCCESS)
		return fail("could not allocate the compose command buffer");

	VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vkCreateFence(vk_dev, &fci, nullptr, &compose_fence) != VK_SUCCESS)
		return fail("could not create the compose fence");

	if (timestamp_period_ns > 0)
	{
		VkQueryPoolCreateInfo qci{
		        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		        .queryType = VK_QUERY_TYPE_TIMESTAMP,
		        .queryCount = 2,
		};
		if (vkCreateQueryPool(vk_dev, &qci, nullptr, &compose_qpool) != VK_SUCCESS)
			compose_qpool = VK_NULL_HANDLE; // not fatal, just untimed
	}

	ready = true;
	return true;
}

namespace
{
// Copy one array layer of `src` into the half of the compose image at `dst_x`.
// A two-plane 4:2:0 image is copied a plane at a time: the chroma plane is half
// the luma in both axes, so its offset and extent are halved too. The per-eye
// width is a multiple of 64 (nxvc refuses eyes == 2 otherwise, and the base
// layer inherits that precondition), so `dst_x / 2` is exact and the seam falls
// on a chroma sample.
void copy_layer(VkCommandBuffer cmd,
                VkImage src,
                uint32_t layer,
                VkImage dst,
                uint32_t dst_x,
                uint32_t w,
                uint32_t h)
{
	const VkImageCopy regions[] = {
	        {
	                .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
	                                   .mipLevel = 0,
	                                   .baseArrayLayer = layer,
	                                   .layerCount = 1},
	                .srcOffset = {0, 0, 0},
	                .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
	                                   .mipLevel = 0,
	                                   .baseArrayLayer = 0,
	                                   .layerCount = 1},
	                .dstOffset = {int32_t(dst_x), 0, 0},
	                .extent = {w, h, 1},
	        },
	        {
	                .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
	                                   .mipLevel = 0,
	                                   .baseArrayLayer = layer,
	                                   .layerCount = 1},
	                .srcOffset = {0, 0, 0},
	                .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
	                                   .mipLevel = 0,
	                                   .baseArrayLayer = 0,
	                                   .layerCount = 1},
	                .dstOffset = {int32_t(dst_x / 2), 0, 0},
	                .extent = {w / 2, h / 2, 1},
	        },
	};
	vkCmdCopyImage(cmd,
	               src,
	               VK_IMAGE_LAYOUT_GENERAL,
	               dst,
	               VK_IMAGE_LAYOUT_GENERAL,
	               uint32_t(std::size(regions)),
	               regions);
}
} // namespace

VkImage pair_compose::compose(VkImage src,
                              uint32_t layer_left,
                              uint32_t layer_right,
                              uint64_t frame_index,
                              VkSemaphore wait_sem,
                              uint64_t wait_value)
{
	std::lock_guard lock(mutex);

	// The frame guard. Two encoders want the same pair of the same frame, and
	// the second one must not copy it again -- that is the whole point of
	// sharing this object. A frame index that goes backwards (a stream that
	// started over) composes again rather than handing back a stale picture.
	if (have_frame and frame_index == last_frame)
		return compose_image;

	if (not ensure())
		return VK_NULL_HANDLE;

	vkResetCommandBuffer(compose_cmd, 0);
	VkCommandBufferBeginInfo bi{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(compose_cmd, &bi);

	// UNDEFINED only once: after the first frame the image already holds the
	// previous pair, and discarding it would be a lie the driver is entitled to
	// act on. Every later frame transitions GENERAL -> GENERAL, which is just
	// the execution and memory dependency against the consumers' reads of the
	// frame before.
	//
	// The stage masks are ALL_COMMANDS on both sides, and that is deliberate
	// rather than lazy: this image now has two consumers that read it in
	// different ways -- nxvc's E0 as a storage image in a compute shader, a
	// video encoder by copying or sampling on a queue of its own -- so naming
	// COMPUTE_SHADER here, as the single-consumer version did, would be a
	// dependency against one of them and not the other. It is one barrier per
	// frame around a full-frame copy; the measurement in last_ms() is what says
	// whether that matters, and it does not.
	VkImageMemoryBarrier to_dst{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = undefined ? VkAccessFlags(0) : VkAccessFlags(VK_ACCESS_MEMORY_READ_BIT),
	        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .oldLayout = undefined ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
	        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = compose_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(compose_cmd,
	                     undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
	                               : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &to_dst);
	undefined = false;

	if (compose_qpool)
	{
		vkCmdResetQueryPool(compose_cmd, compose_qpool, 0, 2);
		vkCmdWriteTimestamp(compose_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, compose_qpool, 0);
	}

	copy_layer(compose_cmd, src, layer_left, compose_image, 0, width, height);
	if (eyes > 1)
		copy_layer(compose_cmd, src, layer_right, compose_image, width, width, height);

	if (compose_qpool)
		vkCmdWriteTimestamp(compose_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, compose_qpool, 1);

	VkImageMemoryBarrier to_read{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
	                         VK_ACCESS_MEMORY_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = compose_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(compose_cmd,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &to_read);
	vkEndCommandBuffer(compose_cmd);

	vkResetFences(vk_dev, 1, &compose_fence);
	// The compositor is still drawing this frame when a present-time caller gets
	// here, so the copy waits on its timeline value before reading a single
	// texel. TRANSFER is the only stage that reads the source.
	const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkTimelineSemaphoreSubmitInfo tsi{
	        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
	        .waitSemaphoreValueCount = 1,
	        .pWaitSemaphoreValues = &wait_value,
	};
	VkSubmitInfo si{
	        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	        .pNext = wait_sem ? &tsi : nullptr,
	        .waitSemaphoreCount = wait_sem ? 1u : 0u,
	        .pWaitSemaphores = wait_sem ? &wait_sem : nullptr,
	        .pWaitDstStageMask = wait_sem ? &wait_stage : nullptr,
	        .commandBufferCount = 1,
	        .pCommandBuffers = &compose_cmd,
	};
	if (vkQueueSubmit(vk_queue, 1, &si, compose_fence) != VK_SUCCESS)
	{
		fail("the compose submit failed");
		return VK_NULL_HANDLE;
	}
	vkWaitForFences(vk_dev, 1, &compose_fence, VK_TRUE, UINT64_MAX);

	if (compose_qpool)
	{
		uint64_t ts[2] = {0, 0};
		if (vkGetQueryPoolResults(vk_dev, compose_qpool, 0, 2, sizeof ts, ts,
		                          sizeof(uint64_t),
		                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS and
		    ts[1] >= ts[0])
			last_compose_ms = double(ts[1] - ts[0]) * timestamp_period_ns * 1e-6;
	}

	have_frame = true;
	last_frame = frame_index;
	return compose_image;
}

} // namespace wivrn
