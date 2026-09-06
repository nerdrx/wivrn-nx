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

#include "nxwarp_base_patch.h"

#include <cstring>
#include <format>
#include <stdexcept>

namespace wivrn
{

namespace
{
uint32_t find_memory(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp{};
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
		if ((bits & (1u << i)) and (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	throw std::runtime_error("nxwarp base patch: no host-visible memory type for the staging buffer");
}
} // namespace

base_patcher::base_patcher(nxwarp_codec & codec, VkPhysicalDevice phys, VkDevice dev, uint32_t eyes) :
        codec(codec), dev(dev), eyes(eyes)
{
	if (not codec.supports_base_patch() or not codec.atlas_layout(L))
		throw std::runtime_error("nxwarp base patch: this codec has no atlas to patch");

	// HOST_VISIBLE|HOST_COHERENT and permanently mapped. The buffer is rewritten
	// every frame from a host-side decode, so a device-local buffer would need a
	// staging copy of its own and a second upload -- for a picture that is a few
	// hundred kilobytes and is not on the frame deadline.
	const VkDeviceSize bytes = VkDeviceSize(L.slot_u16) * sizeof(uint16_t);
	const VkBufferCreateInfo bi{
	        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	        .size = bytes,
	        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vkCreateBuffer(dev, &bi, nullptr, &buf) != VK_SUCCESS)
		throw std::runtime_error("nxwarp base patch: vkCreateBuffer failed");

	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(dev, buf, &req);
	const VkMemoryAllocateInfo ai{
	        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	        .allocationSize = req.size,
	        .memoryTypeIndex = find_memory(phys, req.memoryTypeBits,
	                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
	};
	if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS)
	{
		vkDestroyBuffer(dev, buf, nullptr);
		buf = VK_NULL_HANDLE;
		throw std::runtime_error("nxwarp base patch: vkAllocateMemory failed");
	}
	vkBindBufferMemory(dev, buf, mem, 0);
	void * p = nullptr;
	vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p);
	mapped = static_cast<uint16_t *>(p);
	// The padding columns each row stride carries are never written by stage()
	// and are never read by a patch -- a tile's rows stop at the eye's own
	// width. Zero once so the buffer holds no uninitialised bytes at all,
	// rather than leaving a validation-visible hole that nothing accounts for.
	std::memset(mapped, 0, size_t(bytes));
}

base_patcher::~base_patcher()
{
	if (mem)
	{
		vkUnmapMemory(dev, mem);
		vkFreeMemory(dev, mem, nullptr);
	}
	if (buf)
		vkDestroyBuffer(dev, buf, nullptr);
}

bool base_patcher::stage(const base_picture & pic)
{
	// The coded picture is the eye PAIR; the layout's plane_w is PER EYE.
	if (pic.width != L.plane_w[0] * eyes or pic.height != L.plane_h[0])
		return false;
	if (not pic.y or not pic.cb or not pic.cr)
		return false;

	for (uint32_t p = 0; p < L.planes; ++p)
	{
		const uint8_t * src = p == 0 ? pic.y : (p == 1 ? pic.cb : pic.cr);
		const size_t sstride = p == 0 ? pic.y_stride : pic.c_stride;
		const uint32_t pw = L.plane_w[p];
		const uint32_t ph = L.plane_h[p];
		uint16_t * dst = mapped + L.off[p];
		for (uint32_t e = 0; e < eyes; ++e)
		{
			for (uint32_t row = 0; row < ph; ++row)
			{
				// The source row runs across the pair, so eye e starts at
				// column e * pw of it; the destination puts that eye at
				// column e * pw of a stride-padded row. Same arithmetic on
				// both sides, which is the whole reason the API can then say
				// a tile's source address is its destination address plus an
				// offset.
				const uint8_t * s = src + row * sstride + size_t(e) * pw;
				uint16_t * d = dst + size_t(row) * L.stride[p] + size_t(e) * pw;
				// Zero-extend, not a range conversion: the base is encoded
				// full-range BT.709 precisely so that this is the identity.
				for (uint32_t x = 0; x < pw; ++x)
					d[x] = uint16_t(s[x]);
			}
		}
	}
	return true;
}

bool base_patcher::patch_run(uint32_t eye,
                             uint32_t first_tile,
                             uint32_t count,
                             uint32_t src_frame,
                             uint32_t & applied,
                             uint32_t & superseded)
{
	uint32_t a = 0, s = 0;
	const bool ok = codec.base_patch_tiles(eye, first_tile, count, buf, 0, src_frame, a, s);
	applied += a;
	superseded += s;
	return ok;
}

bool base_patcher::patch_all(uint32_t src_frame, uint32_t & applied, uint32_t & superseded)
{
	applied = 0;
	superseded = 0;
	bool ok = true;
	// One run per tile ROW per eye. A whole eye is contiguous in that eye's own
	// row-major order, so a single run of cols_per_eye * rows would also be
	// legal -- but the writes coalesce to row strips regardless, and a run per
	// row is what a partial or foveated refresh will subdivide, so the shape
	// that ships is the shape that gets exercised.
	for (uint32_t e = 0; e < eyes; ++e)
		for (uint32_t r = 0; r < L.rows; ++r)
			if (not patch_run(e, r * L.cols_per_eye, L.cols_per_eye, src_frame,
			                  applied, superseded))
				ok = false;
	return ok;
}

} // namespace wivrn
