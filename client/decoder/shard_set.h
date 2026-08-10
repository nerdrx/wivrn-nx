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

#include "fec.h"
#include "wivrn_packets.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace wivrn
{

// The shards of one video frame of one stream, as they arrive.
//
// Deliberately free of everything the rest of the decoder needs — no Vulkan, no OpenXR
// session, no scene — so that the reassembly rules can be driven directly by
// tests/accumulator_test.cpp. The only thing it wants from the outside is the current time,
// which it stamps the feedback with, and the caller passes it in.
struct shard_set
{
	using data_shard = wivrn::to_headset::video_stream_data_shard;
	using parity_shard = wivrn::to_headset::video_stream_parity_shard;

	size_t min_for_reconstruction = -1;
	std::vector<std::optional<data_shard>> data;
	// Parity shards of this frame whose group still has a hole in it. A parity
	// shard for a group that is already whole is dropped on arrival, so on a
	// clean link this stays empty and costs nothing.
	std::vector<parity_shard> parity;

	// Shards already handed to the decoder. The decoder is fed the frame in
	// contiguous runs as they complete, and it appends what it is given, so this
	// is what keeps a second look at the same frame from feeding it twice — which
	// a window several frames deep, revisiting a frame after a newer one moved,
	// makes reachable in a way the old two-deep one never did.
	uint16_t submitted = 0;

	wivrn::from_headset::feedback feedback{};

	explicit shard_set(uint8_t stream_index = 0)
	{
		feedback.stream_index = stream_index;
	}

	uint64_t frame_index() const
	{
		return feedback.frame_index;
	}

	void reset(uint64_t frame_index)
	{
		min_for_reconstruction = -1;
		data.clear();
		parity.clear();
		submitted = 0;

		uint8_t stream_index = feedback.stream_index;
		feedback = {};
		feedback.frame_index = frame_index;
		feedback.stream_index = stream_index;
	}

	bool empty() const
	{
		return data.empty();
	}

	// Every shard of the frame is here, and the last one says it is the last one.
	bool complete() const
	{
		if (data.empty())
			return false;
		if (not(data.back() and data.back()->timing_info))
			return false;
		for (const auto & shard: data)
			if (not shard)
				return false;
		return true;
	}

	// A duplicate — the same shard over the other path, or a copy arriving after
	// it was rebuilt from parity — returns nothing and changes nothing.
	std::optional<uint16_t> insert(data_shard && shard, XrTime now)
	{
		if (empty())
			feedback.received_first_packet = now;

		auto idx = shard.shard_idx;
		if (idx >= data.size())
			data.resize(idx + 1);
		if (data[idx])
			return {};
		data[idx] = std::move(shard);
		return idx;
	}

	// Rebuild the single missing data shard of `p`'s group, if that is what the
	// group is short of. Returns its index when one was rebuilt and inserted.
	std::optional<uint16_t> reconstruct(const parity_shard & p, XrTime now)
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
		return insert(std::move(*shard), now);
	}

	// Whether every data shard `p` covers has arrived, which makes `p` useless
	bool group_complete(const parity_shard & p) const
	{
		const size_t last = size_t(p.first_shard_idx) + p.blob_size.size();
		if (data.size() < last)
			return false;
		for (size_t i = p.first_shard_idx; i < last; ++i)
			if (not data[i])
				return false;
		return true;
	}
};

} // namespace wivrn
