/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "driver/clock_offset.h"
#include "encoder_watchdog.h"
#include "fec.h"
#include "idr_handler.h"
#include "shard_history.h"
#include "shard_pacer.h"
#include "wivrn_packets.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

struct encoder_settings;
struct vk_bundle;
class pair_compose;
class wivrn_session;

inline const char * encoder_nvenc = "nvenc";
inline const char * encoder_vaapi = "vaapi";
inline const char * encoder_x264 = "x264";
inline const char * encoder_vulkan = "vulkan";
inline const char * encoder_raw = "raw";
inline const char * encoder_nxwarp = "nxwarp";

class video_encoder
{
protected:
	struct data
	{
		video_encoder * encoder;
		std::span<uint8_t> span;
		std::shared_ptr<void> mem;
		// true if data should be sent over reliable (TCP) socket
		bool prefer_control = false;
	};

private:
	class sender
	{
		// One queue and one thread per destination socket: a stalled send on
		// the control (TCP) socket must not delay shards on the stream (UDP)
		// socket.
		struct queue
		{
			std::deque<data> pending;
			// encoder whose frame is currently being sent, if any
			video_encoder * in_flight = nullptr;
			std::jthread thread;
			// Packet pacing state, shared by every stream that drains through
			// this socket. Only the stream (UDP) queue ever uses it: the
			// control queue carries IDRs and parameter sets and is never
			// delayed. Touched by this queue's thread only.
			pacing_slot pacing;
		};

		// Maximum number of whole frames queued per socket, oldest frames are
		// dropped first on overflow
		static const size_t max_queued_frames = 8;

		std::mutex mutex;
		std::condition_variable cv;
		// [0]: stream socket, [1]: control socket
		std::array<queue, 2> queues;

		// When one frame's shards go out, and which path each of them takes.
		// Both parts are inactive whenever they do not apply.
		struct schedule
		{
			shard_pacer pacer;
			spill_scheduler spill;
		};

		void run(std::stop_token, queue &);
		// Schedule for one frame. `queued` is how many frames wait behind this
		// one on the same socket, with which it shares the pacing window.
		static schedule make_schedule(queue &, const data &, size_t queued);
		sender();

	public:
		void push(data &&);
		static std::shared_ptr<sender> get();
		void wait_idle(video_encoder *);
	};

public:
	const uint8_t stream_idx;
	// Array layer of the presented image this encoder reads. The three streams
	// that share the eye image read their own layer, the quad layer stream has an
	// image of its own and reads layer 0.
	const uint32_t src_layer;
	const uint32_t target_queue;
	// The hybrid base layer reads the EYE PAIR, brought together into one
	// side-by-side picture by the shared wivrn::pair_compose. When these are set
	// the present_image wrapper below substitutes that composed image for the
	// compositor's array image before the subclass ever sees it, so every
	// existing encoder implementation encodes the pair without knowing there is
	// such a thing as an eye. `extent` is pair-wide for such a stream, which is
	// why it is set from settings.width * eyes at construction.
	std::shared_ptr<pair_compose> composer;
	const uint32_t src_layer_right;
	const bool composes_pair = false;
	const bool need_transfer;
	// Layout the encoder needs y_cbcr to arrive in. Set during construction
	// or init() and read by the compositor on every layer_commit — must not
	// change after the first frame.
	vk::ImageLayout target_layout = vk::ImageLayout::eGeneral;
	static const uint8_t num_slots = 2;
	const double bitrate_multiplier;

	// Health of this encoder and the decision to give up on it. Fed by whichever
	// thread runs the encoder, polled by the compositor's present path — which is
	// the one still running when the encoder thread is wedged inside a driver.
	encoder_watchdog watchdog;

protected:
	// The server -> headset clock offset, refreshed at the top of every encode().
	//
	// Reachable by a subclass because a subclass can need it: the shard path fills the
	// base class's `timing_info` and converts with this, and a codec that does not go
	// through that path -- video_encoder_nxwarp carries its own on the last datagram of
	// a frame -- has to fill the same four fields in the same clock. The alternative is
	// a second copy of the conversion in the subclass, which is how two clocks drift.
	const clock_offset & headset_clock() const
	{
		return clock;
	}

	// --- the per-frame latency budget, accumulated from the headset's feedback -----
	//
	// Contiguous stages that sum to the total, in the headset clock on both ends: the
	// server converts its four stamps with clock_offset::to_headset() before they go on
	// the wire, and the headset's six are already in it. So these are differences
	// between numbers in one clock and need no offset of their own.
	//
	// Accumulated here rather than in a subclass because every field is generic -- the
	// shard path fills them and so does anything that carries a timing_info -- and a
	// subclass that wants to report them should not have to re-derive which subtraction
	// means what.
	struct latency_acc
	{
		double encode = 0, wait_send = 0, send = 0, net = 0;
		double wire = 0, queue = 0, decode = 0, present = 0, total = 0;
		uint64_t n = 0;

		void reset()
		{
			*this = {};
		}
	};
	latency_acc latency{};

	// One frame's report, taken only when every stamp it needs is present: a frame that
	// was never displayed has no `blitted`, and one whose feedback arrived before the
	// decoder finished has no `received_from_decoder`. Counting a partial one would
	// report a negative stage.
	void account_latency(const from_headset::feedback & fb)
	{
		if (not fb.encode_begin or not fb.encode_end or not fb.send_begin or
		    not fb.send_end or not fb.received_first_packet or
		    not fb.received_last_packet or not fb.sent_to_decoder or
		    not fb.received_from_decoder or not fb.blitted)
			return;
		// The headset reports a frame several times as it moves through the pipeline;
		// only the copy that has reached `blitted` is complete, and that one is last.
		auto ms = [](XrTime a, XrTime b) {
			return double(b - a) * 1e-6;
		};
		latency.encode += ms(fb.encode_begin, fb.encode_end);
		latency.wait_send += ms(fb.encode_end, fb.send_begin);
		latency.send += ms(fb.send_begin, fb.send_end);
		latency.net += ms(fb.send_begin, fb.received_first_packet);
		latency.wire += ms(fb.received_first_packet, fb.received_last_packet);
		latency.queue += ms(fb.received_last_packet, fb.sent_to_decoder);
		latency.decode += ms(fb.sent_to_decoder, fb.received_from_decoder);
		latency.present += ms(fb.received_from_decoder, fb.blitted);
		latency.total += ms(fb.encode_begin, fb.blitted);
		++latency.n;
	}

private:
	using state_t = std::atomic_unsigned_lock_free::value_type;
	static const state_t idle = 0;
	static const state_t busy = 1;
	static const state_t skip = 2;
	std::array<std::atomic_unsigned_lock_free, num_slots> state = {idle, idle};
	uint8_t present_slot = 0;
	uint8_t encode_slot = 0;
	// Images presented into this encoder that have not been encoded yet. Normally
	// one, since present and encode are called in lockstep by the compositor —
	// but an encoder that replaces a failed one is handed the stream mid-cycle and
	// must not encode a slot nothing was ever put into. Without this the two slot
	// cursors would also stay one apart for the rest of the session.
	std::atomic_int pending_presents = 0;

	std::mutex mutex;

	// temporary data
	wivrn_session * cnx = nullptr;

	// shard to send
	to_headset::video_stream_data_shard shard;

	to_headset::video_stream_data_shard::timing_info_t timing_info;
	clock_offset clock;

	// Payload bytes this frame has put on the wire so far, parity shards included: the unit
	// the bitrate is expressed in, and what the bandwidth estimating bitrate control law
	// measures its delivery rate from. Reset at the first shard of a frame, reported to the
	// session at the last one. One SendData call per NAL, so it has to be a member rather
	// than a local. Only touched under `mutex`.
	uint32_t frame_bytes = 0;
	// The same bytes split by the path they went out on, while the two are combined
	// (multipath stage 3). Reported to the session at the last shard of the frame.
	uint32_t frame_bytes_primary = 0;
	uint32_t frame_bytes_secondary = 0;
	// Byte offset within the frame of the shard about to go out: what the split point
	// is compared against. Reset at the first shard of a frame, like frame_bytes, and
	// deliberately counting data payload only — parity never enters the split.
	size_t frame_offset = 0;

	std::ofstream video_dump;

	std::shared_ptr<sender> shared_sender;

	// --- Forward error correction -------------------------------------------
	// Headset toggle, read by the sender thread. Parity is only ever produced for
	// shards that actually ride the UDP stream socket, see SendData.
	std::atomic<bool> fec_enabled = false;
	// Whether the parity ratio follows the measured loss and the groups are
	// interleaved, rather than being the fixed contiguous 8+1. Only meaningful with
	// fec_enabled.
	std::atomic<bool> fec_adaptive = false;
	// Open block of the frame being sent. Only touched under `mutex`.
	fec::group_builder fec_group;
	// Group size the next frame will be sharded and protected with. Written by the
	// network thread from the loss measurement, read by the sender thread at the
	// first shard of a frame — a frame is sharded to one size throughout, because
	// the shard payload budget is derived from it (see fec::payload_reserve).
	std::atomic<uint16_t> fec_group_size = fec::group_size;
	// The loss measurement itself, fed one sample per frame from the feedback.
	// Network thread only, but under its own lock so that a status read can join.
	std::mutex fec_rate_mutex;
	fec::rate_controller fec_rate;
	// Highest frame index the rate controller has already taken a sample from: the
	// headset sends several feedback packets per frame as it works its way through
	// the decoder, and they all carry the same loss counts.
	uint64_t fec_rate_frame = 0;
	// Bitrate the controller last asked for, before the encoder's share and the
	// parity overhead are taken out of it. Kept so that toggling FEC can re-derive
	// the encoder bitrate without waiting for the controller to move.
	std::atomic_uint32_t requested_bitrate = 0;

	void apply_bitrate();
	// Send the parity shards the open block owes and open the next one. Called with
	// `mutex` held.
	void send_parity();

	// --- Shard retransmission -----------------------------------------------
	// What this stream has just sent, and the per-frame shard counts the loss
	// measurement above is a fraction of. Thread safe in its own right.
	shard_history history;
	// Recovery blob of the shard going out, so that pushing it into the history does
	// not allocate per shard. Only touched under `mutex`.
	std::vector<uint8_t> history_blob;
	// Retransmissions this stream has sent, monotonic, for a status read
	std::atomic<uint64_t> retransmitted = 0;
	// Token bucket over the retransmissions, so that a headset asking for the world —
	// a link that has collapsed, or simply a bug — cannot turn into a second video
	// stream's worth of traffic on the path that is already losing packets.
	std::mutex retransmit_mutex;
	int64_t retransmit_window_ns = 0;
	uint32_t retransmit_in_window = 0;
	// Rate-limited reporting of the above
	uint64_t retransmit_reported = 0;
	int64_t retransmit_last_report = 0;

	// One line every retransmit_report_period at most, whatever the loss rate.
	// Takes retransmit_mutex.
	void report_retransmissions();

	// Shards one request may be answered with, and shards a second may
	static constexpr size_t max_retransmit_per_nack = 64;
	static constexpr uint32_t max_retransmit_per_second = 2000;
	// One line at most every this many nanoseconds, whatever the loss rate
	static constexpr int64_t retransmit_report_period = 10'000'000'000;

	// --- Intra refresh loss recovery ----------------------------------------
	// Whether this encoder was built with a refresh mechanism at all. Fixed for the
	// encode session: every backend that has one has to configure it when the session
	// is created, so a stream that started without it cannot gain it before a
	// reconnect. Set by the backend through enable_intra_refresh().
	bool intra_refresh_supported = false;
	// Sweep length the backend was configured for, in frames
	uint32_t intra_refresh_sweep = 0;
	// Headset toggle ANDed with the server switch, from the session. Written by the
	// network thread; only ever narrows what the backend can do.
	std::atomic<bool> intra_refresh_enabled = true;

	// --- Reference frame invalidation ---------------------------------------
	// Same shape one rung down the recovery ladder: whether this encoder has an
	// invalidation call at all (fixed for the encode session, since the DPB it needs
	// is part of that session's configuration), how deep that DPB is, and the live
	// half of the switch.
	bool ref_invalidation_supported = false;
	uint32_t ref_invalidation_dpb = 0;
	std::atomic<bool> ref_invalidation_enabled = true;

	// --- Packet pacing ------------------------------------------------------
	// Effective state: the headset toggle and the server configuration must
	// both allow it. Read by the sender thread, written by the network thread.
	std::atomic<bool> pacing_enabled = false;
	std::atomic<float> pacing_window = 0.4f;
	// Frame period the pacing window is a fraction of, from the stream rate
	std::atomic<int64_t> frame_period_ns = 0;

protected:
	std::atomic_uint32_t pending_bitrate;
	std::atomic<float> pending_framerate;
	std::unique_ptr<idr_handler> idr;
	const vk::Extent2D extent;

public:
	static std::unique_ptr<video_encoder> create(
	        wivrn::vk_bundle &,
	        const encoder_settings & settings,
	        uint8_t stream_idx);

	video_encoder(vk_bundle &, uint8_t stream_idx, uint32_t target_queue, const encoder_settings & settings, std::unique_ptr<idr_handler>, bool async_send);
	virtual ~video_encoder();

	// Where NX Warp's datagrams go when they are not going to a headset.
	//
	// SendPacket normally hands them to the wivrn_session, which is a concrete class
	// wrapped around real sockets, a crypto handshake and an xrt_system_devices — none
	// of which an in-process test can stand up. This is the one seam that lets
	// wivrn-nxwarp-e2e drive the shipping encoder: a sink here takes the datagrams
	// instead, and nothing else about the class changes. Null in the server, always.
	struct packet_sink
	{
		virtual ~packet_sink() = default;
		// The lossy path: what the UDP stream socket would carry.
		virtual void send_stream(to_headset::nxwarp_datagram &&) = 0;
		// The reliable path: the codec stream header, on TCP.
		virtual void send_control(to_headset::nxwarp_datagram &&) = 0;
	};
	void set_packet_sink(packet_sink * sink)
	{
		std::lock_guard lock(mutex);
		nxwarp_sink = sink;
	}

	// `view_info` is the pose and projection the frame was rendered for. It is the
	// same object encode() is handed one call later; a codec whose predictor warps
	// the previous reconstruction needs it at present time, before any work is
	// submitted, so it arrives here as well (INTEGRATION-DECISIONS 9).
	void present_image(vk::Image y_cbcr,
	                   vk::SemaphoreSubmitInfo sem_info,
	                   uint64_t frame_index,
	                   const to_headset::video_stream_data_shard::view_info_t & view_info);

	void on_feedback(const from_headset::feedback &);

	// The client lost its reference frames and needs to be able to start decoding
	// again: send a keyframe. It is the SAME client on the other end, with the same
	// transport state, so an encoder that carries per-client sequence numbers must not
	// disturb them here.
	virtual void reset();

	// The session was resumed and the client on the other end is a NEW one: it has
	// just been created, it knows nothing about the stream, and it cannot infer any
	// state the old one had accumulated. Everything the two cannot share -- codec
	// references, stream headers, per-client transport sequence numbers -- has to go
	// back to where it was at the start of a session.
	//
	// Called from compositor::resume(), i.e. wivrn_session::resume_session. The
	// default is a keyframe, which is all a codec whose stream carries no per-client
	// state needs; video_encoder_nxwarp overrides it because NX Warp's transport does
	// carry such state and a fresh nxt::Receiver rejects every datagram without it.
	virtual void reset_stream()
	{
		reset();
	}

	// bitrate_bps is the bitrate for the whole stream
	// the encoder bitrate will be scaled accordingly
	void set_bitrate(uint32_t bitrate_bps);
	void set_framerate(float framerate);

	// Whole-stream bitrate the controller last asked for, so that an encoder that
	// replaces this one starts where the controller had got to rather than back at
	// the value the session was configured with.
	uint32_t get_bitrate() const
	{
		return requested_bitrate;
	}

	// Spread this encoder's shards over `window` of a frame period instead of
	// handing them to the socket as fast as the kernel takes them. Live.
	void set_pacing(bool enabled, float window);

	// Send a parity shard per group of video shards so that the headset can rebuild
	// a lost one. Live, and re-derives the encoder bitrate: the parity rides the
	// same link, so the encoder gets fec::data_share of what the controller asked
	// for and the total on the wire is unchanged.
	void set_fec(bool enabled);

	// Let the parity ratio follow the loss the headset reports (16+1 on a clean link,
	// 8+1, 4+1 under heavy loss) and interleave the groups so a burst of consecutive
	// datagrams costs one erasure in several groups instead of several in one. Live;
	// off is the fixed contiguous 8+1. Only does anything with FEC on.
	void set_fec_adaptive(bool enabled);

	// Keep a short history of the shards this stream sends, so that one the headset
	// says it never received can be sent again. Live; off releases the ring, so the
	// feature costs no memory when it is not wanted.
	void set_shard_retransmit(bool enabled);

	// Rebuild the shards `n` asks for out of the history. Appended to `out` for the
	// caller to put on the wire — the connection belongs to the session, and this runs
	// on the network thread while the sender thread is inside SendData.
	//
	// Rate limited and bounded: at most max_retransmit_per_nack shards per request and
	// max_retransmit_per_second overall. Shards that have aged out of the history, that
	// went over the secondary (TCP) path, or that were never sent simply find nothing.
	void collect_retransmits(const from_headset::nack & n,
	                         std::vector<to_headset::video_stream_data_shard> & out);

	// Video shards this stream has sent again on request since it was created
	uint64_t retransmitted_shards() const
	{
		return retransmitted;
	}

	// Data shards per parity shard the next frame will use, and the loss rate that
	// chose it. The fixed group size unless the adaptive ratio is on.
	uint16_t fec_group_size_now() const
	{
		return fec_group_size;
	}

	// Repair unrecoverable loss with a rolling column of intra coded blocks spread over
	// the next few dozen frames instead of a full keyframe. Only ever narrows: the refresh
	// mechanism itself is part of the encode session's configuration and can only be turned
	// on when the encoder is created, so turning this back on mid-session does nothing until
	// the headset reconnects. Turning it off is live.
	void set_intra_refresh(bool enabled);

	// Repair loss by telling the encoder not to predict from the frame that was lost, so
	// that the next P frame references an older one the headset acknowledged. The rung
	// below intra refresh in cost: one ordinary P frame, landing on the next frame. Only
	// ever narrows, for the same reason as the refresh above — the deeper DPB it needs is
	// part of the encode session's configuration.
	void set_ref_invalidation(bool enabled);

	void encode(wivrn_session & cnx,
	            const to_headset::video_stream_data_shard::view_info_t & view_info,
	            uint64_t frame_index);

	// One NX Warp feedback packet from the headset, verbatim, on the network
	// thread. Only the NX Warp encoder does anything with it; every other backend
	// is on the shard path and hears about loss through on_feedback and nack.
	// `decode_us` is the headset's own decode cost per frame for this stream, or 0
	// before it has decoded anything; see from_headset::nxwarp_feedback::decode_us.
	// `held_base` / `held_mask` are the frames the headset has RECONSTRUCTED: the
	// newest, and a backward bitmask from it. A zero mask means none yet. See
	// from_headset::nxwarp_feedback::held_base for why the positive report has to
	// exist alongside the negative one.
	virtual void on_nxwarp_feedback(uint8_t path_id, std::span<const uint8_t> payload,
	                                uint16_t decode_us, uint16_t held_base,
	                                uint32_t held_mask) {}
	// One frame the headset received and did not reconstruct. See
	// from_headset::nxwarp_frame_not_held: it is the correction the transport's own
	// feedback structurally cannot carry, because that is sent before the decoder has
	// touched the frame.
	virtual void on_nxwarp_frame_not_held(uint16_t frame_id,
	                                      from_headset::nxwarp_frame_not_held::reason why) {}
	// The headset's own GPU decode breakdown for this stream, about twice a second.
	// Reported, never acted on: it exists so the dashboard can show where the decode
	// time goes beside the encoder's numbers. See from_headset::nxwarp_decode_profile
	// for why it is a packet of its own and not another field on the feedback.
	virtual void on_nxwarp_decode_profile(const from_headset::nxwarp_decode_profile &) {}

protected:
	// called on present to submit command buffers for the image.
	// Every backend but NX Warp ignores `view_info`.
	virtual void present_image(vk::Image y_cbcr,
	                           vk::SemaphoreSubmitInfo sem_info,
	                           uint8_t slot,
	                           uint64_t frame_index,
	                           const to_headset::video_stream_data_shard::view_info_t & view_info) = 0;

	// called when command buffer finished executing
	virtual std::optional<data> encode(uint8_t slot, uint64_t frame_index) = 0;

	// Called once by a backend that configured a rolling intra refresh over `sweep_frames`
	// frames, from its constructor. From then on the IDR handler asks for a sweep rather
	// than a keyframe when a frame is lost, until set_intra_refresh(false) says otherwise.
	void enable_intra_refresh(uint32_t sweep_frames);

	// The sweep length that was configured, for backends whose refresh has to be asked for
	// again with a length on every frame that starts one. Zero if there is no refresh.
	uint32_t intra_refresh_frames() const
	{
		return intra_refresh_sweep;
	}

	// Called once by a backend that configured its encode session with a reference DPB deep
	// enough to invalidate into, from its constructor. From then on the IDR handler tries an
	// invalidation before anything else when a frame is lost, until set_ref_invalidation(false)
	// says otherwise. `dpb_frames` bounds how far back a loss may be and still be repairable
	// this way.
	void enable_ref_invalidation(uint32_t dpb_frames);

	// `pacer` and `spill` are built by the sender thread for whole-frame sends on
	// the stream socket; default constructed ones never sleep and never split, which
	// is what the encoders that send synchronously from inside encode() (x264) get.
	void SendData(std::span<uint8_t> data, bool end_of_frame, bool control = false, shard_pacer pacer = {}, spill_scheduler spill = {});

	// Put one already-formed packet on the stream (UDP) socket, outside the shard
	// path. What NX Warp sends: its transport owns framing, FEC and pacing, so
	// there is nothing here to shard, pace or protect — only the frame byte
	// accounting the bitrate controller reads, which SendData would otherwise do.
	// Falls back to the control socket when there is no stream socket.
	void SendPacket(to_headset::nxwarp_datagram && packet, bool end_of_frame);

	// The same on the control (TCP) socket, for the one part of an NX Warp stream
	// that must not be lost: the codec's stream header.
	void SendControlPacket(to_headset::nxwarp_datagram && packet);

private:
	packet_sink * nxwarp_sink = nullptr;
};

} // namespace wivrn
