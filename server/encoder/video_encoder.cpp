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

// Include first because of incompatibility between Eigen and X includes
#include "driver/wivrn_session.h"

#include "video_encoder.h"

#include "encoder_settings.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "utils/wivrn_trace.h"
#include "wivrn_config.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <ctime>
#include <string>

#if WIVRN_USE_NVENC
#include "video_encoder_nvenc.h"
#endif
#if WIVRN_USE_VAAPI
#include "ffmpeg/video_encoder_va.h"
#endif
#if WIVRN_USE_X264
#include "video_encoder_x264.h"
#endif
#if WIVRN_USE_VULKAN_ENCODE
#include "video_encoder_vulkan_h264.h"
#include "video_encoder_vulkan_h265.h"
#endif
#include "video_encoder_raw.h"
#if WIVRN_USE_NXWARP
#include "video_encoder_nxwarp.h"
#endif

namespace
{
// Absolute deadline sleep on the clock os_monotonic_get_ns reads (CLOCK_MONOTONIC).
// Absolute rather than relative so that the schedule cannot drift: every wakeup
// is measured against the frame's own start time, not against the previous one.
void sleep_until_ns(int64_t deadline_ns)
{
	timespec ts{
	        .tv_sec = time_t(deadline_ns / 1'000'000'000),
	        .tv_nsec = long(deadline_ns % 1'000'000'000),
	};

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR)
		;
}
} // namespace

namespace wivrn
{

video_encoder::sender::sender()
{
	for (queue & q: queues)
		q.thread = std::jthread([this, &q](std::stop_token t) { run(std::move(t), q); });
}

void video_encoder::sender::run(std::stop_token t, queue & q)
{
	while (not t.stop_requested())
	{
		data d{};
		size_t queued = 0;
		{
			std::unique_lock lock(mutex);
			if (q.pending.empty())
			{
				cv.wait_for(lock, std::chrono::milliseconds(100));
				continue;
			}
			// The frame is kept out of the queue while it is sent, so that it
			// cannot be dropped from under us, wait_idle uses in_flight
			d = std::move(q.pending.front());
			q.pending.pop_front();
			q.in_flight = d.encoder;
			// Frames of the other streams that were encoded at the same time
			// and belong to the same slot: they share the pacing window
			queued = q.pending.size();
		}

		if (not d.span.empty())
		{
			auto s = make_schedule(q, d, queued);
			d.encoder->SendData(d.span, true, d.prefer_control, s.pacer, s.spill);
		}

		std::unique_lock lock(mutex);
		q.in_flight = nullptr;
		cv.notify_all();
	}

	std::unique_lock lock(mutex);
	q.pending.clear();
	cv.notify_all();
}

video_encoder::sender::schedule video_encoder::sender::make_schedule(queue & q, const data & d, size_t queued)
{
	video_encoder * encoder = d.encoder;

	// The control queue carries the IDRs and the parameter sets: never delayed,
	// and never split — that socket is a TCP one already.
	if (d.prefer_control or not encoder->cnx)
		return {};

	// Neither pacing nor striping applies unless the shards actually ride the UDP
	// stream socket. Over the secondary (TCP) path the kernel's congestion control
	// already decides when bytes go out and spacing them on top only adds latency;
	// with no stream socket at all everything is TCP, same story, and there is no
	// second path to spill onto because the one path is already it.
	if (not encoder->cnx->has_stream() or encoder->cnx->video_on_secondary())
	{
		q.pacing.reset();
		return {};
	}

	// A combine posture with no Wi-Fi share to size the split against would put
	// every frame whole on the tunnel, which is not what combining means and not
	// what anything measured. Behave exactly as a single path until there is one.
	const uint32_t wifi_share = encoder->cnx->wifi_share_bps();
	const bool combining = encoder->cnx->video_combining() and wifi_share != 0;
	const bool paced = encoder->pacing_enabled;
	if (not paced and not combining)
		return {};

	const int64_t now = os_monotonic_get_ns();
	const int64_t period = encoder->frame_period_ns;
	const int64_t budget = paced ? q.pacing.begin_frame(now, period, encoder->pacing_window, queued) : 0;

	schedule s;

	// Bytes of this frame the primary path is going to see. Everything past the
	// split point rides the secondary one, so the pacing schedule must spread the
	// prefix — and only the prefix — over the window: pacing the whole frame there
	// would hand the primary its share at 1/(spilled fraction) times the rate the
	// window was sized for, which is the burst pacing exists to prevent.
	size_t on_primary = d.span.size();

	if (combining)
	{
		// Wall time this frame has to reach the headset: the slice of the pacing
		// window it was given, or — with the shards unpaced, or a slot whose
		// window is already spent — its share of a frame period, which is then
		// the only deadline it has.
		const int64_t deliver = budget > 0 ? budget : period / int64_t(queued + 1);
		s.spill = spill_scheduler(wifi_share, deliver);
		on_primary = std::min(on_primary, s.spill.split_at());
	}

	if (paced)
		s.pacer = shard_pacer(now, budget, on_primary);

	return s;
}

void video_encoder::sender::push(data && d)
{
	queue & q = queues[d.prefer_control ? 1 : 0];

	size_t dropped = 0;
	{
		std::unique_lock lock(mutex);
		// Drop whole frames, oldest first: a partial frame is useless to the
		// decoder
		while (q.pending.size() >= max_queued_frames)
		{
			q.pending.pop_front();
			++dropped;
		}
		q.pending.push_back(std::move(d));
	}
	cv.notify_all();

	if (dropped)
		U_LOG_W("Video sender queue full, dropped %zu frame(s)", dropped);
}

void video_encoder::sender::wait_idle(video_encoder * encoder)
{
	auto busy = [this, encoder]() {
		return std::ranges::any_of(queues, [encoder](const queue & q) {
			return q.in_flight == encoder or
			       std::ranges::any_of(q.pending, [encoder](const data & d) { return d.encoder == encoder; });
		});
	};

	std::unique_lock lock(mutex);
	while (busy())
		cv.wait_for(lock, std::chrono::milliseconds(100));
}

std::shared_ptr<video_encoder::sender> video_encoder::sender::get()
{
	static std::weak_ptr<video_encoder::sender> instance;
	static std::mutex m;
	std::unique_lock lock(m);
	auto s = instance.lock();
	if (s)
		return s;
	s.reset(new video_encoder::sender());
	instance = s;
	return s;
}

std::unique_ptr<video_encoder> video_encoder::create(
        wivrn::vk_bundle & wivrn_vk,
        const encoder_settings & settings,
        uint8_t stream_idx)
{
	using namespace std::string_literals;
	std::unique_ptr<video_encoder> res;
	if (settings.encoder_name == encoder_vulkan)
	{
#if WIVRN_USE_VULKAN_ENCODE
		switch (settings.codec)
		{
			case video_codec::h264:
				res = video_encoder_vulkan_h264::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::h265:
				res = video_encoder_vulkan_h265::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::av1:
				throw std::runtime_error("av1 not supported for vulkan video encode");
			case video_codec::raw:
				throw std::runtime_error("raw codec only supported on raw encoder");
			case video_codec::nxwarp:
				throw std::runtime_error("nxwarp codec only supported on the nxwarp encoder");
		}
#else
		throw std::runtime_error("Vulkan video encode not enabled");
#endif
	}
	if (settings.encoder_name == encoder_x264)
	{
#if WIVRN_USE_X264
		res = std::make_unique<video_encoder_x264>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("x264 encoder not enabled");
#endif
	}
	if (settings.encoder_name == encoder_nvenc)
	{
#if WIVRN_USE_NVENC
		res = std::make_unique<video_encoder_nvenc>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("nvenc support not enabled");
#endif
	}
	if (settings.encoder_name == encoder_vaapi)
	{
#if WIVRN_USE_VAAPI
		res = std::make_unique<video_encoder_va>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("vaapi support not enabled");
#endif
	}

	if (settings.encoder_name == encoder_raw)
	{
		res = std::make_unique<video_encoder_raw>(wivrn_vk, settings, stream_idx);
	}

	if (settings.encoder_name == encoder_nxwarp)
	{
#if WIVRN_USE_NXWARP
		res = std::make_unique<video_encoder_nxwarp>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("NX Warp encoder not enabled");
#endif
	}

	if (not res)
		throw std::runtime_error("Failed to create encoder " + settings.encoder_name);

	auto wivrn_dump_video = std::getenv("WIVRN_DUMP_VIDEO");
	if (wivrn_dump_video)
	{
		std::string file(wivrn_dump_video);
		file += "-" + std::to_string(stream_idx);
		switch (settings.codec)
		{
			case h264:
				file += ".h264";
				break;
			case h265:
				file += ".h265";
				break;
			case av1:
				file += ".av1";
				break;
			case raw:
				file += ".yuv";
				break;
			case nxwarp:
				file += ".nxv";
				break;
		}
		res->video_dump.open(file);
	}
	return res;
}

video_encoder::video_encoder(vk_bundle & vk,
                             uint8_t stream_idx,
                             uint32_t target_queue,
                             const encoder_settings & settings,
                             std::unique_ptr<idr_handler> idr,
                             bool async_send) :
        stream_idx(stream_idx),
        src_layer(settings.src_layer),
        target_queue(target_queue),
        need_transfer(not vk.optimal_transfer(vk.queue.family_index, target_queue)),
        bitrate_multiplier(settings.bitrate_multiplier),
        shared_sender(async_send ? sender::get() : nullptr),
        idr(std::move(idr)),
        extent{
                .width = settings.width,
                .height = settings.height,
        }
{
	assert(this->idr);

	// Only a hardware encoder has somewhere to fall to. The software one is the
	// floor, and the raw "encoder" is a debugging tool whose whole point is that it
	// does not compress.
	watchdog.set_eligible(settings.encoder_name != encoder_x264 and settings.encoder_name != encoder_raw);

	// So that pacing has a frame period from the very first frame, before the
	// headset has had a chance to change the refresh rate. Deliberately not
	// set_framerate: that would also queue a rate control reconfiguration for
	// the framerate the encoder was just created with.
	if (settings.fps > 0)
		frame_period_ns = int64_t(1'000'000'000.f / settings.fps);
}

video_encoder::~video_encoder()
{
	if (shared_sender)
		shared_sender->wait_idle(this);
}

void video_encoder::on_feedback(const from_headset::feedback & feedback)
{
	assert(feedback.stream_index == stream_idx);
	idr->on_feedback(feedback);
	account_latency(feedback);

	if (not fec_enabled or not fec_adaptive)
		return;

	// The headset reports a frame several times over as it works its way through the
	// decoder and the compositor, and every one of those copies carries the same loss
	// counts. Only the first is evidence. A frame index far *behind* the cursor is a
	// stream that started over, which must not wedge the measurement for good.
	if (fec_rate_frame and feedback.frame_index + 64 < fec_rate_frame)
		fec_rate_frame = 0;
	if (fec_rate_frame and feedback.frame_index <= fec_rate_frame)
		return;

	auto cost = history.frame_cost(feedback.frame_index);
	if (not cost or cost->shards_sent == 0)
		return;
	fec_rate_frame = feedback.frame_index;

	// The two are disjoint by construction: the headset only ever asks for shards its
	// parity cannot rebuild, so a shard is counted here as reconstructed or as asked
	// for, never as both.
	const uint32_t lost = uint32_t(feedback.reconstructed_shards) + cost->shards_nacked;

	uint16_t k;
	{
		std::lock_guard lock(fec_rate_mutex);
		fec_rate.on_frame(cost->shards_sent, lost, feedback.sent_to_decoder != 0);
		k = fec_rate.group_size();
	}

	// The parity overhead is part of the link budget, so a ratio change is a bitrate
	// change: see apply_bitrate.
	if (fec_group_size.exchange(k) != k)
	{
		U_LOG_D("Stream %d: parity ratio now %u+1", (int)stream_idx, (unsigned)k);
		apply_bitrate();
	}
}

void video_encoder::reset()
{
	idr->reset();
}

void video_encoder::set_bitrate(uint32_t bitrate_bps)
{
	requested_bitrate = bitrate_bps;
	apply_bitrate();
}

void video_encoder::apply_bitrate()
{
	const uint32_t requested = requested_bitrate;
	if (requested == 0)
		return;

	// The bitrate the controller decides is a budget for the whole link, and the
	// parity shards are on that link too: with FEC on, an encoder left at the full
	// number would put 12.5% more than the budget on the wire and the controller
	// would then spend its time chasing the loss it caused itself. So the encoder
	// gets the data share and the parity gets the rest.
	//
	// The share is not a constant once the ratio is adaptive — 6% of overhead at 16+1,
	// 25% at 4+1 — so every move of the ratio comes back through here.
	const double share = fec_enabled ? fec::data_share(fec_group_size) : 1.0;
	pending_bitrate = uint32_t(requested * bitrate_multiplier * share);
}

void video_encoder::set_fec(bool enabled)
{
	if (fec_enabled.exchange(enabled) == enabled)
		return;
	apply_bitrate();
}

void video_encoder::set_fec_adaptive(bool enabled)
{
	if (fec_adaptive.exchange(enabled) == enabled)
		return;

	if (not enabled)
	{
		// Back to the fixed ratio, and back to the bitrate that goes with it
		{
			std::lock_guard lock(fec_rate_mutex);
			fec_rate.reset();
		}
		if (fec_group_size.exchange(fec::group_size) != fec::group_size)
			apply_bitrate();
	}
}

void video_encoder::set_shard_retransmit(bool enabled)
{
	history.set_enabled(enabled);
}

void video_encoder::collect_retransmits(const from_headset::nack & n,
                                        std::vector<to_headset::video_stream_data_shard> & out)
{
	if (not history.enabled())
		return;

	// Whatever the headset says is missing is loss the parity did not absorb, and it
	// counts towards the ratio whether or not the shards are still here to send.
	uint32_t asked = 0;
	for (uint8_t byte: n.bitmap)
		asked += uint32_t(std::popcount(byte));
	history.note_nacked(n.frame_idx, asked);

	size_t budget = max_retransmit_per_nack;
	{
		std::lock_guard lock(retransmit_mutex);
		const int64_t now = os_monotonic_get_ns();
		if (now - retransmit_window_ns >= 1'000'000'000)
		{
			retransmit_window_ns = now;
			retransmit_in_window = 0;
		}
		if (retransmit_in_window >= max_retransmit_per_second)
			return;
		budget = std::min(budget, size_t(max_retransmit_per_second - retransmit_in_window));
	}

	thread_local std::vector<shard_history::hit> hits;
	hits.clear();
	const size_t found = history.collect(n.frame_idx, n.first_shard_idx, n.bitmap, budget, hits);
	if (found == 0)
		return;

	for (shard_history::hit & h: hits)
	{
		try
		{
			// Back through the ordinary send path, which is what gives the
			// datagram a fresh IV off the global counter. Re-sending stored
			// ciphertext would reuse one, and an AES-CTR keystream reused is
			// no keystream at all.
			out.push_back(fec::decode_blob(stream_idx, n.frame_idx, h.shard_idx, h.blob));
		}
		catch (...)
		{
			// A blob that no longer decodes is one the ring overwrote under us;
			// there is nothing to send and nothing to report.
		}
	}

	retransmitted += out.size();
	{
		std::lock_guard lock(retransmit_mutex);
		retransmit_in_window += uint32_t(out.size());
	}

	report_retransmissions();
}

void video_encoder::report_retransmissions()
{
	std::lock_guard lock(retransmit_mutex);

	const uint64_t total = retransmitted;
	if (total == retransmit_reported)
		return;

	const int64_t now = os_monotonic_get_ns();
	if (retransmit_last_report == 0)
	{
		// First one: open the window rather than log a report covering no time
		retransmit_last_report = now;
		retransmit_reported = total;
		return;
	}
	if (now - retransmit_last_report < retransmit_report_period)
		return;

	U_LOG_I("Stream %d: sent %lu video shard(s) again on request over the last %.0f s",
	        (int)stream_idx,
	        (unsigned long)(total - retransmit_reported),
	        (now - retransmit_last_report) / 1e9);

	retransmit_reported = total;
	retransmit_last_report = now;
}

void video_encoder::enable_intra_refresh(uint32_t sweep_frames)
{
	intra_refresh_supported = true;
	intra_refresh_sweep = sweep_frames;
	idr->set_intra_refresh(intra_refresh_enabled, sweep_frames);
}

void video_encoder::set_intra_refresh(bool enabled)
{
	intra_refresh_enabled = enabled;
	// No refresh mechanism on this encoder: the handler stays on keyframes whatever the
	// switch says, and would have nothing to drive if it did not.
	if (not intra_refresh_supported)
		return;
	idr->set_intra_refresh(enabled, intra_refresh_sweep);
}

void video_encoder::enable_ref_invalidation(uint32_t dpb_frames)
{
	ref_invalidation_supported = true;
	ref_invalidation_dpb = dpb_frames;
	idr->set_ref_invalidation(ref_invalidation_enabled, dpb_frames);
}

void video_encoder::set_ref_invalidation(bool enabled)
{
	ref_invalidation_enabled = enabled;
	// No invalidation call on this encoder: the handler stays on the rungs above it whatever
	// the switch says, and would have nothing to drive if it did not.
	if (not ref_invalidation_supported)
		return;
	idr->set_ref_invalidation(enabled, ref_invalidation_dpb);
}

void video_encoder::set_framerate(float framerate)
{
	pending_framerate = framerate;
	if (framerate > 0)
		frame_period_ns = int64_t(1'000'000'000.f / framerate);
}

void video_encoder::set_pacing(bool enabled, float window)
{
	pacing_window = std::clamp(window, 0.f, shard_pacer::max_window);
	pacing_enabled = enabled;
}

void video_encoder::present_image(vk::Image y_cbcr,
                                  vk::SemaphoreSubmitInfo info,
                                  uint64_t frame_index,
                                  const to_headset::video_stream_data_shard::view_info_t & view_info)
{
	wivrn::trace::scope trace_present(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "present_image");
	// Wait for encoder to be done
	present_slot = (present_slot + 1) % num_slots;
	state[present_slot].wait(busy);
	if (idr->should_skip(frame_index))
	{
		state[present_slot] = skip;
		++pending_presents;
		return;
	}
	state[present_slot] = busy;
	++pending_presents;
	return present_image(y_cbcr, info, present_slot, frame_index, view_info);
}

void video_encoder::encode(wivrn_session & cnx,
                           const to_headset::video_stream_data_shard::view_info_t & view_info,
                           uint64_t frame_index)
{
	// Nothing was ever presented into this encoder: it took over from a failed one
	// after that frame's present had already gone into the encoder it replaced.
	// Encoding here would emit whatever the freshly allocated buffers hold, and
	// would leave the two slot cursors one apart for good.
	if (pending_presents.load() <= 0)
		return;
	--pending_presents;

	encode_slot = (encode_slot + 1) % num_slots;

	struct idle_setter
	{
		std::atomic_unsigned_lock_free & state;
		~idle_setter()
		{
			state = idle;
			state.notify_all();
		}
	};
	idle_setter i{state[encode_slot]};

	if (state[encode_slot] == skip)
		return;

	if (shared_sender)
		shared_sender->wait_idle(this);
	this->cnx = &cnx;
	clock = cnx.get_offset();

	wivrn::trace::scope trace_encode(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "encode");
	auto encode_begin = os_monotonic_get_ns();
	timing_info = {
	        .encode_begin = clock.to_headset(encode_begin),
	};

	// Prepare the video shard template
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_index;
	shard.shard_idx = 0;
	shard.view_info = view_info;
	shard.timing_info.reset();

	// The watchdog only ever sees frames that really reach the backend: a frame the
	// IDR handler skipped, or one for a stream that is silent by design, took the
	// early returns above and says nothing about the encoder's health.
	watchdog.encode_begin(encode_begin);
	std::optional<data> data;
	try
	{
		data = encode(encode_slot, frame_index);
	}
	catch (const std::exception & e)
	{
		watchdog.encode_error(os_monotonic_get_ns(), e.what());
		throw;
	}
	catch (...)
	{
		watchdog.encode_error(os_monotonic_get_ns(), "unknown error");
		throw;
	}
	// An encoder that sends synchronously (x264) hands its NALs to SendData from
	// inside the encode call and returns nothing by design, so for those "no data"
	// is what success looks like.
	watchdog.encode_end(os_monotonic_get_ns(), data.has_value() or not shared_sender);

	cnx.dump_time("encode_begin", frame_index, encode_begin, stream_idx);
	cnx.dump_time("encode_end", frame_index, os_monotonic_get_ns(), stream_idx);
	if (data)
	{
		timing_info.encode_end = clock.to_headset(os_monotonic_get_ns());
		assert(shared_sender);
		shared_sender->push(std::move(*data));
	}
}

void video_encoder::send_parity()
{
	// One parity per group of the block that has just closed — one of them with the
	// contiguous scheme, fec::interleave_depth of them with the interleaved one. The
	// loop runs to the end whatever it finds: it is the last, empty-handed take() that
	// opens the next block.
	//
	// A group every shard of which spilled to the secondary (TCP) path cannot lose
	// anything, so its parity would repair nothing — and it would be spent on the
	// primary path, taking Wi-Fi bandwidth away from the shards that *can* be lost.
	// group_builder skips those. Groups straddling the split keep theirs: the shards
	// that went over UDP are exactly the ones at risk, and the ones that went over TCP
	// are as good as received for the purpose of rebuilding them.
	while (auto parity = fec_group.take())
	{
		// Parity is on the link like everything else, and the bitrate the controller
		// decides is a budget for the link (see apply_bitrate), so it counts. It
		// always rides the primary path, which is the only one that can drop a packet.
		frame_bytes += uint32_t(parity->payload.size());
		frame_bytes_primary += uint32_t(parity->payload.size());

		try
		{
			cnx->send_stream(std::move(*parity));
		}
		catch (...)
		{
			// Ignore network errors, same as for the data shards
		}
	}
}

void video_encoder::SendData(std::span<uint8_t> data, bool end_of_frame, bool control, shard_pacer pacer, spill_scheduler spill)
{
	std::lock_guard lock(mutex);
	if (shard.shard_idx == 0)
	{
		// One SendData call per NAL; span the whole frame, not each call.
		wivrn::trace::cpu_begin(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
		cnx->dump_time("send_begin", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
		timing_info.send_begin = clock.to_headset(os_monotonic_get_ns());
		// The layout the whole frame is protected with, fixed here: the shard payload
		// budget below is derived from the group size (see fec::payload_reserve) and a
		// frame has to be sharded to one size throughout. Depth 1 is the contiguous
		// scheme; interleaving is what the adaptive switch adds on top of the ratio.
		fec_group.set_layout(fec_adaptive ? fec_group_size.load() : fec::group_size,
		                     fec_adaptive ? fec::interleave_depth : 1);
		fec_group.reset(stream_idx, shard.frame_idx);
		frame_bytes = 0;
		frame_bytes_primary = 0;
		frame_bytes_secondary = 0;
		frame_offset = 0;
	}
	if (end_of_frame)
	{
		timing_info.send_end = clock.to_headset(os_monotonic_get_ns());
		if (not timing_info.encode_end)
			timing_info.encode_end = timing_info.send_end;
	}
	if (video_dump)
		video_dump.write((char *)data.data(), data.size());

	// Parity is only worth anything on the lossy path. The control socket and the
	// secondary (USB) path are both TCP: nothing is dropped there, so a parity shard
	// would be pure overhead, and an IDR that goes out on the control socket is
	// exactly the frame that must not be made larger.
	const bool fec_active = fec_enabled and not control and cnx->has_stream() and not cnx->video_on_secondary();

	ssize_t max_payload_size = (cnx->has_stream() and not control) ? ssize_t(fec::shard_payload_budget(fec_active, fec_group.group_size())) : std::numeric_limits<uint32_t>::max();

	auto begin = data.begin();
	auto end = data.end();
	while (begin != end)
	{
		size_t payload_size = std::max(0z, max_payload_size - ssize_t(serialized_size(shard.view_info)));
		if (payload_size == 0)
		{
			// The first shard carries view_info; if it ever grew to fill the whole
			// payload budget, next would equal begin, begin would never advance and
			// this loop would spin forever. Not reachable with today's budgets, but
			// force at least one byte of progress rather than hang if it ever becomes so.
			static bool warned = false;
			if (not warned)
			{
				warned = true;
				U_LOG_W("video_encoder: view_info fills the shard payload budget, forcing progress");
			}
			payload_size = 1;
		}
		auto next = std::min(end, begin + payload_size);
		if (next == end)
		{
			if (end_of_frame)
			{
				// Take the timestamp here rather than before the loop: pacing
				// spreads the frame over several milliseconds and the headset
				// would otherwise be told the frame left all at once.
				timing_info.send_end = clock.to_headset(os_monotonic_get_ns());
				shard.timing_info = timing_info;
			}
		}
		shard.payload = {begin, next};
		const uint32_t payload_bytes = uint32_t(next - begin);
		frame_bytes += payload_bytes;

		// Striping (multipath stage 3): everything past the split point goes over the
		// secondary path instead, where it travels in parallel with what is still
		// going out over Wi-Fi. The split is by byte offset, so it is a prefix/suffix
		// one and the order within each path is the order within the frame.
		bool on_primary = true;
		if (spill.spill(frame_offset))
		{
			if (cnx->send_spill(to_headset::video_stream_data_shard{shard}))
				on_primary = false;
			else
				// The path has just been dropped, and a TCP socket whose send threw
				// is poisoned for good: this shard and the rest of the frame go back
				// on the primary, and the selector collapses out of the combine
				// posture on its next update.
				spill.fail();
		}

		if (on_primary)
		{
			frame_bytes_primary += payload_bytes;
			try
			{
				if (control)
					cnx->send_control(to_headset::video_stream_data_shard{shard});
				else
					cnx->send_stream(to_headset::video_stream_data_shard{shard});
			}
			catch (...)
			{
				// Ignore network errors
			}
		}
		else
		{
			frame_bytes_secondary += payload_bytes;
		}

		frame_offset += payload_bytes;

		// The parity shard of a group goes out immediately after the group's last
		// data shard, so it travels in (or right at the edge of) the same pacing
		// micro-burst and reaches the headset while the group is still open there.
		// Deliberately not held back to the end of the frame: that would put every
		// parity shard of the frame in one tail burst, and a hiccup that swallowed
		// the tail would take the whole frame's protection with it.
		//
		// The groups are built before the split and are not affected by it: a shard
		// covers the same group whichever path carried it, which is exactly what
		// lets the headset rebuild a lost UDP shard from copies that arrived over
		// the tunnel.
		if (fec_active)
		{
			fec_group.add(shard, on_primary);
			if (fec_group.block_full())
				send_parity();
		}

		// What the headset may ask to have sent again. Deliberately outside the FEC
		// gate — a shard is worth remembering whether or not a parity covers it — and
		// deliberately only the shards that went over the path that can lose one: a
		// hole where a TCP shard should be is the two paths arriving out of order, and
		// answering that over Wi-Fi would spend the lossy path's bandwidth on a shard
		// already in flight over the other. The blob is the same encoding the parity
		// scheme uses, so a retransmission is a decode_blob and nothing more.
		if (history.enabled() and not control)
		{
			fec::encode_blob(shard, history_blob);
			history.push(shard.frame_idx, shard.shard_idx, history_blob, on_primary);
		}

		++shard.shard_idx;
		shard.view_info.reset();
		begin = next;

		// Leaky bucket: hold the next micro-burst back until the frame's
		// schedule says it may go out. Never past the frame's budget, and the
		// budget is a fraction of a frame period, so this can never push the
		// end of the frame into the next one.
		//
		// Parity bytes are not counted into the schedule: the frame's own bytes
		// still take exactly the window they were given, and the parity rides
		// alongside. The bitrate accounting already took the overhead out of the
		// encoder (see apply_bitrate), so the link sees the same rate either way.
		if (auto at = pacer.wait_until(size_t(begin - data.begin()), os_monotonic_get_ns()))
			sleep_until_ns(*at);
	}

	// Last block of the frame, usually a partial one. Emitted even for a group of
	// a single shard: a one-shard frame is cheap to duplicate in absolute bytes and
	// losing it costs exactly as much as losing a big one — a frame plus the IDR
	// round trip it triggers.
	if (fec_active and end_of_frame)
		send_parity();

	if (end_of_frame)
	{
		// How long the frame was, which is what the loss the headset reports is a
		// fraction of. Kept whether or not retransmission is on: the adaptive parity
		// ratio needs it either way.
		history.end_frame(shard.frame_idx, shard.shard_idx);

		cnx->dump_time("send_end", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
		wivrn::trace::cpu_end(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");

		// The bandwidth estimating bitrate control law divides these bytes by the time
		// the headset says it spent receiving the frame. Shard headers are not counted:
		// a few tens of bytes on a 1.4 kB datagram, consistently, and the control law
		// only ever uses the estimate through a gain.
		//
		// Deliberately the whole frame, both paths together: while combining, the span
		// the headset reports runs from the first arrival on either path to the last on
		// either, so the rate it yields is the rate of the two links as one.
		cnx->on_frame_sent(shard.frame_idx, stream_idx, frame_bytes);
		cnx->on_frame_paths(frame_bytes_primary, frame_bytes_secondary);

		if (spill.active())
			U_LOG_D("Stream %d frame %lu: %u bytes on the primary path, %u spilled to the secondary (split at %zu%s)",
			        (int)stream_idx,
			        (unsigned long)shard.frame_idx,
			        (unsigned)frame_bytes_primary,
			        (unsigned)frame_bytes_secondary,
			        spill.split_at(),
			        spill.failed() ? ", path lost mid-frame" : "");
	}
}

void video_encoder::SendControlPacket(to_headset::nxwarp_datagram && packet)
{
	std::lock_guard lock(mutex);
	if (nxwarp_sink)
	{
		nxwarp_sink->send_control(std::move(packet));
		return;
	}
	try
	{
		cnx->send_control(std::move(packet));
	}
	catch (...)
	{
		// Ignore network errors; the header is resent periodically.
	}
}

void video_encoder::SendPacket(to_headset::nxwarp_datagram && packet, bool end_of_frame)
{
	std::lock_guard lock(mutex);

	if (nxwarp_sink)
	{
		frame_bytes += uint32_t(packet.payload.size());
		nxwarp_sink->send_stream(std::move(packet));
		return;
	}

	if (frame_offset == 0 and frame_bytes == 0)
	{
		wivrn::trace::cpu_begin(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
		cnx->dump_time("send_begin", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
		timing_info.send_begin = clock.to_headset(os_monotonic_get_ns());
	}

	const uint32_t bytes = uint32_t(packet.payload.size());
	frame_bytes += bytes;
	frame_bytes_primary += bytes;
	frame_offset += bytes;

	if (video_dump)
		video_dump.write((char *)packet.payload.data(), packet.payload.size());

	try
	{
		if (cnx->has_stream())
			cnx->send_stream(std::move(packet));
		else
			cnx->send_control(std::move(packet));
	}
	catch (...)
	{
		// Ignore network errors, same as the shard path: a lost datagram is the
		// case the codec's concealment and the transport's FEC exist for.
	}

	if (end_of_frame)
	{
		timing_info.send_end = clock.to_headset(os_monotonic_get_ns());
		if (not timing_info.encode_end)
			timing_info.encode_end = timing_info.send_end;

		cnx->dump_time("send_end", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
		wivrn::trace::cpu_end(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");

		// Same contract as SendData: what the bandwidth estimator divides by the
		// time the headset says it spent receiving the frame. Every NX Warp
		// datagram rides the primary path today — the transport's own striper is
		// not wired to WiVRn's secondary path yet — so the split is trivial.
		cnx->on_frame_sent(shard.frame_idx, stream_idx, frame_bytes);
		cnx->on_frame_paths(frame_bytes_primary, 0);

		frame_bytes = 0;
		frame_bytes_primary = 0;
		frame_bytes_secondary = 0;
		frame_offset = 0;
	}
}

} // namespace wivrn
