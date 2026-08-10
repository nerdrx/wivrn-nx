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

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace wivrn
{

// The frames of one video stream currently being reassembled, oldest first.
//
// Until multipath striping there were two of them: whatever was arriving, and the one
// after it. That is the right depth when the shards of a frame all take the same route
// and arrive roughly in order — a shard of frame N+1 then really is proof that frame N is
// over. It is the wrong depth the moment the frame is split across two links with
// different latencies: the USB path can hand over the whole tail of frame N+1 while a
// Wi-Fi shard of frame N is still in the air, and a two-deep window reads that as "frame N
// is finished, it just lost some shards".
//
// So the window is `depth` frames deep and a frame is not given up on merely because a
// newer one showed up. It is given up on when one of two things is true:
//
//   * a *complete* frame more than `skew` indices newer exists. That is the inter-path
//     skew tolerance: up to `skew` frame periods of difference between the two paths costs
//     nothing, past it the older frame is abandoned exactly as it always was. The bound is
//     on complete frames on purpose — a newer frame that is itself still missing shards is
//     no evidence that anything is late.
//   * it falls off the far end of the window, i.e. a shard arrives for a frame `depth`
//     newer. That is the hard bound, and the reason the memory is bounded: at most `depth`
//     frames' shards are held at once.
//
// Only the oldest frame is ever handed to the decoder, and always in index order: a video
// decoder cannot be fed out of order, so a newer frame that completed first waits its turn.
//
// Pure index arithmetic, no payload of its own — the caller's `set_t` holds the shards.
// `set_t` must have `reset(uint64_t frame_index)` and `frame_index()`.
// tests/accumulator_test.cpp drives it directly.
template <typename set_t, size_t depth_ = 6, uint64_t skew_ = 3>
class frame_window
{
public:
	static constexpr size_t depth = depth_;
	static constexpr uint64_t skew = skew_;

	static_assert(depth > skew, "the window has to be deeper than the skew it tolerates, "
	                            "or a frame would fall off the end before the skew rule ever fires");

	// Every slot starts as a copy of `prototype` — which is how the stream index, the
	// only thing a set carries that is not per frame, reaches all of them.
	explicit frame_window(const set_t & prototype = set_t{})
	{
		for (size_t i = 0; i < depth; ++i)
		{
			sets[i] = prototype;
			sets[i].reset(i);
		}
	}

	uint64_t front_index() const
	{
		return front_;
	}

	set_t & front()
	{
		return sets[front_ % depth];
	}

	const set_t & front() const
	{
		return sets[front_ % depth];
	}

	// The set holding `index`, or nullptr when it is older than the window.
	//
	// Frames that have to leave to make room are handed to `retire`, oldest first, so
	// that the caller can report them as never delivered before their shards are freed.
	template <typename retire_t>
	set_t * slot(uint64_t index, retire_t && retire)
	{
		if (index < front_)
			return nullptr;

		if (index >= front_ + depth)
		{
			// Retire what the new frame pushes out, but never more than the window
			// holds: a stream that has been silent for a while (the quad layer one
			// whenever no layer is promoted) comes back with a gap of any size, and
			// walking it one frame at a time would be unbounded work for no result.
			const uint64_t gap = index - front_ - depth + 1;
			for (uint64_t i = 0; i < gap and i < depth; ++i)
			{
				retire(front());
				advance();
			}

			if (index >= front_ + depth)
				restart(index);
		}

		return &sets[index % depth];
	}

	// A frame is whole. Only its index matters here; the newest such index is what the
	// skew rule measures the oldest frame against.
	void note_complete(uint64_t index)
	{
		if (not has_complete or index > newest_complete)
		{
			newest_complete = index;
			has_complete = true;
		}
	}

	// Whether the oldest frame has waited long enough to be given up on. Says nothing
	// about whether it is complete — a complete front is submitted, not dropped.
	bool front_stale() const
	{
		return has_complete and newest_complete > front_ + skew;
	}

	// What one look at the oldest frame came to.
	enum class step
	{
		// Still waiting on shards for it. Whether it is given up on now is the
		// window's decision, not the caller's.
		wait,
		// It went to the decoder and is finished with.
		done,
		// It is whole but cannot be used at all. Nothing else can happen to it, so
		// it must not be left blocking the window.
		unusable,
	};

	// Drain the window from its oldest end, for as long as anything can be decided
	// there. `visit` is offered the oldest frame; `retire` is handed every frame that
	// is given up on, so that the caller can report it as never delivered before its
	// shards are freed.
	//
	// This is the whole advance/discard policy, in one place: a frame leaves the
	// window when it has been decoded, when it is provably undecodable, or when a
	// complete frame more than `skew` newer exists — and never merely because a newer
	// frame index turned up, which over two paths of different latency is the normal
	// course of events.
	template <typename visit_t, typename retire_t>
	void drain(visit_t && visit, retire_t && retire)
	{
		// Every iteration either advances the window or returns, and the window is
		// `depth` deep, so this cannot run away.
		for (size_t i = 0; i <= depth; ++i)
		{
			switch (visit(front()))
			{
				case step::done:
					advance();
					continue;

				case step::unusable:
					retire(front());
					advance();
					continue;

				case step::wait:
					if (not front_stale())
						return;
					retire(front());
					advance();
					continue;
			}
		}
	}

	// Done with the oldest frame, whichever way it went. Its slot becomes the newest
	// one in the window, which is what frees the shards it was holding.
	void advance()
	{
		sets[front_ % depth].reset(front_ + depth);
		++front_;
	}

private:
	// Start over at `index`, e.g. after a gap wider than the window
	void restart(uint64_t index)
	{
		for (size_t i = 0; i < depth; ++i)
			sets[(index + i) % depth].reset(index + i);
		front_ = index;
		has_complete = false;
		newest_complete = 0;
	}

	std::array<set_t, depth> sets;
	uint64_t front_ = 0;
	uint64_t newest_complete = 0;
	bool has_complete = false;
};

} // namespace wivrn
