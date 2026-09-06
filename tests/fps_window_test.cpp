// The frame-rate readout's window, and the gap that made it lie.
//
// The HUD differences monotonic counters over a window about a second wide. Two of the
// figures it now carries are the render loop's own: how many times render() was entered
// per second, and the mean display period the runtime predicted across those entries.
// They exist because the compositor's own PxrMetric output went silent on this device, and
// because "33 shown out of a loop running 90 times a second" and "33 shown out of a loop
// running 33 times" are different bugs that the shown/decoded pair alone cannot tell apart.
//
// The case this test is really about: the sampler runs from the render loop, and the
// render loop only runs while the OpenXR session does. With the headset off the face,
// application::loop() sleeps 250 ms at a time instead of calling render() -- for minutes,
// while the network and decoder threads keep running and keep logging. That is exactly the
// state a logcat capture was taken in, where every nxwarp[0] line was present and not one
// "render: N iterations" line was, and it read as a broken logger rather than as a loop
// that was not being called. A window left open across that gap divides one window's
// frames by the whole parked span and reports a rate near zero for a loop that is fine.
//
// Build:
//   g++ -std=c++23 -I client -o fps_window_test tests/fps_window_test.cpp
//   ./fps_window_test

#include "fps_window.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace wivrn::client;

static int checks = 0;
static int failures = 0;

static void check(bool ok, const std::string & what)
{
	++checks;
	if (ok)
		std::printf("  ok   %s\n", what.c_str());
	else
	{
		++failures;
		std::printf("  FAIL %s\n", what.c_str());
	}
}

static bool near(double a, double b, double eps = 1e-3)
{
	return std::fabs(a - b) < eps;
}

// A 90 Hz panel, in the units the counters are kept in.
static constexpr uint64_t ns_90hz = 11'111'111;

static void part_a()
{
	std::printf("\nPart A: which windows are measurements\n");

	check(judge_window(1.0) == window_verdict::usable, "a one second window is the normal case");
	check(judge_window(0.25) == window_verdict::usable, "so is a single sample period");
	check(judge_window(0.0) == window_verdict::too_short, "no elapsed time measures nothing");
	check(judge_window(0.01) == window_verdict::too_short,
	      "and a window narrower than a couple of frames would read in the hundreds");

	// The boundaries themselves, so a later edit to the constants has to be deliberate.
	check(judge_window(fps_window_min_seconds) == window_verdict::too_short,
	      "the minimum is exclusive");
	check(judge_window(fps_window_min_seconds + 1e-6) == window_verdict::usable,
	      "just past it is usable");
	check(judge_window(fps_window_stale_seconds) == window_verdict::usable,
	      "the stale bound is exclusive too");
	check(judge_window(fps_window_stale_seconds + 1e-6) == window_verdict::stale,
	      "and just past it the window is a gap");

	// The whole point: a parked loop must be recognised, not divided through.
	check(judge_window(88.7) == window_verdict::stale,
	      "88.7 s -- the gap actually seen in the capture -- is stale, not a 0.1/s loop");
	check(judge_window(600.0) == window_verdict::stale, "and so is ten minutes off the face");
}

static void part_b()
{
	std::printf("\nPart B: the rates\n");

	// A loop running at panel rate for one second, submitting every iteration.
	check(near(rate_over(90, 0, 1.0), 90.0), "90 iterations in a second is 90/s");
	check(near(rate_over(1090, 1000, 1.0), 90.0), "and the counters are differenced, not read");
	check(near(rate_over(23, 0, 0.25), 92.0), "a quarter second window scales up");
	check(near(rate_over(100, 100, 1.0), 0.0), "a counter that did not move is a rate of zero");
	check(near(rate_over(90, 0, 0.0), 0.0), "and no elapsed time is zero rather than infinity");

	// The shape of the bug the pair exists to name. Both sessions show 33 fps.
	{
		// The loop runs at panel rate and submits a third of its iterations.
		const float loop = rate_over(90, 0, 1.0);
		const float shown = rate_over(33, 0, 1.0);
		check(near(loop, 90.0) and near(shown, 33.0),
		      "a loop at 90 showing 33 reads as 90/s beside 33 shown");
	}
	{
		// The loop itself is late; every iteration it manages does submit.
		const float loop = rate_over(33, 0, 1.0);
		const float shown = rate_over(33, 0, 1.0);
		check(near(loop, 33.0) and near(shown, 33.0),
		      "a loop at 33 showing 33 reads as 33/s beside 33 shown");
	}
}

static void part_c()
{
	std::printf("\nPart C: the display period is per iteration, not per second\n");

	// 90 iterations, each predicting a 90 Hz period.
	check(near(mean_period_ms(90 * ns_90hz, 0, 90, 0), 11.111),
	      "90 iterations at 90 Hz average 11.1 ms");

	// The same panel, but the loop only managed a third of the iterations. The period is
	// a property of the panel, so it must not move -- this is the whole reason it is
	// averaged over iterations rather than over the window's seconds.
	check(near(mean_period_ms(30 * ns_90hz, 0, 30, 0), 11.111),
	      "a third as many iterations on the same panel still average 11.1 ms");
	{
		// What dividing by wall time instead would have produced, for the record.
		const double wrong = double(30 * ns_90hz) / 1e6 / 1.0;
		check(near(wrong, 333.333, 1e-2),
		      "where dividing by the window's seconds would have said 333 ms");
	}

	// 72 Hz, the other rate this headset runs at.
	check(near(mean_period_ms(72 * 13'888'888ull, 0, 72, 0), 13.889),
	      "and a 72 Hz panel averages 13.9 ms");

	// A window with no iteration in it reports nothing, which the HUD suppresses. A
	// measured 0.0 ms and an absent measurement read identically on a HUD and mean
	// opposite things.
	check(near(mean_period_ms(1000, 1000, 5, 5), 0.0),
	      "no iterations in the window reports zero, not a division by nothing");
	check(near(mean_period_ms(0, 0, 0, 0), 0.0), "and so does an empty window");
}

static void part_d()
{
	std::printf("\nPart D: a session that was parked, replayed through the window\n");

	// Reconstructed from the capture: a stereo session at a 90 Hz panel, then the headset
	// comes off and the loop stops being called for 88.7 s while the decoder threads carry
	// on, then it resumes.
	struct sample
	{
		double t;
		uint64_t iterations;
		uint64_t period_ns;
	};

	const sample before{10.00, 900, 900 * ns_90hz};
	const sample after_park{98.70, 900, 900 * ns_90hz};   // not one iteration in between
	const sample resumed{98.95, 922, 922 * ns_90hz};      // a quarter second back at 90 Hz

	// Differencing across the park is exactly the "0.2/s loop" a naive window reports.
	{
		const double dt = after_park.t - before.t;
		check(judge_window(dt) == window_verdict::stale,
		      "the window spanning the park is rejected before it is divided");
		const float would_have_said = rate_over(after_park.iterations, before.iterations, dt);
		check(near(would_have_said, 0.0),
		      "which is right: differencing it would have reported a 0/s render loop");
	}

	// After the ring restarts, the first usable window measures the resumed loop only.
	{
		const double dt = resumed.t - after_park.t;
		check(judge_window(dt) == window_verdict::usable, "the window after the restart is usable");
		check(near(rate_over(resumed.iterations, after_park.iterations, dt), 88.0),
		      "and reports the loop back at 88/s rather than the average with the park");
		check(near(mean_period_ms(resumed.period_ns, after_park.period_ns,
		                          resumed.iterations, after_park.iterations),
		           11.111),
		      "with the panel's own 11.1 ms period intact");
	}
}

int main()
{
	part_a();
	part_b();
	part_c();
	part_d();

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
