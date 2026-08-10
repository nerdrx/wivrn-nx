// Radio-aware bitrate: the policy that turns the headset's Wi-Fi reports into a preemptive
// bitrate decrease, driven on a virtual clock with synthetic feedback and RSSI ramps.
//
// The whole point of the feature is that it acts *before* the frame timings notice anything,
// so every part below feeds a link that is measuring perfectly healthy (utilisation 0.3, no
// lost frame, no late frame) and only varies the radio.
//
// Part A: a flat signal, and a noisy one, move nothing — a bitrate that walks down while the
// user stands still would be worse than no feature at all.
// Part B: a falling ramp fires one gentle decrease, with no loss anywhere in the window, and
// respects its own cooldown afterwards.
// Part C: the trend never raises the bitrate on its own, and the hold it leaves behind blocks
// the normal probing while the signal is still falling, until the signal recovers.
// Part D: the deep-drop recovery owns the bitrate while it runs; the radio keeps quiet.
// Part E: stale data (the headset stopped reporting) is ignored entirely and releases the hold.
// Part F: sentinels and implausible readings are rejected instead of being fed to the trend.
// Part G: the PHY rate collapse trigger, and the headroom it needs before it counts.
// Part H: both switches gate the whole thing.
// Part I: a signal that stops falling and settles at a lower level releases the hold, so the
// bitrate is not stuck below the ceiling forever after the user walks to a new spot.
//
// Build:
//   g++ -std=c++23 -I server -I common -I build-server/common \
//       -I build-server/_deps/monado-src/src/xrt/include \
//       -I build-server/_deps/monado-src/src/xrt/auxiliary \
//       -I build-server/_deps/monado-src/src/external/openxr_includes \
//       -isystem external -isystem build-server/_deps/boost-src/libs/pfr/include \
//       -o bitrate_radio_test tests/bitrate_radio_test.cpp \
//       server/driver/bitrate_controller.cpp common/smp.cpp -lcrypto
//   ./bitrate_radio_test

#include "driver/bitrate_controller.h"
#include "util/u_logging.h"

#include <cstdarg>
#include <cstdio>
#include <optional>
#include <string>

using wivrn::bitrate_controller;
using tp = bitrate_controller::clock::time_point;
using namespace std::chrono_literals;

namespace
{
int failures = 0;
int checks = 0;
bool verbose = false;

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

// 90 fps
constexpr int64_t period = 11'111'111;
constexpr uint32_t ceiling = 50'000'000;

// Drives one controller on a virtual clock. Frames are fed at the display rate, radio
// reports at 1 Hz, and every bitrate the controller hands back is remembered so a test can
// tell "nothing happened" from "something happened and came back".
struct harness
{
	bitrate_controller ctl;
	tp now;
	uint64_t frame = 0;
	// Every change the controller asked for, in order
	std::vector<std::pair<tp, uint32_t>> changes;
	// Changes that came out of a radio report rather than out of the frame timings
	std::vector<std::pair<tp, uint32_t>> radio_changes;

	harness(bool enabled = true, bool client_enabled = true, bool radio_aware = true)
	{
		// Not the epoch: an unset "last decrease" is the epoch, and starting there
		// would put every cooldown inside its own window.
		now = tp{} + 1h;
		ctl.configure({.enabled = enabled}, ceiling, client_enabled, radio_aware);
	}

	void advance(std::chrono::nanoseconds d)
	{
		now += d;
	}

	// One feedback packet for the next frame, on stream 0.
	void feed_frame(double utilisation, bool lost = false, bool late = false)
	{
		wivrn::from_headset::feedback fb{};
		fb.frame_index = frame++;
		fb.stream_index = 0;

		const XrTime base = 1'000'000'000 + XrTime(fb.frame_index) * period;
		fb.received_first_packet = base;

		if (not lost)
		{
			fb.received_last_packet = base + XrTime(utilisation * double(period));
			fb.sent_to_decoder = base + period;
			fb.received_from_decoder = base + 2 * period;
			if (not late)
			{
				fb.blitted = base + 3 * period;
				fb.times_displayed = 1;
			}
		}

		if (auto b = ctl.on_feedback(fb, period, true, now))
			changes.emplace_back(now, *b);

		advance(std::chrono::nanoseconds(period));
	}

	// A healthy stretch: n frames at a comfortable utilisation, nothing lost, nothing late.
	void feed_healthy(size_t n, double utilisation = 0.3)
	{
		for (size_t i = 0; i < n; ++i)
			feed_frame(utilisation);
	}

	void feed_radio(int rssi, int link_speed = 866)
	{
		if (auto b = ctl.on_wifi_state(rssi, link_speed, now))
		{
			changes.emplace_back(now, *b);
			radio_changes.emplace_back(now, *b);
			if (verbose)
				std::printf("    radio step at %d dBm -> %.1f Mbit/s\n", rssi, *b * 1e-6);
		}
	}

	// One second of healthy frames with one radio report at the start of it.
	void second(int rssi, int link_speed = 866)
	{
		feed_radio(rssi, link_speed);
		feed_healthy(90);
	}

	uint32_t current() const
	{
		return ctl.current();
	}
};

void part_a()
{
	std::printf("Part A: a signal that is not going anywhere moves nothing\n");

	// --- Dead flat, well below the low threshold: still not a degradation -----------
	{
		harness h;
		for (int i = 0; i < 30; ++i)
			h.second(-70);

		CHECK(h.radio_changes.empty());
		CHECK(h.current() == ceiling);
	}

	// --- The +-3 dB the radio reports while standing still --------------------------
	{
		harness h;
		// A deterministic wobble around -70, amplitude 3 dB, period 5 s: over any
		// 4 s window it looks like a slope, which is exactly what must not fire.
		static constexpr int wobble[] = {0, 3, 1, -3, -1, 2, -2, 3, -3, 0};
		for (int i = 0; i < 40; ++i)
			h.second(-70 + wobble[i % 10]);

		CHECK(h.radio_changes.empty());
		CHECK(h.current() == ceiling);
	}

	// --- A real fall, but with plenty of margin left --------------------------------
	{
		harness h;
		// -40 down to -58: 18 dB, far more than radio_fall_db, but never below
		// radio_low_rssi_dbm, so there is nothing to preempt yet.
		for (int i = 0; i < 20; ++i)
			h.second(-40 - i);

		CHECK(h.radio_changes.empty());
		CHECK(h.current() == ceiling);
	}
}

void part_b()
{
	std::printf("Part B: walking away from the access point\n");

	harness h;

	// Settle first, so that "nothing was wrong with the link" is on the record.
	for (int i = 0; i < 5; ++i)
		h.second(-52);
	CHECK(h.changes.empty());

	// 2 dB/s, which is a normal walking pace away from a 5 GHz access point.
	int rssi = -52;
	for (int i = 0; i < 15 and h.radio_changes.empty(); ++i)
	{
		rssi -= 2;
		h.second(rssi);
	}

	CHECK(h.radio_changes.size() == 1);
	if (h.radio_changes.empty())
		return;

	const uint32_t stepped = h.radio_changes.front().second;

	// The gentle decrease, not the deep drop.
	CHECK(stepped == uint32_t(ceiling * bitrate_controller::decrease_factor));
	CHECK(h.current() == stepped);
	// Every change so far came from the radio: the frame timings never complained.
	CHECK(h.changes.size() == 1);
	// And it happened while the signal was still usable, i.e. ahead of the loss.
	CHECK(rssi > -80);
	if (verbose)
		std::printf("    fired at %d dBm, %.1f -> %.1f Mbit/s\n", rssi, ceiling * 1e-6, stepped * 1e-6);

	// --- The step has a cooldown of its own -----------------------------------------
	// The signal keeps falling; the next step must wait radio_step_interval, so at most
	// one more in the four seconds that follow.
	const size_t before = h.radio_changes.size();
	for (int i = 0; i < 3; ++i)
	{
		rssi -= 2;
		h.second(rssi);
	}
	CHECK(h.radio_changes.size() == before);
}

// Ramp the signal down until the first preemptive step fires, and hand back the RSSI it
// fired at (the harness is then one step down, the signal sitting at that level).
int step_in(harness & h)
{
	int rssi = -52;
	for (int i = 0; i < 20 and h.radio_changes.empty(); ++i)
	{
		rssi -= 2;
		h.second(rssi);
	}
	CHECK(h.radio_changes.size() == 1);
	return rssi;
}

void part_c()
{
	std::printf("Part C: the trend only ever pushes down, and its hold\n");

	// --- The hold blocks the probing back up while the signal is still falling -------
	// The link measures perfect, but the radio says the signal is still on its way
	// down, so probing up now would only walk straight back into the degradation. The
	// bitrate may be pushed further down; it must never rise while the fall continues.
	{
		harness h;
		int rssi = step_in(h);
		const uint32_t stepped = h.current();
		CHECK(stepped < ceiling);

		for (int i = 0; i < 12; ++i)
		{
			rssi -= 2;
			h.second(rssi);
		}

		CHECK(h.current() <= stepped);
	}

	// --- A recovering signal releases it --------------------------------------------
	{
		harness h;
		int rssi = step_in(h);
		const uint32_t stepped = h.current();

		for (int i = 0; i < 10; ++i)
		{
			rssi += 3;
			h.second(rssi);
		}
		// Now the healthy-increase path is allowed again, and only that path: whatever
		// the radio does, it never returns a bitrate of its own on the way up.
		CHECK(h.radio_changes.size() == 1);

		for (int i = 0; i < 10; ++i)
			h.second(-50);

		CHECK(h.current() > stepped);
		CHECK(h.changes.size() > 1);
		CHECK(h.radio_changes.size() == 1);
	}
}

void part_i()
{
	std::printf("Part I: a signal that stabilises lower releases the hold\n");

	harness h;
	const int rssi = step_in(h);
	const uint32_t stepped = h.current();
	CHECK(stepped < ceiling);

	// The user stops walking and settles: the signal holds flat at this new, lower
	// level, reports keep coming and the link stays healthy. There is no rise back up,
	// so the rise-based release of Part C never fires — but the fall has *stopped*, and
	// the hold must not outlive it. Once the slope has read flat for radio_stable_hold
	// the hold lifts and the normal healthy probing walks the bitrate back up. Without
	// the stabilisation release this latched forever and the bitrate stayed stuck low.
	for (int i = 0; i < 30; ++i)
		h.second(rssi);

	CHECK(h.current() > stepped);
	// And it came back through the healthy probe, never the radio: the radio never
	// returns a higher bitrate of its own.
	CHECK(h.radio_changes.size() == 1);
}

void part_d()
{
	std::printf("Part D: the deep-drop recovery is left alone\n");

	harness h;

	// An acute spike: enough frames that never arrived to be severe.
	for (int i = 0; i < 60; ++i)
		h.feed_frame(0.5, /* lost = */ i % 8 == 0);

	CHECK(not h.changes.empty());
	const uint32_t dropped = h.current();
	// Deep drop, not the gentle one.
	CHECK(dropped <= uint32_t(ceiling * bitrate_controller::deep_decrease_factor));

	// Now the signal collapses as well. The controller is rebounding and the radio must
	// not touch it: a preemptive guess on top of the rebound is exactly the fight the
	// two-regime design exists to avoid.
	int rssi = -50;
	for (int i = 0; i < 15; ++i)
	{
		rssi -= 3;
		h.feed_radio(rssi);
		h.advance(1s);
	}

	CHECK(h.radio_changes.empty());
	CHECK(h.current() == dropped);
}

void part_e()
{
	std::printf("Part E: stale reports do nothing\n");

	// --- A gap in the middle of a ramp restarts the trend ----------------------------
	{
		harness h;

		// Three quarters of a falling ramp...
		int rssi = -60;
		for (int i = 0; i < 3; ++i)
		{
			rssi -= 3;
			h.second(rssi);
		}

		// ... then the headset goes quiet for longer than radio_max_age, and comes
		// back with the value it would have had. The samples across the gap say
		// nothing about each other, so the trend starts over and nothing fires.
		h.advance(10s);
		h.feed_healthy(900);

		rssi -= 30;
		h.feed_radio(rssi);
		h.feed_healthy(90);

		CHECK(h.radio_changes.empty());
		CHECK(h.current() == ceiling);
	}

	// --- A hold does not outlive the reports that justified it ----------------------
	{
		harness h;

		int rssi = -52;
		for (int i = 0; i < 20 and h.radio_changes.empty(); ++i)
		{
			rssi -= 2;
			h.second(rssi);
		}
		CHECK(h.radio_changes.size() == 1);
		const uint32_t stepped = h.current();

		// The headset stops reporting entirely (an older client, a failed read, the
		// switch turned off). The frame timings are healthy, so after radio_max_age
		// plus increase_hold the normal probing must be running again.
		for (int i = 0; i < 20; ++i)
			h.feed_healthy(90);

		CHECK(h.current() > stepped);
	}
}

void part_f()
{
	std::printf("Part F: sentinels are not measurements\n");

	harness h;

	// Android's "I will not tell you", plus the values a vendor placeholder produces.
	// A ramp of these would look like a catastrophic fall if it were believed.
	for (int i = 0; i < 20; ++i)
	{
		h.feed_radio(-127);
		h.feed_radio(0);
		h.feed_radio(-200);
		h.feed_radio(42);
		h.feed_healthy(90);
	}

	CHECK(h.radio_changes.empty());
	CHECK(h.current() == ceiling);

	// And they do not pollute a real trend that follows: mixing them into a flat signal
	// must still leave it flat.
	for (int i = 0; i < 20; ++i)
	{
		h.feed_radio(-127);
		h.second(-70);
	}

	CHECK(h.radio_changes.empty());
}

void part_g()
{
	std::printf("Part G: the PHY rate collapsing under the stream\n");

	// --- Rate adaptation gives up half the link, and what is left is too little -----
	{
		harness h;

		// Flat signal, so the RSSI trigger cannot be what fires.
		for (int i = 0; i < 6; ++i)
			h.second(-70, 866);

		// 866 -> 65 Mbit/s of PHY rate, against 50 Mbit/s of video: less than the
		// two times headroom a nominal PHY rate needs.
		for (int i = 0; i < 8 and h.radio_changes.empty(); ++i)
			h.second(-70, 65);

		CHECK(h.radio_changes.size() == 1);
		CHECK(h.current() == uint32_t(ceiling * bitrate_controller::decrease_factor));
	}

	// --- The same collapse, but with room to spare ----------------------------------
	{
		harness h;

		for (int i = 0; i < 6; ++i)
			h.second(-70, 866);

		// Halved as well, but 300 Mbit/s against 50 Mbit/s of video is six times the
		// headroom: the radio is fine, it just is not at its top rate any more.
		for (int i = 0; i < 12; ++i)
			h.second(-70, 300);

		CHECK(h.radio_changes.empty());
		CHECK(h.current() == ceiling);
	}

	// --- An unknown PHY rate is not a collapsed one ---------------------------------
	{
		harness h;

		for (int i = 0; i < 6; ++i)
			h.second(-70, 866);
		for (int i = 0; i < 12; ++i)
			h.second(-70, 0);

		CHECK(h.radio_changes.empty());
	}
}

void part_h()
{
	std::printf("Part H: both switches gate it\n");

	auto ramp = [](harness & h) {
		int rssi = -52;
		for (int i = 0; i < 25; ++i)
		{
			rssi -= 2;
			h.second(rssi);
		}
	};

	// --- Radio trend off on the headset ---------------------------------------------
	{
		harness h(true, true, /* radio_aware = */ false);
		ramp(h);
		CHECK(h.radio_changes.empty());
		CHECK(h.current() == ceiling);
	}

	// --- Automatic bitrate off on the headset ---------------------------------------
	{
		harness h(true, /* client_enabled = */ false, true);
		ramp(h);
		CHECK(h.radio_changes.empty());
		CHECK(h.current() == ceiling);
	}

	// --- Automatic bitrate off on the server ----------------------------------------
	{
		harness h(/* enabled = */ false, true, true);
		ramp(h);
		CHECK(h.radio_changes.empty());
		CHECK(h.current() == ceiling);
	}

	// --- Turned off half way through: the hold goes with it -------------------------
	{
		harness h;

		int rssi = -52;
		for (int i = 0; i < 20 and h.radio_changes.empty(); ++i)
		{
			rssi -= 2;
			h.second(rssi);
		}
		CHECK(h.radio_changes.size() == 1);
		const uint32_t stepped = h.current();

		h.ctl.set_radio_aware(false);

		// Nothing more from the radio, and the normal probing is free again.
		for (int i = 0; i < 20; ++i)
			h.second(rssi);

		CHECK(h.radio_changes.size() == 1);
		CHECK(h.current() > stepped);
	}
}
} // namespace

// The controller logs through Monado's u_log; standing in for it is all it takes to run the
// policy on its own, with no server, no encoder and no headset.
extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char * format, ...)
{
	if (not verbose)
		return;

	std::printf("    [log] ");
	va_list args;
	va_start(args, format);
	std::vprintf(format, args);
	va_end(args);
	std::printf("\n");
}

extern "C" enum u_logging_level u_log_get_global_level(void)
{
	return U_LOGGING_INFO;
}

int main(int argc, char ** argv)
{
	verbose = argc > 1 and std::string(argv[1]) == "-v";

	part_a();
	part_b();
	part_c();
	part_d();
	part_e();
	part_f();
	part_g();
	part_h();
	part_i();

	std::printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
