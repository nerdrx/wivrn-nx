/*
 * The frame admission test runs on compositor ticks.  Its half-tick tolerance
 * means the effective admitted rate can be higher than 1 / pace_interval.
 */
#pragma once

#include <algorithm>

namespace wivrn
{

constexpr double pace_admission_tolerance(double source_fps)
{
	return source_fps > 0.0 ? 0.5 / source_fps : 0.0;
}

// Conservative upper bound for the rate at which pace_admit can admit frames.
// Quantized compositor ticks can still admit more slowly than this bound, which
// may underuse some link allowance; that is preferable to budgeting above it.
// It is capped at the compositor rate and falls back to that rate when pacing
// is disabled or has no positive interval.
constexpr double effective_admission_fps(double source_fps, double pace_interval)
{
	if (!(source_fps > 0.0) or !(pace_interval > 0.0))
		return source_fps;

	const double interval = pace_interval - pace_admission_tolerance(source_fps);
	if (!(interval > 0.0))
		return source_fps;
	return std::min(source_fps, 1.0 / interval);
}

} // namespace wivrn
