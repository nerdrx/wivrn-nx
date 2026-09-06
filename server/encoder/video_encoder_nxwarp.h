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
#include <algorithm>
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
	// The transport's own path budget: what striper().configure_path was told, and what
	// is re-applied to the Sender a resumed session builds.
	//
	// It follows the bitrate controller. It used to be set once, at construction, from
	// the initial split -- so a session whose controller had walked the bitrate down to
	// a fifth of that still had a transport pacing as though it had the whole of the
	// original budget. The QP controller keeps the frames small either way, so nothing
	// looked broken; what was lost is that the transport's pacing and its striping
	// decisions were being made against a number nobody was at any more.
	double path_bps = 0;
	// Reconfigures the striper when the controller has moved far enough to matter.
	// Takes sender_mutex.
	void follow_path_budget();
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

	// --- frames the headset received and did not reconstruct -----------------
	//
	// Set on the network thread by on_nxwarp_frame_not_held, consumed by the next
	// encode(). See from_headset::nxwarp_frame_not_held for what the packet is; what
	// matters here is the rule:
	//
	//   a frame the headset did not reconstruct resets the receipt map, UNLESS it is
	//   older than a frame this encoder has already coded intra.
	//
	// The reset is not "zero the tiles of that frame", because a frame the headset
	// never built poisons the whole chain that predicts from it, and by the time the
	// packet has crossed the network the encoder has usually moved on -- so the tiles
	// of the named frame are no longer the tiles anything is predicting from. The only
	// statement still true when the packet lands is the general one: this headset's
	// reference is not what the encoder thinks it is. An all-zero receipt map says
	// exactly that, nxvc documents it as the way to say it, and it costs one all-intra
	// frame.
	//
	// The exception is what keeps that cost bounded, and last_resync_id below is the
	// whole of it: everything the encoder has in flight chains back to the last frame
	// it coded intra, so a report naming anything older is already answered.
	//
	// Two things therefore keep a burst of reports to one intra frame: this atomic
	// flag rather than a queue of ids, which coalesces the ones that arrive between
	// two encodes, and last_resync_id, which discards the ones that arrive after the
	// answer has already gone out.
	//
	// Falling back to an OLDER reference -- keeping frame N-2 as the prediction source
	// when the headset lost N-1 -- is what nxvc_vk_encoder_set_frame_held() now does,
	// and it is why the reports below are a QUEUE of frame ids rather than the single
	// flag they used to be. The codec needs to know WHICH frames were lost to walk
	// ref_sel out to the newest one that survives; a coalesced "something was lost"
	// can only be answered with an intra frame.
	//
	// A backend whose codec cannot take the report (nxwarp_codec::supports_frame_held
	// is false -- the CPU reference codec, whose reference distance is fixed at
	// create()) still gets the old answer: an all-zero receipt map and one intra
	// frame. That is what `held_fallback_reset` carries.
	std::mutex not_held_mutex;
	std::vector<uint16_t> not_held_ids;
	// And the POSITIVE half: the frames the headset says it reconstructed, as a newest
	// id and a backward bitmask, merged from every feedback packet since the last
	// encode. It is what lets the encoder reference a frame it KNOWS was built; the
	// negative report cannot say that, because silence means "held" and a frame
	// dropped a moment ago is also silent. Under the same mutex as the ids above: both
	// are written by the network thread and drained by the encode thread, once a
	// frame, and one lock for the pair is one lock fewer than two.
	uint16_t ack_base = 0;
	uint32_t ack_mask = 0;
	bool ack_valid = false;
	std::atomic<bool> held_fallback_reset{false};
	// Set when a report has been handed to the codec and not yet answered by a frame
	// the headset can trust. Only an all-INTRA frame answers it, and only then is the
	// resync notice worth sending: an ordinary inter frame that merely stepped ref_sel
	// out is not a point the headset may start trusting its own output from.
	bool chain_broken = false;

	// --- wire frame id to the codec's own frame number -----------------------
	//
	// The headset names a frame by its WIRE id; the codec names it by the number in
	// the bitstream's own frame header, which is what its reference ring is keyed on.
	// The two are equal in a session that starts from zero and are NOT after a resume
	// (--start-frame-id), so the pair is recorded as each frame goes out rather than
	// assumed. Sixty-four deep: a report that arrives later than that names a frame no
	// reference range can reach.
	struct wire_to_codec
	{
		uint16_t wire = 0;
		uint32_t codec = 0;
		bool used = false;
		// How many frames this encoder had sent when this one went out, and when.
		// The difference against "now" at the moment its confirmation is applied is
		// the confirmation latency in the only unit that matters -- ENCODER FRAMES,
		// because that is what ref_sel's three-frame reach is counted in -- and in
		// milliseconds, which is what says whether the feedback cadence or the
		// headset's decode is the term that dominates it.
		uint64_t sent_index = 0;
		std::chrono::steady_clock::time_point sent_at{};
		bool confirmed = false;
	};
	static constexpr size_t kFrameMapDepth = 64;
	std::array<wire_to_codec, kFrameMapDepth> frame_map{};
	// Confirmation latency, accumulated over the session and reported once at the end.
	uint64_t confirm_n = 0, confirm_frames_sum = 0, confirm_frames_max = 0;
	double confirm_ms_sum = 0, confirm_ms_max = 0;
	// The last one reported and why, for the two-second report. Written under no lock:
	// they are for a log line, and a torn read of them costs a wrong number in a log.
	std::atomic<uint16_t> last_not_held_id{0};
	std::atomic<uint8_t> last_not_held_why{0};
	std::atomic<uint64_t> not_held_total{0};
	uint64_t not_held_reported = 0;

	// --- which not-held reports are already answered -------------------------
	//
	// The wire frame id of the last frame this encoder coded with an all-zero receipt
	// map -- the last resync point -- written by the encode thread and read by the
	// network thread.
	//
	// It is what makes a not-held report cost at most one intra frame between two
	// resyncs rather than one each. A report crosses the network, so by the time it
	// lands the encoder has usually moved on several frames, and a burst of them (two
	// eyes, or the client's bounded queue discarding four frames at once and naming
	// all four) arrives spread over several encodes. Every one of those used to force
	// its own all-intra frame.
	//
	// The rule is exact rather than a heuristic. Every frame after the resync point R
	// predicts, through its predecessors, from R; R itself predicts from nothing. So a
	// report naming a frame OLDER than R names a picture nothing in flight depends on:
	// the newest reference the encoder's shadow still holds is R, R is intra, and the
	// headset can decode it whatever it did with the older frame. There is nothing
	// left to answer, and answering anyway costs an intra frame and, at 29 KB a frame
	// on a Pico 4, makes the next decode slower -- which is how the storm fed itself.
	//
	// N == R is NOT older: if the headset did not reconstruct the resync frame itself,
	// everything predicting from it is contaminated and a new one is owed.
	std::atomic<uint16_t> last_resync_id{0};
	std::atomic<bool> have_resync{false};
	// Reports the rule above discarded, for the two-second line.
	std::atomic<uint64_t> not_held_already_answered{0};
	uint64_t not_held_answered_reported = 0;

	// --- what the headset costs to decode ------------------------------------
	//
	// Microseconds per frame, as the headset measures it, off every feedback packet
	// (from_headset::nxwarp_feedback::decode_us). Written on the network thread, read
	// by the encode thread. Zero until the headset has decoded something.
	//
	// This is the number the whole pacing question turns on. The server composites at
	// 90 Hz and used to encode and send all 90 of them; a headset whose decoder takes
	// 31 ms a frame can take 32 a second, and each of the other 58 was received, thrown
	// away, reported as not held, and answered with an all-intra frame -- which is
	// larger, which makes the decode slower still.
	std::atomic<uint16_t> client_decode_us{0};

	// --- send pacing ---------------------------------------------------------
	//
	// THE PROBLEM. The compositor produces 90 frames a second and this encoder used to
	// encode and send every one of them. The Pico 4's NX Warp decoder takes 15-17 ms
	// per eye and keeps a queue of one, so it decoded about one frame in four and threw
	// the rest away -- and it is not free to throw a frame away: the headset reports
	// each one (from_headset::nxwarp_frame_not_held) and the answer is an all-intra
	// frame, which is three times the size, which makes the decode slower, which
	// throws away more. Measured on a live session: 614 frames dropped late per two
	// seconds per stream, 60 all-intra resyncs in the same window, inter prediction
	// never engaging once, every frame ~29 KB. The bytes for three frames in four were
	// spent to make the picture worse.
	//
	// THE FIX. Do not send what the headset cannot decode. The headset says what a
	// frame costs it (from_headset::nxwarp_feedback::decode_us); a composited frame
	// that arrives sooner than that cost since the last SENT frame is dropped HERE --
	// no encode, no bytes, no shadow update, no wire frame id. What the headset then
	// receives is a stream at its own rate, which it decodes whole, which means it
	// stops reporting frames it did not reconstruct, which means the encoder stops
	// coding intra and inter prediction engages, which makes the frames smaller and
	// the decode faster.
	//
	// WHY THE FRAME IDS MOVE WITH IT. `sent_frame_id` counts sent frames, not
	// composited ones. The client's reassembly window and its widening of the 16-bit
	// id (nxwarp_decoder::wire_frame_index) read a gap in the sequence as a frame that
	// was lost, and the delivery reports built from it are what WiVRn's automatic
	// bitrate calls congestion. A paced gap is not loss and must not look like it, so
	// there is no gap: the wire sequence is dense over the frames that were sent.
	enum class pace_mode_t
	{
		off,       // "pace": "off" -- send every composited frame, the old behaviour
		automatic, // "pace": "auto" -- follow the headset's reported decode cost
		fixed,     // "pace": "<fps>" -- an explicit rate, whatever the headset says
	};
	pace_mode_t pace_mode = pace_mode_t::automatic;
	// Seconds between sent frames. Starts at the fastest the band allows, so a stream
	// whose headset has not reported anything yet behaves exactly as it did before.
	double pace_interval = 1.0 / 90.0;
	// The band the controller may move in. 90 fps because there is nothing above the
	// compositor's rate to pace to, and 15 fps because a headset that cannot decode
	// 15 frames a second has a problem pacing cannot solve and slowing further only
	// makes the picture staler.
	static constexpr double pace_min_interval = 1.0 / 90.0;
	static constexpr double pace_max_interval = 1.0 / 15.0;
	std::chrono::steady_clock::time_point pace_last_sent{};
	bool pace_have_last = false;
	// Not-held reports whose reason was the decode stride, and the count the pace
	// controller has already acted on. Only the stride: it is the one reason that
	// means "you are sending faster than I can decode". A hole is the link's fault, a
	// codec refusal is the codec's, and pacing down for either would be treating a
	// different problem.
	std::atomic<uint64_t> stride_not_held{0};
	uint64_t pace_stride_seen = 0;

	// Composited frames dropped by the pace, over the two-second report window and
	// over the life of the stream.
	uint64_t prof_paced_out = 0;
	uint64_t paced_out_total = 0;

	// The frame id on the wire. Counts SENT frames: seeded from the first frame the
	// encoder actually sends and incremented once per frame that reaches the socket,
	// so the sequence the client sees has no holes in it that pacing put there.
	uint16_t sent_frame_id = 0;
	bool sent_frame_id_seeded = false;
	// Sent frames since the stream started, which is what the stream header's period
	// counts in -- a paced stream still gets a header every header_period_frames
	// frames the client can actually see.
	uint64_t sent_frames = 0;

	// Move `pace_interval` toward what the headset's reported decode cost asks for.
	// Called once per composited frame, before the admission test below.
	void run_pace_control();
	// Should this composited frame be sent? Takes the pace decision and, when the
	// answer is yes, marks `now` as the last send.
	bool pace_admit(std::chrono::steady_clock::time_point now);

	// The stream header goes out on the control (TCP) socket, because a client
	// that misses it cannot decode anything at all. Repeated periodically so a
	// decoder that was restarted mid-session recovers without a reconnect.
	uint64_t last_header_frame = 0;
	bool header_sent = false;
	static constexpr uint64_t header_period_frames = 90;

	// --- rate control -------------------------------------------------------
	//
	// NX Warp has no rate control inside the codec: a frame is coded at one
	// quantiser and the encoder never moves it. Bytes per frame are therefore
	// the only thing that decides both the link load and — because the Pico 4's
	// NX Warp decode costs about a millisecond per kilobyte of frame — the frame
	// rate the headset can actually sustain. 12 KB frames decode at 90 fps and
	// 52 KB frames at about 10. So the quantiser is not one knob among several;
	// it is the knob, and this is the loop that turns it.
	//
	// `base_qp` is where the loop starts (and, with "rc": "fixed", where it
	// stays); `current_qp` is what the next frame will actually be coded at.
	uint32_t base_qp;
	uint32_t current_qp;
	// "rc": "auto" honours the bitrate ceiling; "fixed" is the old behaviour,
	// this stream's `qp` for the whole session.
	bool rc_auto = true;
	// The band the controller may move in. Below min_qp the frames are large
	// enough to cost frame rate on the headset whatever the link can carry;
	// above max_qp the picture is not worth sending.
	uint32_t rc_min_qp = 20;
	uint32_t rc_max_qp = 44;
	// Frame rate the byte budget is per-frame OF, when the session has not told
	// the encoder a live one. From encoder_settings::fps at construction.
	float rc_fps;
	// Byte budget the last frame was measured against, and the encoder-share
	// bitrate it came from. Kept for the two-second report; 0 before the
	// bitrate controller has said anything.
	double rc_target_bytes = 0;
	uint32_t rc_bitrate = 0;
	bool logged_rc_mode = false;

	// --- an unreachable ceiling ---------------------------------------------
	//
	// The quantiser band has an end, and NX Warp's is closer than a conventional
	// codec's: every frame is intra, so there is no P-frame an order of magnitude
	// smaller than its I-frame to average the bitrate down with. A 1088x1088
	// intra frame at max-qp is a few tens of kilobytes and there is nothing below
	// it. When the ceiling asks for less than that, the controller does the only
	// thing it can — sit on max-qp — and the encode report would otherwise show a
	// large positive error forever with no indication that it is not a controller
	// that failed to converge but a target that cannot be hit.
	//
	// So the condition is named: QP pinned at max_qp with the frames still over
	// budget, held long enough not to be a scene change.
	// Counted in FRAMES rather than measured in seconds, because the condition is about
	// frames: it is "the last N frames were all over budget at the coarsest quantiser
	// there is". Counting them also makes it observable in a harness, which encodes as
	// fast as the GPU allows and would never accumulate two seconds of anything.
	uint32_t rc_over_budget_run = 0;
	bool rc_unreachable = false;
	// Two seconds' worth at 90 Hz: long enough that a busy stretch at the top of the
	// band does not get called unreachable.
	static constexpr uint32_t rc_unreachable_confirm_frames = 180;
	// The log line is rate-limited on wall time, because that is what a log line is
	// about: it is a standing condition, not an event, and the two-second encode report
	// carries it in the meantime.
	std::chrono::steady_clock::time_point rc_unreachable_logged{};
	static constexpr std::chrono::seconds rc_unreachable_repeat{30};

	// Move `current_qp` toward the byte budget implied by the bitrate share and
	// the frame rate. Called once per encoded frame with that frame's size.
	// `frame_was_intra` is the encoder's own knowledge that the frame it just
	// coded was an all-intra resync, which is what makes a fitting frame evidence
	// rather than an accident. A content scene cut is not signalled and is caught
	// by the size test instead.
	void run_rate_control(size_t last_frame_bytes, bool frame_was_intra);

	// Rolling encode timing, reported every two seconds (encode()).
	std::chrono::steady_clock::time_point prof_since = std::chrono::steady_clock::now();
	uint64_t prof_n = 0;
	double prof_ms = 0, prof_max_ms = 0;
	uint64_t prof_bytes = 0;
	// The quantiser over that interval: the sum for a mean, and the extremes,
	// which is what says whether the controller settled or is hunting.
	uint64_t prof_qp_sum = 0;
	uint32_t prof_qp_lo = 63, prof_qp_hi = 0;
	// --- the transport ceiling ----------------------------------------------
	//
	// The frame is cut into chunks of `chunk_bytes` and chunk i is placed at tile
	// index i, so the grid IS the frame's byte ceiling: tiles_per_frame *
	// chunk_bytes, about 337 kB at 17x17 tiles and a 1280-byte MTU. A frame over
	// it cannot be sent at all -- nxwarp_send_frame returns nothing and the
	// client sees a gap, which is worse than any picture.
	//
	// It is not a bitrate and it is not negotiable, so it is not left to the
	// bitrate loop to discover: the target below is capped at 0.9 of it, and a
	// frame that still lands over it moves the quantiser on its own.
	//
	// It is reachable in practice. ENTROPY_LITE at QP 22 on 1088x1088 detail
	// codes a 355 kB frame where the same picture in rANS codes 272 kB, so the
	// tool that buys the headset decode time is exactly the one that finds this
	// edge first.
	double transport_ceiling_bytes() const
	{
		return double(tiles_per_frame) * double(chunk_bytes);
	}
	// While non-zero, the quantiser floor the ceiling escape has imposed: the
	// bitrate loop may not step below it. `rc_min_qp` is what the operator asked
	// for and this is what the transport requires, and when they disagree the
	// transport wins -- a frame that does not fit is not a quality decision.
	uint32_t rc_ceiling_floor = 0;
	// The floor is released by ONE frame, but only by a frame whose fitting means
	// something: an intra frame, or any frame over rc_ceiling_large_frac of the
	// ceiling. Three consecutive SMALL frames was the first rule and it does not
	// work -- on a 48-frame pan clip the small frames are inter frames, they say
	// nothing about the next scene cut, and the floor was released straight into
	// one. Measured: 37 of 48 frames sent, three spent re-climbing at every cut.
	// An intra frame is the worst case by construction, so a intra frame that
	// fits is the evidence the floor is no longer needed.
	static constexpr double rc_ceiling_release_frac = 0.8;
	static constexpr double rc_ceiling_large_frac = 0.5;
	// The highest quantiser at which a frame has EVER overflowed this session.
	// The band below it is known bad at this resolution and content, so the
	// controller does not go back down into it: the floor never falls below
	// rc_burnt_qp - 2, which puts a scene cut one +2 step from fitting instead of
	// three. 0 until something overflows.
	uint32_t rc_burnt_qp = 0;
	static constexpr uint32_t rc_burnt_margin = 2;
	// The three floors resolved into the one the controller actually honours.
	uint32_t effective_qp_floor() const
	{
		const uint32_t burnt =
		        rc_burnt_qp > rc_burnt_margin ? rc_burnt_qp - rc_burnt_margin : 0;
		return std::max({rc_min_qp, rc_ceiling_floor, burnt});
	}
	// And the fraction the byte target is capped at, so the bitrate loop aims
	// below the ceiling rather than at it.
	static constexpr double rc_ceiling_target_frac = 0.9;

	// The oversize log line is rate-limited on wall time rather than fired once:
	// with "rc": "fixed" nothing moves the quantiser, so the condition stands and
	// a single line at the top of a session is not much use half an hour in.
	std::chrono::steady_clock::time_point oversize_logged{};
	static constexpr std::chrono::seconds oversize_repeat{2};
	// The same measurements as prof_*, but never reset, for profile().
	uint64_t prof_total_n = 0;
	double prof_total_ms = 0, prof_total_max_ms = 0;
	uint64_t prof_total_bytes = 0;

	void send_stream_header();
	// One "this frame needs no reference" notice, on the control socket.
	void send_resync_notice(uint16_t frame_id);

public:
	video_encoder_nxwarp(wivrn::vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);
	~video_encoder_nxwarp() override;

	void present_image(vk::Image y_cbcr,
	                   vk::SemaphoreSubmitInfo info,
	                   uint8_t slot,
	                   uint64_t frame_index,
	                   const to_headset::video_stream_data_shard::view_info_t & view_info) override;

	std::optional<data> encode(uint8_t slot, uint64_t frame_id) override;

	void on_nxwarp_feedback(uint8_t path_id, std::span<const uint8_t> payload,
	                        uint16_t decode_us, uint16_t held_base,
	                        uint32_t held_mask) override;
	void on_nxwarp_frame_not_held(uint16_t frame_id,
	                              from_headset::nxwarp_frame_not_held::reason why) override;

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
		// The quantiser the NEXT frame will be coded at, and the byte budget the
		// controller is aiming it at (0 with "rc": "fixed", or before a bitrate
		// has been decided). Read before an encode() they describe that frame;
		// read after it, the QP is the one the controller just moved to.
		uint32_t qp = 0;
		// The lowest quantiser the controller may currently choose: the
		// operator's min-qp, or higher when the transport ceiling is holding it
		// up. Reported because "under budget" is only a controller fault when it
		// had somewhere lower to go, and with a ceiling in play min-qp is no
		// longer the answer to that.
		uint32_t qp_floor = 0;
		double target_bytes = 0;
		// Frames the headset reported it did not reconstruct, and how many of those
		// named a frame older than one already coded intra and so cost nothing. The
		// difference is what the not-held path actually charged the stream.
		uint64_t not_held = 0;
		uint64_t not_held_already_answered = 0;
		// Composited frames the pace declined to send, and the interval it is at.
		uint64_t paced_out = 0;
		double pace_fps = 0;
		// How long the headset's CONFIRMATION of a frame takes to come back, over the
		// frames that were confirmed at all. In encoder frames, which is the unit
		// ref_sel's three-frame reach is counted in and therefore the one that decides
		// how many frames have to be coded INTRA; and in milliseconds, which says
		// whether the feedback cadence or the headset's own decode is what dominates
		// it.
		uint64_t confirms = 0;
		double confirm_frames_mean = 0, confirm_ms_mean = 0;
		uint64_t confirm_frames_max = 0;
		double confirm_ms_max = 0;
	};
	encode_profile profile() const
	{
		return {prof_total_n,
		        prof_total_ms,
		        prof_total_max_ms,
		        prof_total_bytes,
		        current_qp,
		        effective_qp_floor(),
		        rc_target_bytes,
		        not_held_total.load(std::memory_order_relaxed),
		        not_held_already_answered.load(std::memory_order_relaxed),
		        paced_out_total,
		        pace_mode == pace_mode_t::off ? 0.0 : (pace_interval > 0 ? 1.0 / pace_interval : 0.0),
		        confirm_n,
		        confirm_n ? double(confirm_frames_sum) / double(confirm_n) : 0.0,
		        confirm_n ? confirm_ms_sum / double(confirm_n) : 0.0,
		        confirm_frames_max,
		        confirm_ms_max};
	}
};

} // namespace wivrn
