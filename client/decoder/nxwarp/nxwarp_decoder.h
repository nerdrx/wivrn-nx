/*
 * WiVRn VR streaming — NX Warp codec
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once
#include <chrono>

// decoder_nxwarp: the NX Warp (nxvc) codec inside the WiVRn NX client.
//
// The contract with the server is the header comment of
// server/encoder/nxwarp_packetize.h in the WiVRn tree, and docs/nxwarp.md here repeats it
// from the client's side. In four steps:
//
//   1. the datagram whose path_id is 0xFF is not an nxt datagram at all: it is the codec's
//      raw stream header, sent on the control socket and repeated every 90 frames, and it
//      goes to nxvc_vk_decoder_parse_stream_header;
//   2. every other datagram goes to nxt::Receiver::on_datagram with its path_id;
//   3. nxt::Receiver::band_deadline runs per band and its bytes go back as
//      from_headset::nxwarp_feedback;
//   4. a frame's delivered tiles are concatenated in tile-index order, the 4-byte length
//      prefix on chunk 0 is stripped and checked, and the result is one .nxv frame unit.
//
// Threading, following nx-warp docs/INTEGRATION.md 2.5:
//
//   network thread                       worker thread
//   --------------                       -------------
//   push_datagram()                      nxvc_vk_decode_frame(ASYNC)
//     nxt::Receiver::on_datagram()  -->   vkCmdCopyImage into the pool image
//     copy tile bytes into the slot       publish blit_handle to scenes::stream
//     band deadlines -> feedback
//
// push_datagram() runs on WiVRn's single network thread, synchronously, next to the
// recvmmsg loop, so it does only what is safe there: depacketize, copy tile bytes (the
// receiver's spans point into scratch that the next call reuses), fire deadlines, hand
// feedback to the socket. Everything that can block on the Vulkan queue is a job on the
// worker, as android::decoder already does with its sync_queue.
//
// The output is the two-plane 4:2:0 YCbCr store of nxvc (NXVC_VKD_OUT_YCBCR420, nx-warp
// docs/INTEGRATION-DECISIONS.md 3). Its two images are copied into one
// G8_B8R8_2PLANE_420_UNORM image sampled through the same VkSamplerYcbcrConversion the
// hardware decoder path already uses, so client/shaders/reprojection.glsl needs zero
// changes: the defoveator sees an image view and a sampler and does not care where they
// came from.

#include "wivrn_config.h"

#if WIVRN_USE_NXWARP

#include "decoder/decoder.h"
#include "decoder/nxwarp/nxwarp_reassemble.h"

#include "utils/sync_queue.h"
#include "vk/allocation.h"

#include <atomic>
#include <memory>
#include <thread>

#include "nxwarp_host.h"

#include <nxvc/nxvc_vk.h>
#include <nxvc/transport/aead.h>
#include <nxvc/transport/receiver.h>

namespace wivrn
{

class nxwarp_decoder : public decoder
{
	static const int image_count = 5;

	struct image
	{
		image_allocation image;
		vk::raii::ImageView view_full = nullptr;
		vk::ImageLayout current_layout = vk::ImageLayout::eUndefined;
		std::atomic_bool free = true;
		vk::raii::Semaphore semaphore = nullptr;
		uint64_t semaphore_val = 0;
	};
	// Set when the device refuses timeline semaphores (Adreno 650): frames are then
	// fenced on the host and published with no semaphore.
	bool host_sync = false;
	// Rolling per-stream timing, reported every two seconds (decode_unit).
	struct
	{
		std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
		uint64_t n = 0;
		double wait_ms = 0, wall_ms = 0, pass_a_ms = 0, pass_b_ms = 0, gpu_ms = 0;
		uint64_t bytes = 0;
	} prof;
	// Network-thread counters for the same report.
	std::chrono::steady_clock::time_point net_since = std::chrono::steady_clock::now();
	uint64_t net_frames = 0, net_holes = 0, stragglers_dropped = 0;
	nxwarp_wire::reassemble_report last_hole;
	std::atomic<int64_t> jobs_pending = 0;
	// Deepest the worker's backlog is allowed to get before the older frames are
	// discarded in favour of the newest one. Two lets one frame decode while the
	// next waits; anything more is latency.
	static constexpr int64_t kMaxQueuedFrames = 1;
	uint64_t frames_dropped_late = 0;

	// One frame's work, complete in itself: by the time the worker runs, the tile slots
	// and the pending feedback already belong to the next frame.
	struct decode_job
	{
		std::vector<uint8_t> unit;
		from_headset::feedback fb;
		uint16_t frame_id = 0;
		bool have_view_info = false;
		// The pose the frame was rendered for, off its first datagram. Carried on the
		// job rather than read from the member state, because by the time the worker
		// runs the network thread is already assembling the next frame.
		to_headset::video_stream_data_shard::view_info_t view_info{};
	};

public:
	// The client's constructor: builds the application host below and owns it.
	nxwarp_decoder(vk::raii::Device & device,
	               vk::raii::PhysicalDevice & physical_device,
	               uint32_t vk_queue_family_index,
	               const wivrn::to_headset::video_stream_description & description,
	               uint8_t stream_index,
	               std::weak_ptr<scenes::stream> scene,
	               shard_accumulator * accumulator);

	// The same decoder against any host (see nxwarp_host.h). `host` must outlive it.
	// This is the one wivrn-nxwarp-e2e uses; the client's constructor above delegates
	// to it, so there is exactly one code path.
	nxwarp_decoder(vk::raii::Device & device,
	               vk::raii::PhysicalDevice & physical_device,
	               uint32_t vk_queue_family_index,
	               const wivrn::to_headset::video_stream_description & description,
	               uint8_t stream_index,
	               nxwarp_host & host,
	               shard_accumulator * accumulator);

	~nxwarp_decoder() override;

	// The shard path is not used: NX Warp has its own datagram type.
	void push_data(std::span<std::span<const uint8_t>> data, uint64_t frame_index, bool partial) override {}
	void frame_completed(const wivrn::from_headset::feedback &,
	                     const wivrn::to_headset::video_stream_data_shard::view_info_t &) override {}

	vk::Sampler sampler() override
	{
		return *sampler_;
	}

	// The real entry point. Called on the network thread, once per arriving packet.
	void push_datagram(to_headset::nxwarp_datagram && dg);

	static std::vector<wivrn::video_codec> supported_codecs();

private:
	image * get_free();
	bool on_stream_header(std::span<const uint8_t> header);
	void finish_frame(uint8_t path_id);
	void decode_unit(decode_job & job);
	void fire_bands_through(uint16_t frame_id, uint8_t last_band, uint8_t path_id);

	vk::raii::Device & device;
	vk::raii::PhysicalDevice & physical_device;
	uint32_t queue_family_index;
	vk::raii::SamplerYcbcrConversion ycbcr_conversion;
	vk::raii::Sampler sampler_;

	vk::raii::CommandPool command_pool;
	vk::CommandBuffer cmd;
	vk::raii::Fence fence;

	uint8_t stream_index;
	vk::Extent2D extent;

	std::array<image, image_count> image_pool;

	// Owned only when the client constructor made it; `host` is the reference actually
	// used and may point at somebody else's.
	std::unique_ptr<nxwarp_host> owned_host;
	nxwarp_host & host;
	shard_accumulator * accumulator;

	// --- codec and transport, both created when the stream header arrives
	nxvc_vk_decoder * nxvc = nullptr;
	bool nxvc_failed = false;
	std::unique_ptr<nxt::Aead> aead;
	std::unique_ptr<nxt::Receiver> receiver;
	nxt::StreamConfig cfg;
	size_t chunk = 0;

	// --- the frame under assembly, network thread only
	bool assembling = false;
	uint16_t assembling_frame = 0;
	std::vector<std::vector<uint8_t>> slots; // one per tile index
	std::vector<uint8_t> band_fired;
	from_headset::feedback fb{};
	uint64_t wivrn_frame_idx = 0;
	// view_info off the frame's first datagram (to_headset::nxwarp_datagram::view_info).
	// have_view_info stays false when that datagram was the one that got lost.
	to_headset::video_stream_data_shard::view_info_t assembling_view_info{};
	bool have_view_info = false;

	// --- worker
	utils::sync_queue<decode_job> jobs;
	std::thread worker;

	// Counters logged when the stream ends: the only way to tell "the link is dropping
	// datagrams" from "the mapping is wrong".
	uint64_t frames_decoded = 0, frames_dropped_holes = 0, frames_dropped_codec = 0;
	bool warned_view_info = false;
};

} // namespace wivrn

#endif // WIVRN_USE_NXWARP
