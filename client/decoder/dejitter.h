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
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wivrn
{

// Adaptive playout delay for the video: how long a decoded frame is held before it is
// eligible to be shown.
//
// Everything else in the loss ladder — the parity, the retransmissions, the intra refresh —
// is about shards that never arrived. This is about shards that all arrived, at the wrong
// times. A link can deliver every byte and still look terrible: two frames turn up late,
// then three land in the same millisecond. The headset picks the frame nearest the vsync it
// is rendering for, so the two late refreshes repeat the previous picture and the clump is
// then thrown away down to its newest member. Nothing was lost and the world still judders.
//
// The fix is the one every audio pipeline has had for decades, and the one WiVRn's own audio
// path already runs (client/audio/android/audio.cpp deepens its ring on underrun and trims it
// past 50 ms): stop showing frames the instant they are ready, and show them on a clock that
// runs a little behind. A frame that is D late for its slot is on time for its slot plus D.
// The clump is then released one frame per refresh instead of collapsing to its last member.
//
// D is not a constant, because the right value is a property of the link and paying it on a
// link that does not need it is pure latency. So it is measured:
//
//   lateness = when the frame was ready - when it was meant to be shown
//
// both in the headset's clock, the second being the display time the server predicted and
// stamped into the frame. On a healthy link every frame is ready before its slot and every
// sample is negative. D is the 95th percentile of a sliding window of those samples, clamped
// to at least zero: the delay at which 95% of frames make their slot. A link with no jitter
// therefore lands on D = 0 by construction — not as a special case in the code, but because
// the number it is measuring genuinely is zero — and the frame selection is then exactly what
// it was before this existed.
//
// The dynamics are asymmetric, like the audio ring's: D rises to the measurement immediately,
// because a frame shown too early is a frame skipped and there is nothing to gain by easing
// into the fix; it falls back by a fraction of the gap per frame, over a couple of seconds,
// because a link that was bad a moment ago will probably be bad again and repeatedly draining
// the buffer costs a skipped frame each time.
//
// tests/dejitter_test.cpp drives this directly. No Vulkan, no OpenXR, no clock: the whole
// class is (arrival, target) pairs in and a delay out.
class dejitter_buffer
{
public:
	// Samples in the sliding window. 128 is about 1.4 s at 90 Hz — long enough that one
	// unlucky frame does not move the percentile, short enough that the delay follows a
	// link that changes character within a couple of seconds.
	static constexpr size_t window = 128;

	// Below this many samples the window is not yet evidence of anything and the delay stays
	// at zero. Covers the first few frames of a session and of every reconnect, where the
	// pipeline is still filling and lateness means nothing.
	static constexpr size_t min_samples = 32;

	// Percentile of the window the delay targets, in percent. High enough that the delay
	// covers the clumps rather than the median frame, low enough that a single outlier — one
	// frame that waited on a retransmission — does not set it.
	static constexpr size_t percentile = 95;

	// Hard ceiling on the delay. Two frames at 90 Hz, which is the most latency worth trading
	// for pacing: past that the headset's own reprojection is doing more harm than the judder
	// this fixes.
	static constexpr int64_t max_delay_ns = 20'000'000;

	// The delay also cannot usefully exceed what the frame ring holds. The headset keeps
	// scenes::stream::image_buffer_size decoded frames per stream, so at most this many
	// frame periods of history can actually be selected; asking for more just picks the
	// oldest frame that exists and stops buying anything.
	static constexpr int64_t max_delay_periods = 2;

	// Decay of the delay towards a lower measurement: this fraction of the remaining gap per
	// sample. 1/256 is a time constant of about 2.8 s at 90 Hz.
	static constexpr int decay_shift = 8;

	// Floor on that decay, so it reaches its target in finite time rather than asymptotically.
	// It also sets the shape of the tail: the exponential above governs until the remaining
	// gap is 256 × this, and the last five milliseconds come off linearly. A full drain from
	// the ceiling is a little under 700 frames, about eight seconds at 90 Hz — which is the
	// point. Draining costs a skipped frame every time the delay steps down, so a link that
	// was clumping a moment ago is not worth hurrying back from.
	static constexpr int64_t min_decay_ns = 20'000;

	// Whether the delay is applied at all, and the refresh period it is measured in frames
	// of. Called from the render thread, once per refresh; touches only atomics, so it never
	// races the sampling below.
	void configure(bool enabled, int64_t frame_period_ns)
	{
		on.store(enabled, std::memory_order_relaxed);
		period.store(frame_period_ns, std::memory_order_relaxed);
	}

	// Forget the window. For the points where the timings that fill it stop meaning anything:
	// a new stream, and a reconnect, after which the frames on the other side of the gap are
	// not evidence about the link on this side.
	void reset()
	{
		count = 0;
		cursor = 0;
		delay.store(0, std::memory_order_relaxed);
	}

	// One decoded frame, from the thread that decoded it. `arrival` is when it became ready,
	// `target` the display time the server stamped on it; both in the headset's clock.
	// Returns the delay in force from now on.
	int64_t sample(int64_t arrival, int64_t target)
	{
		const bool enabled = on.load(std::memory_order_relaxed);
		if (not enabled)
		{
			// Nothing is measured while the feature is off, so the window that is there
			// when it is switched back on describes a link that may be minutes old. Drop
			// it rather than adapt away from it.
			if (was_on)
			{
				was_on = false;
				reset();
			}
			return 0;
		}
		was_on = true;

		lateness[cursor] = arrival - target;
		cursor = (cursor + 1) % window;
		if (count < window)
			++count;

		const int64_t cap = ceiling();
		int64_t target_delay = 0;
		if (count >= min_samples)
			target_delay = std::clamp(percentile_of_window(), int64_t(0), cap);

		int64_t d = delay.load(std::memory_order_relaxed);
		if (target_delay > d)
		{
			// Rise at once: every refresh spent below the delay the link is asking for is
			// a refresh that shows the wrong frame.
			d = target_delay;
		}
		else if (target_delay < d)
		{
			const int64_t gap = d - target_delay;
			d -= std::min(gap, std::max(gap >> decay_shift, min_decay_ns));
		}

		// The ceiling can move under us — it follows the refresh rate, and the emergency
		// half-rate mode halves that mid-session — so clamp on the way out too, not just on
		// the way in.
		d = std::clamp(d, int64_t(0), cap);
		delay.store(d, std::memory_order_relaxed);
		return d;
	}

	// The playout delay in force. Zero whenever the feature is off, unconditionally and
	// without consulting anything that sampling may have left behind: that is what makes the
	// frame selection with the toggle off identical to having no de-jitter buffer at all.
	int64_t delay_ns() const
	{
		if (not on.load(std::memory_order_relaxed))
			return 0;
		return delay.load(std::memory_order_relaxed);
	}

	// Samples in the window, for the tests and for anything that wants to know whether the
	// delay above is measured or merely default
	size_t samples() const
	{
		return count;
	}

private:
	// How far the delay may go: the fixed ceiling, and whatever the frame ring can actually
	// hold. A refresh period of zero means nothing has told us the rate yet, in which case
	// only the fixed ceiling applies.
	int64_t ceiling() const
	{
		const int64_t p = period.load(std::memory_order_relaxed);
		if (p <= 0)
			return max_delay_ns;
		return std::min(max_delay_ns, max_delay_periods * p);
	}

	int64_t percentile_of_window() const
	{
		std::array<int64_t, window> sorted;
		std::copy_n(lateness.begin(), count, sorted.begin());
		const size_t k = std::min(count - 1, count * percentile / 100);
		std::nth_element(sorted.begin(), sorted.begin() + k, sorted.begin() + count);
		return sorted[k];
	}

	// Written by the render thread, read by the decoder thread
	std::atomic<bool> on = false;
	std::atomic<int64_t> period = 0;
	// Written by the decoder thread, read by both it and the render thread
	std::atomic<int64_t> delay = 0;

	// Owned by the decoder thread alone
	std::array<int64_t, window> lateness{};
	size_t count = 0;
	size_t cursor = 0;
	bool was_on = false;
};

} // namespace wivrn
