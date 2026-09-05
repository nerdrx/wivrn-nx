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

#include <array>
#include <atomic>
#include <deque>
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
	uint64_t net_holes = 0, stragglers_dropped = 0;
	// Datagrams that arrived belonging to a frame older than the newest one seen, and
	// frames that were completed by such a datagram. On a link that never reorders both
	// stay zero; on a live 90 fps session with two eye streams on one socket they do not,
	// and the second number is exactly the frames the one-frame reassembler used to lose.
	uint64_t net_out_of_order = 0, net_late_completed = 0;
	// Transport counters at the last report, so the "rx:" line carries deltas rather than
	// running totals.
	uint64_t net_tiles_placed_at = 0, net_tiles_late_at = 0;
	// Value of net_frames when the current two-second window opened.
	uint64_t net_frames_mark = 0;
	nxwarp_wire::reassemble_report last_hole;
	std::atomic<int64_t> jobs_pending = 0;
	// Deepest the worker's backlog is allowed to get before the older frames are
	// discarded in favour of the newest one. Two lets one frame decode while the
	// next waits; anything more is latency.
	static constexpr int64_t kMaxQueuedFrames = 1;
	std::atomic<uint64_t> frames_dropped_late = 0;

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

	// --- the frames in flight, network thread only
	//
	// THE WINDOW POLICY
	//
	// A small window of frames is under assembly at once and every datagram is routed to
	// its own frame by frame_id, rather than to "the frame under assembly". Keeping one
	// frame was not merely lossy, it was unstable: a straggler of frame N arriving after
	// the head of N+1 reopened N and closed N+1 with a hole, whereupon N+1's next
	// datagram reopened N+1 and closed N -- a cascade that holed 180 of 180 frames per
	// two seconds, on both eyes, over a clean Wi-Fi link. Dropping the straggler stops
	// the cascade but still throws away every frame whose tail arrives after the next
	// frame's head, which at 90 fps with two streams interleaved on one socket is common.
	//
	// Frame ids are sequential modulo 2^16, so all comparisons are on int16_t differences
	// (seq_lt below). With `newest` the highest frame id seen so far, the window is the
	// closed range [newest - (kFrameWindow - 1), newest], and:
	//
	//   * a datagram whose frame is inside the window and not yet retired is routed to
	//     that frame's entry, creating it if this is the frame's first datagram;
	//   * a datagram older than the window floor, or belonging to a frame already
	//     retired, is dropped as a straggler -- it can no longer change any outcome;
	//   * a frame closes when its last run arrives (complete), when a newer frame pushes
	//     it below the window floor (incomplete, a hole), or at flush_frames().
	//
	// Frames are always closed oldest first, so they reach the worker in frame order and
	// `retired` only ever moves forward: a frame that completes while an older one is
	// still in flight waits for it. That wait is bounded by the window -- the older frame
	// is closed the moment a frame kFrameWindow ahead of it arrives -- so a lost frame
	// costs at most kFrameWindow - 1 frames of extra latency and never a stall.
	//
	// Three is the smallest window that covers the case this exists for (the tail of N
	// arriving after the head of N+1, while N+2 has not started yet) with one frame of
	// slack. Everything else is unchanged: band_deadline still runs per band per frame and
	// send_feedback still goes out per frame with its own from_headset::feedback
	// (received_first_packet stamped when the frame's first datagram arrives), a frame
	// that closes incomplete still goes through the same drop-with-hole accounting, and
	// the worker's queue is still bounded by kMaxQueuedFrames.
	//
	//
	// THE BAND DEADLINE POLICY
	//
	// A band's deadline is the moment the receiver stops counting arrivals for it: a tile
	// placed afterwards is marked `late` and drops out of the feedback, which is how the
	// encoder learns what the headset has. Getting it wrong is quiet and expensive.
	//
	// It used to fire on the first datagram of a band, of the same frame -- the loop ran
	// `b <= last_band`, so a band closed its own deadline the instant it opened and every
	// datagram of that band after the first was late. A live headset counted 13629 late
	// tiles out of 13991 placed over two seconds on a clean link with no holes at all:
	// 97 percent of what arrived was reported as not having arrived. Closing on the *next*
	// band's first datagram instead is only slightly better, because ordinary within-frame
	// reordering puts a datagram of band b+1 in front of the rest of band b.
	//
	// So a band closes on evidence that has nothing to do with its own frame's datagram
	// order, whichever comes first:
	//
	//   * a *later frame* carrying data for band b. The sender emits frames in order, so
	//     band b of a newer frame means band b of every older frame is finished;
	//   * the clock: band b of a frame is due at that frame's first arrival plus
	//     (b + 1) / bands of the frame period, which is the deadline nx-warp
	//     docs/TRANSPORT.md 7.4 describes, anchored on the only frame-relative time the
	//     decoder has (scenes::stream owns the predicted display time, one level up);
	//   * the frame closing, which fires whatever it still owes so its feedback goes out.
	static constexpr size_t kFrameWindow = 3;

	// Sequential modulo 2^16: a is before b.
	static bool seq_lt(uint16_t a, uint16_t b)
	{
		return int16_t(uint16_t(a - b)) < 0;
	}

	struct inflight_frame
	{
		bool used = false;
		uint16_t frame_id = 0;
		// The datagram carrying the frame's last run has arrived. Not the same thing
		// as the frame being here: on a link that reorders, that datagram routinely
		// overtakes an earlier one of the same frame.
		bool have_last_run = false;
		// The frame's bytes are all here (nxwarp_wire::is_complete). Only then is it
		// ready for the worker.
		bool complete = false;
		// At least one of this frame's datagrams arrived after a newer frame's had.
		bool reordered = false;
		// The path the frame's datagrams came in on, for band_deadline's feedback.
		uint8_t path_id = 0;
		// Client-clock microseconds at the frame's first datagram: the anchor the band
		// deadlines are measured from.
		uint64_t first_us = 0;
		std::vector<std::vector<uint8_t>> slots; // one per tile index
		std::vector<uint8_t> band_fired;
		from_headset::feedback fb{};
		// view_info off the frame's first datagram (nxwarp_datagram::view_info).
		// have_view_info stays false when that datagram was the one that got lost.
		to_headset::video_stream_data_shard::view_info_t view_info{};
		bool have_view_info = false;
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

	// End of stream: close whatever is still under assembly, so a frame whose tail was
	// merely late is not counted as lost because nothing came after it to push it out.
	// Called from the same thread as push_datagram.
	void flush_frames();

	// The transport's own counters, or nullptr before the stream header has arrived.
	// Read on the network thread, or after it has stopped: tiles_placed against
	// tiles_late is what says whether the band deadlines are firing sanely. A test
	// harness also uses it to name the counter that rejected a datagram rather than
	// infer it from a black picture.
	const nxt::ReceiverStats * receiver_stats() const
	{
		return receiver ? &receiver->stats : nullptr;
	}

	// What the GUI is allowed to see. Monotonic counters plus the last completed
	// two-second profile window, all read through atomics: the readout differences
	// the counters itself rather than parsing the log lines decode_unit prints.
	struct live_stats
	{
		uint64_t frames_closed = 0;        // reassembled off the wire, decoded or not
		uint64_t frames_decoded = 0;       // handed to the render thread
		uint64_t frames_dropped_late = 0;  // refused to keep the worker's backlog short
		uint64_t frames_dropped_holes = 0; // a chunk never arrived
		uint64_t frames_dropped_codec = 0; // nxvc refused the unit
		float decode_wall_ms = 0;          // mean, over the last two-second window
		float decode_gpu_ms = 0;
		float bytes_per_frame = 0;
	};

	live_stats stats() const
	{
		return {
		        .frames_closed = net_frames.load(std::memory_order_relaxed),
		        .frames_decoded = frames_decoded.load(std::memory_order_relaxed),
		        .frames_dropped_late = frames_dropped_late.load(std::memory_order_relaxed),
		        .frames_dropped_holes = frames_dropped_holes.load(std::memory_order_relaxed),
		        .frames_dropped_codec = frames_dropped_codec.load(std::memory_order_relaxed),
		        .decode_wall_ms = prof_wall_ms.load(std::memory_order_relaxed),
		        .decode_gpu_ms = prof_gpu_ms.load(std::memory_order_relaxed),
		        .bytes_per_frame = prof_bytes.load(std::memory_order_relaxed),
		};
	}

private:
	image * get_free();
	bool on_stream_header(std::span<const uint8_t> header);
	void decode_unit(decode_job & job);
	void fire_bands_through(inflight_frame & f, uint8_t last_band);
	// The window, all on the network thread. See THE WINDOW POLICY above.
	inflight_frame * oldest_in_flight();
	inflight_frame * frame_slot(uint16_t frame_id, uint8_t path_id);
	void close_frame(inflight_frame & f);
	void close_complete_prefix();
	void evict_below_window();
	// Bands whose share of the frame period has elapsed, over every frame in flight.
	void fire_elapsed_deadlines(uint64_t now);

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
	// One frame at the negotiated rate, in microseconds. The band deadlines are fractions
	// of it; 90 Hz when the description does not say.
	uint32_t frame_period_us = 11111;

	// --- the frames in flight, network thread only. See THE WINDOW POLICY above.
	// Fixed storage, sized once from the stream header: the per-tile slot vectors keep
	// their allocation across frames, which is what the network thread wants.
	std::array<inflight_frame, kFrameWindow> window;
	bool seen_any_frame = false;
	uint16_t newest_frame = 0; // highest frame id seen
	uint16_t retired_frame = 0;
	bool any_retired = false;
	// The 64-bit frame index every from_headset::feedback for this stream carries.
	//
	// It is the SENDER's frame id, widened, and it has to be: the server's bitrate
	// controller joins the per-stream feedback of one frame by this number (its frame
	// ring keys on it), and the render thread pairs the eyes by it. A client-local
	// counter over "frames this decoder managed to reassemble" -- which is what this
	// used to be -- is neither. It advances once per surviving frame, so the instant
	// one eye loses a frame the other did not, the two eyes number every later frame
	// differently and never agree again: the eyes stop pairing ("Failed to find a
	// common frame for all decoders") and, worse, the bitrate controller starts
	// joining two DIFFERENT frames into one of its ring entries and measuring the
	// span between their arrivals as though it were one frame's time on the wire.
	// That reads as a link delivering a frame in a whole frame period, i.e. as severe
	// congestion, on a link with nothing wrong with it -- which is exactly how the
	// automatic bitrate ended up pinned at its floor and never probing back up.
	//
	// The wire id is the same number on both eyes for one compositor frame (the
	// server stamps uint16_t(frame_id) on every stream), survives a frame this
	// decoder drops, and is what every other part of this class already keys on.
	// Widening it is only about the 16-bit wrap; see wire_frame_index().
	// --- the reference chain --------------------------------------------------
	//
	// Set the moment this decoder declines to reconstruct a frame, by any route: the
	// frame is not in the codec's ring, so every later frame that warps from it warps
	// from something that was never built. Decoding those is harmless -- the ring is
	// about to be reset anyway -- but SHOWING them is the bug this exists to stop:
	// a few tiles of real picture over a field of grey.
	//
	// It is cleared by the encoder's resync notice (wivrn_packets.h, path 0xFE) and
	// not before. The headset cannot work the moment out for itself: the per-tile
	// ref_delta it can see on the wire rides the chunk mapping, so on a frame with
	// more codec tiles than chunks "every tile I was told about is intra" is not the
	// same claim as "this frame is intra", and acting on it would put the corruption
	// back on the screen occasionally instead of always.
	// The last frame id this decoder actually put through the codec, worker-thread
	// only. The ring advanced for it, so it is the one reference the NEXT frame may
	// safely predict from -- and a frame that does not continue that run is a frame
	// whose reference this decoder never built.
	//
	// The publish rule is derived from these two and nothing else, deliberately: it is
	// evaluated entirely on the worker, from a value the worker itself wrote, so there
	// is no race with the network thread to lose. A flag set at the drop sites had one
	// and it was not theoretical -- a frame's hole is only confirmed when the window
	// evicts it, three frames later, by which time the frame after it could already
	// have been decoded and shown.
	uint16_t last_decoded_id = 0;
	bool have_last_decoded = false;
	// The encoder's answers to our not-held reports: "THIS frame needs no reference".
	//
	// A set, not a single id, and matched EXACTLY. Both of those are load-bearing.
	// Exactly, because "this frame or any later one" is not what the notice says and
	// is not true: an id-or-later test passes every inter frame that follows a resync
	// and puts the corruption straight back. A set, because several notices can be in
	// flight at once -- with the worker queue discarding most frames the encoder is
	// told the client holds nothing almost every frame -- and a single slot would be
	// overwritten before the frame it names arrives, so the run would never restart.
	//
	// Bounded and oldest-out: an id that never arrives is a frame that was dropped
	// too, and the encoder has already been told about that and sent another.
	//
	// Network thread writes, worker reads and consumes; a late notice only withholds a
	// frame that could have been shown, a lost one would not, which is why it rides
	// the control socket.
	std::mutex resync_mutex;
	std::deque<uint16_t> resync_ids;
	static constexpr size_t kMaxPendingResync = 16;
	// Frames decoded and deliberately not published because of the above.
	std::atomic<uint64_t> frames_withheld{0};

	uint64_t wire_epoch = 1; // 1-based so an id behind a wrap can look one back
	uint16_t wire_newest = 0;
	bool wire_seeded = false;
	uint64_t wire_frame_index(uint16_t frame_id);


	// --- worker
	utils::sync_queue<decode_job> jobs;
	std::thread worker;

	// Counters logged when the stream ends: the only way to tell "the link is dropping
	// datagrams" from "the mapping is wrong".
	std::atomic<uint64_t> frames_decoded = 0, frames_dropped_holes = 0, frames_dropped_codec = 0;
	// Frames that decoded but whose first datagram -- the only one carrying view_info --
	// was lost and whose tiles came back through FEC. Published with a default pose.
	std::atomic<uint64_t> frames_no_view_info = 0;
	// Frames the reassembler closed, decoded or not: the rate the server is actually
	// putting on the wire, as seen from here.
	std::atomic<uint64_t> net_frames = 0;
	// Last completed two-second profile window, republished for stats() below.
	std::atomic<float> prof_wall_ms = 0, prof_gpu_ms = 0, prof_bytes = 0;
	bool warned_view_info = false;
};

} // namespace wivrn

#endif // WIVRN_USE_NXWARP
