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

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <vulkan/vulkan.h>

namespace wivrn
{

// The eye pair, brought together into ONE side-by-side picture.
//
// WiVRn keeps the two eyes in separate ARRAY LAYERS of one image. Two different
// consumers want them side by side instead and want the identical picture:
//
//   * the NX Warp encoder, because nxvc's image entry point takes one picture
//     `eyes * width` wide and its `array_layer` selects a layer of an array
//     image rather than an eye;
//   * the hybrid base layer's HEVC encoder, because the base codes the pair as
//     one picture so that with CTB 64 every quantisation and deblocking
//     boundary coincides with an nxvc tile boundary in BOTH eyes, with no
//     per-eye offset correction (docs/NXWARP-HYBRID.md §10.3).
//
// It used to live inside the nxvc codec wrapper, which was the right place
// while there was one consumer. With two it is a full-frame copy that would be
// paid twice for the same pixels, so it moves here and is shared: `get()`
// hands the same instance to everyone who asks for the same device and
// geometry, the same way video_encoder_nvenc_shared_state does for CUDA, and
// `compose()` is idempotent within a frame -- the second consumer to ask for a
// given frame_index gets the image the first one built, without a second copy
// or a second submit.
//
// It costs one full-frame device-side copy per frame that the mono path does
// not pay. Nothing crosses the bus and no plane is laid out on the host, so the
// "no plane ever touches host memory" property of the image entry point
// survives; what it gives up is only that the picture is copied ON the device.
// The copy is timed with a timestamp pair so the cost is measured rather than
// asserted -- see last_ms().
//
// The right long-term fix is neither this nor widening the compositor's image:
// it is teaching nxvc's E0 to read eye 1 from a second array layer, which it
// could do almost for free because its tile index is already pair-wide. That is
// a change in nx-warp, so it is a follow-up and not this.
class pair_compose
{
public:
	// One instance per (device, per-eye geometry). `width` and `height` are PER
	// EYE, as everywhere in nxvc and in encoder_settings; the composed picture
	// is `eyes * width` wide.
	//
	// Returns nullptr if the compose could not be set up, which is not fatal:
	// the caller falls back to whatever it did before there was a pair.
	static std::shared_ptr<pair_compose> get(VkPhysicalDevice physical_device,
	                                         VkDevice device,
	                                         VkQueue queue,
	                                         uint32_t queue_family,
	                                         uint32_t width,
	                                         uint32_t height,
	                                         uint32_t eyes);

	// Copy `src`'s two array layers into the two halves of the shared image and
	// return it, or VK_NULL_HANDLE if the compose is unavailable.
	//
	// `frame_index` is what makes this shareable: within one frame the first
	// caller does the copy and every later caller gets the same image back
	// untouched. Callers must therefore pass the SAME frame index for the same
	// compositor frame, and a strictly increasing one across frames.
	//
	// `wait_sem`/`wait_value` are the compositor's TIMELINE semaphore, and
	// passing them is not optional for a caller that runs at present time. The
	// compositor is still writing the eye image when present_image is called;
	// every encoder waits on this semaphore in its own submit before touching
	// the picture, and a compose that did not would read the frame while it was
	// being drawn. A timeline semaphore admits any number of waiters, so this
	// wait does not consume the one the encoder itself performs -- which it
	// would if this were a binary semaphore, and which is why it is worth
	// saying here.
	//
	// A caller that already runs after the compositor is done -- the nxvc codec
	// does, at encode time under the queue mutex -- passes VK_NULL_HANDLE.
	VkImage compose(VkImage src,
	                uint32_t layer_left,
	                uint32_t layer_right,
	                uint64_t frame_index,
	                VkSemaphore wait_sem = VK_NULL_HANDLE,
	                uint64_t wait_value = 0);

	// The composed image, valid only after a successful compose().
	VkImage image() const
	{
		return compose_image;
	}

	uint32_t pair_width() const
	{
		return width * eyes;
	}

	// GPU milliseconds the last copy actually took, or 0 where the device
	// cannot time it.
	double last_ms() const
	{
		return last_compose_ms;
	}

	pair_compose(VkPhysicalDevice, VkDevice, VkQueue, uint32_t, uint32_t, uint32_t, uint32_t);
	~pair_compose();

	pair_compose(const pair_compose &) = delete;
	pair_compose & operator=(const pair_compose &) = delete;
	pair_compose(pair_compose &&) = delete;
	pair_compose & operator=(pair_compose &&) = delete;

private:
	bool ensure();
	bool fail(const char * why);

	VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
	VkDevice vk_dev = VK_NULL_HANDLE;
	VkQueue vk_queue = VK_NULL_HANDLE;
	uint32_t vk_family = 0;
	uint32_t width = 0, height = 0, eyes = 2;

	VkImage compose_image = VK_NULL_HANDLE;
	VkDeviceMemory compose_mem = VK_NULL_HANDLE;
	VkCommandPool compose_pool = VK_NULL_HANDLE;
	VkCommandBuffer compose_cmd = VK_NULL_HANDLE;
	VkFence compose_fence = VK_NULL_HANDLE;
	VkQueryPool compose_qpool = VK_NULL_HANDLE;
	bool ready = false;
	bool undefined = true;
	bool warned = false;
	double timestamp_period_ns = 0; // 0 = the device cannot time this
	double last_compose_ms = 0;

	// Two encoders on two threads ask for the same frame. The mutex is held
	// across the submit and its fence wait, which is what makes the frame guard
	// below a guard and not a race.
	std::mutex mutex;
	bool have_frame = false;
	uint64_t last_frame = 0;
};

} // namespace wivrn
