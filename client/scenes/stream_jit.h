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

#include <algorithm>
#include <cstdint>

namespace wivrn
{

// --- just-in-time display scheduling -----------------------------------------------
//
// The client's render loop free-runs: xrWaitFrame returns and render() starts drawing
// immediately. On this device the runtime hands the frame back with VsyncDelay=3, so
// xrWaitFrame returns about three refresh periods (33 ms at 90 Hz) BEFORE the frame it
// just described will be on the panel, and the pass itself costs a few milliseconds. The
// remaining ~30 ms is dead time in which the client is holding a decoded frame it chose
// at the START of the interval. A frame that finishes decoding one millisecond after
// that choice waits a whole further refresh, and the pose baked into the frame that was
// chosen is one interval older than it needed to be.
//
// So: sleep through the dead time, and start the pass as late as it can safely be
// started -- predicted display time minus what the pass costs minus a margin. The choice
// of frame (common_frame) and everything downstream of it then happens at the far end of
// the interval instead of the near one.
//
// The two numbers this needs are both measured, not configured:
//
//   cost   a PEAK HOLD of the pass's own wall time (from the moment the sleep ends to
//          the moment the frame is submitted), not a mean: a mean budget is too small
//          half the time by construction. It rises to a new cost instantly and gives one
//          up only at `cost_decay_ns_per_frame`, which is slow on purpose. A plain
//          sliding-window maximum was tried first and is wrong here: a pass that spikes
//          less often than the window is long is a surprise EVERY time, and the harness
//          shows exactly that -- a 12 ms spike every 200 frames behind a 128-frame window
//          costs a late frame on every spike, which is the miss the guard forbids. The
//          hold covers a spike for about ten seconds after it, so a periodic one is paid
//          for once and then budgeted for.
//
//   margin a safety term on top of it. It starts at the floor and only ever WIDENS, on
//          a miss, and is capped. It is deliberately not allowed to narrow: a margin
//          that shrinks after every good frame will find the cliff edge again and again,
//          and the cost of one missed refresh on a headset is far higher than the cost
//          of a millisecond of margin.
//
// Three independent things count as a miss, because each of them is a different way the
// schedule can be wrong and only one of them is visible from inside the pass:
//
//   overrun     the pass took longer than the budget it was given
//   late        the pass finished after the frame's own predicted display time
//   skipped     the runtime's predicted display time advanced by more than one period,
//               which means a refresh went by with nothing new submitted
//
// `skipped` is the one that needs a second control, and it is the reason `sleep_cap_ns`
// exists next to the margin. A runtime paces xrWaitFrame against the frames it still has
// in flight, and the depth of that pipeline is not something the client is told. Sleeping
// most of a three-period lead pushes every submission to the far end of its interval; if
// the runtime's depth is shallower than the lead suggests, the next xrWaitFrame does not
// return until this frame is submitted and the loop's RATE halves -- which is precisely
// the failure the guard forbids, and it is invisible to a margin measured in
// milliseconds. So the cap on the sleep is itself adaptive and RATCHETS DOWN one refresh
// period at a time, never up. Whatever the pipeline depth turns out to be, the loop finds
// it within a few frames and then stays on the safe side of it for the rest of the
// session. Both adaptations move in the same direction: earlier, never later.
//
// While `frames_seen` is below the warm-up count nothing sleeps at all: the scheduler is
// only measuring what the pass costs on this device, at this resolution, with these
// settings. Every clamp below is a bound on the SLEEP, never on the submission, so the
// worst this can do when its inputs are nonsense is behave like the free-running loop.
struct jit_scheduler
{
	// --- tuning, all nanoseconds -------------------------------------------------
	//
	// The margin's floor. One millisecond covers the scheduler-wakeup jitter of a
	// nanosleep on this device; below that the wake itself is the dominant error.
	int64_t min_margin_ns = 1'000'000;
	// And its ceiling. At 8 ms the schedule has given up most of what it was trying
	// to win, so widening further would trade the whole feature away rather than
	// protect anything; from here on it behaves like a fixed conservative lead.
	int64_t max_margin_ns = 8'000'000;
	// How much one miss buys. Coarse on purpose: converging in a handful of misses
	// matters more than converging precisely, because every step of the search costs
	// a dropped refresh.
	int64_t margin_step_ns = 1'000'000;
	// A skipped refresh is the expensive miss and the least ambiguous, so it widens
	// harder than a mere overrun.
	int64_t margin_skip_step_ns = 2'000'000;

	// Never sleep longer than this whatever the runtime claims, so a nonsensical
	// predicted display time (a clock that jumped, a runtime that reports a time far
	// in the future) cannot stall the render loop. Four refresh periods at 90 Hz.
	int64_t max_sleep_ns = 45'000'000;

	// Iterations spent measuring before the first sleep.
	uint64_t warmup_frames = 90;

	// How fast the peak hold gives up a cost it is no longer seeing. 10 us a frame is
	// ten seconds to walk back from a 12 ms spike to a 3 ms steady state at 90 Hz.
	int64_t cost_decay_ns_per_frame = 10'000;

	// Ceiling on the cost the peak hold will remember, in multiples of the refresh
	// period. Measured on the device: the first iterations of a stream scene cost 60-70
	// ms (swapchain creation, pipeline warm-up, the first decode), and at the decay rate
	// above that single outlier keeps the budget over the whole available slack for
	// more than a minute -- so the schedule sleeps zero for the first minute of every
	// session, which is most of a short one.
	//
	// Remembering it precisely buys nothing. Once the budget exceeds the slack the
	// runtime hands over, the sleep is zero whatever the number is; the only thing a
	// larger value changes is how long recovery takes. So the hold saturates here.
	int64_t cost_ceiling_periods = 4;

	// --- measured state ----------------------------------------------------------
	int64_t cost_peak_ns = 0;
	uint64_t frames_seen = 0;
	int64_t margin_ns = 0;
	// The ratchet described above. Starts permissive and only ever shrinks.
	int64_t sleep_cap_ns = 45'000'000;

	// Counters, for the report line. Reset with reset_counters(), which leaves the
	// LEARNT state (the cost window and the margin) alone: a two-second report window
	// closing is not a reason to forget what the pass costs.
	uint64_t slept = 0, missed_overrun = 0, missed_late = 0, missed_skipped = 0;
	int64_t sleep_total_ns = 0, sleep_max_ns = 0;
	int64_t slack_total_ns = 0;
	int64_t lead_total_ns = 0, lead_min_ns = 0;
	uint64_t lead_n = 0;

	jit_scheduler()
	{
		margin_ns = min_margin_ns;
		sleep_cap_ns = max_sleep_ns;
	}

	// The worst the pass has recently cost. Zero until the first frame is recorded,
	// which is why the warm-up gate exists rather than trusting this.
	int64_t cost_ns() const
	{
		return cost_peak_ns;
	}

	int64_t budget_ns() const
	{
		return cost_ns() + margin_ns;
	}

	// How long to sleep before starting the pass, given the frame the runtime just
	// described and the current time, both in the runtime's clock. Zero means "start
	// now", which is exactly the behaviour of the loop without this.
	//
	// `enabled` is passed rather than read from a member so that the toggle is
	// evaluated by the caller once per frame and this stays a pure function of its
	// arguments -- which is what makes it testable without a headset.
	int64_t sleep_ns(bool enabled, int64_t now, int64_t predicted_display_time) const
	{
		if (not enabled or frames_seen < warmup_frames)
			return 0;

		const int64_t slack = predicted_display_time - now;
		// The runtime is already at or past the deadline, or its numbers are not
		// usable. Nothing to give away.
		if (slack <= 0)
			return 0;

		int64_t sleep = slack - budget_ns();
		if (sleep <= 0)
			return 0;
		return std::min({sleep, max_sleep_ns, sleep_cap_ns});
	}

	// Called once per iteration after the frame has been submitted.
	//
	//   cost      wall time from the end of the sleep to the end of the submission
	//   budget    what sleep_ns() reserved for it on this frame (0 while warming up)
	//   slept     what it actually slept
	//   lead      predicted display time minus the time the submission finished; a
	//             negative lead is a frame handed over after it was due
	//   period_jump  the runtime's predicted display time advanced by more than one
	//                and a half periods since the previous iteration
	//   period       the runtime's predicted display period, which is the step the
	//                sleep cap ratchets down by
	void account(int64_t cost, int64_t budget, int64_t slept_ns, int64_t lead, bool period_jump, int64_t period)
	{
		const int64_t ceiling = period > 0 ? period * cost_ceiling_periods : 0;
		// Two-speed decay, and the threshold is where it is for a reason that the
		// harness found the hard way. Coverage of a REPEATING spike is arithmetic: the
		// hold must not fall further than the margin between one spike and the next,
		// or every spike overruns. At 10 us a frame and a 200-frame interval that is
		// 2 ms, which the margin reaches; a proportional decay applied everywhere made
		// it 9 ms, which it does not, and case [3] went from one late frame to six.
		//
		// So the flat rate stays wherever the schedule is still doing its job, and the
		// fast rate applies only ABOVE two refresh periods -- a region where the budget
		// already exceeds anything the slack can hide, the sleep is zero regardless,
		// and holding the value precisely buys nothing but a slower recovery. That is
		// the region a 67 ms scene start lands in, and the only one it needs.
		const int64_t fast_above = period > 0 ? period * 2 : 0;
		const int64_t decay = (fast_above > 0 and cost_peak_ns > fast_above)
		                              ? std::max(cost_decay_ns_per_frame, cost_peak_ns / 256)
		                              : cost_decay_ns_per_frame;
		cost_peak_ns = std::max(cost, cost_peak_ns - decay);
		if (ceiling > 0)
			cost_peak_ns = std::min(cost_peak_ns, ceiling);
		++frames_seen;

		sleep_total_ns += slept_ns;
		sleep_max_ns = std::max(sleep_max_ns, slept_ns);
		if (slept_ns > 0)
			++slept;

		slack_total_ns += budget;
		lead_total_ns += lead;
		lead_min_ns = lead_n == 0 ? lead : std::min(lead_min_ns, lead);
		++lead_n;

		// Only ever widens, and only on a frame whose sleep could plausibly BE the
		// cause. `slept_ns > 0` was the first attempt at that and it is far too weak:
		// measured on the device, the opening seconds of a stream scene skip refreshes
		// for their own reasons while the schedule is sleeping a fraction of a
		// millisecond out of fifty of slack, and charging those to the sleep ratcheted
		// the cap to zero within one report window -- switching the feature off for the
		// rest of the session before it had ever slept a meaningful amount.
		//
		// A sleep shorter than one refresh period cannot have pushed a submission
		// across a refresh boundary it would otherwise have made: the submission moved
		// by less than the distance between boundaries. So that is the bar. It keeps
		// every miss the schedule can actually cause, and drops the ones it cannot.
		const int64_t attributable = period > 0 ? period : margin_skip_step_ns;
		if (slept_ns >= attributable)
		{
			int64_t widen = 0;
			// A pass that outran its budget is a costing error: the margin is the
			// right control and the sleep cap is not at fault.
			if (cost > budget)
			{
				++missed_overrun;
				widen = std::max(widen, margin_step_ns);
			}
			// Handed over after it was due. Both controls: the margin because the
			// budget was too small, the cap because submitting this late is what
			// left no room.
			if (lead < 0)
			{
				++missed_late;
				widen = std::max(widen, margin_step_ns);
				ratchet(period);
			}
			// A refresh went by. Milliseconds of margin cannot fix a pipeline that
			// is one frame too deep, so this only moves the cap -- by a whole
			// refresh period, so a wrong guess about the depth is corrected in as
			// many frames as the guess was periods out.
			if (period_jump)
			{
				++missed_skipped;
				ratchet(period);
			}
			if (widen > 0)
				margin_ns = std::min(max_margin_ns, margin_ns + widen);
		}
	}

	// One step down the sleep ratchet, never up, never below zero (at zero the loop is
	// free-running again, which is the behaviour this replaces and so is always safe).
	void ratchet(int64_t period)
	{
		const int64_t step = period > 0 ? period : margin_skip_step_ns;
		sleep_cap_ns = std::max<int64_t>(0, sleep_cap_ns - step);
	}

	void reset_counters()
	{
		slept = missed_overrun = missed_late = missed_skipped = 0;
		sleep_total_ns = sleep_max_ns = slack_total_ns = 0;
		lead_total_ns = 0;
		lead_min_ns = 0;
		lead_n = 0;
	}
};

} // namespace wivrn
