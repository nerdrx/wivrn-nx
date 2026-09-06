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
//                    [--backend ref|vk] [--qp N] [--inter on|off]
//                    [--intra-period N] [--coded-vectors default|none|static]
//                    [--reconnect-at N] [--start-frame-id F]
//                    [--pace auto|off|FPS] [--client-decode-ms N] [--present-hz N]
//                    [--feedback-delay N]
//
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
//   --idr-at N         at frame N the encoder is asked for a keyframe
//                      (compositor::request_idr -> video_encoder::reset), with the same
//                      client still on the other end. The stream must not hiccup: an
//                      encoder that restarted its transport state here would break a
//                      session that was working.

#include "driver/bitrate_controller.h"
#include "encoder/encoder_settings.h"
#include "encoder/video_encoder_nxwarp.h"
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
#include <mutex>
#include <random>
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

// A pose that is different every frame, so that a decoder republishing a stale one, or
// publishing a default, cannot pass by accident.
view_info_t make_view_info(uint64_t frame)
{
	const float t = float(frame) * 0.017f;
	view_info_t vi{};
	vi.display_time = XrTime(1'000'000'000ll + int64_t(frame) * 11'111'111ll);
	vi.alpha = false;
	for (int eye = 0; eye < 2; ++eye)
	{
		const float s = t + float(eye) * 0.5f;
		vi.pose[eye].orientation = {std::sin(s) * 0.1f, std::cos(s) * 0.1f, 0.0f, std::sqrt(1.0f - 0.02f)};
		vi.pose[eye].position = {0.032f * (eye ? 1.f : -1.f), 1.6f + 0.01f * std::sin(s), 0.05f * std::cos(s)};
		vi.fov[eye] = {-0.9f - 0.001f * t, 0.9f, 0.9f, -0.9f};
		// The foveation runs: source pixels per output pixel, middle entry 1:1. Made
		// to vary with the frame so a stale or default one is visible.
		vi.foveation[eye].x = {uint16_t(1 + frame % 3), 4, 5, 3, 1};
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

	XrTime now() override
	{
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
		cv.notify_all();
	}

	bool wait_for(size_t n, std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m);
		return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
	}
};

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
	// Datagrams waiting for their slot to come up: {slot at which it is released, packet}.
	std::vector<std::pair<uint64_t, to_headset::nxwarp_datagram>> held;
	uint64_t slot_index = 0;

public:
	uint64_t sent = 0, dropped = 0, delayed = 0;
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

	lossy_link(nxwarp_decoder & dec, double loss, double reorder, uint32_t seed) :
	        dec(&dec), rng(seed), loss(loss), reorder(reorder) {}

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
		const uint64_t slot = slot_index++;
		release_due(slot);
		if (loss > 0 and std::uniform_real_distribution<double>(0, 1)(rng) < loss)
		{
			++dropped;
			return;
		}
		if (reorder > 0 and std::uniform_real_distribution<double>(0, 1)(rng) < reorder)
		{
			++delayed;
			const uint64_t d = 1 + (uint64_t(rng()) % 3);
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
		// Real serializer, both directions. A field that does not survive this does not
		// reach the decoder, which is the whole point of routing it through here.
		auto wire = to_wire(packet);
		auto back = from_wire<to_headset::nxwarp_datagram>(std::move(wire));
		dec->push_datagram(std::move(back));
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

source_image make_source_image(vk_bundle & vk, uint32_t w, uint32_t h)
{
	source_image s;
	s.w = w;
	s.h = h;

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
	                .arrayLayers = 1,
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

	const vk::DeviceSize bytes = vk::DeviceSize(w) * h * 3 / 2;
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

// Copy one yuv420p frame in, converting the two chroma planes to the interleaved layout
// the two-plane image wants, and leave the image in eGeneral — which is the layout
// video_encoder_nxwarp's own copyImageToBuffer reads it from.
void upload(vk_bundle & vk, source_image & s, std::span<const uint8_t> yuv)
{
	const size_t y_size = size_t(s.w) * s.h;
	const size_t c_size = y_size / 4;
	auto * dst = (uint8_t *)s.staging_map;
	std::memcpy(dst, yuv.data(), y_size);
	const uint8_t * cb = yuv.data() + y_size;
	const uint8_t * cr = cb + c_size;
	uint8_t * uv = dst + y_size;
	for (size_t i = 0; i < c_size; ++i)
	{
		uv[2 * i] = cb[i];
		uv[2 * i + 1] = cr[i];
	}

	s.cmd.reset();
	s.cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	vk::ImageMemoryBarrier to_dst{
	        .srcAccessMask = {},
	        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
	        .oldLayout = vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eTransferDstOptimal,
	        .image = *s.image,
	        .subresourceRange = {vk::ImageAspectFlagBits::ePlane0 | vk::ImageAspectFlagBits::ePlane1, 0, 1, 0, 1},
	};
	s.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
	                      {}, {}, {}, to_dst);

	std::array<vk::BufferImageCopy, 2> regions{
	        vk::BufferImageCopy{
	                .bufferOffset = 0,
	                .imageSubresource = {vk::ImageAspectFlagBits::ePlane0, 0, 0, 1},
	                .imageExtent = {s.w, s.h, 1},
	        },
	        vk::BufferImageCopy{
	                .bufferOffset = y_size,
	                .imageSubresource = {vk::ImageAspectFlagBits::ePlane1, 0, 0, 1},
	                .imageExtent = {s.w / 2, s.h / 2, 1},
	        },
	};
	s.cmd.copyBufferToImage(*s.staging, *s.image, vk::ImageLayout::eTransferDstOptimal, regions);

	vk::ImageMemoryBarrier to_general{
	        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = *s.image,
	        .subresourceRange = {vk::ImageAspectFlagBits::ePlane0 | vk::ImageAspectFlagBits::ePlane1, 0, 1, 0, 1},
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
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eTransfer,
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
	// "default" (STATIC when inter is on), "none", or "static".
	std::string coded_vectors = "default";
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
	// Hold each not-held report back this many presented frames before handing it to
	// the encoder. Zero is the harness's own behaviour -- the report is delivered in
	// the same loop iteration that produced it, which no network does. On a live link
	// it crosses the control socket, so the encoder has coded several more frames by
	// the time it lands, and a report that names a frame the encoder has already
	// answered with an all-intra one is the common case rather than the rare one.
	// That is the case last_resync_id exists for, and this is how it gets exercised.
	uint32_t feedback_delay = 0;
	uint32_t width = 320, height = 240, frames = 12, seed = 1;
	double loss = 0.0, reorder = 0.0;
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
		else if (a == "--first-frame")
			first_frame = std::stoull(next());
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
		else if (a == "--coded-vectors")
			coded_vectors = next();
		else if (a == "--client-tools")
			client_tools = next();
		else if (a == "--entropy")
			entropy = next();
		else if (a == "--qp")
			qp = uint32_t(std::stoul(next()));
		else if (a == "--reconnect-at")
			reconnect_at = uint32_t(std::stoul(next()));
		else if (a == "--start-frame-id") // the other name for --first-frame
			first_frame = std::stoull(next());
		else if (a == "--no-resume-notice")
			no_resume_notice = true;
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

	auto yuv = read_file(yuv_path);
	const size_t frame_bytes = size_t(width) * height * 3 / 2;
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
	settings.options["qp"] = std::to_string(qp);
	settings.options["backend"] = backend;
	settings.options["inter"] = inter;
	settings.options["intra-period"] = std::to_string(intra_period);
	settings.options["coded-vectors"] = coded_vectors;
	settings.options["entropy"] = entropy;
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
	std::fprintf(stderr,
	             "[e2e] simulated headset nxvc_tools = 0x%llx (entropy request \"%s\")\n",
	             (unsigned long long)settings.nxvc_tools,
	             entropy.c_str());
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
	host.width = width;
	host.height = height;
	auto make_decoder = [&] {
		auto d = std::make_unique<nxwarp_decoder>(vk.device, vk.physical_device,
		                                          vk.queue.family_index, desc, 0, host, nullptr);
		if (client_decode_ms > 0)
			d->set_simulated_decode_ms(client_decode_ms);
		return d;
	};
	auto dec = make_decoder();

	lossy_link link(*dec, loss, reorder, seed);
	enc.set_packet_sink(&link);

	auto src = make_source_image(vk, width, height);

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

	if (first_frame)
		std::printf("frame ids start at %llu (16-bit wrap at %llu, %s crossed by this run)\n",
		            (unsigned long long)first_frame,
		            (unsigned long long)((first_frame / 65536 + 1) * 65536),
		            first_frame % 65536 + frames > 65536 ? "is" : "is NOT");

	const auto run_start = std::chrono::steady_clock::now();
	for (uint32_t i = 0; i < frames; ++i)
	{
		const uint64_t f = first_frame + i;

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
		source_frames.emplace_back(frame.begin(), frame.end());
		upload(vk, src, frame);

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

		// The other return half: the per-frame delivery reports into WiVRn's own
		// automatic bitrate, whose answer goes back to the encoder as a new ceiling.
		pump_aimd(i);
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
	const bool clean = loss <= 0;

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
	else
	{
		check(arrived or not host.frames.empty(),
		      "lossy run keeps publishing rather than stalling");
		check(host.frames.size() < sent_frames,
		      "lossy run drops the frames with holes rather than inventing them");
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
		if (defaulted)
			std::printf("note: %zu published frame%s had no view_info -- its first datagram was "
			            "lost and its tiles were recovered by FEC\n",
			            defaulted, defaulted == 1 ? "" : "s");
		check(not clean or defaulted == 0,
		      "a link that lost nothing publishes no frame with a default pose");

		// And that it is not merely a default that happens to compare equal.
		bool any_nonzero = false;
		for (const auto & p: host.frames)
			any_nonzero |= p.view_info.display_time != 0;
		check(any_nonzero, "published view_info is a real pose, not a default-constructed one");
	}

	// --- feedback reached the encoder shadow ------------------------------------
	// The encoder folds feedback into nxt::Sender's client shadow and derives its
	// per-tile receipt map from it. A run that produced feedback at all, and an encoder
	// that accepted every packet without throwing, is what this level can observe from
	// outside; the shadow's own state is checked by nx-warp's transport tests.
	check(link.sent > 0, "encoder produced datagrams");
	std::printf("feedback: %llu packets, %llu bytes returned to the encoder\n",
	            (unsigned long long)feedback_packets, (unsigned long long)feedback_bytes);
	check(feedback_packets > 0,
	      "the decoder's band deadlines produced feedback (" +
	              std::to_string(feedback_packets) + " packets)");
	check(feedback_bytes > 0, "the feedback carried a payload");
	// on_nxwarp_feedback hands the bytes to nxt::Sender, which rejects a packet it
	// cannot parse. Every packet was accepted, so the encoder's shadow took all of them.
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
			const size_t frame_size = size_t(width) * height * 3 / 2;
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
	{
		const auto p = enc.profile();
		if (p.frames)
			std::printf("\nencode (%s backend, %ux%u, QP %u): mean %.2f ms, "
			            "worst %.2f ms over %llu frames, %llu bytes/frame\n",
			            backend.c_str(), width, height, qp,
			            p.total_ms / double(p.frames), p.max_ms,
			            (unsigned long long)p.frames,
			            (unsigned long long)(p.bytes / p.frames));
	}

	std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
	            failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
