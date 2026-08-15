// Adaptive video playout delay: the de-jitter buffer's adaptation, and the frame selection
// it feeds, driven as arrival timings with no decoder, no GPU and no clock.
//
// The class under test is the real one the headset runs — wivrn::dejitter_buffer
// (client/decoder/dejitter.h) — so this is not a model of the policy, it is the policy. What
// it cannot cover is the Vulkan/OpenXR end: whether the frame the selection picks actually
// reaches the display.
//
// Part A: off means zero, whatever the link is doing. This is the one that matters most —
//         with the toggle off the selection in common_frame subtracts a delay that is
//         identically zero, which is what makes the feature's "off" the behaviour that was
//         there before it existed.
// Part B: a calm link measures zero. Not by a special case, but because the number being
//         measured — how late frames are for their slots — genuinely is negative there.
// Part C: clumped arrivals raise the delay to about the worst of them.
// Part D: the asymmetry. Up in one sample, down over thousands, and never below zero.
// Part E: the clamps. The fixed 20 ms ceiling and the frame-ring bound of two refresh
//         periods, whichever is lower, and the ceiling following a refresh rate that moves.
// Part F: warm-up and reset. Too few samples is not evidence, and a reconnect throws the
//         window away rather than adapting away from it.
// Part G: the payoff, as a faithful model of what common_frame does with the delay. The same
//         clumped arrivals, through the same three-deep frame ring and the same
//         nearest-display-time rule: without the delay the picture stalls twice and jumps
//         three, with it every frame is shown exactly once.
//
// Build:
//   g++ -std=c++23 -I client -o dejitter_test tests/dejitter_test.cpp
//   ./dejitter_test

#include "decoder/dejitter.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

using wivrn::dejitter_buffer;

static int failures = 0;
static int checks = 0;
static bool verbose = false;

#define CHECK(...)                                                                           \
	do                                                                                   \
	{                                                                                    \
		++checks;                                                                    \
		if (not(__VA_ARGS__))                                                        \
		{                                                                            \
			++failures;                                                          \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
		}                                                                            \
	} while (0)

namespace
{

constexpr int64_t ms = 1'000'000;

// 90 Hz, the rate the ceiling and the clamps are quoted at everywhere else
constexpr int64_t period_90 = 11'111'111;
constexpr int64_t period_120 = 8'333'333;

// Frame n was meant to be shown here. The origin is arbitrary and deliberately not zero:
// nothing in the buffer may depend on the clock starting anywhere in particular.
int64_t target_of(int64_t n, int64_t period)
{
	return 1'000'000'000'000 + n * period;
}

// Feed `count` frames whose arrival is `lateness(n)` away from their slot. Returns the delay
// in force at the end.
template <typename F>
int64_t feed(dejitter_buffer & d, int64_t first, int64_t count, int64_t period, F lateness)
{
	int64_t delay = d.delay_ns();
	for (int64_t n = first; n < first + count; ++n)
	{
		const int64_t target = target_of(n, period);
		delay = d.sample(target + lateness(n), target);
		if (verbose and n % 32 == 0)
			std::printf("    frame %4lld late %8lld -> delay %8lld\n",
			            (long long)n,
			            (long long)lateness(n),
			            (long long)delay);
	}
	return delay;
}

// A link that delivers every frame comfortably before its slot, with a little noise
auto calm = [](int64_t n) {
	return -4 * ms + (n % 5) * 100'000 - 200'000;
};

// A link that delivers in clumps of three: two frames miss their slots entirely and then all
// three land together, just before the third one's. Nothing is lost — every byte arrives —
// and the picture still judders, which is the whole reason this feature exists.
auto clumped = [](int64_t n) {
	const int64_t phase = ((n % 3) + 3) % 3;
	return (2 - phase) * period_90 - 4 * ms;
};

// --- Part A -----------------------------------------------------------------
void part_a()
{
	std::printf("Part A: off is zero, whatever the link does\n");

	dejitter_buffer d;
	d.configure(false, period_90);

	for (int64_t n = 0; n < 500; ++n)
	{
		const int64_t target = target_of(n, period_90);
		CHECK(d.sample(target + clumped(n), target) == 0);
		CHECK(d.delay_ns() == 0);
	}

	// And nothing was accumulated behind the switch either: turning it on starts from a
	// clean window rather than from a measurement made minutes ago.
	CHECK(d.samples() == 0);
}

// --- Part B -----------------------------------------------------------------
void part_b()
{
	std::printf("Part B: a calm link measures zero\n");

	dejitter_buffer d;
	d.configure(true, period_90);

	CHECK(feed(d, 0, 600, period_90, calm) == 0);
	CHECK(d.delay_ns() == 0);
	CHECK(d.samples() == dejitter_buffer::window);
}

// --- Part C -----------------------------------------------------------------
void part_c()
{
	std::printf("Part C: clumped arrivals raise the delay\n");

	dejitter_buffer d;
	d.configure(true, period_90);

	const int64_t delay = feed(d, 0, 400, period_90, clumped);

	// The worst frame of a clump is two refresh periods minus its head start late. The
	// delay should be that, near enough — it is a percentile of exactly three distinct
	// values, so there is nothing to average.
	const int64_t worst = 2 * period_90 - 4 * ms;
	CHECK(delay > 0);
	CHECK(delay >= worst - 100'000);
	CHECK(delay <= worst + 100'000);
}

// --- Part D -----------------------------------------------------------------
void part_d()
{
	std::printf("Part D: up in one sample, down over thousands\n");

	dejitter_buffer d;
	d.configure(true, period_90);

	// Fill the window on a calm link first, so the rise below is not the warm-up
	feed(d, 0, 300, period_90, calm);
	CHECK(d.delay_ns() == 0);

	// One clump. Enough of the window has to turn over for the 95th percentile to see it,
	// but once it does the delay must arrive in a single sample rather than ease in — a
	// refresh spent below the delay the link is asking for is a refresh showing the wrong
	// frame.
	int64_t before = d.delay_ns();
	int64_t jumped_at = -1;
	for (int64_t n = 300; n < 340; ++n)
	{
		const int64_t target = target_of(n, period_90);
		const int64_t after = d.sample(target + clumped(n), target);
		if (after > before and jumped_at < 0)
			jumped_at = n;
		before = after;
	}
	CHECK(jumped_at >= 0);

	// Now peg it, then let the link go calm and watch it drain
	const int64_t peak = feed(d, 340, 400, period_90, clumped);
	CHECK(peak > 10 * ms);

	int64_t previous = peak;
	int64_t samples_to_zero = -1;
	for (int64_t n = 740; n < 740 + 20'000; ++n)
	{
		const int64_t target = target_of(n, period_90);
		const int64_t now = d.sample(target + calm(n), target);
		// Monotone the whole way down: a delay that wobbled while draining would be
		// worse than either endpoint, since every step down is a skipped frame.
		CHECK(now <= previous);
		CHECK(now >= 0);
		previous = now;
		if (now == 0 and samples_to_zero < 0)
			samples_to_zero = n - 740;
	}
	CHECK(samples_to_zero > 0);
	// Slow means slow: well over a second of frames, not a handful.
	CHECK(samples_to_zero > 200);
	if (verbose)
		std::printf("    drained in %lld samples\n", (long long)samples_to_zero);
}

// --- Part E -----------------------------------------------------------------
void part_e()
{
	std::printf("Part E: the clamps\n");

	// A link so late that the measurement is hopeless: the delay stops at the fixed ceiling
	// rather than following it. At 90 Hz two refresh periods is 22.2 ms, so the 20 ms
	// ceiling is the binding one.
	{
		dejitter_buffer d;
		d.configure(true, period_90);
		const int64_t delay = feed(d, 0, 400, period_90, [](int64_t) { return 100 * ms; });
		CHECK(delay == dejitter_buffer::max_delay_ns);
	}

	// At 120 Hz two periods is 16.7 ms, and that is the binding one: past it the frame ring
	// simply does not hold anything older to pick.
	{
		dejitter_buffer d;
		d.configure(true, period_120);
		const int64_t delay = feed(d, 0, 400, period_120, [](int64_t) { return 100 * ms; });
		CHECK(delay == 2 * period_120);
		CHECK(delay < dejitter_buffer::max_delay_ns);
	}

	// The ceiling follows the rate, including downwards mid-session — which is what the
	// server's emergency half-rate mode does to it.
	{
		dejitter_buffer d;
		d.configure(true, period_90);
		CHECK(feed(d, 0, 400, period_90, [](int64_t) { return 100 * ms; }) == dejitter_buffer::max_delay_ns);

		d.configure(true, period_120);
		const int64_t delay = feed(d, 400, 1, period_120, [](int64_t) { return 100 * ms; });
		CHECK(delay == 2 * period_120);
	}

	// Never negative, however early the frames are. An early frame is not an invitation to
	// show the next one sooner than it was meant to be shown.
	{
		dejitter_buffer d;
		d.configure(true, period_90);
		CHECK(feed(d, 0, 400, period_90, [](int64_t) { return -50 * ms; }) == 0);
	}
}

// --- Part F -----------------------------------------------------------------
void part_f()
{
	std::printf("Part F: warm-up and reset\n");

	// Under min_samples the window is not evidence: the pipeline is still filling and the
	// first frames of a session are late for reasons that have nothing to do with the link.
	{
		dejitter_buffer d;
		d.configure(true, period_90);
		for (size_t i = 0; i < dejitter_buffer::min_samples - 1; ++i)
		{
			const int64_t target = target_of(int64_t(i), period_90);
			CHECK(d.sample(target + 100 * ms, target) == 0);
		}
		// One more and it has seen enough
		const int64_t target = target_of(int64_t(dejitter_buffer::min_samples) - 1, period_90);
		CHECK(d.sample(target + 100 * ms, target) > 0);
	}

	// A reconnect throws the window away. The frames that were arriving as the connection
	// died are all wildly late and say nothing about the link on the other side of the gap;
	// keeping them would peg the delay at its ceiling for the first seconds of the resumed
	// stream.
	{
		dejitter_buffer d;
		d.configure(true, period_90);
		CHECK(feed(d, 0, 400, period_90, [](int64_t) { return 100 * ms; }) > 0);

		d.reset();
		CHECK(d.delay_ns() == 0);
		CHECK(d.samples() == 0);

		// And it stays at zero through a fresh warm-up on a calm link
		CHECK(feed(d, 400, 300, period_90, calm) == 0);
	}
}

// --- Part G -----------------------------------------------------------------
// What the delay is actually for. This is scenes::stream::common_frame's single-stream path,
// reproduced: a ring of image_buffer_size decoded frames, and the frame whose display time is
// nearest the target. The only thing the feature changes there is that the target is the
// vsync minus the delay rather than the vsync itself.
constexpr size_t image_buffer_size = 3;

struct frame_ring
{
	// -1 for a slot nothing has landed in yet
	std::array<int64_t, image_buffer_size> frames{-1, -1, -1};

	void push(int64_t n)
	{
		frames[size_t(n) % image_buffer_size] = n;
	}

	// argmin |display_time(f) - target|, exactly as the min_element in common_frame
	int64_t pick(int64_t target, int64_t period) const
	{
		int64_t best = -1;
		int64_t best_distance = 0;
		for (int64_t f: frames)
		{
			if (f < 0)
				continue;
			const int64_t distance = std::abs(target_of(f, period) - target);
			if (best < 0 or distance < best_distance)
			{
				best = f;
				best_distance = distance;
			}
		}
		return best;
	}
};

// Run the clumped link through the ring and the selection, at a fixed delay, and return which
// frame each refresh displayed.
std::vector<int64_t> displayed(int64_t delay, int64_t frames)
{
	frame_ring ring;
	std::vector<int64_t> out;

	// Refresh k happens at the display time frame k was stamped with. Frames arrive as
	// `clumped` says; everything that has arrived by then is in the ring.
	for (int64_t k = 0; k < frames; ++k)
	{
		const int64_t now = target_of(k, period_90);
		for (int64_t n = 0; n < frames; ++n)
		{
			if (target_of(n, period_90) + clumped(n) <= now)
				ring.push(n);
		}
		out.push_back(ring.pick(now - delay, period_90));
	}
	return out;
}

void part_g()
{
	std::printf("Part G: the selection, with and without the delay\n");

	// What the delay settles at on this link
	dejitter_buffer d;
	d.configure(true, period_90);
	const int64_t delay = feed(d, 0, 400, period_90, clumped);

	// Without it: the picture stalls for two refreshes and then jumps three frames. Nothing
	// was lost — every frame arrived — and two of every three are thrown away unseen.
	{
		const auto shown = displayed(0, 60);
		int repeats = 0;
		int jumps = 0;
		for (size_t i = 21; i < shown.size(); ++i)
		{
			if (shown[i] == shown[i - 1])
				++repeats;
			if (shown[i] - shown[i - 1] > 1)
				++jumps;
		}
		CHECK(repeats > 0);
		CHECK(jumps > 0);
		if (verbose)
			std::printf("    delay 0: %d repeats, %d jumps\n", repeats, jumps);
	}

	// With it: one frame per refresh, in order, none repeated and none skipped. The steady
	// state only starts once the ring has filled and the first frames are past, hence the
	// offset — the opening of a stream is not what this is about.
	{
		const auto shown = displayed(delay, 60);
		int advanced = 0;
		for (size_t i = 21; i < shown.size(); ++i)
		{
			CHECK(shown[i] - shown[i - 1] == 1);
			++advanced;
		}
		CHECK(advanced > 30);
		if (verbose)
			std::printf("    delay %lld ns: %d refreshes, all advancing by one\n",
			            (long long)delay,
			            advanced);
	}
}

} // namespace

int main(int argc, char ** argv)
{
	verbose = argc > 1 and std::string_view(argv[1]) == "-v";

	part_a();
	part_b();
	part_c();
	part_d();
	part_e();
	part_f();
	part_g();

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
