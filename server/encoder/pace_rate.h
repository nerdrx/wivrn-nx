/*
 * The frame admission test runs on compositor ticks.  Its half-tick tolerance
 * means the effective admitted rate can be higher than 1 / pace_interval.
 */
#pragma once

#include <algorithm>
#include <chrono>

namespace wivrn
{

// Preserve fractional cadence across source ticks, discarding missed periods.
// After acceptance less than one interval of credit remains, even after stalls.
inline bool pace_accumulated_admit(std::chrono::steady_clock::time_point now,
                                   double interval,
                                   std::chrono::steady_clock::time_point & phase)
{
    const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(interval));
    if (now - phase < step)
        return false;
    phase += step;
    if (now - phase >= step)
        phase = now;
    return true;
}

constexpr double pace_admission_tolerance(double source_fps)
{
	return source_fps > 0.0 ? 0.5 / source_fps : 0.0;
}

// Conservative upper bound for the rate at which pace_admit can admit frames.
// Quantized compositor ticks can still admit more slowly than this bound, which
// may underuse some link allowance; that is preferable to budgeting above it.
// It is capped at the compositor rate and falls back to that rate when pacing
// is disabled or has no positive interval.
constexpr double effective_admission_fps(double source_fps, double pace_interval,
                                         bool accumulate = false)
{
	if (!(source_fps > 0.0) or !(pace_interval > 0.0))
		return source_fps;

	const double interval = pace_interval - pace_admission_tolerance(source_fps);
	if (!(interval > 0.0))
		return source_fps;
	return std::min(source_fps, 1.0 / (accumulate ? pace_interval : interval));
}

} // namespace wivrn
