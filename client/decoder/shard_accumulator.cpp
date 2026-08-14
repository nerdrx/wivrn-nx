/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2023  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2023  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "shard_accumulator.h"
#include "application.h"
#include "configuration.h"
#include "fec.h"
#include "scenes/stream.h" // IWYU pragma: keep
#include "spdlog/spdlog.h"
#include "xr/instance.h"

#include <algorithm>
#include <span>
#include <vector>

namespace wivrn
{

using namespace wivrn::to_headset;
using shard_set = shard_accumulator::shard_set;
using data_shard = shard_accumulator::data_shard;
using parity_shard = shard_accumulator::parity_shard;

namespace
{
// One line at most every this many nanoseconds, whatever the loss rate
constexpr int64_t fec_report_period = 10'000'000'000;
} // namespace

static void debug_why_not_sent(const shard_set & shards)
{
	const auto & frame = shards.data;
	if (frame.empty())
	{
		spdlog::info("frame {} was not sent because no shard was received", shards.frame_index());
		return;
	}
	int frame_idx = -1;
	size_t data = 0;
	size_t missing = 0;
	for (const auto & shard: frame)
	{
		if (shard)
		{
			frame_idx = shard->frame_idx;
			++data;
		}
		else
			++missing;
	}

	bool end = frame.back() and frame.back()->timing_info;
	spdlog::info("frame {} was not sent with {} data shards, {}{} missing", frame_idx, data, end ? "" : "at least ", missing);
}

void shard_accumulator::push_shard(video_stream_data_shard && shard)
{
	// Not truncated to 8 bits: a stream that is silent for a while, as the quad
	// layer stream is whenever no layer is promoted, comes back with a gap of any
	// size, and a gap that happened to be a multiple of 256 would look like no gap
	// at all and file the new frame's shards under the old frame index.
	const uint64_t frame_idx = shard.frame_idx;

	shard_set * set = window.slot(frame_idx, [this](shard_set & s) {
		debug_why_not_sent(s);
		send_feedback(s.feedback);
	});

	if (not set)
	{
		// Older than anything still being reassembled. With two paths in play this
		// is also what a shard that lost a race by more than the window looks like.
		spdlog::info("Drop shard for old frame {} (oldest {})", frame_idx, window.front_index());
		return;
	}

	const XrTime now = instance.now();
	set->insert(std::move(shard), now);
	// The shard that just landed may have been the last one a group was short of
	// bar one, which is the point at which its parity becomes usable.
	drain_parity(*set);

	if (set->complete())
		window.note_complete(frame_idx);

	// After the parity drain, never before it: what the parity is about to rebuild
	// must not be asked for again.
	try_nack(now);

	// Only the oldest frame is ever handed to the decoder, and the pump is what
	// does it; a shard for a newer one can only ever change whether the oldest is
	// still worth waiting for.
	pump();
}

void shard_accumulator::push_parity(video_stream_parity_shard && parity)
{
	// A parity shard is never evidence that a frame has started or ended: it only
	// ever fills a hole in a frame the data shards have already put us on. One that
	// names a frame outside the window is for a frame we have given up on, or one we
	// have not reached yet and whose data shards will move us along in their own time.
	const uint64_t frame_idx = parity.frame_idx;
	if (frame_idx < window.front_index() or frame_idx >= window.front_index() + window_t::depth)
		return;

	shard_set * set = window.slot(frame_idx, [](shard_set &) {});
	if (not set or set->frame_index() != frame_idx)
		return;

	// Nothing to hold on to for a group that is already whole, which on a link that
	// is not losing anything is every group.
	if (set->group_complete(parity) or set->parity.size() >= max_parity_per_frame)
		return;

	set->parity.push_back(std::move(parity));

	drain_parity(*set);

	if (set->complete())
		window.note_complete(frame_idx);

	try_nack(instance.now());

	pump();
}

void shard_accumulator::pump()
{
	window.drain(
	        [this](shard_set & set) { return try_submit_front(set); },
	        [this](shard_set & set) {
		        debug_why_not_sent(set);
		        send_feedback(set.feedback);
	        });
}

std::optional<uint16_t> shard_accumulator::drain_parity(shard_set & set)
{
	if (set.parity.empty())
		return {};

	std::optional<uint16_t> rebuilt;

	// Groups are disjoint, so rebuilding in one of them can never unblock another:
	// a single pass is enough.
	size_t kept = 0;
	for (size_t i = 0; i < set.parity.size(); ++i)
	{
		parity_shard & p = set.parity[i];

		// Spent: either the group filled up on its own or we have just filled it
		if (set.group_complete(p))
			continue;

		if (auto idx = set.reconstruct(p, instance.now()))
		{
			++fec_reconstructed;
			++fec_reconstructed_total;
			rebuilt = std::min(rebuilt.value_or(*idx), *idx);
			continue;
		}

		if (kept != i)
			set.parity[kept] = std::move(p);
		++kept;
	}
	set.parity.resize(kept);

	report_reconstructions();
	return rebuilt;
}

void shard_accumulator::report_reconstructions()
{
	if (fec_reconstructed == 0)
		return;

	const int64_t now = instance.now();
	if (fec_last_report == 0)
	{
		// First one: open the window rather than log a report covering no time
		fec_last_report = now;
		return;
	}
	if (now - fec_last_report < fec_report_period)
		return;

	spdlog::info("Stream {}: rebuilt {} lost video shard(s) from parity over the last {:.0f} s",
	             window.front().feedback.stream_index,
	             fec_reconstructed,
	             (now - fec_last_report) / 1e9);

	fec_reconstructed = 0;
	fec_last_report = now;
}

void shard_accumulator::try_nack(XrTime now)
{
	// Read live rather than plumbed through: the headset sends its whole settings
	// block on every change and this is one bool on a path that already reads the
	// frame's worth of state around it.
	if (not application::get_config().shard_retransmit)
		return;

	// The newest frame anything has arrived for. Everything older than it has stopped
	// arriving in its own right — a hole in it is loss, not a shard still in the air —
	// and, more to the point, the shard past the highest one it holds is known to
	// exist, which is the one case where the frame length can be guessed at.
	uint64_t newest = 0;
	bool any = false;
	window.for_each([&](shard_set & set) {
		if (set.empty())
			return;
		if (not any or set.frame_index() > newest)
		{
			newest = set.frame_index();
			any = true;
		}
	});
	if (not any)
		return;

	window.for_each([&](shard_set & set) {
		// The clock first, because it is the cheap test and it is the one that
		// answers "no" for almost every frame: this runs on every shard arrival, and
		// the frame the shards belong to has by definition just been heard from.
		//
		// It is also the rate limit. One round in flight at a time: a request sent
		// less than the delay ago has not had the chance to be answered yet, and a
		// second copy of it would only spend bandwidth on the path that is already
		// dropping packets.
		const XrTime since = std::max(set.last_shard, set.nack_last);
		if (since == 0 or now - since < nack_delay_ns)
			return;
		if (set.nack_rounds >= max_nack_rounds or set.empty() or set.complete())
			return;

		set.missing_shards(nack_scratch, set.frame_index() < newest);
		if (nack_scratch.empty())
			return;

		auto scene = weak_scene.lock();
		if (not scene)
			return;

		const uint16_t first = nack_scratch.front();
		wivrn::from_headset::nack request{
		        .stream_index = set.feedback.stream_index,
		        .frame_idx = set.frame_index(),
		        .first_shard_idx = first,
		        .bitmap = {},
		};

		size_t asked = 0;
		for (uint16_t idx: nack_scratch)
		{
			const size_t bit = size_t(idx) - first;
			const size_t byte = bit / 8;
			// Past the window one request can name. A frame missing more than
			// 256 shards from its first hole is not one a retransmission round
			// is going to save.
			if (byte >= wivrn::from_headset::max_nack_bitmap_bytes)
				break;
			if (request.bitmap.size() <= byte)
				request.bitmap.resize(byte + 1, 0);
			request.bitmap[byte] |= uint8_t(1u << (bit % 8));
			++asked;
		}

		++set.nack_rounds;
		set.nack_last = now;
		++nack_requests;
		nack_shards += asked;
		nack_shards_total += asked;

		scene->send_nack(request);
	});

	report_nacks(now);
}

void shard_accumulator::report_nacks(XrTime now)
{
	if (nack_requests == 0)
		return;

	if (nack_last_report == 0)
	{
		// First one: open the window rather than log a report covering no time
		nack_last_report = now;
		return;
	}
	if (now - nack_last_report < nack_report_period)
		return;

	spdlog::info("Stream {}: asked for {} lost video shard(s) again over {} request(s) in the last {:.0f} s",
	             window.front().feedback.stream_index,
	             nack_shards,
	             nack_requests,
	             (now - nack_last_report) / 1e9);

	nack_requests = 0;
	nack_shards = 0;
	nack_last_report = now;
}

shard_accumulator::window_t::step shard_accumulator::try_submit_front(shard_set & current)
{
	using step = window_t::step;
	auto & data_shards = current.data;

	// Everything before `submitted` is already in the decoder's input buffer, and
	// the decoder appends what it is given: the run to hand over starts there and
	// stops at the first hole.
	uint16_t first = current.submitted;
	uint16_t last = first;
	while (last < data_shards.size() and data_shards[last])
		++last;

	const bool frame_complete = last == data_shards.size() and
	                            not data_shards.empty() and
	                            data_shards.back()->timing_info;

	if (last > first)
	{
		if (frame_complete and not data_shards.front()->view_info)
		{
			// Nothing can be done with this frame: the decoder needs the view
			// info that rides the first shard to submit the layer at all.
			spdlog::warn("first shard has no view_info");
			return step::unusable;
		}

		std::vector<std::span<const uint8_t>> payload;
		payload.reserve(last - first);
		for (size_t idx = first; idx < last; ++idx)
			payload.emplace_back(data_shards[idx]->payload);

		decoder_->push_data(payload, data_shards[first]->frame_idx, not frame_complete);
		current.submitted = last;
	}

	if (not frame_complete)
		return step::wait;

	current.feedback.received_last_packet = instance.now();
	current.feedback.sent_to_decoder = current.feedback.received_last_packet;
	data_shard::timing_info_t timing_info = data_shards.back()->timing_info.value_or(data_shard::timing_info_t{});
	current.feedback.encode_begin = timing_info.encode_begin;
	current.feedback.encode_end = timing_info.encode_end;
	current.feedback.send_begin = timing_info.send_begin;
	current.feedback.send_end = timing_info.send_end;

	if (not data_shards.front()->view_info)
		return step::unusable;

	// Try to extract a frame
	decoder_->frame_completed(current.feedback, *data_shards.front()->view_info);

	send_feedback(current.feedback);

	// The window advances on `done`; advancing here as well would skip a frame.
	return step::done;
}

void shard_accumulator::send_feedback(wivrn::from_headset::feedback & feedback)
{
	if (not feedback.received_last_packet)
		feedback.received_first_packet = instance.now();
	auto scene = weak_scene.lock();
	if (scene)
		scene->send_feedback(feedback);
}
} // namespace wivrn
