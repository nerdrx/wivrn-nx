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

// Arithmetic behind the headset's Transport page. Pure, so that the awkward cases — the
// first sample, a counter that was reset under the page, a signal sitting exactly on a
// threshold — are checked in tests/transport_status_test.cpp rather than in a headset.

#include <cstdint>

namespace wivrn
{

// Rate of a monotonic counter over an interval, in units per second.
//
// Returns 0 rather than something enormous for a non-positive interval, and for a counter
// that went backwards: every counter feeding the page is monotonic for the life of the
// session, but the objects holding them (a decoder, the audio handle) are recreated when
// the stream is reconfigured, and the page must show a zero rather than a spike when that
// happens.
constexpr double counter_rate(uint64_t previous, uint64_t current, int64_t interval_ns)
{
	if (interval_ns <= 0 or current < previous)
		return 0;

	return double(current - previous) * 1e9 / double(interval_ns);
}

// One step of an exponential moving average. alpha is the weight of the new sample, so a
// larger one follows the signal faster.
constexpr float ema_step(float average, float sample, float alpha)
{
	return average + alpha * (sample - average);
}

// Direction a signal is moving, from a fast and a slow average of it: +1 rising, -1
// falling, 0 within the dead band. The gap between the two averages is used rather than a
// difference of consecutive samples so that one noisy reading cannot flip the arrow.
//
// The dead band is inclusive at both edges: exactly `deadband` of drift is noise, not a
// trend, so a signal sitting on the threshold reads as steady rather than flickering.
constexpr int trend_direction(float fast, float slow, float deadband)
{
	const float drift = fast - slow;
	if (drift > deadband)
		return 1;
	if (drift < -deadband)
		return -1;
	return 0;
}

} // namespace wivrn
