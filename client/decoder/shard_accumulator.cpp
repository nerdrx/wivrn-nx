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
#include "fec.h"
#include "scenes/stream.h" // IWYU pragma: keep
#include "spdlog/spdlog.h"
#include "xr/instance.h"

#include <limits>

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

shard_set::shard_set(uint8_t stream_index)
{
	feedback.stream_index = stream_index;
}

void shard_set::reset(uint64_t frame_index)
{
	min_for_reconstruction = -1;
	data.clear();
	parity.clear();

	uint8_t stream_index = feedback.stream_index;
	feedback = {};
	feedback.frame_index = frame_index;
	feedback.stream_index = stream_index;
}

bool shard_set::empty() const
{
	return data.empty();
}

static bool is_complete(const shard_set & shards)
{
	const auto & frame = shards.data;
	if (frame.empty())
		return false;
	if (not(frame.back() and frame.back()->timing_info))
		return false;
	for (const auto & shard: frame)
		if (not shard)
			return false;
	return true;
}

std::optional<uint16_t> shard_set::insert(data_shard && shard, xr::instance & instance)
{
	if (empty())
		feedback.received_first_packet = instance.now();

	auto idx = shard.shard_idx;
	if (idx >= data.size())
		data.resize(idx + 1);
	if (data[idx])
		return {};
	data[idx] = std::move(shard);
	return idx;
}

std::optional<uint16_t> shard_set::reconstruct(const parity_shard & p, xr::instance & instance)
{
	const size_t n = p.blob_size.size();
	if (n == 0)
		return {};

	const size_t last = size_t(p.first_shard_idx) + n;

	// A group needs all but one of its shards to be rebuildable, so a parity shard
	// may only ever tell us about one index past what we already hold. That is also
	// what keeps a corrupt first_shard_idx from growing `data` without bound.
	if (data.size() + 1 < last)
		return {};
	if (data.size() < last)
		data.resize(last);

	auto shard = wivrn::fec::reconstruct(p, [this](uint16_t idx) -> const data_shard * {
		return (idx < data.size() and data[idx]) ? &*data[idx] : nullptr;
	});
	if (not shard)
		return {};

	// Through the normal path: a real copy arriving late afterwards is then simply
	// a duplicate, which insert() already drops.
	if (feedback.reconstructed_shards < std::numeric_limits<uint8_t>::max())
		++feedback.reconstructed_shards;
	return insert(std::move(*shard), instance);
}

bool shard_set::group_complete(const parity_shard & p) const
{
	const size_t last = size_t(p.first_shard_idx) + p.blob_size.size();
	if (data.size() < last)
		return false;
	for (size_t i = p.first_shard_idx; i < last; ++i)
		if (not data[i])
			return false;
	return true;
}

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

void shard_accumulator::advance()
{
	std::swap(current, next);
	next.reset(current.frame_index() + 1);
}

void shard_accumulator::push_shard(video_stream_data_shard && shard)
{
	assert(current.frame_index() + 1 == next.frame_index());

	// Not truncated to 8 bits: a stream that is silent for a while, as the quad
	// layer stream is whenever no layer is promoted, comes back with a gap of any
	// size, and a gap that happened to be a multiple of 256 would look like no gap
	// at all and file the new frame's shards under the old frame index.
	uint64_t frame_diff = shard.frame_idx - current.frame_index();
	if (shard.frame_idx < current.frame_index())
	{
		// frame is in the past, drop it
		spdlog::info("Drop shard for old frame {} (current {})", shard.frame_idx, current.frame_index());
	}
	else if (frame_diff == 0)
	{
		auto shard_idx = current.insert(std::move(shard), instance);
		// The shard that just landed may have been the last one a group was short
		// of bar one, which is the point at which its parity becomes usable.
		if (auto rebuilt = drain_parity(current))
			shard_idx = std::min(shard_idx.value_or(*rebuilt), *rebuilt);
		try_submit_frame(shard_idx);
	}
	else if (frame_diff == 1)
	{
		next.insert(std::move(shard), instance);
		drain_parity(next);
		if (is_complete(next))
		{
			debug_why_not_sent(current);
			send_feedback(current.feedback);

			advance();

			try_submit_frame(0);
		}
	}
	else if (frame_diff == 2)
	{
		debug_why_not_sent(current);
		send_feedback(current.feedback);

		advance();

		push_shard(std::move(shard));
	}
	else
	{
		// We have lost more than one frame
		send_feedback(current.feedback);
		send_feedback(next.feedback);

		current.reset(shard.frame_idx);
		next.reset(shard.frame_idx + 1);

		push_shard(std::move(shard));
	}
}

void shard_accumulator::push_parity(video_stream_parity_shard && parity)
{
	assert(current.frame_index() + 1 == next.frame_index());

	// A parity shard is never evidence that a frame has started or ended: it only
	// ever fills a hole in a frame the data shards have already put us on. One that
	// names any other frame is for a frame we have given up on, or one we have not
	// reached yet and whose data shards will move us along in their own time.
	shard_set * set = nullptr;
	if (parity.frame_idx == current.frame_index())
		set = &current;
	else if (parity.frame_idx == next.frame_index())
		set = &next;
	else
		return;

	// Nothing to hold on to for a group that is already whole, which on a link that
	// is not losing anything is every group.
	if (set->group_complete(parity) or set->parity.size() >= max_parity_per_frame)
		return;

	set->parity.push_back(std::move(parity));

	if (set == &current)
	{
		if (auto rebuilt = drain_parity(current))
			try_submit_frame(*rebuilt);
		return;
	}

	drain_parity(next);
	if (is_complete(next))
	{
		debug_why_not_sent(current);
		send_feedback(current.feedback);

		advance();

		try_submit_frame(0);
	}
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

		if (auto idx = set.reconstruct(p, instance))
		{
			++fec_reconstructed;
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
	             current.feedback.stream_index,
	             fec_reconstructed,
	             (now - fec_last_report) / 1e9);

	fec_reconstructed = 0;
	fec_last_report = now;
}

void shard_accumulator::try_submit_frame(std::optional<uint16_t> shard_idx)
{
	if (shard_idx)
		try_submit_frame(*shard_idx);
}

void shard_accumulator::try_submit_frame(uint16_t shard_idx)
{
	auto & data_shards = current.data;

	for (size_t idx = 0; idx < shard_idx; ++idx)
		if (not data_shards[idx])
			return;

	uint16_t last_idx = shard_idx + 1;
	for (size_t size = data_shards.size();
	     last_idx < size and data_shards[last_idx];
	     ++last_idx)
	{
	}

	std::vector<std::span<const uint8_t>> payload;
	payload.reserve(last_idx - shard_idx);
	for (size_t idx = shard_idx; idx < last_idx; ++idx)
		payload.emplace_back(data_shards[idx]->payload);

	bool frame_complete = last_idx == data_shards.size() and data_shards.back()->timing_info;
	decoder_->push_data(payload, data_shards[shard_idx]->frame_idx, not frame_complete);

	if (not frame_complete)
		return;

	current.feedback.received_last_packet = instance.now();
	current.feedback.sent_to_decoder = current.feedback.received_last_packet;
	data_shard::timing_info_t timing_info = data_shards.back()->timing_info.value_or(data_shard::timing_info_t{});
	current.feedback.encode_begin = timing_info.encode_begin;
	current.feedback.encode_end = timing_info.encode_end;
	current.feedback.send_begin = timing_info.send_begin;
	current.feedback.send_end = timing_info.send_end;

	if (not data_shards.front()->view_info)
	{
		spdlog::warn("first shard has no view_info");
		return;
	}

	// Try to extract a frame
	decoder_->frame_completed(current.feedback, *data_shards.front()->view_info);

	send_feedback(current.feedback);

	advance();
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
