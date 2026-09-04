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

#pragma once

#include "vk/allocation.h"
#include "wivrn_packets.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

struct vk_bundle;

// Test-material tap on the encoder input.
//
// Off unless WIVRN_RAW_DUMP names a writable directory. When it is set, every frame
// that reaches an eye encoder is copied out of the compositor's YCbCr image into host
// memory and appended to a file, next to a JSON-lines log of the pose the frame was
// rendered for. That pair — pixels the encoder saw plus the pose that produced them —
// is what an offline codec quality harness needs and what no existing WiVRn dump gives:
// WIVRN_DUMP_VIDEO writes the *encoded* bitstream, and the pose dumps carry no picture.
//
// Deliberately not a protocol change, not a codec, and not on any path a normal session
// takes: create() returns nullptr unless the environment asks for a dump, and every
// call below is then a no-op branch on a null pointer.
//
// Environment:
//   WIVRN_RAW_DUMP=/path      directory to write into; must exist. Enables the tap.
//   WIVRN_RAW_DUMP_FRAMES=N   frames per stream, default 300. Cap, not a target.
//   WIVRN_RAW_DUMP_MAX_MB=N   total budget over every stream, default 8192 MB.
//
// Output, per stream (0 = left eye, 1 = right eye; the alpha and quad streams are not
// tapped, they are not codec test material):
//   <dir>/stream<i>.yuv        raw frames, appended, no container and no padding
//   <dir>/stream<i>.jsonl      one JSON object per frame written to that .yuv
//   <dir>/stream<i>-info.json  geometry and pixel format of the .yuv, written once
//
// Pixel format is the encoder's own input, copied plane for plane with no conversion:
// NV12 at 8 bit (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: a W*H Y plane then a W/2 x H/2
// interleaved CbCr plane) and P010 at 10 bit (VK_FORMAT_G10X6..._2PLANE_420_UNORM_3PACK16:
// the same shape in 16-bit little-endian samples whose top 10 bits carry the value).
// Neither is converted to 4:4:4 or RGB here on purpose: the chroma really is 4:2:0 at
// this point in the pipeline, so upsampling in the tap would only invent precision and
// double the bytes on the way to the disk. -info.json names the format exactly so the
// harness can do the conversion where it can be reasoned about.
class raw_dump
{
	vk_bundle & vk;
	const uint8_t stream_idx;
	const vk::Extent2D extent;
	const uint32_t src_layer;
	const vk::ImageLayout layout;
	const bool ten_bit;
	// Queue family the encoder reads on, which owns the image when the tap runs, and
	// the queue of that family the tap submits its copy on.
	const uint32_t queue_family;

	vk::raii::CommandPool cmd_pool;

	static constexpr uint8_t num_slots = 2;
	struct slot_t
	{
		vk::raii::Fence fence = nullptr;
		vk::raii::CommandBuffer cmd = nullptr;
		buffer_allocation buffer;
		// A copy was submitted into this slot and not yet written out. Frames the
		// IDR handler skips never reach present(), so a slot is not always armed.
		bool armed = false;
	};
	std::array<slot_t, num_slots> slots;

	struct file_deleter
	{
		void operator()(std::FILE * f) const
		{
			std::fclose(f);
		}
	};
	std::unique_ptr<std::FILE, file_deleter> yuv;
	std::unique_ptr<std::FILE, file_deleter> jsonl;

	vk::DeviceSize frame_bytes;
	uint64_t frames_written = 0;
	uint64_t frame_limit;
	// Set once the cap or the budget is reached, so the message is logged once and
	// nothing else is submitted or written.
	bool done = false;

	raw_dump(vk_bundle &,
	         uint8_t stream_idx,
	         vk::Extent2D,
	         uint32_t src_layer,
	         vk::ImageLayout,
	         int bit_depth,
	         uint32_t queue_family,
	         const std::string & dir);

	void finish(const char * why);

public:
	// nullptr unless WIVRN_RAW_DUMP is set and this stream can be tapped. Never throws:
	// a dump that cannot be set up logs and stays off rather than costing a session.
	//
	// Tapped streams are the two eyes, in eGeneral, read on the compositor queue family
	// or the transfer queue family. The Vulkan video encoders move the image into a
	// video-encode layout and read it on an encode queue, which this cannot copy from,
	// and the alpha and quad streams are not test material.
	static std::unique_ptr<raw_dump> create(vk_bundle &,
	                                        uint8_t stream_idx,
	                                        vk::Extent2D,
	                                        uint32_t src_layer,
	                                        vk::ImageLayout,
	                                        int bit_depth,
	                                        uint32_t queue_family);

	~raw_dump();

	// Record and submit the copy for the frame just presented, waiting on the same
	// compositor semaphore value the encoder waits on. Cheap and asynchronous.
	void present(vk::Image, vk::SemaphoreSubmitInfo, uint8_t slot);

	// Wait for that copy and append the frame and its pose. Called from the encode
	// path, where the view info exists; blocking, and slow by nature.
	void write(uint8_t slot,
	           uint64_t frame_index,
	           const to_headset::video_stream_data_shard::view_info_t &);
};

} // namespace wivrn
