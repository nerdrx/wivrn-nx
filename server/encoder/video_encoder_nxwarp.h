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
#include <chrono>

#include "nxwarp_codec.h"
#include "video_encoder.h"
#include "vk/allocation.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <nxvc/transport/aead.h>
#include <nxvc/transport/sender.h>

namespace wivrn
{

// NX Warp on the WiVRn server.
//
// Shape, and why it is this shape:
//
//   present_image  remembers the pose the frame was rendered for, and waits on
//                  the compositor semaphore. What else it does depends on what
//                  the codec eats:
//
//                  * a codec that reads the image (the GPU backend) gets it as
//                    it is — no copy at all, the image stays on the queue it
//                    was drawn on, and this submission exists only to carry the
//                    semaphore wait to the fence encode() waits on;
//                  * a codec that takes host planes (the CPU reference, which
//                    cannot read a VkImage) gets video_encoder_raw's copy into
//                    host memory on the transfer queue.
//
//   encode         runs the codec — straight from the image, or from the two
//                  planes it de-interleaves out of the readback — cuts the
//                  frame into transport tiles and hands each band to
//                  nxt::Sender, whose datagrams go out one per
//                  to_headset::nxwarp_datagram.
//
// It sends synchronously from inside encode() and returns nothing, the way
// video_encoder_x264 does: the transport owns its own framing, FEC, pacing and
// path striping, so there is nothing for WiVRn's sender thread to do with the
// bytes and handing them over would only add a queue.
//
// Two things it deliberately does NOT do, both from INTEGRATION-DECISIONS:
//
//   * it is not eligible for the encoder watchdog's failover (decision 6). A
//     silent swap to VAAPI mid-session, with an NX Warp decoder on the other
//     end, is a black screen; the codec cannot be renegotiated without a
//     reconnect, so the watchdog is told to leave this encoder alone.
//   * it does not use WiVRn's shard FEC, retransmission or pacing. The codec's
//     unit is a tile run with its own header, and re-slicing it into 1400-byte
//     shards would destroy that boundary. Retransmission is kept in the sense
//     of decision 4, but it is nxt's own (the client shadow plus the reference
//     epoch), not shard_history's.
class video_encoder_nxwarp : public video_encoder
{
	vk_bundle & vk;
	vk::raii::CommandPool cmd_pool;

	// Which eye's pose feeds the predictor. Streams 0 and 1 are the eyes; the
	// alpha and quad streams borrow the left eye's, which is only used for the
	// warp matrix and is close enough for content that is not stereo anyway.
	const uint32_t eye;

	struct in_t
	{
		vk::raii::Fence fence = nullptr;
		vk::raii::CommandBuffer cmd = nullptr;
		buffer_allocation buffer;
		// The compositor image this slot was presented with, for the codec that
		// reads it directly. Not owned: it belongs to the compositor, and the
		// slot state machine is what keeps it alive and unwritten until encode()
		// is done with it.
		vk::Image image = nullptr;
		// The pose the frame in this slot was rendered for, captured at present
		// time (INTEGRATION-DECISIONS 9). The predictor needs it before any work
		// is submitted, one call earlier than encode() would give it.
		to_headset::video_stream_data_shard::view_info_t view_info{};
		bool have_view_info = false;
	};
	std::array<in_t, num_slots> in;

	// Chroma de-interleaving scratch: the compositor writes NV12 (one plane of
	// interleaved CbCr) and the codec takes planar Cb and Cr.
	std::vector<uint8_t> cb_plane;
	std::vector<uint8_t> cr_plane;

	std::unique_ptr<nxwarp_codec> codec;
	// The GPU backend submits on vk.queue and so must hold WiVRn's queue mutex
	// across encode(); the CPU one never touches a queue. See encode().
	bool codec_uses_vk_queue = false;
	// The codec reads the compositor's image itself (nxwarp_codec::accepts_image).
	// Set once from the codec, and it decides the shape of present_image, of
	// encode(), and of what this class allocates per slot.
	bool codec_reads_image = false;

	// The transport. No sockets in it: it hands back datagram buffers and this
	// class puts them on WiVRn's stream socket.
	std::unique_ptr<nxt::Aead> aead;
	std::unique_ptr<nxt::Sender> sender;
	nxt::StreamConfig stream_cfg;
	// The AEAD key material, kept because reset_stream() builds a new Sender with it.
	nxt::Key session_key{}, session_salt{};
	// What striper().configure_path was told at construction, re-applied to the Sender
	// a resumed session builds.
	double path_bps = 0;
	// Rebuilds `sender` and re-applies its configuration. Callers other than the
	// constructor hold sender_mutex across it.
	void rebuild_sender();
	// Serialises encode()'s use of the sender against feedback arriving on the
	// network thread; nxt::Sender is not internally synchronised.
	std::mutex sender_mutex;

	// Bytes of the frame carried per transport tile. See the long comment on
	// tile chunking in the .cpp: the reference codec's C ABI does not expose
	// per-tile byte offsets, so a frame is carried as an ordered run of
	// MTU-sized chunks placed on the tile grid.
	size_t chunk_bytes = 0;
	uint32_t tiles_per_frame = 0;

	// Per-tile receipt map derived from the client shadow, handed back to the
	// codec so it predicts from what the client actually holds.
	std::vector<uint8_t> received_tiles;
	uint16_t previous_frame_id = 0;
	bool have_previous_frame = false;
	// Set by reset() on the session thread, acted on by the next encode(): the client
	// holds nothing, so the frame is coded with no temporal reference.
	std::atomic<bool> client_holds_nothing{false};

	// The stream header goes out on the control (TCP) socket, because a client
	// that misses it cannot decode anything at all. Repeated periodically so a
	// decoder that was restarted mid-session recovers without a reconnect.
	uint64_t last_header_frame = 0;
	bool header_sent = false;
	static constexpr uint64_t header_period_frames = 90;

	uint32_t base_qp;
	bool logged_bitrate_note = false;
	// Rolling encode timing, reported every two seconds (encode()).
	std::chrono::steady_clock::time_point prof_since = std::chrono::steady_clock::now();
	uint64_t prof_n = 0;
	double prof_ms = 0, prof_max_ms = 0;
	uint64_t prof_bytes = 0;
	bool logged_oversize = false;
	// The same measurements as prof_*, but never reset, for profile().
	uint64_t prof_total_n = 0;
	double prof_total_ms = 0, prof_total_max_ms = 0;
	uint64_t prof_total_bytes = 0;

	void send_stream_header();

public:
	video_encoder_nxwarp(wivrn::vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);
	~video_encoder_nxwarp() override;

	void present_image(vk::Image y_cbcr,
	                   vk::SemaphoreSubmitInfo info,
	                   uint8_t slot,
	                   uint64_t frame_index,
	                   const to_headset::video_stream_data_shard::view_info_t & view_info) override;

	std::optional<data> encode(uint8_t slot, uint64_t frame_id) override;

	void on_nxwarp_feedback(uint8_t path_id, std::span<const uint8_t> payload) override;

	// The client holds nothing of the frame just sent: code the next one without a
	// temporal reference. Same client, same transport -- the sender is left alone.
	void reset() override;

	// A new client. See video_encoder::reset_stream: this is the one that also puts
	// the transport back to its initial state and resends the stream header.
	void reset_stream() override;

	// Cumulative encode timing, for a harness that wants the number rather
	// than the two-second log line: total frames, total and worst-case
	// milliseconds spent in codec->encode(), and total bytes produced. That
	// interval is the codec alone -- for the GPU backend, the plane repack
	// plus the submit and its wait -- and excludes the image readback, which
	// present_image already paid for on the transfer queue.
	struct encode_profile
	{
		uint64_t frames = 0;
		double total_ms = 0;
		double max_ms = 0;
		uint64_t bytes = 0;
	};
	encode_profile profile() const
	{
		return {prof_total_n, prof_total_ms, prof_total_max_ms, prof_total_bytes};
	}
};

} // namespace wivrn
