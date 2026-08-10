// Multipath stage 3: the spillover scheduler that splits one frame's shards
// between the Wi-Fi (paced UDP) path and the USB (TCP) one.
//
// Part A: the split point. wivrn::spill_scheduler turns a Wi-Fi capacity and the
// wall time a frame has to be delivered in into a byte budget; the checks pin the
// arithmetic, its monotonicity, and the two degenerate ends (no capacity, no
// deadline). One of them is the operating point the design is sized around —
// 100 Mbit/s over Wi-Fi, a 40% pacing window, 90 fps, three video streams — where
// the budget has to come out equal to one stream's frame, i.e. nothing spills
// until the bitrate controller raises the bitrate past what Wi-Fi was doing.
//
// Part B: routing one frame. A stand-in for video_encoder::SendData walks a frame
// shard by shard, asking the scheduler where each one goes. Checks that the split
// is a prefix/suffix one (so order within each path is order within the frame),
// that every shard goes out exactly once, and that the two halves add back up.
//
// Part C: failure mid-frame. The secondary path dies at shard k. The remainder —
// including the shard that failed — must go on the primary, the secondary must
// never be tried again (wivrn::TCP poisons a socket whose send threw, so a retry
// is not merely wasteful but wrong), and the frame must still be whole.
//
// Part D: pacing interaction. The pacer is given the primary's share and not the
// whole frame, so the prefix is spread over the whole window rather than over the
// fraction of it that corresponds to the prefix.
//
// Build:
//   g++ -std=c++23 -I server/encoder -I common -o striping_test tests/striping_test.cpp
//   ./striping_test

#include "shard_pacer.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using wivrn::pacing_slot;
using wivrn::shard_pacer;
using wivrn::spill_scheduler;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                   \
	do                                                                            \
	{                                                                             \
		++checks;                                                             \
		if (not(cond))                                                        \
		{                                                                     \
			++failures;                                                   \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                     \
	} while (0)

namespace
{

constexpr int64_t ms = 1'000'000;

// Where one shard went
enum class path
{
	primary,
	secondary,
};

struct sent_shard
{
	size_t offset;
	size_t size;
	path where;
};

// A stand-in for the shard loop of video_encoder::SendData: same order of
// operations, same use of the scheduler, no sockets and no encoder.
//
// `fail_at_secondary_shard` is the index (counted over the shards that actually
// go to the secondary) whose send fails; -1 for a path that never fails.
struct frame_sender
{
	std::vector<sent_shard> sent;
	size_t primary_bytes = 0;
	size_t secondary_bytes = 0;
	int secondary_attempts = 0;

	void run(size_t frame_bytes, size_t shard_bytes, spill_scheduler spill, int fail_after_n_secondary = -1)
	{
		size_t offset = 0;
		int secondary_ok = 0;

		while (offset < frame_bytes)
		{
			const size_t size = std::min(shard_bytes, frame_bytes - offset);

			bool on_primary = true;
			if (spill.spill(offset))
			{
				++secondary_attempts;
				const bool ok = fail_after_n_secondary < 0 or secondary_ok < fail_after_n_secondary;
				if (ok)
				{
					++secondary_ok;
					on_primary = false;
				}
				else
				{
					// Exactly what SendData does: the path is gone, this shard
					// and everything after it goes back on the primary
					spill.fail();
				}
			}

			sent.push_back({offset, size, on_primary ? path::primary : path::secondary});
			(on_primary ? primary_bytes : secondary_bytes) += size;
			offset += size;
		}
	}
};

// --- Part A ---------------------------------------------------------------

void part_a()
{
	std::printf("Part A: the split point\n");

	// An unarmed scheduler is the not-combining case: nothing ever spills.
	{
		spill_scheduler s;
		CHECK(not s.active());
		CHECK(not s.spill(0));
		CHECK(not s.spill(1u << 30));
	}

	// 80 Mbit/s for 10 ms is 100 000 bytes.
	CHECK(spill_scheduler::budget_bytes(80'000'000, 10 * ms) == 100'000);
	// Halving either halves the budget.
	CHECK(spill_scheduler::budget_bytes(40'000'000, 10 * ms) == 50'000);
	CHECK(spill_scheduler::budget_bytes(80'000'000, 5 * ms) == 50'000);

	// Degenerate ends: no capacity or no deadline means the primary can take
	// nothing, so the whole frame spills. That is the correct reading of both —
	// a Wi-Fi share that measured zero, and a pacing slot with nothing left.
	CHECK(spill_scheduler::budget_bytes(0, 10 * ms) == 0);
	CHECK(spill_scheduler::budget_bytes(80'000'000, 0) == 0);
	CHECK(spill_scheduler::budget_bytes(80'000'000, -1) == 0);
	{
		spill_scheduler s(0, 10 * ms);
		CHECK(s.active());
		CHECK(s.split_at() == 0);
		CHECK(s.spill(0));
	}

	// The operating point the design is sized around. 100 Mbit/s of Wi-Fi, spread
	// over 40% of a frame period, is a 250 Mbit/s send rate; three streams share
	// the window, so one frame gets 40% * 11.11 ms / 3 of it. The budget that
	// comes out has to be one stream's frame at 100 Mbit/s and 90 fps — i.e. at
	// the operating point nothing spills, and every bit the controller adds on
	// top of it does.
	{
		const int64_t period = int64_t(1e9 / 90);
		const int64_t window = int64_t(period * 0.4);
		const int64_t deliver = window / 3;
		const uint32_t wifi_capacity = uint32_t(100'000'000 / 0.4);

		const size_t budget = spill_scheduler::budget_bytes(wifi_capacity, deliver);
		const size_t frame = size_t(100'000'000.0 / 8 / 90 / 3);

		// Within a percent of each other: the only difference is integer rounding
		// of the period and the window.
		CHECK(budget > frame * 99 / 100 and budget < frame * 101 / 100);

		spill_scheduler s(wifi_capacity, deliver);
		CHECK(not s.spill(0));
		CHECK(not s.spill(budget - 1));
		CHECK(s.spill(budget));

		// The controller raises the bitrate by half: the extra half spills, and
		// nothing else does.
		const size_t bigger = frame * 3 / 2;
		frame_sender fs;
		fs.run(bigger, 1300, s);
		// The split lands on a shard boundary, so the primary keeps at most one
		// shard more than the budget
		CHECK(fs.primary_bytes >= budget and fs.primary_bytes < budget + 1300);
		CHECK(fs.primary_bytes + fs.secondary_bytes == bigger);
		CHECK(fs.secondary_bytes > 0);
	}

	// Monotone in the offset: the split is a threshold, so once it spills it keeps
	// spilling. That is what makes the two halves a prefix and a suffix.
	{
		spill_scheduler s(50'000'000, 4 * ms);
		bool seen_spill = false;
		for (size_t offset = 0; offset < 100'000; offset += 137)
		{
			const bool sp = s.spill(offset);
			if (sp)
				seen_spill = true;
			CHECK(not(seen_spill and not sp));
		}
		CHECK(seen_spill);
	}
}

// --- Part B ---------------------------------------------------------------

void part_b()
{
	std::printf("Part B: routing one frame\n");

	const size_t shard = 1300;

	// A frame that fits the budget outright never touches the secondary path.
	{
		spill_scheduler s(80'000'000, 10 * ms); // 100 000 bytes
		frame_sender fs;
		fs.run(60'000, shard, s);
		CHECK(fs.secondary_attempts == 0);
		CHECK(fs.secondary_bytes == 0);
		CHECK(fs.primary_bytes == 60'000);
	}

	// A frame twice the budget: the first budget bytes on Wi-Fi, the rest on USB,
	// every shard sent exactly once and in order.
	{
		spill_scheduler s(80'000'000, 10 * ms);
		frame_sender fs;
		fs.run(200'000, shard, s);

		CHECK(fs.primary_bytes + fs.secondary_bytes == 200'000);
		CHECK(fs.primary_bytes >= 100'000 and fs.primary_bytes < 100'000 + shard);
		CHECK(fs.secondary_bytes == 200'000 - fs.primary_bytes);

		// Contiguous, in order, and a clean prefix/suffix
		size_t expected = 0;
		bool in_suffix = false;
		for (const auto & e: fs.sent)
		{
			CHECK(e.offset == expected);
			expected += e.size;
			if (e.where == path::secondary)
				in_suffix = true;
			else
				CHECK(not in_suffix);
		}
		CHECK(expected == 200'000);
		CHECK(in_suffix);
	}

	// A budget of zero puts the whole frame on the secondary; a budget larger than
	// the frame puts all of it on the primary. Both are reachable in practice — a
	// spent pacing slot and a quiet frame respectively.
	{
		spill_scheduler zero(0, 10 * ms);
		frame_sender a;
		a.run(50'000, shard, zero);
		CHECK(a.primary_bytes == 0);
		CHECK(a.secondary_bytes == 50'000);

		spill_scheduler big(800'000'000, 10 * ms);
		frame_sender b;
		b.run(50'000, shard, big);
		CHECK(b.primary_bytes == 50'000);
		CHECK(b.secondary_bytes == 0);
	}
}

// --- Part C ---------------------------------------------------------------

void part_c()
{
	std::printf("Part C: the secondary path dies mid-frame\n");

	const size_t shard = 1300;
	const size_t frame = 200'000;

	for (int fail_after: {0, 1, 5, 20})
	{
		spill_scheduler s(80'000'000, 10 * ms);
		frame_sender fs;
		fs.run(frame, shard, s, fail_after);

		// The whole frame still went out, once, in order
		size_t expected = 0;
		for (const auto & e: fs.sent)
		{
			CHECK(e.offset == expected);
			expected += e.size;
		}
		CHECK(expected == frame);
		CHECK(fs.primary_bytes + fs.secondary_bytes == frame);

		// Exactly `fail_after` shards made it to the secondary, and the socket was
		// tried exactly once more — the one that failed. Never again after that:
		// a TCP socket whose send threw is poisoned, and retrying it is what the
		// stage 3 code must not do.
		CHECK(fs.secondary_attempts == fail_after + 1);

		// Everything after the failure is on the primary
		bool failed_yet = false;
		int secondary_seen = 0;
		for (const auto & e: fs.sent)
		{
			if (e.where == path::secondary)
			{
				CHECK(not failed_yet);
				++secondary_seen;
				if (secondary_seen == fail_after)
					failed_yet = false;
			}
		}
		CHECK(secondary_seen == fail_after);
	}

	// A path that dies before the split point is ever reached is never noticed at
	// all: nothing was going to go over it in this frame.
	{
		spill_scheduler s(80'000'000, 10 * ms);
		frame_sender fs;
		fs.run(50'000, shard, s, 0);
		CHECK(fs.secondary_attempts == 0);
		CHECK(fs.primary_bytes == 50'000);
	}
}

// --- Part D ---------------------------------------------------------------

void part_d()
{
	std::printf("Part D: the pacer is given the primary's share\n");

	// One 200 kB frame, a 4 ms window, and a Wi-Fi budget of 100 kB. The pacer is
	// built over the *prefix*, so the last byte the primary sends is scheduled at
	// the end of the window. Pacing the whole frame instead would have put those
	// same 100 kB out in the first half of it, at twice the rate the window was
	// sized for — the burst pacing exists to prevent.
	const int64_t window = 4 * ms;
	const size_t frame = 200'000;

	spill_scheduler spill(80'000'000, 10 * ms);
	const size_t on_primary = std::min(frame, spill.split_at());
	CHECK(on_primary == 100'000);

	shard_pacer paced_prefix(0, window, on_primary);
	shard_pacer paced_whole(0, window, frame);

	CHECK(paced_prefix.deadline(on_primary) == window);
	CHECK(paced_whole.deadline(on_primary) == window / 2);
	// Halfway through the primary's share is halfway through the window
	CHECK(paced_prefix.deadline(on_primary / 2) == window / 2);

	// And the pacer never asks for a sleep past the end of the primary's share:
	// the spilled tail is handed over as fast as the socket takes it, which is the
	// whole point of putting it on the other path.
	CHECK(not paced_prefix.wait_until(on_primary, window).has_value());
	CHECK(not paced_prefix.wait_until(frame, window).has_value());

	// A slot whose window is spent gives the frame no budget at all; the caller
	// then sizes the spill from the frame period instead, and pacing stands down.
	{
		pacing_slot slot;
		const int64_t period = 11 * ms;
		const int64_t first = slot.begin_frame(0, period, 0.4f, 0);
		CHECK(first > 0);
		const int64_t late = slot.begin_frame(int64_t(period * 0.4f), period, 0.4f, 0);
		CHECK(late == 0);

		const int64_t deliver = late > 0 ? late : period;
		spill_scheduler s(80'000'000, deliver);
		CHECK(s.split_at() == spill_scheduler::budget_bytes(80'000'000, period));
	}
}

} // namespace

int main()
{
	std::printf("Multipath stage 3 (striping) test\n\n");
	part_a();
	part_b();
	part_c();
	part_d();
	std::printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
