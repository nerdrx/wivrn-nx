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
			d.encoder->SendData(d.span, true, d.prefer_control, make_pacer(q, d, queued));

		std::unique_lock lock(mutex);
		q.in_flight = nullptr;
		cv.notify_all();
	}

	std::unique_lock lock(mutex);
	q.pending.clear();
	cv.notify_all();
}

shard_pacer video_encoder::sender::make_pacer(queue & q, const data & d, size_t queued)
{
	video_encoder * encoder = d.encoder;

	// The control queue carries the IDRs and the parameter sets: never delayed.
	if (d.prefer_control or not encoder->pacing_enabled)
		return {};

	// Nothing to pace unless the shards actually ride the UDP stream socket.
	// Over the secondary (TCP) path the kernel's congestion control already
	// decides when bytes go out and spacing them on top only adds latency; with
	// no stream socket at all everything is TCP, same story.
	if (not encoder->cnx or not encoder->cnx->has_stream() or encoder->cnx->video_on_secondary())
	{
		q.pacing.reset();
		return {};
	}

	const int64_t now = os_monotonic_get_ns();
	const int64_t budget = q.pacing.begin_frame(now, encoder->frame_period_ns, encoder->pacing_window, queued);

	return shard_pacer(now, budget, d.span.size());
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
	const double share = fec_enabled ? fec::data_share : 1.0;
	pending_bitrate = uint32_t(requested * bitrate_multiplier * share);
}

void video_encoder::set_fec(bool enabled)
{
	if (fec_enabled.exchange(enabled) == enabled)
		return;
	apply_bitrate();
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

void video_encoder::present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo info, uint64_t frame_index)
{
	wivrn::trace::scope trace_present(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "present_image");
	// Wait for encoder to be done
	present_slot = (present_slot + 1) % num_slots;
	state[present_slot].wait(busy);
	if (idr->should_skip(frame_index))
	{
		state[present_slot] = skip;
		return;
	}
	state[present_slot] = busy;
	return present_image(y_cbcr, info, present_slot, frame_index);
}

void video_encoder::encode(wivrn_session & cnx,
                           const to_headset::video_stream_data_shard::view_info_t & view_info,
                           uint64_t frame_index)
{
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

	auto data = encode(encode_slot, frame_index);
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
	auto parity = fec_group.take();
	if (not parity)
		return;

	try
	{
		cnx->send_stream(std::move(*parity));
	}
	catch (...)
	{
		// Ignore network errors, same as for the data shards
	}
}

void video_encoder::SendData(std::span<uint8_t> data, bool end_of_frame, bool control, shard_pacer pacer)
{
	std::lock_guard lock(mutex);
	if (shard.shard_idx == 0)
	{
		// One SendData call per NAL; span the whole frame, not each call.
		wivrn::trace::cpu_begin(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
		cnx->dump_time("send_begin", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
		timing_info.send_begin = clock.to_headset(os_monotonic_get_ns());
		fec_group.reset(stream_idx, shard.frame_idx);
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

	ssize_t max_payload_size = (cnx->has_stream() and not control) ? ssize_t(fec::shard_payload_budget(fec_active)) : std::numeric_limits<uint32_t>::max();

	auto begin = data.begin();
	auto end = data.end();
	while (begin != end)
	{
		const size_t payload_size = std::max(0z, max_payload_size - ssize_t(serialized_size(shard.view_info)));
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

		// The parity shard of a group goes out immediately after the group's last
		// data shard, so it travels in (or right at the edge of) the same pacing
		// micro-burst and reaches the headset while the group is still open there.
		// Deliberately not held back to the end of the frame: that would put every
		// parity shard of the frame in one tail burst, and a hiccup that swallowed
		// the tail would take the whole frame's protection with it.
		if (fec_active)
		{
			fec_group.add(shard);
			if (fec_group.full())
				send_parity();
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

	// Last group of the frame, usually a partial one. Emitted even for a group of
	// a single shard: a one-shard frame is cheap to duplicate in absolute bytes and
	// losing it costs exactly as much as losing a big one — a frame plus the IDR
	// round trip it triggers.
	if (fec_active and end_of_frame)
		send_parity();

	if (end_of_frame)
	{
		cnx->dump_time("send_end", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
		wivrn::trace::cpu_end(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
	}
}

} // namespace wivrn
