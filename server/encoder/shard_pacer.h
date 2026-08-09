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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wivrn
{

// Leaky-bucket schedule for one frame's worth of video shards.
//
// Draining a frame into the socket as fast as the kernel accepts it puts a few
// hundred kilobytes on the wire in one go every frame period (208 kB at
// 150 Mbit/s and 90 fps). That burst is what overflows an access point's buffer
// and produces the lag-then-recover wedge the automatic bitrate's deep drop
// exists to clean up after. Spreading the same bytes over a fraction of the
// frame period costs nothing in latency as long as the fraction is small, and
// keeps the queue at the access point shallow.
//
// The schedule is a straight line: the byte at offset x may not be handed to
// the kernel before start + budget * x / total. Sleeping happens per group of
// shards rather than per shard, see group_bytes.
//
// Pure arithmetic, no clock and no syscalls: the caller supplies the current
// time and does the sleeping. tests/pacing_test.cpp drives it directly.
class shard_pacer
{
public:
	// Bytes handed to the kernel back to back between two sleeps. A shard is
	// about 1.4 kB, so this is 8-9 datagrams: small enough that the burst fits
	// inside a single Wi-Fi TXOP (~100 us of airtime on a decent 5 GHz link)
	// and so cannot on its own overflow anything, large enough to keep the
	// wakeup rate sane. A 208 kB frame is 18 groups, i.e. ~1.6k wakeups per
	// second per stream, against ~13k if every shard were paced individually.
	// Grouping also leaves frame aggregation alone: the driver still sees
	// several datagrams at once and can put them in one A-MPDU.
	static constexpr size_t group_bytes = 12 * 1024;

	// Do not enter the kernel for a sleep shorter than this. It is below what a
	// timer wakeup costs and below the slack a non-realtime thread gets anyway,
	// so the sleep would not be honoured. A frame big enough for its per-group
	// interval to fall under it degenerates into an unpaced blast, which is the
	// correct behaviour: there is no budget left to spread it over.
	static constexpr int64_t min_sleep_ns = 150'000;

	// Hard ceiling on the configured window fraction.
	//
	// The automatic bitrate controller derives link utilisation from
	// (received_last - received_first) / frame_period, so pacing over a
	// fraction w puts a floor of w under every utilisation sample. Its
	// "there is spare capacity, probe upwards" threshold is 0.60: a window at
	// or above that would park the controller in its hysteresis band forever
	// and the bitrate would never climb again. Keep a wide margin.
	static constexpr float max_window = 0.5f;

	// An inactive pacer: never sleeps.
	shard_pacer() = default;

	shard_pacer(int64_t start_ns, int64_t budget_ns, size_t total_bytes) :
	        start(start_ns),
	        budget(std::max<int64_t>(budget_ns, 0)),
	        total(total_bytes)
	{}

	// False when there is nothing to spread: no budget left (a late frame, or
	// a frame slot whose window is already spent), or a frame that fits in a
	// single group and is harmless as it is.
	bool active() const
	{
		return budget > 0 and total > group_bytes;
	}

	int64_t window_ns() const
	{
		return active() ? budget : 0;
	}

	// Absolute time at which the byte at `offset` may go out. Monotone
	// non-decreasing in offset, equal to the start time at offset 0, and never
	// later than start + budget.
	//
	// budget is at most a frame period (1e8 ns at 10 fps) and offset at most
	// the frame size, so the product stays far below the int64 range.
	int64_t deadline(size_t offset) const
	{
		if (not active())
			return start;
		if (offset >= total)
			return start + budget;
		return start + int64_t(budget * uint64_t(offset) / total);
	}

	// Number of micro-bursts the frame is split into. There is one sleep before
	// each of them but the first.
	size_t group_count() const
	{
		if (not active())
			return 1;
		return (total + group_bytes - 1) / group_bytes;
	}

	// Deadline of the first byte of group `index`.
	int64_t group_deadline(size_t index) const
	{
		return deadline(std::min(index * group_bytes, total));
	}

	// Called by the sender after every shard, with the running count of bytes
	// already handed to the kernel and the current time. Returns the absolute
	// time to sleep until before the next shard goes out, or nothing at all to
	// keep sending back to back.
	std::optional<int64_t> wait_until(size_t sent_bytes, int64_t now)
	{
		if (not active() or sent_bytes < next_group or sent_bytes >= total)
			return {};

		// Skip whole groups rather than sleeping once per group that was
		// crossed: a single shard never spans a group, but a caller handing
		// over larger chunks must not be paced several times over.
		next_group = (sent_bytes / group_bytes + 1) * group_bytes;

		const int64_t at = deadline(sent_bytes);
		if (at - now < min_sleep_ns)
			return {};

		return at;
	}

private:
	int64_t start = 0;
	int64_t budget = 0;
	size_t total = 0;
	size_t next_group = group_bytes;
};

// Pacing state of one socket, i.e. of one sender thread.
//
// The window is a property of the frame slot, not of a single frame: every
// video stream (left, right, alpha, the promoted quad layer) pushes its frames
// into the same queue and they are drained by the same thread, so three streams
// each paced over 40% of the frame period would take 120% of it. Instead the
// slot owns one window and the frames in it share what is left.
class pacing_slot
{
public:
	// Budget for a frame that starts going out at `now`, with `queued` further
	// frames already waiting behind it on the same socket.
	//
	// The first frame of a slot opens it and gets the whole window. Frames that
	// follow within the same slot get an equal share of whatever is left of it,
	// and once the window is spent they get nothing and are blasted out. A
	// frame that arrives a whole period after the slot opened starts a new one:
	// either the previous slot is long finished, or the encoder delivered late
	// and the window it did not use is gone with it. Either way completion is
	// never pushed past the frame period.
	int64_t begin_frame(int64_t now, int64_t frame_period_ns, float window, size_t queued)
	{
		if (frame_period_ns <= 0 or window <= 0)
			return 0;

		const int64_t full = int64_t(frame_period_ns * std::clamp(window, 0.f, shard_pacer::max_window));

		if (not slot_open or now - slot_start >= frame_period_ns)
		{
			slot_start = now;
			slot_open = true;
		}

		const int64_t left = slot_start + full - now;
		if (left <= 0)
			return 0;

		return left / int64_t(queued + 1);
	}

	void reset()
	{
		slot_open = false;
	}

private:
	// Monotonic time the current frame slot opened
	int64_t slot_start = 0;
	bool slot_open = false;
};

} // namespace wivrn
