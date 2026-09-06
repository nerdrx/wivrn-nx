/*
 * WiVRn VR streaming
 *
 * The rolling window the frame-rate readout is computed over.
 *
 * Extracted from scenes::stream so the arithmetic can be tested without an OpenXR session,
 * a Vulkan device or a decoder: everything here is a pure function of two counter
 * snapshots and the time between them. See tests/fps_window_test.cpp.
 *
 * The readout differences monotonic counters over a window about a second wide, resampled
 * four times a second. Three rules govern that window, and all three exist because of a
 * failure that was actually seen rather than out of caution:
 *
 *   too short   Two samples taken almost together divide a frame or two by almost no time
 *               and produce a number in the hundreds. The window has to have some width
 *               before it says anything.
 *
 *   stale       The sampler runs from the render loop, and the render loop only runs while
 *               the OpenXR session does. With the headset off the face the session stops
 *               and application::loop() sleeps 250 ms at a time instead of calling
 *               render() -- for minutes, while the network and decoder threads carry on
 *               and carry on logging, which is exactly what makes this state look like a
 *               live session from outside. A window left open across that gap divides one
 *               window's worth of frames by the whole parked span and reports a rate of
 *               nearly zero for a loop that is running perfectly. Such a window measures
 *               nothing and is thrown away rather than shown.
 *
 *   usable      Everything between the two.
 *
 * The period is the one figure here that is NOT a rate: it is a per-iteration quantity, so
 * it is averaged over the iterations in the window and not over its seconds. Dividing it
 * by wall time would produce a number that moves when the loop rate moves, which is the
 * opposite of what a display period is for.
 */

#pragma once

#include <cstdint>

namespace wivrn::client
{

// A window narrower than this is not yet a measurement.
inline constexpr double fps_window_min_seconds = 0.05;
// A window wider than this is a gap in the sampling, not a slow loop. The window is one
// second wide by construction, so a few times that cannot be reached by sampling that is
// merely late.
inline constexpr double fps_window_stale_seconds = 3.0;

enum class window_verdict
{
	too_short,
	usable,
	stale,
};

constexpr window_verdict judge_window(double dt_seconds)
{
	if (dt_seconds > fps_window_stale_seconds)
		return window_verdict::stale;
	if (dt_seconds > fps_window_min_seconds)
		return window_verdict::usable;
	return window_verdict::too_short;
}

// Frames per second from two readings of one monotonic counter. `newer` is not checked
// against `older`: every counter fed to this is monotonic within a session, and a readout
// is not the place to defend against a counter that went backwards.
constexpr float rate_over(uint64_t newer, uint64_t older, double dt_seconds)
{
	return dt_seconds > 0 ? float(double(newer - older) / dt_seconds) : 0.f;
}

// The mean predicted display period over the iterations in the window, in milliseconds.
// Zero when no iteration fell inside it, which the caller shows as nothing rather than as
// a 0.0 ms panel -- an absent measurement and a measured zero read the same on a HUD and
// mean opposite things.
constexpr float mean_period_ms(uint64_t period_ns_newer,
                               uint64_t period_ns_older,
                               uint64_t iterations_newer,
                               uint64_t iterations_older)
{
	const uint64_t iterations = iterations_newer - iterations_older;
	if (iterations == 0)
		return 0.f;
	return float(double(period_ns_newer - period_ns_older) / double(iterations) / 1e6);
}

} // namespace wivrn::client
