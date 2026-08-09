// Hardware encoder failover: the decision core, driven on a virtual clock with no
// GPU, no driver and no compositor.
//
// wivrn::encoder_watchdog is the whole policy. Everything around it (building the
// x264 encoder, swapping it into the slot, forcing the keyframe) is mechanical; what
// is worth pinning down is *when* a stream is written off, because both mistakes are
// expensive: too eager and a working session is dropped onto the CPU encoder over a
// hiccup, too slow and the user stares at a frozen eye.
//
// Part A: a hard error is not negotiable — one is enough, at once, with no strikes.
// Part B: silence earns a strike per stall window, and only the third one acts.
// Part C: a single picture wipes the strikes; a stream that hiccups forever without
//         ever going quiet for a whole window is never written off.
// Part D: a call that never returns strikes on the clock alone, which is the mode
//         the encoder thread cannot report on because it is the one stuck in it.
// Part E: frames that never reached the encoder (the IDR handler skipped them, or
//         the stream is silent by design) are not evidence of anything.
// Part F: the software encoder has nothing to fall to and is never written off.
// Part G: both switches off means no action at all, and a verdict is reported once.
//
// Build:
//   g++ -std=c++23 -I server -o encoder_failover_test tests/encoder_failover_test.cpp
//   ./encoder_failover_test

#include "encoder/encoder_watchdog.h"

#include <cstdio>
#include <optional>

using wivrn::encoder_watchdog;

static int failures = 0;
static int checks = 0;

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
constexpr int64_t stall = 500 * ms;
// 90 Hz
constexpr int64_t frame = 11 * ms;

// Wire a watchdog the way video_encoder wires a hardware encoder's: eligible (there
// is a software encoder below it) and switched on. Not a factory returning one — it
// holds a mutex, so it neither copies nor moves.
void as_hardware(encoder_watchdog & w)
{
	w.set_eligible(true);
	w.set_enabled(true);
}

// One frame through a working encoder
void good_frame(encoder_watchdog & w, int64_t & now)
{
	w.encode_begin(now);
	now += 2 * ms;
	w.encode_end(now, true);
	now += frame - 2 * ms;
}

// One frame that went in and produced nothing
void silent_frame(encoder_watchdog & w, int64_t & now)
{
	w.encode_begin(now);
	now += 2 * ms;
	w.encode_end(now, false);
	now += frame - 2 * ms;
}

// --- Part A ----------------------------------------------------------------------

void test_hard_error_is_immediate()
{
	std::printf("A. hard error\n");

	encoder_watchdog w;
	as_hardware(w);
	int64_t now = 1'000 * ms;

	for (int i = 0; i < 100; ++i)
		good_frame(w, now);

	CHECK(not w.poll(now).has_value());
	CHECK(w.strikes() == 0);

	w.encode_error(now, "nvenc error");

	// No clock movement, no strikes: the next look at it is already a verdict.
	auto verdict = w.poll(now);
	CHECK(verdict.has_value());
	CHECK(w.strikes() == 0);
	if (verdict)
		CHECK(verdict->reason.find("nvenc error") != std::string::npos);

	// And it is reported exactly once, however often it is asked afterwards
	CHECK(not w.poll(now).has_value());
	CHECK(not w.poll(now + 10'000 * ms).has_value());
}

// --- Part B ----------------------------------------------------------------------

void test_three_stalls()
{
	std::printf("B. three silent windows\n");

	encoder_watchdog w;
	as_hardware(w);
	int64_t now = 0;

	good_frame(w, now);

	std::optional<encoder_watchdog::decision> verdict;
	int strikes_seen = 0;

	// Frames keep being submitted at 90 Hz and nothing ever comes out. Poll once
	// per frame, the way the present path does.
	for (int i = 0; i < 200 and not verdict; ++i)
	{
		silent_frame(w, now);
		verdict = w.poll(now);
		if (w.strikes() > strikes_seen)
		{
			strikes_seen = w.strikes();
			// Strikes land one stall window apart, not sooner
			CHECK(now >= stall * strikes_seen);
		}
	}

	CHECK(verdict.has_value());
	CHECK(strikes_seen == 3);
	// Three windows of half a second, plus at most one frame of rounding
	CHECK(now >= 3 * stall);
	CHECK(now < 3 * stall + 4 * frame);
	if (verdict)
		CHECK(verdict->reason.find("no picture") != std::string::npos);
}

void test_two_stalls_are_not_enough()
{
	std::printf("B'. two silent windows are survivable\n");

	encoder_watchdog w;
	as_hardware(w);
	int64_t now = 0;
	good_frame(w, now);

	// Just under three windows
	while (now < 3 * stall - 10 * ms)
	{
		silent_frame(w, now);
		CHECK(not w.poll(now).has_value());
	}
	CHECK(w.strikes() == 2);
}

// --- Part C ----------------------------------------------------------------------

void test_output_resets_strikes()
{
	std::printf("C. healthy traffic resets the count\n");

	encoder_watchdog w;
	as_hardware(w);
	int64_t now = 0;
	good_frame(w, now);

	// Two windows of silence, then the encoder comes back
	while (w.strikes() < 2)
	{
		silent_frame(w, now);
		CHECK(not w.poll(now).has_value());
	}
	CHECK(w.strikes() == 2);

	good_frame(w, now);
	CHECK(w.strikes() == 0);

	// An encoder that drops most frames but never goes quiet for a whole window is
	// a slow encoder, not a dead one, and must never be written off. Ten seconds of
	// it, which is longer than any real hiccup.
	const int64_t begin = now;
	while (now - begin < 10'000 * ms)
	{
		for (int i = 0; i < 20; ++i)
		{
			silent_frame(w, now);
			CHECK(not w.poll(now).has_value());
		}
		good_frame(w, now);
		CHECK(not w.poll(now).has_value());
	}
	CHECK(w.strikes() == 0);
}

// --- Part D ----------------------------------------------------------------------

void test_wedged_call()
{
	std::printf("D. a call that never returns\n");

	encoder_watchdog w;
	as_hardware(w);
	int64_t now = 0;
	good_frame(w, now);

	// The encoder thread went into the driver and has not come out. Nothing else
	// will ever be submitted on this stream: encode_end is never called, and the
	// only thing that moves is the clock the present thread reads.
	w.encode_begin(now);

	std::optional<encoder_watchdog::decision> verdict;
	for (int i = 0; i < 500 and not verdict; ++i)
	{
		now += frame;
		verdict = w.poll(now);
	}

	// Measured from the last picture, which is what silence means
	CHECK(verdict.has_value());
	CHECK(now >= 3 * stall);
	CHECK(now < 3 * stall + 4 * frame);
	if (verdict)
		CHECK(verdict->reason.find("not returned") != std::string::npos);
}

// --- Part E ----------------------------------------------------------------------

void test_frames_that_never_reached_the_encoder()
{
	std::printf("E. skipped frames say nothing\n");

	encoder_watchdog w;
	as_hardware(w);
	int64_t now = 0;
	good_frame(w, now);

	// The IDR handler is waiting for feedback and skips every frame, or this is the
	// quad stream with nothing promoted for a minute: video_encoder returns before
	// the backend is ever called, so the watchdog sees nothing at all.
	const int64_t begin = now;
	while (now - begin < 60'000 * ms)
	{
		now += frame;
		CHECK(not w.poll(now).has_value());
	}
	CHECK(w.strikes() == 0);

	// ... and the stream is still perfectly able to notice a real failure afterwards
	while (not w.poll(now))
	{
		silent_frame(w, now);
		if (now - begin > 120'000 * ms)
			break;
	}
	CHECK(w.strikes() == 3);
}

// --- Part F ----------------------------------------------------------------------

void test_software_encoder_never_fails_over()
{
	std::printf("F. the software encoder is the floor\n");

	// video_encoder marks x264 (and the raw passthrough) ineligible: there is
	// nothing below them to fall to.
	encoder_watchdog w;
	w.set_eligible(false);
	w.set_enabled(true);

	int64_t now = 0;
	good_frame(w, now);

	// Silence for ten seconds
	const int64_t begin = now;
	while (now - begin < 10'000 * ms)
	{
		silent_frame(w, now);
		CHECK(not w.poll(now).has_value());
	}

	// A wedged call
	w.encode_begin(now);
	now += 10'000 * ms;
	CHECK(not w.poll(now).has_value());

	// And a hard error, which for a hardware encoder would be instant
	w.encode_error(now, "x264_encoder_encode failed");
	CHECK(not w.poll(now).has_value());
	CHECK(w.strikes() == 0);

	// An encoder that already failed over is ineligible for the same reason
	encoder_watchdog swapped;
	as_hardware(swapped);
	swapped.encode_error(0, "boom");
	CHECK(swapped.poll(0).has_value());
	CHECK(not swapped.eligible());
	swapped.encode_error(ms, "boom again");
	CHECK(not swapped.poll(ms).has_value());
}

// --- Part G ----------------------------------------------------------------------

void test_switch()
{
	std::printf("G. the toggle\n");

	// Either switch off (the server key or the headset toggle; wivrn_session ANDs
	// them) and the watchdog takes no decision at all.
	encoder_watchdog w;
	w.set_eligible(true);
	w.set_enabled(false);

	int64_t now = 0;
	good_frame(w, now);

	const int64_t begin = now;
	while (now - begin < 10'000 * ms)
	{
		silent_frame(w, now);
		CHECK(not w.poll(now).has_value());
	}
	CHECK(w.strikes() == 0);

	w.encode_error(now, "nvenc error");
	CHECK(not w.poll(now).has_value());

	// Turned back on mid-session, it starts from a clean slate rather than acting
	// on the failures it declined to count while it was off.
	w.set_enabled(true);
	CHECK(not w.poll(now).has_value());
	CHECK(w.strikes() == 0);

	// ... and works from there
	std::optional<encoder_watchdog::decision> verdict;
	for (int i = 0; i < 500 and not verdict; ++i)
	{
		silent_frame(w, now);
		verdict = w.poll(now);
	}
	CHECK(verdict.has_value());
}

// A custom configuration is honoured (the compositor uses the defaults, but the
// numbers are the part most likely to be tuned later).
void test_config()
{
	std::printf("G'. configured thresholds\n");

	encoder_watchdog w{{.stall_ns = 100 * ms, .strikes = 1}};
	w.set_eligible(true);
	w.set_enabled(true);

	int64_t now = 0;
	good_frame(w, now);

	std::optional<encoder_watchdog::decision> verdict;
	for (int i = 0; i < 100 and not verdict; ++i)
	{
		silent_frame(w, now);
		verdict = w.poll(now);
	}
	CHECK(verdict.has_value());
	CHECK(now >= 100 * ms);
	CHECK(now < 100 * ms + 4 * frame);
}

} // namespace

int main()
{
	std::printf("encoder failover\n");

	test_hard_error_is_immediate();
	test_three_stalls();
	test_two_stalls_are_not_enough();
	test_output_resets_strikes();
	test_wedged_call();
	test_frames_that_never_reached_the_encoder();
	test_software_encoder_never_fails_over();
	test_switch();
	test_config();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
