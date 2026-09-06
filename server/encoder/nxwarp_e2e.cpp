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

// wivrn-nxwarp-e2e — the two ends of NX Warp, in one process, with nothing faked
// between them.
//
// The loopback tools that came before this each proved one half. wivrn-nxwarp-loopback
// drives the reference codec, the packetizer and the transport with no GPU and no WiVRn;
// nxwarp-loopback drives the client's depacketize-and-reassemble path with no server. What
// neither could do is put the shipping `video_encoder_nxwarp` and the shipping
// `nxwarp_decoder` on opposite ends of the same wire and look at the picture that comes
// out — which is the only way to catch the things that live in the seam: a pose that does
// not survive serialization, a chunk mapping the two sides disagree about, feedback the
// encoder never folds into its shadow.
//
// WHAT IS REAL HERE
//
//   * The encoder is `wivrn::video_encoder_nxwarp`, constructed the way the server
//     constructs it, fed a real `vk::Image` in the compositor's own two-plane 4:2:0 layout
//     through `present_image`, and driven by `encode`. Not a subclass, not a mock.
//   * The decoder is `wivrn::nxwarp_decoder`, the class the headset runs, adopting a real
//     VkDevice and submitting real decode work. It reaches the harness through
//     `nxwarp_host` (see client/decoder/nxwarp/nxwarp_host.h), the same interface the
//     client's own `nxwarp_application_host` implements.
//   * The wire is `to_headset::nxwarp_datagram` and `from_headset::nxwarp_feedback`, and
//     every packet is put through WiVRn's real serializer and read back out of the bytes.
//     A field that does not serialize does not arrive, exactly as on a socket.
//   * The link between them loses datagrams, on purpose, at a rate the caller picks.
//
// WHAT IS NOT REAL
//
//   There are no sockets and no threads pretending to be a network: the harness calls
//   `push_datagram` on the decoder directly from the encode loop, which is where WiVRn's
//   network thread would call it. `--reorder` permutes the delivery order across frame
//   boundaries, which is the case a live 90 fps link produces constantly, but the calls are
//   still synchronous: the datagrams of one frame arrive microseconds apart while whole
//   frames are tens of milliseconds apart, so there is no jitter and the clock half of the
//   band deadline policy is only ever exercised between frames.
//
// WHAT IT ASSERTS
//
//   1. Frames decode. A clean run publishes exactly as many frames as were presented.
//   2. The published `view_info` is bit-identical to the one that was presented, field by
//      field. This is the check that the new optional on `nxwarp_datagram` actually
//      survives serialization, and the reason this harness exists at all.
//   3. Byte identity with the reference decoder. The harness shadows the client's own
//      reassembly to rebuild the `.nxv` stream the decoder was fed, writes it out, runs
//      nx-warp's `nxv-dec` over it, and compares that YUV to what the GPU decoder actually
//      produced, plane by plane. Equal bytes means equal PSNR against the source for free;
//      it is a stronger statement than comparing two PSNR numbers, which can agree while
//      the pictures differ.
//   4. Feedback reaches the encoder shadow. The decoder's `band_deadline` output is routed
//      back into `video_encoder_nxwarp::on_nxwarp_feedback` and the encoder must actually
//      fold it in — checked by watching the concealed-tile map it derives from the shadow
//      change once loss has been reported.
//   5. Loss conceals rather than stalls. A five percent run must keep publishing frames and
//      must terminate; frames with holes are dropped, which is this backend's documented
//      behaviour (see nxwarp_packetize.h), but the stream must not wedge.
//   6. Reordering costs nothing. With no loss on the link, every frame presented must
//      reassemble whole however the datagrams were ordered -- judged on
//      nxwarp_host::on_frame_unit, because a frame that reassembled and was then discarded
//      as stale by the bounded worker queue is not a reassembly loss -- and frames must
//      reach the worker in frame order.
//   7. Tiles that arrived are counted as arrived. The transport marks a tile placed after
//      its band deadline `late` and drops it from the feedback, so the encoder is told the
//      headset does not have a tile it has. Under ten percent, or the run fails.
//
// Run:
//   wivrn-nxwarp-e2e --yuv in.yuv --width W --height H [--frames N] [--loss 0.05]
//                    [--reorder 0.05] [--first-frame 65500] [--seed S] [--nxv-out f.nxv] [--decoded-out f.yuv] [--nxv-dec PATH]
//                    [--backend ref|vk] [--qp N] [--inter on|off] [--eyes 1|2]
//                    [--intra-period N] [--coded-vectors default|none|static]
//                    [--reconnect-at N] [--start-frame-id F]
//                    [--pace auto|off|FPS] [--client-decode-ms N] [--present-hz N]
//                    [--feedback-delay N] [--effort 0|1] [--snap-identity N]
//                    [--head-rate S] [--planar off|rd|prefer]
//
//   --eyes             1 (the default, and every run that predates this flag) or 2, which
//                      is encoder_settings::eyes: ONE stream carrying BOTH eyes as a
//                      single nxvc stereo frame. The source image gains a second array
//                      layer, the encoder reads layer 0 and layer 1, and
//                      wivrn::pair_compose brings them into one picture `eyes * width`
//                      across. It needs --backend vk (the reference backend is fed one
//                      layer through host memory) and a per-eye width that is a multiple
//                      of 64 (the pair's eye boundary has to fall on a tile boundary),
//                      and both are refused up front with a message rather than a frame
//                      that quietly fails to encode.
//
//                      The right eye is the left rotated half a picture with its luma
//                      inverted, so it differs from the left at EVERY luma sample: a
//                      compose that dropped a layer, duplicated one or swapped them
//                      cannot come out looking right on any clip. Scored half by half
//                      against a side-by-side the harness composes itself.
//   --pace             the encoder's "pace" option. OFF by default here and "auto" on the
//                      server: the pace is a wall-clock decision about the interval
//                      between SENT frames, so leaving it on would make every other
//                      assertion's frame count depend on how fast this machine encodes.
//   --client-decode-ms the shipping decoder waits this long on its worker thread for
//                      every frame. A desktop GPU decodes 320x240 in about a
//                      millisecond, so without it nothing here reproduces what a Pico 4
//                      does: the decode stride, the bounded queue, and the flood of
//                      not-held reports that used to hold inter prediction off entirely.
//   --present-hz       present composited frames at this rate rather than as fast as the
//                      GPU allows, which is what the pace has to be measured against.
//   --feedback-delay   hold each not-held report back this many presented frames. Zero
//                      is this harness's own behaviour -- the report arrives in the same
//                      loop iteration that produced it, which no network does -- and it
//                      is what hides the case the encoder's last_resync_id rule is for.
//
//   --reconnect-at N   at frame N the client is thrown away and rebuilt -- new decoder,
//                      new nxt::Receiver, no memory of the stream -- while the encoder
//                      and its nxt::Sender keep running. This is what the headset app
//                      restarting does to a server session that stays up
//                      (wivrn_session::resume_session), and what the harness could not
//                      express before.
//   --start-frame-id F the other name for --first-frame: frame ids start at F instead of
//                      0. frame_id is 16 bits on the wire, so a live session crosses zero
//                      every 65536 frames -- twelve minutes at 90 Hz. Starting at 65400
//                      reaches the wrap in a few hundred frames.
//   --no-resume-notice reconnect without the reset_stream() call compositor::resume()
//                      makes, i.e. the server as it behaved before that call existed.
//                      Reproduces the failure: every datagram after the reconnect fails
//                      authentication and not one frame decodes again.
//   --drop-datagram N[:K]
//                      drop the Nth stream datagram (and K-1 after it), plus the parity
//                      of whatever FEC group they were in, and nothing else. The parity
//                      goes because class-A parity rebuilds a single lost datagram
//                      outright, and the point of this option is to measure what a loss
//                      the FEC did not cover costs -- per TILE, against the headset's own
//                      receipt map, not as a total.
//   --reorder-depth D  the deepest --reorder may push a datagram back, in datagram
//                      slots. Default 3, which reorders within a frame; past a frame's
//                      worth of datagrams it reorders ACROSS frames at the same tile
//                      position, which is what makes a tile superseded rather than late.
//   --tile-map M       "auto" (default), "spans" or "chunks": how a frame's bytes are laid
//                      on the transport's tile grid. The A/B for anything the per-tile
//                      span mapping is suspected of costing -- same clip, same QP, same
//                      frames, two mappings.
//   --tile-stream      run the arrival-order atlas model of docs/NXWARP-TILESTREAM.md
//                      section 6 beside the real decode and assert it ends in the same
//                      state as the frame-complete one over the same delivered tiles.
//                      Observes only; every other figure the run prints is unchanged.
//   --idr-at N         at frame N the encoder is asked for a keyframe
//                      (compositor::request_idr -> video_encoder::reset), with the same
//                      client still on the other end. The stream must not hiccup: an
//                      encoder that restarted its transport state here would break a
//                      session that was working.

#include "driver/bitrate_controller.h"
#include "encoder/encoder_settings.h"
#include "encoder/video_encoder_nxwarp.h"
#include "nxwarp_stats.h"
#include "utils/wivrn_vk_bundle.h"

#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include "decoder/nxwarp/nxwarp_decoder.h"
#include "decoder/nxwarp/nxwarp_host.h"
#include "decoder/nxwarp/nxwarp_reassemble.h"

#include <nxvc/transport/aead.h>
#include <nxvc/transport/receiver.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace wivrn;
namespace fs = std::filesystem;

namespace
{

int failures = 0;

void check(bool ok, const std::string & what)
{
	std::printf("%-6s %s\n", ok ? "  ok  " : " FAIL ", what.c_str());
	if (not ok)
		++failures;
}

// ---------------------------------------------------------------------------
// The presented pose, and the comparison that decides whether it arrived.
//
// Compared field by field rather than with memcmp: view_info_t holds XrPosef and XrFovf,
// which are float structs with no padding of their own, but the enclosing struct has
// optionals in it and comparing their padding would make this test lie in both directions.
// ---------------------------------------------------------------------------
using view_info_t = to_headset::video_stream_data_shard::view_info_t;

bool same(const XrPosef & a, const XrPosef & b)
{
	return a.orientation.x == b.orientation.x and a.orientation.y == b.orientation.y and
	       a.orientation.z == b.orientation.z and a.orientation.w == b.orientation.w and
	       a.position.x == b.position.x and a.position.y == b.position.y and a.position.z == b.position.z;
}

bool same(const XrFovf & a, const XrFovf & b)
{
	return a.angleLeft == b.angleLeft and a.angleRight == b.angleRight and
	       a.angleUp == b.angleUp and a.angleDown == b.angleDown;
}

bool same(const to_headset::foveation_parameter & a, const to_headset::foveation_parameter & b)
{
	return a.x == b.x and a.y == b.y;
}

bool same(const view_info_t & a, const view_info_t & b)
{
	if (a.display_time != b.display_time or a.alpha != b.alpha)
		return false;
	for (size_t i = 0; i < a.pose.size(); ++i)
	{
		if (not same(a.pose[i], b.pose[i]) or not same(a.fov[i], b.fov[i]) or
		    not same(a.foveation[i], b.foveation[i]))
			return false;
	}
	return a.quad.has_value() == b.quad.has_value();
}

// How fast the simulated head turns, as a multiple of this harness's original
// rate.  1.0 is 0.017 rad a frame -- about 87 deg/s at 90 Hz, a brisk turn --
// which is what every run here has always used and what the pose-freshness
// checks want.
//
// It is a knob because a fast head is the wrong fixture for anything that only
// happens when the head is STILL: `snap-identity` cannot fire at 87 deg/s and
// a harness that can only turn quickly can only prove that it does not.
// `--head-rate 0.05` is about 4 deg/s, the drift of someone sitting still.
float g_head_rate = 1.0f;

// A pose that is different every frame, so that a decoder republishing a stale one, or
// publishing a default, cannot pass by accident.  It stays different every frame at
// every head rate: the position and fov terms move with `t` too, and the display time
// is independent of it.
//
// --static-view: hold the view configuration still.
//
// The harness varies the FOV and the foveation runs with the frame number on purpose -- it
// is how a stale or default-constructed view_info on the wire becomes visible. That makes it
// the wrong clip for measuring anything that DEPENDS on the view configuration: the lens
// mask is recomputed whenever the foveation runs move, and runs that move every frame make
// the mask move every frame, so tiles cross in and out of it and each crossing costs a coded
// tile. A live session's runs move when the gaze does and not otherwise. This pins them so
// the A/B measures the mask rather than the harness's own churn.
bool static_view = false;

view_info_t make_view_info(uint64_t frame)
{
	const float t = float(frame) * 0.017f * g_head_rate;
	view_info_t vi{};
	vi.display_time = XrTime(1'000'000'000ll + int64_t(frame) * 11'111'111ll);
	vi.alpha = false;
	for (int eye = 0; eye < 2; ++eye)
	{
		const float s = t + float(eye) * 0.5f;
		vi.pose[eye].orientation = {std::sin(s) * 0.1f, std::cos(s) * 0.1f, 0.0f, std::sqrt(1.0f - 0.02f)};
		vi.pose[eye].position = {0.032f * (eye ? 1.f : -1.f), 1.6f + 0.01f * std::sin(s), 0.05f * std::cos(s)};
		// Only the VIEW CONFIGURATION is pinned by --static-view: the pose and the
		// display time still move, so every check that identifies a frame by its
		// view_info still can.
		vi.fov[eye] = {-0.9f - (static_view ? 0.f : 0.001f * t), 0.9f, 0.9f, -0.9f};
		// The foveation runs: source pixels per output pixel, middle entry 1:1. Made
		// to vary with the frame so a stale or default one is visible.
		vi.foveation[eye].x = {uint16_t(1 + (static_view ? 0 : frame % 3)), 4, 5, 3, 1};
		vi.foveation[eye].y = {1, 4, uint16_t(5 + eye), 3, 1};
	}
	return vi;
}

// ---------------------------------------------------------------------------
// Serialization round trip: what a socket would do to a packet.
// ---------------------------------------------------------------------------
template <typename T>
std::vector<uint8_t> to_wire(const T & packet)
{
	serialization_packet p;
	p.serialize(packet);
	std::vector<std::span<uint8_t>> & spans = p;
	std::vector<uint8_t> flat;
	for (auto & s: spans)
		flat.insert(flat.end(), s.begin(), s.end());
	return flat;
}

template <typename T>
T from_wire(std::vector<uint8_t> bytes)
{
	auto mem = std::shared_ptr<uint8_t[]>(new uint8_t[bytes.size()]);
	std::memcpy(mem.get(), bytes.data(), bytes.size());
	deserialization_packet p(mem, std::span<uint8_t>(mem.get(), bytes.size()));
	return p.deserialize<T>();
}

std::vector<uint8_t> read_file(const fs::path & p)
{
	std::vector<uint8_t> out;
	std::FILE * f = std::fopen(p.c_str(), "rb");
	if (not f)
		return out;
	std::fseek(f, 0, SEEK_END);
	out.resize(size_t(std::ftell(f)));
	std::fseek(f, 0, SEEK_SET);
	if (std::fread(out.data(), 1, out.size(), f) != out.size())
		out.clear();
	std::fclose(f);
	return out;
}

void write_file(const fs::path & p, std::span<const uint8_t> bytes)
{
	std::FILE * f = std::fopen(p.c_str(), "wb");
	if (not f)
		return;
	std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
}

double psnr(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
	if (a.size() != b.size() or a.empty())
		return -1;
	double se = 0;
	for (size_t i = 0; i < a.size(); ++i)
	{
		const double d = double(a[i]) - double(b[i]);
		se += d * d;
	}
	const double mse = se / double(a.size());
	if (mse == 0)
		return 1e9;
	return 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

// ===========================================================================
// The host the decoder runs against.
// ===========================================================================
namespace
{
// Defined below, with the rest of the Vulkan plumbing.
std::vector<uint8_t> readback(vk_bundle & vk, vk::Image image, uint32_t w, uint32_t h,
                              vk::Semaphore sem, uint64_t sem_val);

class e2e_host : public nxwarp_host
{
	vk_bundle & vk;

public:
	struct published
	{
		view_info_t view_info;
		uint64_t frame_index;
	};

	std::mutex m;
	std::condition_variable cv;
	std::vector<published> frames;
	// Feedback the decoder produced, waiting to go back to the encoder: the path it
	// reports on, its bytes, and the decode cost the headset stamped on it.
	struct feedback_packet
	{
		uint8_t path_id;
		std::vector<uint8_t> payload;
		uint16_t decode_us;
		// The frames the decoder confirmed it reconstructed, as they travelled.
		uint16_t held_base;
		uint32_t held_mask;
	};
	std::vector<feedback_packet> feedback;
	// The OTHER feedback: from_headset::feedback, the per-frame delivery report the
	// server's automatic bitrate reads. One per decoded frame and one per frame that
	// closed with a hole, which is the whole set the client sends.
	std::vector<from_headset::feedback> frame_reports;

	// The per-stage latency budget, accumulated over every frame that reached the
	// screen. One process and one clock here, so these are differences and need no
	// offset; on a real link every one of them is in the HEADSET clock, because the
	// server converts its four with clock_offset::to_headset() before they go on the
	// wire and the session converts the headset's six back the same way.
	//
	// Every field this reads was zero for nxwarp until the commit that added it: the
	// server filled no timing_info at all, and `sent_to_decoder` was stamped at publish
	// rather than at hand-off, which put the whole decode into the queue segment and
	// reported the decode itself as taking no time.
	struct stage_acc
	{
		double encode = 0;     // encode_begin  -> encode_end
		double wait_send = 0;  // encode_end    -> send_begin   (pacing + queueing)
		double send = 0;       // send_begin    -> send_end     (datagrams out)
		double net = 0;        // send_begin    -> rx_first     (first byte across)
		double span = 0;       // rx_first      -> rx_last      (frame on the wire)
		double queue = 0;      // rx_last       -> sent_to_dec  (bounded worker queue)
		double decode = 0;     // sent_to_dec   -> rx_from_dec
		double present = 0;    // rx_from_dec   -> blitted
		double total = 0;      // encode_begin  -> blitted
		size_t n = 0;
	} stages;

	void account(const from_headset::feedback & fb)
	{
		// Only a frame with both ends of every interval. A frame that was never
		// displayed has no `blitted` and would report a negative present time.
		if (not fb.encode_begin or not fb.blitted or not fb.sent_to_decoder)
			return;
		auto ms = [](XrTime a, XrTime b) { return double(b - a) * 1e-6; };
		stages.encode += ms(fb.encode_begin, fb.encode_end);
		stages.wait_send += ms(fb.encode_end, fb.send_begin);
		stages.send += ms(fb.send_begin, fb.send_end);
		stages.net += ms(fb.send_begin, fb.received_first_packet);
		stages.span += ms(fb.received_first_packet, fb.received_last_packet);
		stages.queue += ms(fb.received_last_packet, fb.sent_to_decoder);
		stages.decode += ms(fb.sent_to_decoder, fb.received_from_decoder);
		stages.present += ms(fb.received_from_decoder, fb.blitted);
		stages.total += ms(fb.encode_begin, fb.blitted);
		++stages.n;
	}
	// Every decoded picture, as yuv420p, and every .nxv unit the decoder was fed.
	std::vector<std::vector<uint8_t>> pictures;
	std::vector<std::vector<uint8_t>> units;
	// The stream frame id of each unit, so a published picture can be matched to the
	// bytes it was decoded from without assuming the frames close in step with the
	// encode loop -- under loss a frame closes late, when the next one starts arriving.
	std::vector<uint16_t> unit_frame_ids;
	uint32_t width = 0, height = 0;

	explicit e2e_host(vk_bundle & vk) :
	        vk(vk) {}

	vk::Instance instance() override
	{
		return *vk.instance;
	}

	void with_queue(const std::function<void(vk::Queue)> & fn) override
	{
		std::lock_guard lock(vk.queue.mutex);
		fn(*vk.queue.queue);
	}

	// --deterministic: a clock that counts presented frames instead of nanoseconds.
	//
	// Every timestamp that leaves this host ends up back in the encoder -- publish()
	// stamps `blitted` and `displayed` with it, and those ride the feedback packet the
	// controller and the client shadow are driven by. Read off steady_clock they are a
	// measurement of the machine's mood, and two runs of the same binary on the same
	// input disagree about them. Counted in frames they are a function of the input
	// alone, which is the whole point of the mode.
	//
	// Still nanoseconds, and still monotonic: 90 Hz is a nominal cadence, not a claim
	// about how long anything took. Nothing downstream measures a duration it then
	// reports as truth in this mode -- the latency budget the harness prints at the end
	// is measured on steady_clock directly, and says so.
	bool deterministic = false;
	std::atomic<uint64_t> det_tick{0};
	static constexpr int64_t det_period_ns = 1'000'000'000 / 90;

	XrTime now() override
	{
		if (deterministic)
			return XrTime(int64_t(det_tick.load(std::memory_order_relaxed)) * det_period_ns);
		return XrTime(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                      std::chrono::steady_clock::now().time_since_epoch())
		                      .count());
	}

	void send_feedback(uint8_t stream_index, uint8_t path_id, std::vector<uint8_t> payload,
	                   uint16_t decode_us, uint16_t held_base, uint32_t held_mask) override
	{
		// Through the real packet type and the real serializer, like everything else.
		from_headset::nxwarp_feedback fb{
		        .stream_item_idx = stream_index,
		        .path_id = path_id,
		        .payload = std::move(payload),
		        .decode_us = decode_us,
		        .held_base = held_base,
		        .held_mask = held_mask,
		};
		auto wire = to_wire(fb);
		auto back = from_wire<from_headset::nxwarp_feedback>(std::move(wire));
		std::lock_guard lock(m);
		feedback.push_back({back.path_id, std::move(back.payload), back.decode_us,
		                    back.held_base, back.held_mask});
	}

	// Wire frame ids the decoder actually put through the codec, in the order it did.
	std::vector<uint16_t> decoded_ids;

	void on_frame_decoded(uint16_t frame_id) override
	{
		std::lock_guard lock(m);
		decoded_ids.push_back(frame_id);
	}

	void on_frame_unit(uint16_t frame_id, std::span<const uint8_t> unit) override
	{
		std::lock_guard lock(m);
		units.emplace_back(unit.begin(), unit.end());
		unit_frame_ids.push_back(frame_id);
	}

	// Frames this decoder received and will not reconstruct, in arrival order, each
	// through the real packet type and the real serializer.
	std::vector<from_headset::nxwarp_frame_not_held> not_held;

	void report_frame_not_held(uint8_t stream_index, uint16_t frame_id,
	                           from_headset::nxwarp_frame_not_held::reason why) override
	{
		from_headset::nxwarp_frame_not_held p{
		        .stream_item_idx = stream_index,
		        .frame_id = frame_id,
		        .why = why,
		};
		std::lock_guard lock(m);
		not_held.push_back(from_wire<from_headset::nxwarp_frame_not_held>(to_wire(p)));
	}

	void report_frame_lost(const from_headset::feedback & fb) override
	{
		// Through the real serializer like everything else, so a field that does not
		// survive the wire does not reach the controller here either.
		auto back = from_wire<from_headset::feedback>(to_wire(fb));
		std::lock_guard lock(m);
		frame_reports.push_back(back);
		++lost_reports;
	}

	uint64_t lost_reports = 0;

	void publish(shard_accumulator *, std::shared_ptr<decoder::blit_handle> handle) override
	{
		// Read the picture back here, while the handle is still alive: releasing it
		// returns the slot to the decoder's five-image pool, and holding all of them
		// would starve the decoder after five frames.
		auto pic = readback(vk, handle->image, width, height, handle->semaphore,
		                    handle->semaphore_val ? *handle->semaphore_val : 0);
		// The render thread's half of the report, which lives in scenes::stream and
		// therefore not here: a frame this harness publishes is a frame it shows, so
		// it is stamped blitted and displayed exactly as the client stamps one it
		// puts on screen. Without this every frame would reach the controller as
		// "decoded and then dropped before it was ever shown", which is one of the
		// two things the controller calls congestion.
		auto fb = handle->feedback;
		fb.blitted = now();
		fb.displayed = fb.blitted;
		fb.times_displayed = 1;

		std::lock_guard lock(m);
		pictures.push_back(std::move(pic));
		frames.push_back({handle->view_info, handle->feedback.frame_index});
		frame_reports.push_back(from_wire<from_headset::feedback>(to_wire(fb)));
		account(frame_reports.back());
		cv.notify_all();
	}

	bool wait_for(size_t n, std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m);
		return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
	}
};

// --deterministic: block until the decoder has finished with every frame it has closed.
//
// This is the one that matters. The .nxv the harness writes is built from the frames the
// decoder CONSUMED, and the decoder's worker keeps a backlog of one: a frame that arrives
// while the previous one is still on the GPU is dropped as late. Whether that happens is a
// race between the network thread and the worker, so a twelve-frame run on this machine
// consumed nine, nine and eight frames on three consecutive tries and wrote three different
// files -- which is what made --nxv-out useless as a byte-identity vehicle.
//
// Waiting for quiescence after each frame empties the queue before the next one is pushed,
// so the backlog drop cannot fire and every closed frame is decoded, in order. The condition
// is stated in the decoder's own published counters -- every closed frame accounted for one
// way or another -- rather than as "decoded == closed", because a hole or a codec refusal is
// a legitimate end for a frame and the lossy legs of the regression set produce both.
bool wait_quiescent(wivrn::nxwarp_decoder & d, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	for (;;)
	{
		const auto st = d.stats();
		const uint64_t settled = st.frames_decoded + st.frames_dropped_late +
		                         st.frames_dropped_holes + st.frames_dropped_codec +
		                         st.frames_withheld;
		if (settled >= st.frames_closed)
			return true;
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
}

// ===========================================================================
// The lossy link.
// ===========================================================================
class lossy_link : public video_encoder::packet_sink
{
	// A pointer, not a reference, because --reconnect-at replaces the decoder under a
	// running encoder: that is the whole point of the option, and it is exactly what a
	// headset app restart does to the server's sink.
	nxwarp_decoder * dec;
	std::mt19937 rng;
	double loss;
	// Fraction of datagrams held back by one to three datagram slots. At ~7 datagrams
	// per frame that lands a held datagram behind the head of the *next* frame about
	// three times in seven, which is the case a one-frame reassembler cannot survive:
	// the tail of frame N arrives after the head of N+1. On the wire this is what two
	// eye streams interleaved on one socket, plus any path reordering, actually do.
	double reorder;
	// The deepest a held datagram may be pushed back, in datagram slots. Three is what
	// this link has always done and is the shape of a path that reorders WITHIN a frame.
	// Reaching the src_frame monotonicity rule of docs/NXWARP-TILESTREAM.md section 2
	// needs more than that: a tile of frame N has to arrive after a tile of frame N+1 AT
	// THE SAME POSITION, which at six or seven datagrams per frame means a datagram has
	// to fall a whole frame behind, not a few slots.
	uint32_t reorder_depth = 3;
	// Datagrams waiting for their slot to come up: {slot at which it is released, packet}.
	std::vector<std::pair<uint64_t, to_headset::nxwarp_datagram>> held;
	uint64_t slot_index = 0;

public:
	uint64_t sent = 0, dropped = 0, delayed = 0;
	// Data datagrams alone -- the ones carrying tiles. --drop-datagram is indexed on
	// this and not on `sent`; see the note where it is used.
	uint64_t data_sent = 0;
	// One tile run as it travels: a datagram carries `count` CONSECUTIVE tile indices
	// starting at `first`, of one frame. That is the transport's own unit -- the
	// datagram header says so in three fields -- and it is the unit every claim below
	// is made in. Under stereo the indices are over the eye PAIR grid
	// (common/nxwarp_stream_grid.h), so nothing here has to know how many eyes there
	// are: an index names a tile of the pair and that is all it has to name.
	struct tile_run
	{
		uint16_t frame_id = 0;
		uint32_t first = 0;
		uint32_t count = 0;
	};

	// --drop-datagram: the 1-based index of the first stream datagram to drop, how many
	// consecutive ones go with it, and what they were carrying when they went.
	uint64_t drop_one = 0;
	uint32_t drop_count = 1;
	uint32_t dropped_tiles = 0;
	std::vector<tile_run> dropped_runs;
	// Parity datagrams suppressed along with them: see the note in send_stream. Counted
	// separately because a parity datagram carries no tile of its own, so it changes
	// nothing about WHAT was lost -- only about whether the loss could be repaired.
	uint64_t dropped_parity = 0;
	// The FEC groups the dropped datagrams belonged to, as (frame_id << 8) | group.
	std::set<uint32_t> dropped_groups;
	// Transport tiles the sender put on the link, over every datagram it offered --
	// dropped ones included -- and the runs themselves.
	uint64_t tiles_offered = 0;
	std::vector<tile_run> offered_runs;
	// Runs the link actually handed the decoder, IN DELIVERY ORDER. This is what the
	// arrival-order model of docs/NXWARP-TILESTREAM.md section 6 is fed; it is the
	// order the datagrams reached the client, reordering and all.
	bool record_delivery = false;
	std::vector<tile_run> delivered_runs;

	// The datagram header the sender wrote, read back out of the payload. It is in the
	// clear (the AEAD covers the tile bytes, not the routing), which is why the link can
	// account for tile runs at all without holding the session key.
	static bool header_of(const to_headset::nxwarp_datagram & p, nxt::DatagramHeader * h)
	{
		return p.payload.size() >= 24 and nxt::decode_header(p.payload.data(), h);
	}

	// Transport tiles in one datagram. tile_count 0 IS the parity marker, so this
	// returns 0 for parity too, which is right: a parity datagram carries no tile of
	// its own.
	static uint32_t count_tiles(const to_headset::nxwarp_datagram & p)
	{
		nxt::DatagramHeader h{};
		return header_of(p, &h) ? uint32_t(h.tile_count) : 0;
	}
	// The stream header, and the shadow reassembly of every frame, so the harness can
	// rebuild the exact .nxv the decoder was fed.
	std::vector<uint8_t> stream_header;
	std::vector<std::vector<uint8_t>> raw_datagrams;
	// How many stream headers the encoder has put on the control socket, and at which
	// datagram count the last one went out. A resumed session that does not resend the
	// header leaves a fresh decoder with no receiver at all.
	uint64_t headers_sent = 0;
	// Resync notices (path 0xFE): one per frame the encoder coded with an all-zero
	// receipt map, i.e. one per frame it was forced to code entirely INTRA because the
	// headset said it had not reconstructed something. Counting them is the direct
	// measure of whether inter prediction is engaging at all -- bytes per frame depend
	// on the clip, this does not.
	uint64_t resync_notices = 0;

	lossy_link(nxwarp_decoder & dec, double loss, double reorder, uint32_t seed,
	           uint32_t reorder_depth) :
	        dec(&dec), rng(seed), loss(loss), reorder(reorder),
	        reorder_depth(reorder_depth) {}

	// Point the link at the decoder that replaced the old one.
	void retarget(nxwarp_decoder & d)
	{
		dec = &d;
	}

	void send_control(to_headset::nxwarp_datagram && packet) override
	{
		// The control socket is TCP: nothing is lost on it, and the stream header must
		// not be, or nothing decodes at all.
		if (packet.path_id == 0xFF)
		{
			stream_header = packet.payload;
			++headers_sent;
		}
		if (packet.path_id == to_headset::nxwarp_resync_path)
			++resync_notices;
		deliver(std::move(packet));
	}

	void send_stream(to_headset::nxwarp_datagram && packet) override
	{
		++sent;
		nxt::DatagramHeader h{};
		const bool have_header = header_of(packet, &h);
		if (have_header and h.tile_count)
		{
			tiles_offered += h.tile_count;
			offered_runs.push_back({h.frame_id, h.tile_first, h.tile_count});
		}
		// --drop-datagram counts DATA datagrams, not everything on the link. Counting
		// everything made the option's meaning depend on where the parity fell: "drop
		// the 20th" would silently drop a parity block instead, which costs no tile,
		// and the run would then pass every check by having lost nothing at all.
		if (have_header and h.tile_count)
			++data_sent;
		const uint64_t slot = slot_index++;
		release_due(slot);
		// One named datagram, dropped and nothing else: see --drop-datagram.
		if (drop_one and have_header and h.tile_count and data_sent >= drop_one and
		    data_sent < drop_one + drop_count)
		{
			++dropped;
			if (have_header and h.tile_count)
			{
				dropped_tiles += h.tile_count;
				dropped_runs.push_back({h.frame_id, h.tile_first, h.tile_count});
				if (h.fec_k)
					dropped_groups.insert(uint32_t(h.frame_id) << 8 | h.fec_group);
			}
			return;
		}
		// The parity of a group one of those datagrams was in goes with it.
		//
		// Not to make the loss worse -- a parity datagram carries no tile, so dropping
		// it costs no receipt -- but to make it REAL. Class-A parity rebuilds a single
		// lost datagram outright, so with the parity delivered the measurement below
		// would be of the FEC succeeding, which it already is elsewhere. The claim
		// being made here is about what a loss the FEC did not cover costs, and this
		// is what reaches past the FEC to it.
		if (have_header and h.is_parity() and
		    dropped_groups.count(uint32_t(h.frame_id) << 8 | h.fec_group))
		{
			++dropped;
			++dropped_parity;
			return;
		}
		if (loss > 0 and std::uniform_real_distribution<double>(0, 1)(rng) < loss)
		{
			++dropped;
			return;
		}
		if (reorder > 0 and std::uniform_real_distribution<double>(0, 1)(rng) < reorder)
		{
			++delayed;
			const uint64_t d = 1 + (uint64_t(rng()) % std::max<uint32_t>(1, reorder_depth));
			held.emplace_back(slot + d, std::move(packet));
			return;
		}
		raw_datagrams.push_back(packet.payload);
		deliver(std::move(packet));
	}

	// The link has nothing more to carry: everything still held goes now, in the order
	// it was due. Without this the last few frames of a reordered run would be judged
	// on datagrams the harness never handed over, which says nothing about the decoder.
	void flush()
	{
		std::stable_sort(held.begin(), held.end(),
		                 [](const auto & a, const auto & b) { return a.first < b.first; });
		auto pending = std::move(held);
		held.clear();
		for (auto & [due, packet]: pending)
		{
			raw_datagrams.push_back(packet.payload);
			deliver(std::move(packet));
		}
	}

private:
	// Everything whose slot has come up, oldest due first. A datagram held at slot s with
	// a delay of d is released while slot s+d+1 is being handled, so exactly d datagrams
	// that were behind it on the wire go out in front of it. d=1 is a swap with the next
	// datagram; d=3 puts it three behind, which at six or seven datagrams per frame is
	// past the head of the following frame whenever it was near the end of its own.
	void release_due(uint64_t slot)
	{
		for (size_t i = 0; i < held.size();)
		{
			if (held[i].first >= slot)
			{
				++i;
				continue;
			}
			auto packet = std::move(held[i].second);
			held.erase(held.begin() + long(i));
			raw_datagrams.push_back(packet.payload);
			deliver(std::move(packet));
		}
	}

	void deliver(to_headset::nxwarp_datagram && packet)
	{
		if (record_delivery)
		{
			nxt::DatagramHeader h{};
			if (header_of(packet, &h) and h.tile_count)
				delivered_runs.push_back({h.frame_id, h.tile_first, h.tile_count});
		}
		// Real serializer, both directions. A field that does not survive this does not
		// reach the decoder, which is the whole point of routing it through here.
		auto wire = to_wire(packet);
		auto back = from_wire<to_headset::nxwarp_datagram>(std::move(wire));
		dec->push_datagram(std::move(back));
	}
};

// ===========================================================================
// The arrival-order model (docs/NXWARP-TILESTREAM.md sections 2 and 6).
// ===========================================================================
//
// Not a decoder. It is the ATLAS BOOKKEEPING of SYNTAX 13.12 with the pixels taken out,
// which is the half the ordering rule lives in, and it exists so that the rule can be
// tested before nxvc_vk_decode_tiles exists to test it against. What it keeps per tile
// position is exactly what 13.12.3 step 3 writes back -- `src_frame`, `gen`, and the
// composed `C` -- with `C` modelled as the ORDERED SEQUENCE of H steps folded into it.
//
// The fold is what makes the comparison mean something. 13.12.2 builds the composition
// "one step at a time by right-multiplication", so two paths agree on C if and only if
// they performed the same steps in the same order; a hash of that sequence has the same
// property and can be compared with memcmp. An integer-matrix C would test the same claim
// with more code and no more evidence, since the matrices themselves are nx-warp's to
// verify and are pinned by its own conformance tests.
//
// Two ways of driving it, the same state either way:
//
//   * apply_ordered  -- the frame-complete path. A frame's delivered tiles are applied
//                       together, in frame order, after an EAGER advance of every valid
//                       entry by H_N. This is what today's client does, and what a
//                       whole-frame decode call does.
//   * apply_arrival  -- the tile-streaming path. Each run is applied when it arrives,
//                       with the LAZY advance of section 2 and the src_frame
//                       monotonicity rule, and a run that would move a position
//                       backwards is `superseded` rather than lost.
//
// The proof obligation is that the two end in the same state. Section 6 states it as
// byte-identity of the atlas, and that is what `operator==` here is.
struct atlas_model
{
	struct entry
	{
		// -1 for a position no coded tile has ever landed on.
		int64_t src_frame = -1;
		int64_t advanced_to = -1;
		uint32_t gen = 0;
		// FNV-1a over the sequence of H indices folded into C since it was last
		// reset to the identity. kIdentity is C == I.
		uint64_t c = kIdentity;

		static constexpr uint64_t kIdentity = 1469598103934665603ull;

		bool operator==(const entry &) const = default;
	};

	std::vector<entry> tiles;
	// A run dropped by 13.12.3 step 3: the position already holds a NEWER generation
	// than the tile that arrived. Not a loss, and the reason the encoder is told about
	// it is that "not the one I sent" has two causes and only the report separates them.
	uint64_t superseded_runs = 0, superseded_tiles = 0;
	uint64_t applied_runs = 0, applied_tiles = 0;

	explicit atlas_model(size_t n) :
	        tiles(n) {}

	static uint64_t fold(uint64_t c, int64_t frame)
	{
		for (int i = 0; i < 8; ++i)
		{
			c ^= uint64_t(uint8_t(uint64_t(frame) >> (8 * i)));
			c *= 1099511628211ull;
		}
		return c;
	}

	// 13.12.2: one right-multiplication per frame step, in order.
	void advance_to(entry & e, int64_t frame)
	{
		while (e.advanced_to < frame)
		{
			++e.advanced_to;
			e.c = fold(e.c, e.advanced_to);
			++e.gen;
		}
	}

	void decode_into(entry & e, int64_t frame)
	{
		e.c = entry::kIdentity;
		e.src_frame = frame;
		e.advanced_to = frame;
		e.gen = 0;
	}

	// The frame-complete path. Every valid entry is advanced by H_N whether a tile of
	// frame N lands on it or not, which is the eager form 13.12.2 is written in.
	void apply_ordered(int64_t frame, const std::vector<uint32_t> & indices)
	{
		for (auto & e: tiles)
		{
			if (e.src_frame >= 0)
				advance_to(e, frame);
		}
		for (uint32_t t: indices)
		{
			if (t >= tiles.size())
				continue;
			decode_into(tiles[t], frame);
			++applied_tiles;
		}
		++applied_runs;
	}

	// The tile-streaming path. One arriving run, applied now.
	void apply_arrival(int64_t frame, uint32_t first, uint32_t count)
	{
		bool any = false, all_superseded = true;
		for (uint32_t t = first; t < first + count and t < tiles.size(); ++t)
		{
			auto & e = tiles[t];
			any = true;
			if (e.src_frame >= frame)
			{
				++superseded_tiles;
				continue;
			}
			all_superseded = false;
			advance_to(e, frame);
			decode_into(e, frame);
			++applied_tiles;
		}
		if (not any)
			return;
		if (all_superseded)
			++superseded_runs;
		else
			++applied_runs;
	}

	// Bring every entry up to the same frame, so that two paths that stopped advancing
	// at different points can be compared at all. Advancing forward is always legal --
	// it is UN-advancing that section 2 shows is not bit-exact, and neither path ever
	// does it.
	void flush(int64_t frame)
	{
		for (auto & e: tiles)
		{
			if (e.src_frame >= 0)
				advance_to(e, frame);
		}
	}

	bool same_state_as(const atlas_model & other) const
	{
		return tiles == other.tiles;
	}

	size_t first_difference(const atlas_model & other) const
	{
		for (size_t i = 0; i < tiles.size() and i < other.tiles.size(); ++i)
		{
			if (not(tiles[i] == other.tiles[i]))
				return i;
		}
		return size_t(-1);
	}
};

// The 16-bit frame id on the wire, unwrapped into a monotonically increasing number.
// src_frame monotonicity is a comparison, so it cannot be run on a counter that wraps:
// after the wrap every arriving frame would look older than the atlas and every tile
// would be reported superseded. --first-frame 65500 is the run that finds this.
struct frame_id_unwrap
{
	int64_t base = 0;
	uint16_t last = 0;
	bool started = false;

	int64_t operator()(uint16_t id)
	{
		if (not started)
		{
			started = true;
			last = id;
			return base + id;
		}
		if (uint16_t(id - last) < 0x8000)
		{
			if (id < last)
				base += 0x10000;
		}
		else if (id > last)
		{
			base -= 0x10000;
		}
		last = id;
		return base + id;
	}
};

} // namespace

// ===========================================================================
// Uploading a YUV420p frame into the compositor's two-plane image.
// ===========================================================================
namespace
{
struct source_image
{
	vk::raii::Image image = nullptr;
	vk::raii::DeviceMemory memory = nullptr;
	vk::raii::Buffer staging = nullptr;
	vk::raii::DeviceMemory staging_memory = nullptr;
	vk::raii::CommandPool pool = nullptr;
	vk::raii::CommandBuffer cmd = nullptr;
	vk::raii::Semaphore done = nullptr;
	void * staging_map = nullptr;
	uint32_t w = 0, h = 0;
	// Array layers, which is how WiVRn keeps the two eyes: one image, eye 0 in
	// layer 0 and eye 1 in layer 1. One layer is the mono stream and is what
	// every run of this harness did before `--eyes 2` existed; two is the shape
	// the paired encoder reads, and the only shape in which pair_compose is
	// exercised at all.
	uint32_t layers = 1;
};

uint32_t find_memory(vk_bundle & vk, uint32_t bits, vk::MemoryPropertyFlags want)
{
	auto props = vk.physical_device.getMemoryProperties();
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
	{
		if ((bits & (1u << i)) and (props.memoryTypes[i].propertyFlags & want) == want)
			return i;
	}
	throw std::runtime_error("no suitable memory type");
}

source_image make_source_image(vk_bundle & vk, uint32_t w, uint32_t h, uint32_t layers = 1)
{
	source_image s;
	s.w = w;
	s.h = h;
	s.layers = layers;

	// The exact image the compositor hands the encoder: one Y plane, one
	// interleaved CbCr plane at half resolution — and created the way
	// server/compositor/compositor.cpp creates its own, which is the part that
	// matters here. Mutable format, extended usage, storage usage and a format
	// list naming the UINT plane views is what makes the GPU encoder's E0 able
	// to read it; an image created with `flags = 0` and no eStorage is one the
	// direct path cannot take, so the harness would silently exercise the
	// readback path instead and prove nothing about the one that ships.
	const std::array formats{
	        vk::Format::eR8Unorm,
	        vk::Format::eR8G8Unorm,
	        vk::Format::eR8Uint,
	        vk::Format::eR8G8Uint,
	        vk::Format::eG8B8R82Plane420Unorm,
	};
	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat,
	                .imageType = vk::ImageType::e2D,
	                .format = formats.back(),
	                .extent = {.width = w, .height = h, .depth = 1},
	                .mipLevels = 1,
	                .arrayLayers = layers,
	                .samples = vk::SampleCountFlagBits::e1,
	                .tiling = vk::ImageTiling::eOptimal,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst |
	                         vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
	                .sharingMode = vk::SharingMode::eExclusive,
	                .initialLayout = vk::ImageLayout::eUndefined,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = formats.size(),
	                .pViewFormats = formats.data(),
	        },
	};
	s.image = vk::raii::Image(vk.device, image_info.get());

	auto req = s.image.getMemoryRequirements();
	s.memory = vk::raii::DeviceMemory(
	        vk.device,
	        vk::MemoryAllocateInfo{
	                .allocationSize = req.size,
	                .memoryTypeIndex = find_memory(vk, req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
	        });
	s.image.bindMemory(*s.memory, 0);

	const vk::DeviceSize bytes = vk::DeviceSize(w) * h * 3 / 2 * layers;
	s.staging = vk::raii::Buffer(
	        vk.device,
	        vk::BufferCreateInfo{.size = bytes, .usage = vk::BufferUsageFlagBits::eTransferSrc});
	auto sreq = s.staging.getMemoryRequirements();
	s.staging_memory = vk::raii::DeviceMemory(
	        vk.device,
	        vk::MemoryAllocateInfo{
	                .allocationSize = sreq.size,
	                .memoryTypeIndex = find_memory(vk, sreq.memoryTypeBits,
	                                               vk::MemoryPropertyFlagBits::eHostVisible |
	                                                       vk::MemoryPropertyFlagBits::eHostCoherent),
	        });
	s.staging.bindMemory(*s.staging_memory, 0);
	s.staging_map = s.staging_memory.mapMemory(0, bytes);

	s.pool = vk::raii::CommandPool(
	        vk.device,
	        vk::CommandPoolCreateInfo{
	                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
	                .queueFamilyIndex = vk.queue.family_index,
	        });
	s.cmd = std::move(vk::raii::CommandBuffers(
	        vk.device,
	        vk::CommandBufferAllocateInfo{.commandPool = *s.pool, .commandBufferCount = 1})[0]);
	s.done = vk::raii::Semaphore(vk.device, vk::SemaphoreCreateInfo{});
	return s;
}

// Copy `s.layers` yuv420p frames in — one per array layer, laid out back to back in
// `yuv` — converting the two chroma planes to the interleaved layout the two-plane image
// wants, and leave the image in eGeneral — which is the layout video_encoder_nxwarp's own
// copyImageToBuffer reads it from.
void upload(vk_bundle & vk, source_image & s, std::span<const uint8_t> yuv)
{
	const size_t y_size = size_t(s.w) * s.h;
	const size_t c_size = y_size / 4;
	const size_t frame_bytes = y_size + 2 * c_size;
	auto * dst = (uint8_t *)s.staging_map;
	for (uint32_t layer = 0; layer < s.layers; ++layer)
	{
		const uint8_t * in = yuv.data() + size_t(layer) * frame_bytes;
		uint8_t * out = dst + size_t(layer) * frame_bytes;
		std::memcpy(out, in, y_size);
		const uint8_t * cb = in + y_size;
		const uint8_t * cr = cb + c_size;
		uint8_t * uv = out + y_size;
		for (size_t i = 0; i < c_size; ++i)
		{
			uv[2 * i] = cb[i];
			uv[2 * i + 1] = cr[i];
		}
	}

	s.cmd.reset();
	s.cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	vk::ImageMemoryBarrier to_dst{
	        .srcAccessMask = {},
	        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
	        .oldLayout = vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eTransferDstOptimal,
	        .image = *s.image,
	        .subresourceRange = {vk::ImageAspectFlagBits::ePlane0 | vk::ImageAspectFlagBits::ePlane1, 0, 1, 0, s.layers},
	};
	s.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
	                      {}, {}, {}, to_dst);

	std::vector<vk::BufferImageCopy> regions;
	regions.reserve(2 * s.layers);
	for (uint32_t layer = 0; layer < s.layers; ++layer)
	{
		const vk::DeviceSize base = vk::DeviceSize(layer) * frame_bytes;
		regions.push_back(vk::BufferImageCopy{
		        .bufferOffset = base,
		        .imageSubresource = {vk::ImageAspectFlagBits::ePlane0, 0, layer, 1},
		        .imageExtent = {s.w, s.h, 1},
		});
		regions.push_back(vk::BufferImageCopy{
		        .bufferOffset = base + y_size,
		        .imageSubresource = {vk::ImageAspectFlagBits::ePlane1, 0, layer, 1},
		        .imageExtent = {s.w / 2, s.h / 2, 1},
		});
	}
	s.cmd.copyBufferToImage(*s.staging, *s.image, vk::ImageLayout::eTransferDstOptimal, regions);

	vk::ImageMemoryBarrier to_general{
	        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = *s.image,
	        .subresourceRange = {vk::ImageAspectFlagBits::ePlane0 | vk::ImageAspectFlagBits::ePlane1, 0, 1, 0, s.layers},
	};
	s.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
	                      {}, {}, {}, to_general);
	s.cmd.end();

	// Signal the semaphore present_image will wait on, exactly as the compositor does.
	std::lock_guard lock(vk.queue.mutex);
	const vk::CommandBuffer up_cmd = *s.cmd;
	vk::SubmitInfo si{
	        .commandBufferCount = 1,
	        .pCommandBuffers = &up_cmd,
	        .signalSemaphoreCount = 1,
	        .pSignalSemaphores = &*s.done,
	};
	vk.queue.queue.submit(si);
}

// ---------------------------------------------------------------------------
// The second eye, and the picture the compose owes back.
//
// A harness that filled both array layers with the same frame would pass with a
// pair_compose that dropped a layer, duplicated one, or swapped them -- the three
// ways that code can be wrong and the only reasons to run it here at all. So the
// right eye is DERIVED from the left by a transform that no half of a wrongly
// composed picture can accidentally satisfy:
//
//   * every row is rotated left by half a picture, which catches a right half
//     placed at the wrong x offset as well as one taken from the wrong layer;
//   * the luma is then inverted. 255 - y == y has no integer solution, so EVERY
//     luma sample of the right eye differs from the one the left eye holds at
//     the same position. A duplicated layer cannot come out looking right no
//     matter how flat or how periodic the source clip happens to be, which a
//     shift alone could not promise.
//
// The chroma is rotated with the luma and not inverted: the rotation already
// makes the two layers' chroma differ, and leaving the inversion off keeps the
// picture a plausible one for a codec to code rather than a colour the corpus
// never contains.
std::vector<uint8_t> make_right_eye(std::span<const uint8_t> left, uint32_t w, uint32_t h)
{
	const size_t y_size = size_t(w) * h;
	const size_t c_size = y_size / 4;
	const uint32_t cw = w / 2, ch = h / 2;
	std::vector<uint8_t> out(y_size + 2 * c_size);
	for (uint32_t y = 0; y < h; ++y)
	{
		const uint8_t * row = left.data() + size_t(y) * w;
		uint8_t * dst = out.data() + size_t(y) * w;
		for (uint32_t x = 0; x < w; ++x)
			dst[x] = uint8_t(255 - row[(x + w / 2) % w]);
	}
	for (uint32_t p = 0; p < 2; ++p)
	{
		const uint8_t * plane = left.data() + y_size + size_t(p) * c_size;
		uint8_t * dst_plane = out.data() + y_size + size_t(p) * c_size;
		for (uint32_t y = 0; y < ch; ++y)
		{
			const uint8_t * row = plane + size_t(y) * cw;
			uint8_t * dst = dst_plane + size_t(y) * cw;
			for (uint32_t x = 0; x < cw; ++x)
				dst[x] = row[(x + cw / 2) % cw];
		}
	}
	return out;
}

// The two eyes side by side, eye 0 on the left, as yuv420p at `2 * w` by `h`.
// This is what pair_compose is supposed to hand the codec and therefore what the
// decoded picture has to resemble -- the harness's own arithmetic, so a compose
// that gets the halves wrong shows up as PSNR against a picture it never built.
std::vector<uint8_t> compose_pair(std::span<const uint8_t> l, std::span<const uint8_t> r,
                                  uint32_t w, uint32_t h)
{
	const size_t y_size = size_t(w) * h;
	const size_t c_size = y_size / 4;
	const uint32_t cw = w / 2, ch = h / 2;
	std::vector<uint8_t> out(2 * (y_size + 2 * c_size));
	uint8_t * y_dst = out.data();
	for (uint32_t y = 0; y < h; ++y)
	{
		std::memcpy(y_dst + size_t(y) * 2 * w, l.data() + size_t(y) * w, w);
		std::memcpy(y_dst + size_t(y) * 2 * w + w, r.data() + size_t(y) * w, w);
	}
	for (uint32_t p = 0; p < 2; ++p)
	{
		uint8_t * dst = out.data() + 2 * y_size + size_t(p) * 2 * c_size;
		const uint8_t * ls = l.data() + y_size + size_t(p) * c_size;
		const uint8_t * rs = r.data() + y_size + size_t(p) * c_size;
		for (uint32_t y = 0; y < ch; ++y)
		{
			std::memcpy(dst + size_t(y) * 2 * cw, ls + size_t(y) * cw, cw);
			std::memcpy(dst + size_t(y) * 2 * cw + cw, rs + size_t(y) * cw, cw);
		}
	}
	return out;
}

// PSNR of ONE half of a side-by-side pair against the same half of the picture it
// should have been. The whole-picture number already fails when a half is wrong,
// but it fails without saying WHICH half -- and "the right eye scored 6 dB while
// the left scored 41" is the sentence that tells a compose fault apart from a
// codec fault, so it is worth the second pass.
double psnr_half(std::span<const uint8_t> a, std::span<const uint8_t> b,
                 uint32_t pair_w, uint32_t h, uint32_t half)
{
	const size_t y_size = size_t(pair_w) * h;
	if (a.size() != b.size() or a.size() != y_size * 3 / 2)
		return -1;
	const uint32_t w = pair_w / 2;
	const uint32_t cw = pair_w / 2, ch = h / 2;
	const size_t c_size = y_size / 4;
	double se = 0;
	size_t n = 0;
	auto acc = [&](const uint8_t * pa, const uint8_t * pb, size_t count) {
		for (size_t i = 0; i < count; ++i)
		{
			const double d = double(pa[i]) - double(pb[i]);
			se += d * d;
		}
		n += count;
	};
	for (uint32_t y = 0; y < h; ++y)
		acc(a.data() + size_t(y) * pair_w + size_t(half) * w,
		    b.data() + size_t(y) * pair_w + size_t(half) * w, w);
	for (uint32_t p = 0; p < 2; ++p)
	{
		const uint8_t * pa = a.data() + y_size + size_t(p) * c_size;
		const uint8_t * pb = b.data() + y_size + size_t(p) * c_size;
		for (uint32_t y = 0; y < ch; ++y)
			acc(pa + size_t(y) * cw + size_t(half) * (cw / 2),
			    pb + size_t(y) * cw + size_t(half) * (cw / 2), cw / 2);
	}
	if (not n)
		return -1;
	const double mse = se / double(n);
	if (mse == 0)
		return 1e9;
	return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Read a two-plane 4:2:0 image back to yuv420p, waiting on the decoder's timeline first.
std::vector<uint8_t> readback(vk_bundle & vk, vk::Image image, uint32_t w, uint32_t h,
                              vk::Semaphore sem, uint64_t sem_val)
{
	const size_t y_size = size_t(w) * h;
	const size_t c_size = y_size / 4;
	const vk::DeviceSize bytes = y_size + 2 * c_size;

	vk::raii::Buffer buf(vk.device, vk::BufferCreateInfo{.size = bytes, .usage = vk::BufferUsageFlagBits::eTransferDst});
	auto req = buf.getMemoryRequirements();
	vk::raii::DeviceMemory mem(
	        vk.device,
	        vk::MemoryAllocateInfo{
	                .allocationSize = req.size,
	                .memoryTypeIndex = find_memory(vk, req.memoryTypeBits,
	                                               vk::MemoryPropertyFlagBits::eHostVisible |
	                                                       vk::MemoryPropertyFlagBits::eHostCoherent),
	        });
	buf.bindMemory(*mem, 0);

	vk::raii::CommandPool pool(vk.device, vk::CommandPoolCreateInfo{.queueFamilyIndex = vk.queue.family_index});
	auto cmd = std::move(vk::raii::CommandBuffers(
	        vk.device, vk::CommandBufferAllocateInfo{.commandPool = *pool, .commandBufferCount = 1})[0]);
	vk::raii::Fence fence(vk.device, vk::FenceCreateInfo{});

	cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	vk::ImageMemoryBarrier to_src{
	        .srcAccessMask = vk::AccessFlagBits::eShaderRead,
	        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	        .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
	        .image = image,
	        .subresourceRange = {vk::ImageAspectFlagBits::ePlane0 | vk::ImageAspectFlagBits::ePlane1, 0, 1, 0, 1},
	};
	// eAllCommands, not eFragmentShader. This command buffer comes from the bundle's
	// own queue family, which on this device is COMPUTE|TRANSFER|SPARSE and has no
	// graphics bit -- naming a graphics-only stage in either mask is
	// VUID-vkCmdPipelineBarrier-srcStageMask-06461/dstStageMask-06462, which the
	// validation layers report on every readback. The prior use is whatever the
	// decoder did to the image before signalling the timeline this submit waits on,
	// so eAllCommands is both legal on this queue and the honest source scope.
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
	                    {}, {}, {}, to_src);
	std::array<vk::BufferImageCopy, 2> regions{
	        vk::BufferImageCopy{
	                .bufferOffset = 0,
	                .imageSubresource = {vk::ImageAspectFlagBits::ePlane0, 0, 0, 1},
	                .imageExtent = {w, h, 1},
	        },
	        vk::BufferImageCopy{
	                .bufferOffset = y_size,
	                .imageSubresource = {vk::ImageAspectFlagBits::ePlane1, 0, 0, 1},
	                .imageExtent = {w / 2, h / 2, 1},
	        },
	};
	cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, *buf, regions);
	cmd.end();

	{
		std::lock_guard lock(vk.queue.mutex);
		const vk::CommandBuffer rb_cmd = *cmd;
		const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
		if (sem)
		{
			vk::StructureChain chain{
			        vk::SubmitInfo{
			                .waitSemaphoreCount = 1,
			                .pWaitSemaphores = &sem,
			                .pWaitDstStageMask = &wait_stage,
			                .commandBufferCount = 1,
			                .pCommandBuffers = &rb_cmd,
			        },
			        vk::TimelineSemaphoreSubmitInfo{
			                .waitSemaphoreValueCount = 1,
			                .pWaitSemaphoreValues = &sem_val,
			        },
			};
			vk.queue.queue.submit(chain.get(), *fence);
		}
		else
		{
			vk.queue.queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &rb_cmd}, *fence);
		}
	}
	(void)vk.device.waitForFences(*fence, true, 5'000'000'000ull);

	// The decoder writes NV12; unpack to planar so it can be compared with nxv-dec's
	// yuv420p output directly.
	auto * src = (const uint8_t *)mem.mapMemory(0, bytes);
	std::vector<uint8_t> out(bytes);
	std::memcpy(out.data(), src, y_size);
	const uint8_t * uv = src + y_size;
	for (size_t i = 0; i < c_size; ++i)
	{
		out[y_size + i] = uv[2 * i];
		out[y_size + c_size + i] = uv[2 * i + 1];
	}
	mem.unmapMemory();
	return out;
}

} // namespace

// Filled by the stub publish_nxwarp_stats() in nxwarp_e2e_stubs.cpp: the last
// stats record the encoder published.  It lives in namespace wivrn, where the
// stub defines it.
namespace wivrn
{
extern nxwarp_stream_stats nxwarp_stats_last;
}

int main(int argc, char ** argv)
{
	std::string yuv_path, nxv_out = "e2e.nxv", decoded_out, nxv_dec = "nxv-dec";
	// Which codec backend the encoder runs: "ref" (the CPU reference) or "vk"
	// (the Vulkan compute encoder). Both must reach the same conclusions here,
	// which is the point of running the test against each.
	std::string backend = "ref";
	// Inter prediction on the codec backend, and its rolling refresh.  Off by
	// default so every existing invocation of this harness keeps producing the
	// all-intra stream its assertions were written against.
	std::string inter = "false";
	uint32_t intra_period = 180;
	// The lens mask: "on" (the server's default) or "off", and the ring of tiles around
	// the visible region left coded anyway.
	std::string lens_mask = "on";
	uint32_t lens_mask_margin = 1;
	std::string lens_mask_skip = "true";
	// "default" (STATIC when inter is on), "none", or "static".
	std::string coded_vectors = "default";
	std::string stereo_compose = "layers";
	// "auto" honours the bitrate ceiling below; "fixed" pins --qp for the run.
	// Default "fixed" here and not in the server: a test whose bytes per frame
	// drift is not a test whose byte-identity check means anything.
	std::string rc = "fixed";
	uint32_t qp = 26;
	// Whole-link bitrate handed to the encoder's rate controller, bit/s. Zero
	// means the controller is never told a ceiling, which is what every test
	// that predates it expects.
	uint64_t bitrate = 0;
	// A second ceiling, applied halfway through the run: WiVRn's automatic
	// bitrate mode moves the target mid-session, and a controller that only
	// converged from its starting point would look correct here without ever
	// having followed anything.
	uint64_t bitrate2 = 0;
	// The band the controller may move in, mirrored here so the assertions can
	// tell "did not reach the ceiling" from "reached the end of the band".
	uint32_t min_qp = 20, max_qp = 44;
	// Drive WiVRn's own bitrate_controller from the feedback this run's decoder
	// produces, instead of a scripted --bitrate/--bitrate2 schedule: the argument is the
	// ceiling in bit/s, 0 for off. This is the whole loop -- encoder, transport, real
	// decoder, real from_headset::feedback, real control law -- closed in one process.
	uint64_t aimd_ceiling = 0;
	// The encoder's send pacing: "off" (the default here), "auto", or a fixed frame
	// rate. See the settings.options assignment below for why the default differs from
	// the server's.
	std::string pace = "off";
	// Pretend this device's decoder costs this many milliseconds a frame. Zero, and the
	// desktop GPU decodes in about a millisecond, which is nothing like a Pico 4 and
	// exercises none of the decode stride, the bounded queue or the not-held reports
	// that a slow client produces. With a number here the shipping decoder waits that
	// long on its worker thread, so everything downstream of the wait is the real thing
	// reacting to a real cost.
	double client_decode_ms = 0;
	// Present composited frames at this rate instead of as fast as the GPU allows.
	// Zero (the default) is the old behaviour and is what every test that measures the
	// encoder wants. It is the pacing tests that need it: the pace is a decision about
	// WALL TIME between sent frames, so a loop that presents a thousand frames a second
	// makes it drop 97 percent of them and proves nothing about a compositor running at
	// 90 Hz. With --present-hz 90 the frames arrive when a compositor would make them,
	// and "how many of them did the pace send" is the number the headset would see.
	double present_hz = 0;
	// --deterministic: two runs of this binary on this input write the same .nxv, byte
	// for byte. See the block below main's argument parsing for what it costs and what
	// it refuses.
	bool deterministic = false;
	// Hold each not-held report back this many presented frames before handing it to
	// the encoder. Zero is the harness's own behaviour -- the report is delivered in
	// the same loop iteration that produced it, which no network does. On a live link
	// it crosses the control socket, so the encoder has coded several more frames by
	// the time it lands, and a report that names a frame the encoder has already
	// answered with an all-intra one is the common case rather than the rare one.
	// That is the case last_resync_id exists for, and this is how it gets exercised.
	uint32_t feedback_delay = 0;
	uint32_t width = 320, height = 240, frames = 12, seed = 1;
	// Eyes this ONE stream carries, 1 or 2 -- encoder_settings::eyes, verbatim. At 2 the
	// source image gains a second array layer, the encoder reads both of them, and
	// wivrn::pair_compose brings them into one side-by-side picture that nxvc codes as a
	// single stereo frame. Nothing off a headset exercised that path before this flag, so
	// a regression in the compose -- a dropped layer, a duplicated one, the wrong image
	// usage or a barrier too narrow for the stage that reads it next -- was invisible
	// here. The default is 1 and every existing invocation is unchanged by it.
	uint32_t eyes = 1;
	double loss = 0.0, reorder = 0.0;
	// How deep --reorder may push a datagram. Three is the historical behaviour and what
	// every existing run is measured against; more than a frame's worth is what reaches
	// the case where a tile of frame N arrives after a tile of frame N+1 at the same
	// position, which is the only input the src_frame monotonicity rule has an opinion
	// about.
	uint32_t reorder_depth = 3;
	// The headset app restarted, or the link dropped and came back, while the server's
	// session stayed up: the client end is thrown away and rebuilt -- new decoder, new
	// nxt::Receiver, no memory of the stream -- and the encoder and its nxt::Sender keep
	// running. Zero means no reconnect.
	uint32_t reconnect_at = 0;
	// Reconnect without telling the encoder -- the server as it behaved before
	// compositor::resume() learned to call reset_stream(). Kept so the failure this
	// harness was written for can still be produced on demand.
	bool no_resume_notice = false;
	// A keyframe request from the client that is already connected
	// (compositor::request_idr): reset() with no reconnect. The stream must not so much
	// as hiccup -- the receiver on the other end is still counting, so an encoder that
	// restarted its transport here would break a session that was working.
	uint32_t idr_at = 0;
	// Drop exactly ONE datagram, the `n`th the link carries, and nothing else.
	//
	// It is the measurement the per-tile span mapping exists for. Under the chunk
	// mapping a datagram is a slice of the frame's byte stream, so losing one costs
	// the WHOLE frame -- every tile of it, none of which can be decoded. Under real
	// spans it costs exactly the tiles that datagram carried. The difference is not a
	// ratio to be argued about, it is a count, and this makes it one.
	uint32_t drop_datagram = 0;
	uint32_t drop_datagram_count = 1;
	// --tile-stream: run the arrival-order model of docs/NXWARP-TILESTREAM.md section 6
	// alongside the real decode, and compare it against the frame-complete one. Off by
	// default and it changes nothing about the run: it observes the same delivery the
	// decoder saw and asserts on it, so every other figure a --tile-stream run prints is
	// the figure the same run without it prints.
	bool tile_stream = false;
	// The encoder's "tile-map" option, straight through. It is what makes the cost of the
	// per-tile span mapping measurable at all: the same clip, the same QP, the same
	// frames, laid on the grid two different ways. Without it a comparison has to move
	// the quantiser to move the mapping, and then the frames are not the same frames.
	std::string tile_map = "auto";
	// Where the frame counter starts. video_encoder_nxwarp puts uint16_t(frame_id) on
	// the wire, so starting near 65536 walks the stream through the 16-bit wrap -- about
	// twelve minutes into a 90 fps session, and the point at which one went dark.
	// --start-frame-id is the same option under its other name.
	uint64_t first_frame = 0;
	// The headset's nxvc decoder tool mask, as headset_info_packet::nxvc_tools would
	// carry it. There is no headset in this process, so it is simulated: this is the
	// only way to exercise the negotiation without two devices and a network.
	//
	// The default is "none", and that is the truthful one: there is no headset in this
	// process, so nothing reported a mask. It is also what keeps every existing run in
	// this harness byte-for-byte what it was, because "entropy": "auto" reads an absent
	// mask as "no information" and stays on rANS.
	//
	// "all" is a headset that can decode anything this encoder emits, which is what a
	// headset running this same nx-warp build reports; a number with bit 30 clear is a
	// headset whose decoder has no ENTROPY_LITE.
	std::string client_tools = "none";
	// "auto" (the server default), "rans" or "lite".
	std::string entropy = "auto";
	// The encoder effort level, "0" or "1".  EMPTY UNLESS ASKED FOR, so a plain
	// run leaves the option out of the map entirely and gets whatever the
	// server defaults to -- which is the point.  It used to be hard-defaulted
	// to "1" here to match the server, and that made the harness incapable of
	// noticing when the two disagreed: it was passing the answer in.  Now the
	// level the run actually used is read back off the constructed encoder
	// (`resolved_effort()`) and printed, so `--effort` with no value proves the
	// default rather than asserting it.  Not out of the published stats: those
	// are built inside the two-second reporting window and a twelve-frame run
	// publishes none, so a reader would get the struct's initialiser.
	std::string effort;
	// The snap-to-identity threshold, 1/16 luma samples.  Absent unless asked
	// for, so every existing run keeps the server's default of 0.
	std::string snap_identity;
	// The piecewise-planar tile mode: "off", "rd" (the server's default) or
	// "prefer".  It needs --backend ref: the Vulkan encoder has no mode 5, and
	// the server refuses the combination rather than dropping it, which this
	// harness is a good place to prove.
	// Unset unless asked: leaving the key ABSENT is what makes the server's own
	// default apply, and the server refuses an EXPLICIT level the simulated
	// headset cannot decode.  Writing it unconditionally would turn every
	// existing run in this harness -- all of which simulate a headset with no
	// tool mask -- into a startup error.
	std::string planar;

	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--yuv")
			yuv_path = next();
		else if (a == "--width")
			width = uint32_t(std::stoul(next()));
		else if (a == "--height")
			height = uint32_t(std::stoul(next()));
		else if (a == "--frames")
			frames = uint32_t(std::stoul(next()));
		else if (a == "--loss")
			loss = std::stod(next());
		else if (a == "--reorder")
			reorder = std::stod(next());
		else if (a == "--reorder-depth")
			reorder_depth = uint32_t(std::stoul(next()));
		else if (a == "--first-frame")
			first_frame = std::stoull(next());
		else if (a == "--eyes")
			eyes = uint32_t(std::stoul(next()));
		else if (a == "--seed")
			seed = uint32_t(std::stoul(next()));
		else if (a == "--nxv-out")
			nxv_out = next();
		else if (a == "--decoded-out")
			decoded_out = next();
		else if (a == "--nxv-dec")
			nxv_dec = next();
		else if (a == "--backend")
			backend = next();
		else if (a == "--inter")
		{
			const std::string v = next();
			// The option the server config carries is a string "true"/"false";
			// on|off is accepted here because every other flag in this harness
			// spells it that way.
			inter = (v == "on" or v == "true") ? "true" : "false";
		}
		else if (a == "--intra-period")
			intra_period = uint32_t(std::stoul(next()));
		else if (a == "--lens-mask")
		{
			const std::string v = next();
			lens_mask = (v == "on" or v == "true" or v == "1") ? "on" : "off";
		}
		else if (a == "--lens-mask-margin")
			lens_mask_margin = uint32_t(std::stoul(next()));
		else if (a == "--static-view")
			static_view = true;
		else if (a == "--lens-mask-skip")
		{
			const std::string v = next();
			lens_mask_skip = (v == "on" or v == "true" or v == "1") ? "true" : "false";
		}
		else if (a == "--stereo-compose")
			stereo_compose = next();
		else if (a == "--coded-vectors")
			coded_vectors = next();
		else if (a == "--client-tools")
			client_tools = next();
		else if (a == "--entropy")
			entropy = next();
		else if (a == "--effort")
			effort = next();
		else if (a == "--snap-identity")
			snap_identity = next();
		else if (a == "--head-rate")
			g_head_rate = std::stof(next());
		else if (a == "--planar")
			planar = next();
		else if (a == "--qp")
			qp = uint32_t(std::stoul(next()));
		else if (a == "--reconnect-at")
			reconnect_at = uint32_t(std::stoul(next()));
		else if (a == "--start-frame-id") // the other name for --first-frame
			first_frame = std::stoull(next());
		else if (a == "--no-resume-notice")
			no_resume_notice = true;
		else if (a == "--drop-datagram")
		{
			// "n" or "n:k": drop k consecutive DATA datagrams from the nth. The
			// parity of their FEC groups is suppressed with them -- see the
			// note in lossy_link::send_stream -- so k is not how the test
			// reaches past the FEC, it is only how much loss it wants.
			const std::string v = next();
			const size_t colon = v.find(':');
			drop_datagram = uint32_t(std::stoul(v.substr(0, colon)));
			drop_datagram_count =
			        colon == std::string::npos ? 1u : uint32_t(std::stoul(v.substr(colon + 1)));
		}
		else if (a == "--tile-stream")
			tile_stream = true;
		else if (a == "--tile-map")
			tile_map = next();
		else if (a == "--idr-at")
			idr_at = uint32_t(std::stoul(next()));
		else if (a == "--rc")
			rc = next();
		else if (a == "--bitrate")
			bitrate = std::stoull(next());
		else if (a == "--bitrate2")
			bitrate2 = std::stoull(next());
		else if (a == "--min-qp")
			min_qp = uint32_t(std::stoul(next()));
		else if (a == "--max-qp")
			max_qp = uint32_t(std::stoul(next()));
		else if (a == "--aimd")
			aimd_ceiling = std::stoull(next());
		else if (a == "--pace")
			pace = next();
		else if (a == "--client-decode-ms")
			client_decode_ms = std::stod(next());
		else if (a == "--present-hz")
			present_hz = std::stod(next());
		else if (a == "--feedback-delay")
			feedback_delay = uint32_t(std::stoul(next()));
		else if (a == "--deterministic")
			deterministic = true;
		else
		{
			std::fprintf(stderr, "unknown argument %s\n", a.c_str());
			return 2;
		}
	}

	if (yuv_path.empty())
	{
		std::fprintf(stderr, "--yuv is required\n");
		return 2;
	}

	// --- --deterministic: what it refuses, and why -------------------------------
	//
	// The mode's promise is that two runs write the same .nxv. Every option below puts
	// a wall clock back into the bytes, so each is refused rather than quietly ignored
	// -- a flag that is read and then does nothing is worse than one that is rejected,
	// because the run still produces a file and the file still looks like an answer.
	//
	//   --aimd            WiVRn's bitrate controller reacts to delivery timing.
	//   --rc other than fixed   the rate controller's window is a wall-clock window.
	//   --pace other than off   send admission is a steady_clock deadline in the
	//                     encoder, which this harness cannot reach without changing
	//                     the shipping encoder; it is refused rather than faked.
	//   --client-decode-ms      a real sleep on the worker thread, whose whole purpose
	//                     is to make the timing-dependent paths fire.
	//   --present-hz      a compositor cadence measured against steady_clock.
	//
	// --loss, --reorder and --drop-datagram stay legal: they are driven by the seeded
	// mt19937 in lossy_link and are already a function of --seed alone.
	if (deterministic)
	{
		const char * why = nullptr;
		if (aimd_ceiling)
			why = "--aimd";
		else if (rc != "fixed")
			why = "--rc other than fixed";
		else if (pace != "off")
			why = "--pace other than off";
		else if (client_decode_ms > 0)
			why = "--client-decode-ms";
		else if (present_hz > 0)
			why = "--present-hz";
		if (why)
		{
			std::fprintf(stderr,
			             "--deterministic cannot be combined with %s: it makes the "
			             "encode depend on wall-clock time\n",
			             why);
			return 2;
		}
		// The stride is derived from measured decode time against measured arrival
		// period. Pinned before a decoder exists, so no frame is ever judged by it.
		wivrn::nxwarp_pin_decode_stride(true);
		std::printf("deterministic: frame-counter clock, decode stride pinned to 1, "
		            "the decoder drained after every frame\n");
	}

	// --- what --eyes 2 requires, checked before anything is built ----------------
	//
	// Refused here rather than deeper down because both of these come out of the codec
	// as a frame that does not encode, which reaches this harness as "the encoder
	// produced no datagrams" -- a true statement about a run that was never going to
	// work, and one that says nothing about why.
	if (eyes != 1 and eyes != 2)
	{
		std::fprintf(stderr, "--eyes must be 1 or 2, not %u\n", eyes);
		return 2;
	}
	if (eyes == 2 and width % 64 != 0)
	{
		// nxvc's tile grid is 64 wide and the pair is `eyes * width` samples across
		// with the eye boundary falling on a tile boundary ([SYN] 3.3), so a per-eye
		// width that is not a multiple of 64 has no stereo frame to describe and the
		// codec refuses the configuration outright.
		std::fprintf(stderr,
		             "--eyes 2 needs a PER-EYE width that is a multiple of 64; %u is not "
		             "(%u would be the nearest below, %u the nearest above)\n",
		             width, width / 64 * 64, (width / 64 + 1) * 64);
		return 2;
	}
	if (eyes == 2 and backend != "vk")
	{
		// The pair is composed on the device, out of two array layers of the
		// compositor image, so it exists only on the path that reads that image
		// directly. The CPU reference backend is fed a host-side plane copied out of
		// ONE layer (video_encoder_nxwarp's readback path takes src_layer and nothing
		// else), which is a mono frame however `eyes` is set.
		std::fprintf(stderr, "--eyes 2 needs --backend vk: the reference backend is fed one "
		                     "array layer through host memory and never sees the pair\n");
		return 2;
	}

	auto yuv = read_file(yuv_path);
	const size_t frame_bytes = size_t(width) * height * 3 / 2;
	// The geometry everything downstream of the encoder is measured in. With the eyes
	// paired the DECODED picture is the pair -- `eyes * width` wide -- while `width`
	// stays per eye everywhere the encoder and the stream description use it, which is
	// nxvc's own convention and the server's.
	const uint32_t pair_width = width * eyes;
	const size_t pair_frame_bytes = size_t(pair_width) * height * 3 / 2;
	if (yuv.size() < frame_bytes)
	{
		std::fprintf(stderr, "%s holds %zu bytes, one %ux%u yuv420p frame is %zu\n",
		             yuv_path.c_str(), yuv.size(), width, height, frame_bytes);
		return 2;
	}
	const uint32_t available = uint32_t(yuv.size() / frame_bytes);
	std::printf("source: %s, %u frames of %ux%u available, running %u from frame id %llu\n",
	            yuv_path.c_str(), available, width, height, frames,
	            (unsigned long long)first_frame);

	vk_bundle vk;
	std::printf("vulkan: %s\n", vk.physical_device.getProperties().deviceName.data());

	// ---- the encoder, as the server builds it --------------------------------
	encoder_settings settings{};
	settings.width = uint16_t(width);
	settings.height = uint16_t(height);
	settings.codec = video_codec::nxwarp;
	settings.fps = 90;
	settings.encoder_name = encoder_nxwarp;
	settings.bitrate = bitrate ? bitrate : 50'000'000;
	settings.bitrate_multiplier = 1.0;
	settings.bit_depth = 8;
	settings.src_layer = 0;
	// The paired stream, exactly as encoder_settings describes it: one stream carrying
	// both eyes, reading layer 0 and layer 1 of the one compositor image. At one eye
	// src_layer_right is never read, so it is left at its default and the settings this
	// harness builds are byte for byte what they were.
	settings.eyes = eyes;
	if (eyes == 2)
		settings.src_layer_right = 1;
	settings.options["qp"] = std::to_string(qp);
	settings.options["backend"] = backend;
	settings.options["inter"] = inter;
	settings.options["intra-period"] = std::to_string(intra_period);
	// The lens mask. On by default here as it is in the server, so the harness runs the
	// shipping configuration; --lens-mask off is the other half of the A/B.
	settings.options["lens-mask"] = lens_mask;
	settings.options["lens-mask-margin"] = std::to_string(lens_mask_margin);
	settings.options["lens-mask-skip"] = lens_mask_skip;
	settings.options["coded-vectors"] = coded_vectors;
	// "layers" (the default) reads the eye pair straight out of two array layers;
	// "blit" copies them into one side-by-side picture first. nxvc pins that the
	// two produce the IDENTICAL bitstream, so --stereo-compose is here to let
	// that be checked rather than believed: run --eyes 2 both ways and diff the
	// .nxv files.
	settings.options["stereo-compose"] = stereo_compose;
	settings.options["entropy"] = entropy;
	if (not effort.empty())
		settings.options["effort"] = effort;
	if (not snap_identity.empty())
		settings.options["snap-identity"] = snap_identity;
	if (not planar.empty())
		settings.options["planar"] = planar;
	// The simulated headset mask. "all" is every bit set, which is a headset that can
	// decode anything this encoder emits and is what keeps every existing run in this
	// harness unchanged. It is spelled ~0 rather than read from nxvc because this file,
	// like the rest of the server encoder layer, carries no nxvc type -- see the header
	// comment of nxwarp_codec.h. A specific mask goes in as a number.
	if (client_tools == "all")
		settings.nxvc_tools = ~0ull;
	else if (client_tools == "none")
		settings.nxvc_tools = 0;
	else
		settings.nxvc_tools = std::stoull(client_tools, nullptr, 0);
	const std::string snap_note =
	        snap_identity.empty() ? std::string()
	                              : ", snap-identity \"" + snap_identity + "\"";
	// Named rather than built inside the call: a std::string temporary's
	// c_str() does live to the end of the full expression, but a reader should
	// not have to know that to trust the line.
	const std::string planar_note =
	        planar.empty() ? std::string() : ", planar request \"" + planar + "\"";
	std::fprintf(stderr,
	             "[e2e] simulated headset nxvc_tools = 0x%llx (entropy request \"%s\"%s%s)\n",
	             (unsigned long long)settings.nxvc_tools,
	             entropy.c_str(),
	             snap_note.c_str(),
	             planar_note.c_str());
	// Rate control off by default. The byte-identity check below compares this
	// run's bitstream against nxv-dec's decode of it, which a moving quantiser
	// does not disturb -- but the frame sizes and the loss pattern would stop
	// being reproducible, and every assertion about the transport is written
	// against a run whose frames are the same size every time.
	settings.options["rc"] = rc;
	// Send pacing OFF by default here, for the reason rate control is: every other
	// assertion in this harness is written against a run that sends exactly the frames
	// it presents, and the pace is a wall-clock decision, so leaving it on would make
	// the frame count depend on how fast this machine encodes. The server's own default
	// is "auto"; --pace auto is how that is exercised.
	settings.options["pace"] = pace;
	settings.options["tile-map"] = tile_map;
	settings.options["min-qp"] = std::to_string(min_qp);
	settings.options["max-qp"] = std::to_string(max_qp);

	video_encoder_nxwarp enc(vk, settings, 0);

	// The whole-link ceiling, exactly as the session's bitrate controller sets
	// it: video_encoder::set_bitrate takes this stream's share out of it (here a
	// multiplier of 1.0 and no FEC parity, so the share is the whole number) and
	// publishes it as pending_bitrate, which is what the encoder's controller
	// reads. Nothing about the path from the ceiling to the QP is bypassed.
	if (bitrate)
	{
		enc.set_bitrate(uint32_t(bitrate));
		std::printf("rate control: %s, ceiling %llu bit/s -> %.0f B/frame at %.0f Hz\n",
		            rc.c_str(), (unsigned long long)bitrate,
		            double(bitrate) / 8.0 / double(settings.fps), double(settings.fps));
	}

	// ---- the decoder, as the headset builds it -------------------------------
	to_headset::video_stream_description desc{};
	desc.width = uint16_t(width);
	desc.height = uint16_t(height);
	desc.codec = {video_codec::nxwarp, video_codec::nxwarp, video_codec::nxwarp, video_codec::nxwarp};
	desc.frame_rate = 90;
	desc.refresh_rate = 90;

	e2e_host host(vk);
	// The picture the decoder publishes, which is the pair when the stream is paired.
	// The stream DESCRIPTION above stays per eye: there is no wire field for the eye
	// count and the decoder learns it from the .nxv stream header, rebuilding its image
	// pool at twice the width when it does (nxwarp_decoder::build_image_pool). Reading
	// back at `width` here would silently take the left eye and call it the frame.
	host.width = pair_width;
	host.height = height;
	auto make_decoder = [&] {
		auto d = std::make_unique<nxwarp_decoder>(vk.device, vk.physical_device,
		                                          vk.queue.family_index, desc, 0, host, nullptr);
		if (client_decode_ms > 0)
			d->set_simulated_decode_ms(client_decode_ms);
		// See set_unbounded_queue(): the worker's one-frame backlog is the largest
		// single source of run-to-run variance in the file this harness writes.
		if (deterministic)
			d->set_unbounded_queue(true);
		return d;
	};
	auto dec = make_decoder();

	lossy_link link(*dec, loss, reorder, seed, reorder_depth);
	link.drop_one = drop_datagram;
	link.drop_count = drop_datagram_count;
	// The delivery order is only kept when something is going to read it, because on a
	// long run it is one entry per datagram and nothing else in the harness needs it.
	link.record_delivery = tile_stream;
	enc.set_packet_sink(&link);

	auto src = make_source_image(vk, width, height, eyes);
	if (eyes == 2)
		std::printf("eyes: 2 -- one stream, layers 0 and 1 of the source image, composed to "
		            "%ux%u; the right eye is the left rotated half a picture with its luma "
		            "inverted, so a compose that dropped or duplicated a layer cannot pass\n",
		            pair_width, height);
	// Layer 0 then layer 1, back to back, which is what upload() reads. Empty at one eye:
	// the mono path hands upload() the span into the source file exactly as before, with
	// no copy in between, so nothing about that run moves.
	std::vector<uint8_t> layered;

	// ---- WiVRn's own automatic bitrate, closed over this run --------------------
	//
	// The real bitrate_controller, on a VIRTUAL clock. Virtual because the harness
	// encodes as fast as the GPU allows rather than at 90 Hz, and every threshold in
	// that class is a duration -- a 2 s window, a 250 ms evaluation interval, a 5 s
	// hold before each increase. Driven on wall time it would see a whole session's
	// frames inside one window and decide nothing. One frame period per presented
	// frame is the cadence a headset would produce, and it makes the run repeatable.
	bitrate_controller aimd;
	auto aimd_now = bitrate_controller::clock::time_point{} + std::chrono::hours(1);
	std::vector<std::pair<uint64_t, uint32_t>> aimd_changes;
	if (aimd_ceiling)
	{
		aimd.configure({.enabled = true}, uint32_t(aimd_ceiling), true, false,
		               wivrn::bitrate_mode::aimd);
		enc.set_bitrate(uint32_t(aimd_ceiling));
		std::printf("automatic bitrate: AIMD, ceiling %.1f Mbit/s, floor %.1f Mbit/s\n",
		            double(aimd_ceiling) * 1e-6,
		            double(bitrate_controller::config{}.min_bitrate_bps) * 1e-6);
	}
	// Drain whatever delivery reports the decoder has produced and let the control law
	// see them, exactly as wivrn_session::operator()(from_headset::feedback &&) does.
	auto pump_aimd = [&](uint64_t frame_no) {
		if (not aimd_ceiling)
			return;
		std::vector<from_headset::feedback> reports;
		{
			std::lock_guard lock(host.m);
			reports.swap(host.frame_reports);
		}
		for (const auto & fb: reports)
		{
			if (auto b = aimd.on_feedback(fb, int64_t(1e9 / settings.fps), true, aimd_now))
			{
				enc.set_bitrate(*b);
				aimd_changes.emplace_back(frame_no, *b);
			}
		}
		aimd_now += std::chrono::nanoseconds(int64_t(1e9 / settings.fps));
	};

	// ---- drive ----------------------------------------------------------------
	uint64_t feedback_packets = 0, feedback_bytes = 0;
	uint64_t not_held_packets = 0;
	// One row per frame: the quantiser it was coded at and the bytes that
	// bought. Filled from the encoder's own profile counters -- read once
	// before the encode for the QP and once after for the byte total -- so it
	// is the encoder's arithmetic, not a re-derivation of it.
	struct rc_row
	{
		uint32_t qp;
		uint64_t bytes;
		double target;
		// The lowest QP the controller was allowed to pick for this frame. With
		// the transport ceiling in play that is not always min-qp, and "under
		// budget" is only a controller fault when it had somewhere lower to go.
		uint32_t floor;
	};
	std::vector<rc_row> rc_trace;
	// --- what the pace actually sent -------------------------------------------
	//
	// A paced encoder sends fewer frames than the compositor presents, so "how many
	// frames were presented" stops being the number every assertion below is against.
	// A frame is counted as sent when it put datagrams on the link, which is the only
	// definition that cannot disagree with what the decoder saw.
	//
	// The wire frame ids are rebuilt here rather than read off the encoder: they are
	// the part of pacing the client is most exposed to (a gap in the sequence is loss,
	// to a windowed reassembler and to the AIMD report built from it), so the harness
	// keeps its own count and the checks below compare the two.
	uint64_t sent_frames = 0;
	uint16_t next_wire_id = 0;
	bool wire_id_seeded = false;
	std::unordered_map<uint16_t, size_t> presented_of_wire_id;
	// Wall time of the first and last frame that was sent, for the sent rate.
	std::chrono::steady_clock::time_point first_send{}, last_send{};
	// Not-held reports by reason, in the order from_headset::nxwarp_frame_not_held
	// declares them, and the presented-frame index of the last stride one -- which is
	// what says whether the pace settled or is still being told it is too fast.
	std::array<uint64_t, 4> not_held_by_reason{};
	std::deque<std::pair<uint32_t, from_headset::nxwarp_frame_not_held>> not_held_in_flight;
	uint64_t last_stride_not_held_at = 0;
	bool any_stride_not_held = false;
	std::vector<view_info_t> presented;
	std::vector<std::vector<uint8_t>> source_frames;
	const uint8_t num_slots_used = 1;

	// What the reconnect option is measured by: the published-frame and unit counts at
	// the moment the client was replaced, so that "did anything decode AFTER the
	// reconnect" is a number rather than an impression.
	size_t frames_at_reconnect = 0, units_at_reconnect = 0;
	uint64_t sent_at_reconnect = 0;
	uint64_t headers_at_reconnect = 0;
	bool reconnected = false;
	nxt::ReceiverStats post_stats{};
	// See the note where this is filled: the shadow forgets a frame after eight, so the
	// receipt map --drop-datagram is judged on has to be taken during the run.
	std::map<uint16_t, std::vector<uint8_t>> dropped_frame_receipts;

	if (first_frame)
		std::printf("frame ids start at %llu (16-bit wrap at %llu, %s crossed by this run)\n",
		            (unsigned long long)first_frame,
		            (unsigned long long)((first_frame / 65536 + 1) * 65536),
		            first_frame % 65536 + frames > 65536 ? "is" : "is NOT");

	host.deterministic = deterministic;

	const auto run_start = std::chrono::steady_clock::now();
	for (uint32_t i = 0; i < frames; ++i)
	{
		const uint64_t f = first_frame + i;

		// The frame-counter clock, advanced once per presented frame. Before anything
		// this iteration timestamps, so a frame's own feedback carries the frame's own
		// tick and not the previous one's.
		if (deterministic)
			host.det_tick.store(i + 1, std::memory_order_relaxed);

		// The compositor's cadence, when one was asked for. Against the run's own
		// start rather than the previous frame, so a slow frame does not push the
		// whole schedule back -- which is exactly what a compositor does.
		if (present_hz > 0)
		{
			const auto due = run_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			                                     std::chrono::duration<double>(double(i) / present_hz));
			std::this_thread::sleep_until(due);
		}

		if (reconnect_at and i == reconnect_at)
		{
			// Let the old client finish what it already holds, so the frame and unit
			// counts on either side of the cut mean what they say.
			dec->flush_frames();
			host.wait_for(host.units.empty() ? 0 : host.units.size(), std::chrono::seconds(10));
			frames_at_reconnect = host.frames.size();
			units_at_reconnect = host.units.size();
			headers_at_reconnect = link.headers_sent;
			sent_at_reconnect = sent_frames;

			// The client goes away and a new one takes its place. The encoder object
			// survives, exactly as it does across wivrn_session::resume_session, and
			// hears about it the only way the server tells it: compositor::resume()
			// calls reset_stream() on every encoder. Nothing else here is allowed to
			// touch the encoder -- if the stream comes back, it came back because of
			// that one call.
			dec.reset();
			dec = make_decoder();
			link.retarget(*dec);
			if (not no_resume_notice)
				enc.reset_stream();
			reconnected = true;
			std::printf("\n--- reconnect at frame %u: new decoder, new receiver, same encoder "
			            "(%zu frames, %zu units so far)\n\n",
			            i, frames_at_reconnect, units_at_reconnect);
		}

		if (idr_at and i == idr_at)
		{
			std::printf("\n--- keyframe request at frame %u: reset(), same client\n\n", i);
			enc.reset();
		}

		std::span<const uint8_t> frame(yuv.data() + size_t(f % available) * frame_bytes, frame_bytes);
		if (eyes == 2)
		{
			// `source_frames` is what every picture is scored against, so at two
			// eyes it has to be the PAIR the compose owes back and not the eye the
			// file holds. This is the check on pair_compose: the decoded picture is
			// compared to a side-by-side the harness built itself, half by half.
			auto right = make_right_eye(frame, width, height);
			layered.assign(frame.begin(), frame.end());
			layered.insert(layered.end(), right.begin(), right.end());
			source_frames.push_back(compose_pair(frame, right, width, height));
			upload(vk, src, layered);
		}
		else
		{
			source_frames.emplace_back(frame.begin(), frame.end());
			upload(vk, src, frame);
		}

		auto vi = make_view_info(f);
		presented.push_back(vi);

		vk::SemaphoreSubmitInfo sem_info{
		        .semaphore = *src.done,
		        .value = 0,
		        .stageMask = vk::PipelineStageFlagBits2::eTransfer,
		};
		// The session's bitrate controller moving its mind, mid-stream, the only
		// way it ever does: set_bitrate on the live encoder.
		if (bitrate2 and i == frames / 2)
		{
			enc.set_bitrate(uint32_t(bitrate2));
			std::printf("frame %u: ceiling moved to %llu bit/s\n", i,
			            (unsigned long long)bitrate2);
		}

		const uint8_t slot = uint8_t(f % num_slots_used);
		enc.present_image(*src.image, sem_info, slot, f, vi);
		const auto before = enc.profile();
		const uint64_t datagrams_before = link.sent;
		(void)enc.encode(slot, f);
		const auto after = enc.profile();
		if (after.frames > before.frames)
			rc_trace.push_back({before.qp, after.bytes - before.bytes,
			                    after.target_bytes, before.qp_floor});
		// Datagrams on the link is what "sent" means: an encode that produced bytes the
		// tile grid could not carry is not a frame the client ever hears about, and it
		// spends no wire frame id either.
		if (link.sent > datagrams_before)
		{
			++sent_frames;
			const uint16_t id = wire_id_seeded ? uint16_t(next_wire_id + 1) : uint16_t(f);
			next_wire_id = id;
			wire_id_seeded = true;
			presented_of_wire_id[id] = i;
			const auto now = std::chrono::steady_clock::now();
			if (sent_frames == 1)
				first_send = now;
			last_send = now;
		}

		// Feedback the decoder produced while that frame was going through, straight back
		// into the encoder, which is what the network thread does.
		std::vector<e2e_host::feedback_packet> fb;
		{
			std::lock_guard lock(host.m);
			fb.swap(host.feedback);
		}
		for (auto & p: fb)
		{
			++feedback_packets;
			feedback_bytes += p.payload.size();
			// Straight into the encoder, which folds it into nxt::Sender's client
			// shadow. This is the return half of the loop: without it the encoder
			// predicts from tiles the headset never received.
			enc.on_nxwarp_feedback(p.path_id, p.payload, p.decode_us, p.held_base,
			                       p.held_mask);
		}

		// And the correction the transport cannot carry: frames the decoder received
		// and will not reconstruct. Straight into the encoder, exactly as
		// compositor::on_nxwarp_frame_not_held does.
		{
			std::vector<from_headset::nxwarp_frame_not_held> nh;
			{
				std::lock_guard lock(host.m);
				nh.swap(host.not_held);
			}
			// The control socket's latency, when one was asked for: the report is
			// queued and handed over `feedback_delay` presented frames later.
			if (feedback_delay)
			{
				for (auto & p: nh)
					not_held_in_flight.emplace_back(i + feedback_delay, p);
				nh.clear();
				while (not not_held_in_flight.empty() and
				       not_held_in_flight.front().first <= i)
				{
					nh.push_back(not_held_in_flight.front().second);
					not_held_in_flight.pop_front();
				}
			}
			for (const auto & p: nh)
			{
				++not_held_packets;
				const auto w = size_t(p.why);
				if (w < not_held_by_reason.size())
					++not_held_by_reason[w];
				if (p.why == from_headset::nxwarp_frame_not_held::reason::stride)
				{
					last_stride_not_held_at = i;
					any_stride_not_held = true;
				}
				enc.on_nxwarp_frame_not_held(p.frame_id, p.why);
			}
		}

		// The receipt map for a frame that lost a datagram, read WHILE IT IS STILL
		// THERE. nxt::ClientShadow keeps kShadowFrames (8) frames, so a run of any
		// length has forgotten the frame long before the assertions at the bottom of
		// this file get to ask about it; taken here, and overwritten each iteration,
		// what survives to the end is the last state the shadow held for it, which is
		// the one every feedback packet that could cover it has been folded into.
		if (drop_datagram)
		{
			for (const auto & r: link.dropped_runs)
			{
				auto rec = enc.client_receipts(r.frame_id);
				bool any_known = false;
				for (uint8_t v: rec)
					any_known |= v != video_encoder_nxwarp::receipt_unknown;
				if (any_known)
					dropped_frame_receipts[r.frame_id] = std::move(rec);
			}
		}

		// The other return half: the per-frame delivery reports into WiVRn's own
		// automatic bitrate, whose answer goes back to the encoder as a new ceiling.
		pump_aimd(i);

		// --deterministic: let the decoder finish this frame before the next one is
		// encoded. This is what removes the race the mode exists to remove -- see
		// wait_quiescent(). It is deliberately the LAST thing in the iteration, after
		// the feedback and not-held drains above, so that what those drains saw is
		// still "whatever had arrived by now" on an ordinary run and becomes "exactly
		// what frames up to i-1 produced" here.
		if (deterministic and not wait_quiescent(*dec, std::chrono::seconds(10)))
		{
			std::fprintf(stderr,
			             "deterministic: the decoder did not settle within 10 s at "
			             "frame %u; the run would not be reproducible\n",
			             i);
			return 1;
		}
	}

	// The encode loop is over, so nothing more will arrive to push the tail through:
	// the link hands over everything it is still holding back, and the decoder closes
	// whatever is still inside its reassembly window. Without both, the last frames of a
	// reordered run would be counted as lost when they were only late.
	link.flush();
	dec->flush_frames();

	// The decoder finishes a frame when its last run arrives, when a newer frame pushes
	// it out of the window, or at the flush above, so by here everything presented has
	// been decided one way or the other.
	const bool arrived = host.wait_for(frames > 0 ? frames - 1 : 0, std::chrono::seconds(20));
	// And a short grace for the last one, so the counts printed below are the counts the
	// assertions see. A run that lost frames simply spends it.
	(void)host.wait_for(frames, std::chrono::milliseconds(500));

	std::printf("\nlink: %llu datagrams, %llu dropped (%.1f%%), %llu delayed by 1-3 slots (%.1f%%)\n",
	            (unsigned long long)link.sent, (unsigned long long)link.dropped,
	            link.sent ? 100.0 * double(link.dropped) / double(link.sent) : 0.0,
	            (unsigned long long)link.delayed,
	            link.sent ? 100.0 * double(link.delayed) / double(link.sent) : 0.0);
	std::printf("reassembly produced %zu complete frame units of %llu sent (%u presented)\n",
	            host.units.size(), (unsigned long long)sent_frames, frames);
	std::printf("decoder published %zu frames\n", host.frames.size());

	// --- the lens mask -----------------------------------------------------------
	//
	// What it masked, whether the codec was actually told, and what the frames of this
	// run coded. The A/B is the same clip at the same quantiser with --lens-mask off,
	// and the numbers to compare are "coded tiles per frame" and the size of the .nxv.
	{
		const auto lm = enc.lens_mask_report();
		std::printf("\nlens mask: %s%s -- %u of %u tiles per eye masked, margin %u tile%s\n",
		            lm.on ? "on" : "off",
		            lm.on ? (lm.enforced ? " (never coded: nxvc skip map)"
		                                 : " (flattened only: this backend has no skip-map "
		                                   "input)")
		                  : "",
		            lm.masked, lm.tiles_per_eye, lm.margin, lm.margin == 1 ? "" : "s");
		if (lm.frames)
			std::printf("coded tiles: %.1f of %.1f per frame (%.1f%%) over %llu frames\n",
			            double(lm.coded_tiles) / double(lm.frames),
			            double(lm.total_tiles) / double(lm.frames),
			            100.0 * double(lm.coded_tiles) / double(lm.total_tiles ? lm.total_tiles : 1),
			            (unsigned long long)lm.frames);
	}

	// --- the pace ---------------------------------------------------------------
	{
		const double span = sent_frames > 1
		                            ? std::chrono::duration<double>(last_send - first_send).count()
		                            : 0.0;
		std::printf("pace: \"%s\", simulated client decode %.1f ms; %llu of %u composited "
		            "frames sent (%.1f%%)",
		            pace.c_str(), client_decode_ms, (unsigned long long)sent_frames, frames,
		            frames ? 100.0 * double(sent_frames) / double(frames) : 0.0);
		if (span > 0)
			std::printf(", %.1f frames/s on the wire", double(sent_frames - 1) / span);
		std::printf("\n");
		std::printf("not held: %llu total -- %llu hole, %llu decode stride, %llu worker "
		            "backlog, %llu codec refusal\n",
		            (unsigned long long)not_held_packets,
		            (unsigned long long)not_held_by_reason[0],
		            (unsigned long long)not_held_by_reason[1],
		            (unsigned long long)not_held_by_reason[2],
		            (unsigned long long)not_held_by_reason[3]);
		{
			const auto p = enc.profile();
			std::printf("not-held reports the encoder charged for: %llu of %llu; %llu named a "
			            "frame older than one already coded intra and cost nothing\n",
			            (unsigned long long)(p.not_held - p.not_held_already_answered),
			            (unsigned long long)p.not_held,
			            (unsigned long long)p.not_held_already_answered);
		}
		{
			const auto p = enc.profile();
			if (p.confirms)
				std::printf("confirmation latency: %llu frames confirmed, mean %.2f encoder "
				            "frames / %.1f ms, worst %llu frames / %.1f ms (ref_sel reaches "
				            "3)\n",
				            (unsigned long long)p.confirms, p.confirm_frames_mean,
				            p.confirm_ms_mean, (unsigned long long)p.confirm_frames_max,
				            p.confirm_ms_max);
			else
				std::printf("confirmation latency: no frame was confirmed\n");
		}
		std::printf("all-intra resyncs: %llu of %llu sent frames (%.1f%%) were coded with no "
		            "temporal reference because the headset had reported one not held\n",
		            (unsigned long long)link.resync_notices, (unsigned long long)sent_frames,
		            sent_frames ? 100.0 * double(link.resync_notices) / double(sent_frames) : 0.0);
		if (any_stride_not_held)
			std::printf("last decode-stride report was at presented frame %llu of %u\n",
			            (unsigned long long)last_stride_not_held_at, frames);

		// Bytes per SENT frame at the start of the run and at the end of it. This is
		// where inter prediction shows up or does not: while the headset is dropping
		// frames the encoder answers every not-held report with an all-zero receipt
		// map and every frame is intra, so the head of the run is intra-sized. Once
		// the pace has settled and the headset stops dropping, the encoder predicts
		// and the frames fall to a fraction of that. Quarters rather than halves so
		// the settling itself does not sit inside the "after".
		if (rc_trace.size() >= 8)
		{
			const size_t q = rc_trace.size() / 4;
			double head = 0, tail = 0;
			for (size_t k = 0; k < q; ++k)
				head += double(rc_trace[k].bytes);
			for (size_t k = rc_trace.size() - q; k < rc_trace.size(); ++k)
				tail += double(rc_trace[k].bytes);
			head /= double(q);
			tail /= double(q);
			std::printf("bytes per sent frame: first %zu %.0f B, last %zu %.0f B (%.2fx)\n",
			            q, head, q, tail, tail > 0 ? head / tail : 0.0);
		}
	}
	std::printf("\n");

	if (const auto * rs = dec->receiver_stats())
		post_stats = *rs;

	if (aimd_ceiling)
	{
		// Drain anything the decoder produced after the last presented frame.
		pump_aimd(frames);

		const uint32_t settled = aimd.current();
		std::printf("automatic bitrate: %.1f -> %.1f Mbit/s over %u frames, %zu change(s), "
		            "%llu frame(s) reported never delivered\n",
		            double(aimd_ceiling) * 1e-6, double(settled) * 1e-6, frames,
		            aimd_changes.size(), (unsigned long long)host.lost_reports);
		for (const auto & [f, b]: aimd_changes)
			std::printf("    frame %4llu: %.1f Mbit/s\n", (unsigned long long)f, double(b) * 1e-6);

		// The claim this option exists to check. On a link the harness is not losing
		// anything on, the control law must leave the ceiling where it is: it used to
		// walk this to the floor and stay there, because the delivery reports it was
		// reading described frames that did not exist (a per-eye survivor counter for
		// a frame index) and never mentioned the frames that were actually lost. See
		// tests/bitrate_nxwarp_test.cpp, which isolates both halves of that.
		if (loss <= 0)
		{
			check(settled >= uint32_t(aimd_ceiling),
			      "a clean link holds the automatic bitrate at its ceiling");
			check(host.lost_reports == 0,
			      "a clean link reports no undelivered frames (" +
			              std::to_string(host.lost_reports) + ")");
		}
		else
		{
			// With loss injected the reports must actually reach the controller --
			// silence here was the bug, and "it did not decrease" would be the
			// symptom of it coming back.
			check(host.lost_reports > 0,
			      "a lossy link reports frames that never arrived (" +
			              std::to_string(host.lost_reports) + ")");
			check(settled < uint32_t(aimd_ceiling),
			      "a lossy link makes the automatic bitrate back off");
		}
	}

	if (bitrate and not rc_trace.empty())
	{
		// Frame by frame, then the tail. The tail is the number that matters:
		// the controller is allowed to spend the first frames walking to the
		// target, and what it must not do is sit away from it once it arrives.
		std::printf("rate control trace (frame: QP, bytes, target):\n");
		for (size_t i = 0; i < rc_trace.size(); ++i)
			std::printf("  %3zu  QP %2u  %7llu B  target %7.0f B  %+6.1f%%\n",
			            i, unsigned(rc_trace[i].qp),
			            (unsigned long long)rc_trace[i].bytes, rc_trace[i].target,
			            rc_trace[i].target > 0
			                    ? 100.0 * (double(rc_trace[i].bytes) - rc_trace[i].target) / rc_trace[i].target
			                    : 0.0);

		const size_t tail_from = rc_trace.size() * 2 / 3;
		double tail_bytes = 0, tail_target = 0;
		uint32_t tail_qp_lo = 63, tail_qp_hi = 0;
		size_t tail_n = 0;
		for (size_t i = tail_from; i < rc_trace.size(); ++i)
		{
			tail_bytes += double(rc_trace[i].bytes);
			tail_target += rc_trace[i].target;
			tail_qp_lo = std::min(tail_qp_lo, rc_trace[i].qp);
			tail_qp_hi = std::max(tail_qp_hi, rc_trace[i].qp);
			++tail_n;
		}
		if (tail_n)
		{
			const double mean = tail_bytes / double(tail_n);
			const double want = tail_target / double(tail_n);
			std::printf("\nlast %zu frames: %.0f B/frame against a %.0f B target (%+.1f%%), QP %u..%u\n\n",
			            tail_n, mean, want,
			            want > 0 ? 100.0 * (mean - want) / want : 0.0,
			            unsigned(tail_qp_lo), unsigned(tail_qp_hi));
			if (rc == "auto")
			{
				check(want > 0, "the controller was given a byte budget");

				// A ceiling is a ceiling: going over it is the failure, and the
				// controller must not. Coming in under it is only correct when
				// the quantiser has run out of band -- at min_qp the frames are
				// as large as this encoder is willing to make them, and a
				// ceiling above that is simply more link than the picture needs.
				check(mean <= 1.25 * want,
				      "bytes per frame stay within 25% above the ceiling's budget");
				// The bottom of the band is the EFFECTIVE floor, not min-qp:
				// the transport ceiling can hold the quantiser above what the
				// operator asked for, and while it does, coming in under the
				// bitrate budget is the controller doing as it was told rather
				// than failing to converge. Still a real check -- a controller
				// sitting above its floor while under budget still fails.
				uint32_t tail_floor = 0;
				for (size_t i = tail_from; i < rc_trace.size(); ++i)
					tail_floor = std::max(tail_floor, rc_trace[i].floor);
				check(mean >= 0.75 * want or tail_qp_lo <= tail_floor,
				      "bytes per frame come in under the budget only at the bottom of the QP band");

				// And it has to have STOPPED. Bytes on target with the
				// quantiser still swinging is a controller that is averaging,
				// not converging, and the swing is visible as pumping.
				check(tail_qp_hi - tail_qp_lo <= 2,
				      "the quantiser settles into a band of at most 2 QP");
			}
			else
			{
				check(tail_qp_lo == tail_qp_hi and tail_qp_lo == qp,
				      "\"rc\": \"fixed\" leaves the quantiser exactly where it was configured");
			}
		}
	}

	// ==== assertions ==========================================================
	// --drop-datagram counts as lossy, obviously, and saying so is not a formality: the
	// checks gated on `clean` are the ones that read "nothing went missing, so nothing
	// may be missing", and a run that deliberately took a datagram off the wire is the
	// one input for which they are false by construction.
	const bool clean = loss <= 0 and drop_datagram == 0;

	// A THIRD regime, between "clean" and "lossy": the link delivered nothing at
	// all. `--loss 1` (or anything above it, which is how `--loss 3` behaves --
	// the draw is uniform(0,1) < loss) is a blackout, and it is a case worth
	// running: the encoder must keep coding, the transport must place nothing,
	// the decoder must publish nothing, and the process must end. What it is NOT
	// is a lossy run with unlucky numbers, and every assertion below that starts
	// "the frames that arrived..." is vacuous when none did.
	//
	// Six of them used to FAIL on this input, which is the worst answer a test can
	// give: the code was right, the checks were asking a question the input had
	// made meaningless, and a real regression would have been indistinguishable
	// from the noise. They are conditional now, and the blackout has assertions of
	// its own so the run still tests something.
	const bool blackout = link.sent > 0 and link.dropped == link.sent;
	if (blackout)
		std::printf("\nblackout: the link delivered none of %llu datagrams; checking that "
		            "nothing was invented rather than that anything arrived\n",
		            (unsigned long long)link.sent);

	if (not blackout)
		check(not host.frames.empty(), "frames decode");

	// Reassembly is judged on its own, separately from what the worker later does with
	// the queue: a frame that reassembled whole and was then dropped as stale
	// (kMaxQueuedFrames) is not a reassembly loss. With nothing lost on the link every
	// frame presented must reassemble whole no matter how the datagrams were ordered,
	// which is the windowed reassembler's whole contract.
	if (clean)
		check(host.units.size() == sent_frames,
		      "every frame SENT reassembled whole under reordering (" +
		              std::to_string(host.units.size()) + "/" + std::to_string(sent_frames) + ")");

	// Frames must reach the worker in frame order. from_headset::feedback::frame_index is
	// the SENDER's frame id widened (nxwarp_decoder::wire_frame_index), so the published
	// sequence must be strictly increasing in the stream's own 16-bit sequence space --
	// with gaps only where the bounded queue discarded a stale frame. This is what the
	// reassembly window promises and the old one-frame path got for free by never having
	// two frames open at once.
	//
	// Compared as 16-bit sequence differences rather than as widened values: that is the
	// space the ids actually live in, it wraps correctly, and it needs no special case
	// for a reconnect (the new decoder seeds its widening from whatever id is on the
	// wire when it starts, so its numbers continue the old one's rather than restarting).
	{
		bool ordered = true;
		bool have_prev = false;
		uint16_t prev = 0;
		for (const auto & f: host.frames)
		{
			const uint16_t idx = uint16_t(f.frame_index);
			if (have_prev and int16_t(uint16_t(idx - prev)) <= 0)
				ordered = false;
			prev = idx;
			have_prev = true;
		}
		check(ordered, "frames reach the worker and are published in frame order");
	}

	// Tiles placed after their band's deadline had already fired. Those tiles arrived,
	// and the encoder is told they did not: on a link with nothing wrong with it the
	// count must be near zero. It was 97 percent on a live headset when a band closed on
	// the first datagram of its own frame.
	if (const auto * rs = dec->receiver_stats())
	{
		std::printf("transport: %llu tiles placed, %llu after their band deadline (%.1f%%), "
		            "%llu duplicates, %llu stale-frame, %llu replay, %llu auth failures\n",
		            (unsigned long long)rs->tiles_placed, (unsigned long long)rs->tiles_late,
		            rs->tiles_placed ? 100.0 * double(rs->tiles_late) / double(rs->tiles_placed) : 0.0,
		            (unsigned long long)rs->duplicates, (unsigned long long)rs->stale_frame,
		            (unsigned long long)rs->replay, (unsigned long long)rs->auth_fail);
		if (blackout)
			check(rs->tiles_placed == 0 and rs->tiles_late == 0,
			      "a blackout places no tiles and marks none late");
		else
			check(rs->tiles_placed > 0 and rs->tiles_late * 10 < rs->tiles_placed,
			      "band deadlines leave the tiles that arrived counted as arrived (under 10% late)");
		check(rs->stale_frame == 0 and rs->replay == 0 and rs->auth_fail == 0,
		      "no datagram was refused by the transport as stale, replayed or unauthentic");
	}

	// How many frames get *published* is not a property of the decoder alone: the worker
	// keeps at most kMaxQueuedFrames and discards the rest as stale, so on a loaded box,
	// or at a resolution this machine cannot decode at the rate the loop presents, the
	// count moves between runs by design ("late is worse than missing"). What must not
	// move is that every frame is accounted for -- reassembled, or holed, or discarded as
	// stale -- and that is what the two checks above state. So this is a report, not an
	// assertion; asserting it would only make the test flaky about the machine.
	if (clean)
		std::printf("accounting: %u presented, %llu sent, %zu reassembled, %zu published, "
		            "%zu discarded as stale by the bounded worker queue\n",
		            frames, (unsigned long long)sent_frames, host.units.size(), host.frames.size(),
		            host.units.size() - std::min(host.units.size(), host.frames.size()));
	else if (blackout)
	{
		// The whole of what a blackout must be: the encoder tried, and nothing
		// downstream made anything up. A decoder that published a frame here
		// would be publishing a picture it never received.
		std::printf("accounting: %u presented, %llu sent, %zu reassembled, %zu published "
		            "(blackout)\n",
		            frames, (unsigned long long)sent_frames, host.units.size(),
		            host.frames.size());
		check(sent_frames > 0, "the encoder keeps coding into a dead link");
		check(host.units.empty(), "a blackout reassembles no frame");
		check(host.frames.empty(), "a blackout publishes no frame");
	}
	else
	{
		check(arrived or not host.frames.empty(),
		      "lossy run keeps publishing rather than stalling");

		// "Drops the frames with holes" is a statement about frames that HAVE
		// holes, and at a few percent loss the class-A parity often recovers
		// every one of them -- which is the FEC succeeding, not the decoder
		// failing. Asserting it unconditionally made the outcome a property of
		// the seed: over twelve seeds of `--backend ref --frames 12 --inter on
		// --loss 0.05`, two (3 and 5) lost 9 and 4 datagrams and still
		// reassembled 12 of 12, and both were reported as failures.
		//
		// So the invariant that always holds is asserted always -- nothing is
		// invented, at any loss rate -- and the stronger claim only when there
		// is a hole for it to be about.
		check(host.units.size() <= sent_frames and host.frames.size() <= host.units.size(),
		      "a lossy run invents no frame: published <= reassembled <= sent");
		const bool any_hole = host.units.size() < sent_frames;
		if (any_hole)
			check(host.frames.size() < sent_frames,
			      "lossy run drops the frames with holes rather than inventing them");
		else
			std::printf("note: every frame sent reassembled whole -- the parity covered "
			            "all %llu lost datagrams, so there was no hole to drop\n",
			            (unsigned long long)link.dropped);
	}

	if (reconnected)
	{
		const size_t units_after = host.units.size() - units_at_reconnect;
		const size_t after = host.frames.size() - frames_at_reconnect;
		const size_t presented_after = size_t(sent_frames - sent_at_reconnect);
		std::printf("reconnect: %zu of %zu frames sent after the new client appeared were "
		            "reassembled whole, %zu of them decoded\n",
		            units_after, presented_after, after);
		std::printf("reconnect: receiver of the NEW client saw %llu datagrams, placed %llu tiles; "
		            "rejects: auth_fail %llu, replay %llu, stale_frame %llu, bad_range %llu, "
		            "bad_directory %llu, bad_caps %llu, bad_version %llu\n",
		            (unsigned long long)post_stats.datagrams,
		            (unsigned long long)post_stats.tiles_placed,
		            (unsigned long long)post_stats.auth_fail,
		            (unsigned long long)post_stats.replay,
		            (unsigned long long)post_stats.stale_frame,
		            (unsigned long long)post_stats.bad_range,
		            (unsigned long long)post_stats.bad_directory,
		            (unsigned long long)post_stats.bad_caps,
		            (unsigned long long)post_stats.bad_version);
		check(link.headers_sent > headers_at_reconnect,
		      "the resumed session resent the stream header to the new client");
		check(post_stats.auth_fail == 0,
		      "the new client's receiver authenticated every datagram (auth_fail " +
		              std::to_string(post_stats.auth_fail) + ")");
		check(post_stats.replay == 0,
		      "no datagram was rejected by the new client's replay window (replay " +
		              std::to_string(post_stats.replay) + ")");
		check(clean ? units_after == presented_after : units_after > 0,
		      "every frame sent after the reconnect arrived whole (" +
		              std::to_string(units_after) + "/" + std::to_string(presented_after) + ")");
		check(after > 0, "the new client decoded and published frames (" + std::to_string(after) + ")");
	}

	// --- view_info -------------------------------------------------------------
	{
		size_t matched = 0, mismatched = 0, defaulted = 0;
		for (size_t i = 0; i < host.frames.size(); ++i)
		{
			// frame_index counts published frames from 1; presented frames are in order,
			// and a dropped frame simply never appears, so match on the pose itself.
			//
			// A default-constructed view_info is its own outcome, not a mismatch: the
			// field rides the frame's first datagram and nothing else carries it, so a
			// frame whose first datagram was lost and whose tiles the transport's FEC
			// then rebuilt arrives whole with no pose. The decoder publishes it with a
			// default rather than throwing away a picture that decoded (see
			// nxwarp_decoder::decode_unit). It must not happen on a link that lost
			// nothing.
			if (host.frames[i].view_info.display_time == 0)
			{
				++defaulted;
				continue;
			}
			bool found = false;
			for (const auto & p: presented)
			{
				if (same(p, host.frames[i].view_info))
				{
					found = true;
					break;
				}
			}
			found ? ++matched : ++mismatched;
		}
		check(mismatched == 0,
		      "every published view_info that arrived is bit-identical to the presented one (" +
		              std::to_string(matched) + "/" + std::to_string(host.frames.size()) + ")");
		// "lost nothing" has to include "reordered nothing past the point of no
		// return". A datagram held back further than the reassembly window is, to
		// the frame it belonged to, exactly as absent as one that was dropped: the
		// frame closes without it, the FEC rebuilds its tiles from the parity, and
		// the view_info that rode the WiVRn packet around those tiles is gone with
		// it. --reorder-depth past a frame's worth of datagrams reaches that, and
		// the reference backend reaches it sooner than the Vulkan one because its
		// frames are fewer and larger datagrams. It is the same defect the note
		// above describes under loss, found by a second input.
		const bool deep_reorder = reorder > 0 and reorder_depth > 3;
		if (defaulted and not(clean and deep_reorder))
			std::printf("note: %zu published frame%s had no view_info -- its first "
			            "datagram was lost and its tiles were recovered by FEC\n",
			            defaulted, defaulted == 1 ? "" : "s");
		check(not clean or deep_reorder or defaulted == 0,
		      "a link that lost nothing publishes no frame with a default pose");
		if (clean and deep_reorder and defaulted)
			std::printf("note: %zu published frame%s had no view_info under a "
			            "reorder depth of %u, with nothing lost -- the first "
			            "datagram arrived after its frame had closed\n",
			            defaulted, defaulted == 1 ? "" : "s", reorder_depth);

		// And that it is not merely a default that happens to compare equal.
		bool any_nonzero = false;
		for (const auto & p: host.frames)
			any_nonzero |= p.view_info.display_time != 0;
		// Nothing published means no pose to be real, which is a statement about
		// the input and not about the decoder. Under a blackout the assertion
		// above -- that nothing was published at all -- is the one that matters.
		if (not blackout)
			check(any_nonzero,
			      "published view_info is a real pose, not a default-constructed one");
	}

	// --- feedback reached the encoder shadow ------------------------------------
	// The encoder folds feedback into nxt::Sender's client shadow and derives its
	// per-tile receipt map from it. A run that produced feedback at all, and an encoder
	// that accepted every packet without throwing, is what this level can observe from
	// outside; the shadow's own state is checked by nx-warp's transport tests.
	// --drop-datagram: what one lost datagram actually cost, as a SET of tiles.
	//
	// This is the measurement the per-tile span mapping exists for, and it is made
	// against the positive acknowledgement itself rather than against a total. The
	// encoder's client shadow holds, per tile of the frame, what the headset's feedback
	// said about that position; so for the frame that carried the dropped datagram:
	//
	//   every tile the sender OFFERED and did not drop must read `received`
	//   every tile it dropped must read `concealed`
	//
	// Both halves matter and neither alone is the claim. The first is "and nothing
	// else": under the chunk mapping a datagram is a slice of the frame's byte stream,
	// so the tiles either side of the hole are bytes of the same tiles and the loss
	// spreads. The second is "exactly its tiles": a claim that only bounded the damage
	// from above would pass on a run where the FEC quietly repaired everything, which is
	// why the parity of the dropped datagram's own group goes with it.
	//
	// Tiles the sender never offered are outside the claim and are not looked at. A
	// frame codes the tiles it codes; a position it skipped was never on the wire, and
	// what the shadow says about it is a question for the receipt map's own semantics,
	// not for this.
	if (drop_datagram and link.dropped_runs.empty())
		std::printf("note: --drop-datagram %u named a datagram past the end of this "
		            "run (%llu data datagrams went out); nothing was dropped\n",
		            drop_datagram, (unsigned long long)link.data_sent);
	if (drop_datagram and not link.dropped_runs.empty())
	{
		std::printf("one datagram dropped: %u tile(s) in %zu run(s), %llu parity "
		            "datagram(s) of the same FEC group(s) suppressed with them; "
		            "%llu tiles offered over the run; %zu of %llu frames reassembled\n",
		            link.dropped_tiles,
		            link.dropped_runs.size(),
		            (unsigned long long)link.dropped_parity,
		            (unsigned long long)link.tiles_offered,
		            host.units.size(),
		            (unsigned long long)sent_frames);

		// Per frame: what was offered, and what of it went missing.
		std::map<uint16_t, std::set<uint32_t>> offered_by_frame, lost_by_frame;
		for (const auto & r: link.offered_runs)
		{
			for (uint32_t t = r.first; t < r.first + r.count; ++t)
				offered_by_frame[r.frame_id].insert(t);
		}
		for (const auto & r: link.dropped_runs)
		{
			for (uint32_t t = r.first; t < r.first + r.count; ++t)
				lost_by_frame[r.frame_id].insert(t);
		}

		size_t frames_checked = 0, wrongly_concealed = 0, wrongly_received = 0;
		size_t unknown_offered = 0;
		for (const auto & [fid, lost]: lost_by_frame)
		{
			auto it = dropped_frame_receipts.find(fid);
			if (it == dropped_frame_receipts.end())
				continue; // no feedback ever covered it: nothing to read
			const auto & receipts = it->second;
			++frames_checked;
			for (uint32_t t: offered_by_frame[fid])
			{
				if (t >= receipts.size())
					continue;
				const bool was_lost = lost.count(t) != 0;
				if (receipts[t] == video_encoder_nxwarp::receipt_unknown)
				{
					++unknown_offered;
					continue;
				}
				const bool concealed =
				        receipts[t] == video_encoder_nxwarp::receipt_concealed;
				if (was_lost and not concealed)
					++wrongly_received;
				if (not was_lost and concealed)
					++wrongly_concealed;
			}
		}
		std::printf("per-tile receipts over %zu frame(s) that lost a datagram: "
		            "%zu tile(s) the link kept were reported concealed, "
		            "%zu tile(s) it dropped were reported received, "
		            "%zu offered tile(s) no feedback covered\n",
		            frames_checked, wrongly_concealed, wrongly_received, unknown_offered);
		// A run where no feedback came back at all says nothing either way, and
		// saying so is better than passing on an empty set.
		if (frames_checked == 0 or unknown_offered == offered_by_frame.size())
			std::printf("note: no feedback covered the frame that lost a datagram; "
			            "the receipt check had nothing to read\n");
		else
		{
			check(wrongly_concealed == 0,
			      "a lost datagram costs no receipt outside the tiles it carried (" +
			              std::to_string(wrongly_concealed) + " spread)");
			check(wrongly_received == 0,
			      "every tile of the lost datagram is reported not held (" +
			              std::to_string(wrongly_received) + " claimed held)");
		}
	}

	// --- the arrival-order model (docs/NXWARP-TILESTREAM.md section 6) -----------
	if (tile_stream)
	{
		// The grid the tile indices are in, taken from the widest index the sender
		// used rather than re-derived: under stereo it spans the eye PAIR
		// (common/nxwarp_stream_grid.h), and a model that guessed one eye's worth
		// would silently drop half of every frame.
		size_t n_tiles = 0;
		for (const auto & r: link.delivered_runs)
			n_tiles = std::max<size_t>(n_tiles, size_t(r.first) + r.count);

		atlas_model arrival(n_tiles), ordered(n_tiles);
		frame_id_unwrap unwrap;
		// Delivery order, unwrapped once, kept so that both paths see the same
		// frame numbers.
		std::vector<std::pair<int64_t, lossy_link::tile_run>> arrivals;
		arrivals.reserve(link.delivered_runs.size());
		int64_t last_frame = -1;
		for (const auto & r: link.delivered_runs)
		{
			const int64_t f = unwrap(r.frame_id);
			arrivals.emplace_back(f, r);
			last_frame = std::max(last_frame, f);
		}

		// The frame-complete path: the SAME delivered tiles, grouped by frame and
		// applied in frame order. That is the only difference between the two runs,
		// and it is the difference the proof is about.
		std::map<int64_t, std::vector<uint32_t>> by_frame;
		for (const auto & [f, r]: arrivals)
		{
			auto & v = by_frame[f];
			for (uint32_t t = r.first; t < r.first + r.count; ++t)
				v.push_back(t);
		}
		for (auto & [f, v]: by_frame)
		{
			std::sort(v.begin(), v.end());
			v.erase(std::unique(v.begin(), v.end()), v.end());
			ordered.apply_ordered(f, v);
		}

		// The tile-streaming path, in the order the link handed them over.
		for (const auto & [f, r]: arrivals)
			arrival.apply_arrival(f, r.first, r.count);

		// Both to the same frame before comparing: see atlas_model::flush.
		arrival.flush(last_frame);
		ordered.flush(last_frame);

		// How many runs SHOULD have been superseded, computed from the delivery
		// order alone and not from the model: a run of frame N is superseded when
		// every position it covers has already had a tile of some frame > N
		// delivered. This is the assertion section 6 asks for -- the number is
		// computable from the link's own order, so it is not a report.
		std::vector<int64_t> newest(n_tiles, -1);
		uint64_t expect_superseded_runs = 0, expect_superseded_tiles = 0;
		for (const auto & [f, r]: arrivals)
		{
			bool any = false, all_super = true;
			for (uint32_t t = r.first; t < r.first + r.count and t < n_tiles; ++t)
			{
				any = true;
				if (newest[t] >= f)
					++expect_superseded_tiles;
				else
				{
					all_super = false;
					newest[t] = f;
				}
			}
			if (any and all_super)
				++expect_superseded_runs;
		}

		std::printf("tile-stream model: %zu tile positions, %zu run(s) delivered over "
		            "%zu frame(s); arrival order applied %llu run(s) / %llu tile(s), "
		            "superseded %llu run(s) / %llu tile(s)\n",
		            n_tiles, arrivals.size(), by_frame.size(),
		            (unsigned long long)arrival.applied_runs,
		            (unsigned long long)arrival.applied_tiles,
		            (unsigned long long)arrival.superseded_runs,
		            (unsigned long long)arrival.superseded_tiles);
		check(arrival.superseded_runs == expect_superseded_runs and
		              arrival.superseded_tiles == expect_superseded_tiles,
		      "every superseded tile is one the delivery order predicts (" +
		              std::to_string(arrival.superseded_tiles) + " model, " +
		              std::to_string(expect_superseded_tiles) + " predicted)");
		// THE proof obligation of section 6.
		const size_t diff = arrival.first_difference(ordered);
		check(arrival.same_state_as(ordered),
		      "the atlas after the last frame is identical whether tiles were applied "
		      "in arrival order or in frame order" +
		              (diff == size_t(-1) ? std::string()
		                                  : " (first difference at tile " +
		                                            std::to_string(diff) + ")"));
		if (arrival.superseded_tiles == 0)
			std::printf("note: no tile was superseded in this run, so the model "
			            "exercised the monotonicity rule only in the trivial "
			            "direction -- --reorder is what makes it fire\n");
	}

	// --- which mapping the frames actually went out under ------------------------
	//
	// P1b of docs/NXWARP-TILESTREAM.md, made observable. "The backend reports spans" is
	// a property of the backend; "this frame used them" is a property of the frame, and
	// only the second one is what a lost datagram's cost depends on. The reference
	// backend's C ABI has no tile offset, so it must show 0 span frames and the fallback
	// must carry it; the Vulkan backend has the offsets and should show the opposite.
	{
		const auto p = enc.profile();
		if (p.span_frames or p.chunk_frames)
			std::printf("tile mapping: %llu frame(s) sent with per-tile spans "
			            "(%llu coded tiles), %llu with the fixed-chunk fallback\n",
			            (unsigned long long)p.span_frames,
			            (unsigned long long)p.span_tiles,
			            (unsigned long long)p.chunk_frames);
		// The identity claim itself: one transport slot per coded tile, at its own
		// index, and no slot spent on anything else. Only assertable when every frame
		// took the same mapping, since the two put different numbers of tiles on the
		// wire and the link counts one total.
		if (p.span_frames and not p.chunk_frames)
			check(link.tiles_offered == p.span_tiles,
			      "every coded tile got exactly one transport slot (" +
			              std::to_string(link.tiles_offered) + " on the wire, " +
			              std::to_string(p.span_tiles) + " coded)");
		if (backend == "ref")
			check(p.span_frames == 0,
			      "the reference backend reports no tile spans and takes the "
			      "chunk mapping");
	}

	check(link.sent > 0, "encoder produced datagrams");
	std::printf("feedback: %llu packets, %llu bytes returned to the encoder\n",
	            (unsigned long long)feedback_packets, (unsigned long long)feedback_bytes);
	if (blackout)
	{
		// A receiver that hears nothing has nothing to report ON: it has no frame
		// id, no band structure and no tile map, so it cannot NACK. The encoder
		// learns from the SILENCE -- no feedback and no not-held report is what a
		// dead link looks like from the server, and it is why the pacing and
		// resync paths must not require feedback to make progress.
		check(feedback_packets == 0 and feedback_bytes == 0,
		      "a blackout produces no feedback, because there is nothing to report on");
	}
	else
	{
		check(feedback_packets > 0,
		      "the decoder's band deadlines produced feedback (" +
		              std::to_string(feedback_packets) + " packets)");
		check(feedback_bytes > 0, "the feedback carried a payload");
	}
	// on_nxwarp_feedback hands the bytes to nxt::Sender, which rejects a packet it
	// cannot parse. Every packet was accepted, so the encoder's shadow took all of them.
	if (not blackout)
		check(feedback_packets >= host.frames.size() / 2,
		      "feedback arrives at roughly frame rate, not once at the end");

	// --- byte identity with the reference decoder --------------------------------
	//
	// `units` is what the decoder's own reassembly produced, captured through
	// nxwarp_host::on_frame_unit -- so this is not a re-derivation of the stream, it is
	// the stream. Prefixed with the codec stream header off the control socket, it is a
	// complete .nxv, which nx-warp's own nxv-dec can decode. If the GPU decoder and the
	// reference decoder agree byte for byte then their PSNR against the source is
	// identical by construction.
	//
	// The reference is built from the units the decoder actually CONSUMED, in the order
	// it consumed them (nxwarp_host::on_frame_decoded), not from every unit the
	// reassembler produced. On an intra stream the two lists differ only in length and
	// it makes no difference. On an INTER stream it is the whole comparison: a frame
	// that was reassembled and then dropped before the codec saw it never entered this
	// decoder's reference ring, so handing it to nxv-dec would give the two decoders
	// different chains and compare pictures that were never meant to be equal. Feeding
	// the reference exactly what this decoder was fed is the only way the question
	// "does it produce the same pixels" has an answer.
	//
	// The gapped list decodes because the gaps are exactly where the encoder was told
	// the client held nothing and answered with an all-intra frame; a resync frame
	// needs no predecessor, so nxv-dec picks the stream back up there too.
	if (not host.units.empty() and not link.stream_header.empty())
	{
		std::vector<size_t> ref_unit_pos; // index into host.units, in decode order
		{
			std::unordered_map<uint16_t, size_t> pos;
			for (size_t k = 0; k < host.unit_frame_ids.size(); ++k)
				pos.emplace(host.unit_frame_ids[k], k);
			for (uint16_t id: host.decoded_ids)
			{
				auto it = pos.find(id);
				if (it != pos.end())
					ref_unit_pos.push_back(it->second);
			}
		}
		std::vector<uint8_t> nxv = link.stream_header;
		for (size_t k: ref_unit_pos)
			nxv.insert(nxv.end(), host.units[k].begin(), host.units[k].end());
		write_file(nxv_out, nxv);
		std::printf("wrote %s: stream header + %zu frame units the decoder consumed "
		            "(of %zu reassembled), %zu bytes\n",
		            nxv_out.c_str(), ref_unit_pos.size(), host.units.size(), nxv.size());

		std::vector<uint8_t> gpu;
		for (const auto & pic: host.pictures)
			gpu.insert(gpu.end(), pic.begin(), pic.end());
		if (not decoded_out.empty())
			write_file(decoded_out, gpu);

		const fs::path ref_yuv = fs::path(nxv_out).replace_extension(".ref.yuv");
		const std::string cmd = nxv_dec + " --in " + nxv_out + " --out " + ref_yuv.string() +
		                        " --pix yuv420p --quiet";
		const int rc = std::system(cmd.c_str());
		if (rc != 0)
		{
			std::printf("note: %s returned %d; skipping the byte-identity check\n",
			            nxv_dec.c_str(), rc);
			check(false, "nxv-dec decoded the same stream");
		}
		else
		{
			auto ref = read_file(ref_yuv);
			check(not ref.empty(), "nxv-dec produced output");

			// Aligned by index, not by position. `units` is every frame the
			// reassembler produced, in order, so nxv-dec's nth picture is unit n --
			// but the GPU decoder need not have published all of them: the worker's
			// queue is bounded (kMaxQueuedFrames) and discards a frame that went
			// stale while it was busy. from_headset::feedback::frame_index is stamped
			// when the unit is handed over, counting from 1, so it is exactly the
			// index of that frame in `units` plus one. Comparing positionally instead
			// would report a byte difference the moment one frame was dropped late,
			// which says nothing about either decoder.
			//
			// Across a reconnect that counter belongs to the decoder that stamped it:
			// the new client starts it again at one, and its units sit after the old
			// client's in the same `units` list. So a picture published after the
			// reconnect is offset by however many units the old client had produced.
			// The frame ids the decoder reports with each unit are what checks the
			// arithmetic rather than assuming it.
			// The DECODED frame, which is the pair when the eyes are paired -- both
			// nxv-dec and the GPU decoder emit one picture `eyes * width` across, so
			// dividing by a per-eye frame here would cut every picture in half and
			// compare the wrong bytes.
			const size_t frame_size = pair_frame_bytes;
			const size_t ref_frames = ref.size() / frame_size;
			const size_t gpu_frames = gpu.size() / frame_size;
			std::printf("nxv-dec decoded %zu frames, the GPU decoder published %zu "
			            "(%zu unit%s dropped late by the worker's bounded queue)\n",
			            ref_frames, gpu_frames,
			            host.units.size() - std::min(host.units.size(), gpu_frames),
			            host.units.size() - std::min(host.units.size(), gpu_frames) == 1 ? "" : "s");
			check(gpu_frames > 0, "both decoders produced frames to compare");
			check(ref_frames == host.decoded_ids.size(),
			      "nxv-dec decoded every unit this decoder consumed (" +
			              std::to_string(ref_frames) + "/" +
			              std::to_string(host.decoded_ids.size()) + ")");

			// A published picture is matched to nxv-dec's frame by the stream's own
			// 16-bit frame id, which both sides carry: the decoder reports it with
			// every unit (unit_frame_ids, in the order the units were concatenated
			// and therefore in nxv-dec's own frame order) and the feedback carries it
			// widened. No arithmetic on positions, so this needs no special case for
			// a reconnect and none for the 16-bit wrap.
			std::unordered_map<uint16_t, size_t> unit_of_id;
			for (size_t k = 0; k < host.unit_frame_ids.size(); ++k)
				unit_of_id.emplace(host.unit_frame_ids[k], k);
			// Where each frame sits in the REFERENCE stream, which is the units the
			// decoder consumed in the order it consumed them -- see the .nxv above.
			// A published frame is always in this map; a reassembled-then-dropped one
			// is not, and is not in nxv-dec's output either.
			std::unordered_map<uint16_t, size_t> ref_of_id;
			for (size_t k = 0; k < host.decoded_ids.size(); ++k)
				ref_of_id.emplace(host.decoded_ids[k], k);

			size_t compared = 0, differing = 0, first_diff_frame = 0, misplaced = 0, unmatched = 0;

			// EVERY published frame is compared, on an inter stream as much as on an
			// intra one.
			//
			// This used to compare only the longest unbroken run of published units
			// from the start of the stream, on the argument that a frame predicts from
			// a ring holding pictures this decoder itself reconstructed, so a frame
			// after a gap has no reference chain and cannot be expected to match. That
			// argument was true, and it was hiding the bug: it is exactly the state in
			// which a real headset shows a few blocks of picture and the rest grey.
			// The gap is not something to excuse in the comparison, it is something
			// the encoder has to be told about -- and now is
			// (from_headset::nxwarp_frame_not_held), so the frame after a gap is coded
			// all-intra and matches whatever this decoder skipped.
			//
			// Keeping the excuse would mean the harness could not tell the fix from
			// the bug, which is the only reason it was ever worth removing.
			for (size_t j = 0; j < gpu_frames and j < host.frames.size(); ++j)
			{
				const uint16_t id = uint16_t(host.frames[j].frame_index);
				auto it = unit_of_id.find(id);
				if (it == unit_of_id.end())
				{
					++unmatched;
					continue;
				}
				auto rit = ref_of_id.find(id);
				if (rit == ref_of_id.end() or rit->second >= ref_frames)
				{
					++unmatched;
					continue;
				}
				const size_t unit_idx = rit->second;

				// And the picture really is the frame that id names: it carries the
				// pose it was presented with, so the presented frame it matches must
				// be the one that id counts to.
				{
					size_t src_idx = presented.size();
					for (size_t k = 0; k < presented.size(); ++k)
					{
						if (same(presented[k], host.frames[j].view_info))
						{
							src_idx = k;
							break;
						}
					}
					// Which composited frame this wire id was assigned to. Under
					// pacing the two are not the same counter -- the wire sequence
					// is dense over the frames that were SENT -- so the mapping is
					// the one the send loop recorded, not an offset from the first
					// frame id.
					auto pit = presented_of_wire_id.find(id);
					if (src_idx < presented.size() and
					    (pit == presented_of_wire_id.end() or pit->second != src_idx))
						++misplaced;
				}
				const size_t r = unit_idx * frame_size;
				const size_t g = j * frame_size;
				++compared;
				if (std::memcmp(ref.data() + r, gpu.data() + g, frame_size) != 0)
				{
					if (not differing)
						first_diff_frame = unit_idx;
					++differing;
				}
			}
			check(unmatched == 0,
			      "every published picture names a unit the reassembler produced (" +
			              std::to_string(unmatched) + " unmatched)");
			check(misplaced == 0,
			      "every published picture lines up with the unit whose frame id it carries (" +
			              std::to_string(misplaced) + " misplaced)");
			const bool identical = compared > 0 and differing == 0;
			check(identical,
			      "GPU decoder output is byte-identical to nxv-dec's over all " +
			              std::to_string(compared) + " published frames");
			if (not identical)
				std::printf("  %zu of %zu frames differ, first at unit %zu\n",
				            differing, compared, first_diff_frame);

			// PSNR against the source, per frame, which is what the number actually
			// means to a viewer.
			double worst = 1e9, total = 0;
			size_t counted = 0;
			for (size_t i = 0; i < host.pictures.size(); ++i)
			{
				size_t src_idx = presented.size();
				for (size_t j = 0; j < presented.size(); ++j)
				{
					if (same(presented[j], host.frames[i].view_info))
					{
						src_idx = j;
						break;
					}
				}
				if (src_idx >= source_frames.size())
					continue;
				// No inter exemption here either, for the reason the byte
				// comparison gives: a frame whose reference this decoder never
				// reconstructed is now coded all-intra, so it is a picture this
				// codec produced and it has to look like one.
				//
				// The one frame that is excluded is the one whose FIRST datagram
				// was lost, so it arrived with no view_info and was published with
				// a default pose and foveation (docs/nxwarp.md). That is a
				// documented degradation, the harness has already counted and
				// printed it, and PSNR against the source measures the missing
				// pose rather than anything the codec did -- the byte comparison
				// above still covers the frame, and it is the check that would
				// catch a decode fault in it.
				if (host.frames[i].view_info.display_time == 0)
					continue;
				const double v = psnr(host.pictures[i], source_frames[src_idx]);
				if (v < 0)
					continue;
				worst = std::min(worst, v);
				total += v;
				++counted;
			}
			if (counted)
			{
				std::printf("PSNR vs source over %zu frames: mean %.2f dB, worst %.2f dB\n",
				            counted, total / double(counted), worst);

				// --- the two halves, separately, when there are two ---------------
				//
				// eyes-gated because there is only one half at one eye, and the
				// whole-picture number above already is it. It is NOT a weaker
				// statement than the mono one: the whole-picture PSNR already fails
				// when a half is wrong, and this only says WHICH half -- which is the
				// difference between "pair_compose put the wrong pixels in the right
				// eye" and "the codec had a bad day", and the only reason this run
				// exists.
				if (eyes == 2)
				{
					double lo[2] = {1e9, 1e9};
					double sum[2] = {0, 0};
					size_t n_half = 0;
					for (size_t i = 0; i < host.pictures.size() and i < host.frames.size(); ++i)
					{
						size_t src_idx = presented.size();
						for (size_t j = 0; j < presented.size(); ++j)
							if (same(presented[j], host.frames[i].view_info))
							{
								src_idx = j;
								break;
							}
						if (src_idx >= source_frames.size() or
						    host.frames[i].view_info.display_time == 0)
							continue;
						const double l = psnr_half(host.pictures[i], source_frames[src_idx],
						                           pair_width, height, 0);
						const double r = psnr_half(host.pictures[i], source_frames[src_idx],
						                           pair_width, height, 1);
						if (l < 0 or r < 0)
							continue;
						lo[0] = std::min(lo[0], l);
						lo[1] = std::min(lo[1], r);
						sum[0] += l;
						sum[1] += r;
						++n_half;
					}
					if (n_half)
					{
						std::printf("stereo compose: left eye mean %.2f dB (worst %.2f), "
						            "right eye mean %.2f dB (worst %.2f) over %zu frames\n",
						            sum[0] / double(n_half), lo[0],
						            sum[1] / double(n_half), lo[1], n_half);
						const double mean_l = sum[0] / double(n_half);
						const double mean_r = sum[1] / double(n_half);

						// THE MEAN, NOT THE WORST FRAME, and the reason is that
						// this is an IDENTITY check and not a quality gate.
						//
						// What it exists to catch is a compose that put the wrong
						// pixels in a half: a dropped layer leaves it undefined, a
						// duplicated one leaves it holding the other eye's pixels
						// against an inverted reference. Those are WIRING faults --
						// systematic, every frame, and tens of dB down. Measured
						// negative control, filling layer 1 with the left eye
						// instead of make_right_eye's: the right half scores
						// 6.73 dB, against a healthy 36.5.
						//
						// A per-frame floor was the wrong shape for that. On a lossy
						// inter run a single frame can legitimately be a stale warp
						// -- this file says so itself, thirty lines below, when it
						// separates "opening" from "continuing" frames -- and how
						// many such frames get published depends on how many units
						// the bounded worker queue drops late, which is machine
						// dependent: 20 runs of --eyes 2 --backend vk --inter on
						// published between 3 and 6 frames of the same 12. Across
						// those 40 half-scores the means ran 30.95 to 36.60 dB and
						// every run passed -- but the WORST single frame fell to
						// 21.95 dB, under two dB from a 20 dB per-frame floor, on an
						// idle machine. A slower one drops more units late, publishes
						// more stale warps, and goes under. That would be a flake,
						// not a finding.
						//
						// 25 dB is the floor because it is 18.3 dB above the measured
						// wrong-eye score and 5.9 dB below the lowest mean observed
						// across those runs, so it separates the two populations with
						// margin on both sides.
						check(mean_r > 25.0,
						      "the RIGHT eye of the composed pair is the right eye "
						      "(mean PSNR > 25 dB)");
						check(mean_l > 25.0,
						      "the LEFT eye of the composed pair is the left eye "
						      "(mean PSNR > 25 dB)");

						// And the two halves must agree with each other. This is the
						// orthogonal half of the same question: it catches a swapped
						// or duplicated layer WITHOUT depending on an absolute
						// quality level at all, so it still bites on a run whose
						// picture is poor for reasons of its own -- a low QP, a hard
						// fixture, heavy loss -- where an absolute floor would have
						// to be loosened until it stopped meaning anything.
						//
						// The two eyes carry the same content at the same QP, so on a
						// clean run they land within 0.1 dB of each other. They do NOT
						// always: a stale warp lands in one half and not the other,
						// and the widest gap over the 10 runs above was 5.35 dB. The
						// negative control puts them 25.5 dB apart. 12 dB sits between
						// those -- 6.6 dB clear of the worst honest run, 13.5 dB under
						// the control -- which is why the tolerance is not the 6 dB
						// the clean runs alone would suggest.
						check(std::abs(mean_l - mean_r) < 12.0,
						      "the two halves of the composed pair agree "
						      "(means within 12 dB)");
					}
				}
				// Which frames the low scores belong to, and whether each one opened
				// a run or continued one. A frame that continues a run is inter and
				// may legitimately be a stale warp; a frame that OPENS one is the
				// encoder's all-intra resync, and a bad score there is a fault.
				size_t low_opening = 0, low_continuing = 0;
				for (size_t i = 0; i < host.pictures.size() and i < host.frames.size(); ++i)
				{
					size_t src_idx = presented.size();
					for (size_t j = 0; j < presented.size(); ++j)
						if (same(presented[j], host.frames[i].view_info))
						{
							src_idx = j;
							break;
						}
					if (src_idx >= source_frames.size() or
					    host.frames[i].view_info.display_time == 0)
						continue;
					const double v = psnr(host.pictures[i], source_frames[src_idx]);
					if (v >= 0 and v < 20.0)
					{
						const uint16_t id = uint16_t(host.frames[i].frame_index);
						size_t pos = host.decoded_ids.size();
						for (size_t k = 0; k < host.decoded_ids.size(); ++k)
							if (host.decoded_ids[k] == id)
							{
								pos = k;
								break;
							}
						const bool opens_run =
						        pos == 0 or pos >= host.decoded_ids.size() or
						        uint16_t(id - host.decoded_ids[pos - 1]) != 1;
						opens_run ? ++low_opening : ++low_continuing;
						std::printf("  low PSNR %.2f dB at frame id %u (%s)\n", v,
						            unsigned(id),
						            opens_run ? "opens a run: the encoder's all-intra resync"
						                      : "continues a run: inter");
					}
				}
				// A frame that OPENS a run is the encoder's answer to a not-held
				// report: an all-zero receipt map, every tile coded INTRA. It owes
				// nothing to any reference and there is no excuse for it to be a bad
				// picture, on any link. This is the assertion that caught the resync
				// test being written as "this frame or any later one" instead of
				// "this frame", which passed every inter frame after a resync and put
				// the corruption back.
				check(low_opening == 0,
				      "every frame that resynchronises the stream is a good picture (" +
				              std::to_string(low_opening) + " below 20 dB)");
				if (low_continuing)
					std::printf("  %zu inter frame%s below 20 dB: a tile lost on the wire is\n"
					            "  a stale warp until the receipt map codes it fresh one frame\n"
					            "  later, which is the documented recovery latency\n",
					            low_continuing, low_continuing == 1 ? "" : "s");
				// On an intra stream, and on any clean link, every frame stands on its
				// own and must look like its source. On an inter stream with loss or
				// reordering injected, a frame in the one-frame window between a tile
				// being lost and the receipt map coding it fresh is a stale warp, so
				// the per-frame floor moves to the mean and the resync guarantee above
				// carries the weight.
				const bool inter_lossy = inter == "true" and (loss > 0 or reorder > 0);
				if (not inter_lossy)
					check(worst > 20.0, "every decoded frame resembles its source (PSNR > 20 dB)");
				else
					check(total / double(counted) > 30.0,
					      "the stream as a whole resembles its source (mean PSNR > 30 dB)");
			}
		}
	}

	// What the encode actually cost, from the encoder's own measurement of the
	// interval around codec->encode(). This is the number that decides whether
	// a backend can hold a frame budget, so print it whether the run passed or
	// failed.
	// The per-stage latency budget. Printed before the encode summary because it is
	// the wider statement: the encode line is one row of it.
	if (host.stages.n)
	{
		const auto & g = host.stages;
		const double n = double(g.n);
		auto row = [&](const char * what, double sum) {
			std::printf("  %-26s %7.2f ms  %5.1f%%\n", what, sum / n,
			            g.total > 0 ? 100.0 * sum / g.total : 0.0);
		};
		std::printf("\nlatency budget, %zu frames from encode to blit (one process, "
		            "one clock)\n",
		            g.n);
		row("encode", g.encode);
		row("pacing + queue to send", g.wait_send);
		row("datagrams out", g.send);
		row("first byte across", g.net);
		row("frame on the wire", g.span);
		row("bounded worker queue", g.queue);
		row("decode", g.decode);
		row("decode to blit", g.present);
		std::printf("  %-26s %7.2f ms\n", "total", g.total / n);
	}

	{
		const auto p = enc.profile();
		if (p.frames)
			std::printf("\nencode (%s backend, %ux%u, QP %u): mean %.2f ms, "
			            "worst %.2f ms over %llu frames, %llu bytes/frame\n",
			            backend.c_str(), width, height, qp,
			            p.total_ms / double(p.frames), p.max_ms,
			            (unsigned long long)p.frames,
			            (unsigned long long)(p.bytes / p.frames));
		/* The effort level THE RUN ACTUALLY USED, read back out of the
		 * published stats rather than echoed from the command line: with no
		 * --effort the option is never put in the map, so this line is the
		 * server's own default arriving from the other side of the encoder.
		 * The level leaves no tool bit, so the stats are the only place it
		 * can be read at all. */
		const uint32_t used = enc.resolved_effort();
		std::printf("effort: %u (%s), %s\n",
		            used,
		            used >= 1 ? "integer requantiser" : "dead-zone quantiser",
		            effort.empty() ? "the server's default, not asked for"
		                           : "asked for on the command line");
		/* The identity count, from the published stats the encoder filled --
		 * the same number the dashboard card shows, so a harness run and a
		 * live session cannot disagree about it. */
		if (wivrn::nxwarp_stats_last.identity_tiles_total)
			std::printf("identity tiles: %llu of %llu (%.1f %%), "
			            "snap-identity %u/16 sample%s\n",
			            (unsigned long long)wivrn::nxwarp_stats_last.identity_tiles,
			            (unsigned long long)wivrn::nxwarp_stats_last.identity_tiles_total,
			            100.0 * double(wivrn::nxwarp_stats_last.identity_tiles) /
			                    double(wivrn::nxwarp_stats_last.identity_tiles_total),
			            wivrn::nxwarp_stats_last.snap_identity,
			            wivrn::nxwarp_stats_last.identity_from_decoder
			                    ? " (counted by the headset)"
			                    : " (counted by the encoder)");
	}

	std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
	            failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
