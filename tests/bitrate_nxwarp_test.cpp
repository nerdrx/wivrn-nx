// The automatic bitrate against the feedback the NX Warp decoder actually produces.
//
// This is a regression test for a bug that had nothing to do with the control law and
// everything to do with what was fed into it. The NX Warp client decoder used to number
// its from_headset::feedback with a client-local counter that advanced once per frame the
// decoder managed to reassemble:
//
//     job.fb.frame_index = ++wivrn_frame_idx;
//
// which is not a frame index at all. It is a count of survivors, and the two eyes do not
// have the same survivors: the instant one eye loses a frame the other did not, its
// counter falls one behind and every later frame carries a different number on the two
// streams. Nothing about the picture looks wrong, and two things downstream break:
//
//   * the server's bitrate controller joins the per-stream feedback of one frame by that
//     number. With the eyes off by one it joins two DIFFERENT frames into one ring entry
//     and computes that "frame"'s time on the wire as the span from the first arrival of
//     one to the last arrival of the other -- about a frame period, on a link with
//     nothing wrong with it. Utilisation reads at or above 1, which is the controller's
//     definition of acute congestion, so it deep-drops, and it keeps deep-dropping,
//     because the desync is permanent. It ends pinned at the bitrate floor and never
//     probes back up.
//   * the render thread pairs the eyes by the same number, so it stops finding a common
//     frame, so frames are evicted from the display ring without ever being shown --
//     which the controller counts as late frames, which is the same verdict again by a
//     second route.
//
// And the second half of the same story: a frame that never arrived produced no
// from_headset::feedback at all on this path (close_frame returned early on a hole), so
// the one signal that should make the controller back off was invisible to it.
//
// Part A is the bug: the old numbering, a clean link, a rare uneven hole -> the floor.
// Part B is the fix: the wire frame id, the same clean link -> the ceiling is held.
// Part C: loss is now visible, backs the bitrate off, and the link recovers afterwards.
// Part D: an even hole (both eyes lose the same frame) was never enough to desync, so it
//         has to still be harmless -- the fix must not have bought B by making loss inert.
//
// Build:
//   g++ -std=c++23 -I server -I common -I build-server/common \
//       -I build-server/_deps/monado-src/src/xrt/include \
//       -I build-server/_deps/monado-src/src/xrt/auxiliary \
//       -I build-server/_deps/monado-src/src/external/openxr_includes \
//       -isystem external -isystem build-server/_deps/boost-src/libs/pfr/include \
//       -o bitrate_nxwarp_test tests/bitrate_nxwarp_test.cpp \
//       server/driver/bitrate_controller.cpp common/smp.cpp -lcrypto
//   ./bitrate_nxwarp_test

#include "driver/bitrate_controller.h"
#include "util/u_logging.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <optional>
#include <vector>

using wivrn::bitrate_controller;
using mode = bitrate_controller::mode;
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

constexpr int64_t period = 11'111'111; // 90 Hz
constexpr uint32_t ceiling = 50'000'000;
constexpr uint32_t floor_bps = 10'000'000;
// Two eye streams, which is what NX Warp runs; the desync needs two to exist at all.
constexpr uint8_t eyes = 2;

double mbit(uint64_t bps)
{
	return double(bps) * 1e-6;
}

// How a client numbers the feedback it sends.
enum class numbering
{
	// What the NX Warp decoder used to do: one counter per decoder instance, advanced
	// once per frame that survived reassembly on THAT stream.
	survivor_counter,
	// What it does now, and what every other decoder in the client always did: the
	// sender's own frame id, which is the same number on both eyes for one frame and
	// does not care whether this eye kept the frame.
	wire_frame_id,
};

// One server-side controller driven by two simulated eye decoders on a virtual clock.
struct harness
{
	bitrate_controller ctl;
	tp now;
	uint64_t wire_frame = 0;
	numbering how;
	// Whether the client reports a frame it could not reassemble at all. Independent of
	// the numbering on purpose: the two changes have to be separable, or a test that
	// moved both at once could not say which one mattered.
	bool report_losses = true;
	// The survivor counters, one per eye, as the old client kept them.
	uint64_t survivors[eyes] = {0, 0};

	std::vector<std::pair<tp, uint32_t>> changes;

	harness(numbering how, bool report_losses = true) :
	        how(how), report_losses(report_losses)
	{
		now = tp{} + 1h;
		ctl.configure({.enabled = true, .min_bitrate_bps = floor_bps}, ceiling, true, false, mode::aimd);
	}

	uint32_t current() const
	{
		return ctl.current();
	}

	void feed(const wivrn::from_headset::feedback & fb)
	{
		if (auto b = ctl.on_feedback(fb, period, true, now))
			changes.emplace_back(now, *b);
	}

	// One frame, as both eyes report it.
	//
	// `hole` names the eye whose reassembly failed for this frame, or -1 for a frame
	// both eyes got. A clean *link* still produces the occasional one of these: NX Warp
	// carries a frame as a run of chunks with no per-chunk retransmission, so a single
	// reordered or dropped datagram costs that eye the frame, and it is not correlated
	// between the two sockets.
	void frame(int hole = -1)
	{
		const XrTime base = 1'000'000'000 + XrTime(wire_frame) * period;
		// A clean link hands a frame over in a fraction of a frame period. This is the
		// number the whole test turns on: it is nowhere near congestion, and the bug is
		// that the controller never got to see it.
		const XrTime wire = XrTime(0.15 * double(period));

		for (uint8_t eye = 0; eye < eyes; ++eye)
		{
			const bool lost = (hole >= 0 and uint8_t(hole) == eye);

			wivrn::from_headset::feedback fb{};
			fb.stream_index = eye;
			fb.received_first_packet = base;

			if (how == numbering::wire_frame_id)
			{
				// The wire id exists whether or not this eye kept the frame.
				fb.frame_index = wire_frame;
			}
			else
			{
				// A survivor counter has no number for a frame that did not
				// survive, so a client using one cannot report the loss even
				// if it wants to -- which is the other half of why the two
				// changes belong together.
				if (lost)
					continue;
				fb.frame_index = survivors[eye]++;
			}

			if (lost)
			{
				// First packet arrived, the frame never completed, so no
				// sent_to_decoder: the same packet shape every other decoder
				// in this client sends for a frame that never arrived.
				if (report_losses)
					feed(fb);
				continue;
			}

			fb.received_last_packet = base + wire;
			fb.sent_to_decoder = base + wire;
			fb.received_from_decoder = base + wire + period;
			fb.blitted = base + wire + 2 * period;
			fb.times_displayed = 1;
			feed(fb);
		}

		++wire_frame;
		now += std::chrono::nanoseconds(period);
	}

	// n seconds of video with a hole every `hole_period` frames, alternating eyes so
	// that the survivor counters drift apart rather than staying in step. 0 = none.
	void seconds(double n, int hole_period = 0)
	{
		const size_t total = size_t(n * 90);
		for (size_t i = 0; i < total; ++i)
			frame(hole_period and (i % size_t(hole_period)) == 0 ? int((i / size_t(hole_period)) % eyes) : -1);
	}

	// n seconds where BOTH eyes lose the same frame: real, correlated loss.
	void seconds_even_loss(double n, int hole_period)
	{
		const size_t total = size_t(n * 90);
		for (size_t i = 0; i < total; ++i)
		{
			if (hole_period and (i % size_t(hole_period)) == 0)
			{
				// Both eyes: emit each eye's loss report for the same frame.
				const XrTime base = 1'000'000'000 + XrTime(wire_frame) * period;
				for (uint8_t eye = 0; eye < eyes; ++eye)
				{
					wivrn::from_headset::feedback fb{};
					fb.stream_index = eye;
					fb.received_first_packet = base;
					fb.frame_index = how == numbering::wire_frame_id ? wire_frame : survivors[eye];
					if (how == numbering::survivor_counter)
						continue; // the old client sent nothing at all
					feed(fb);
				}
				++wire_frame;
				now += std::chrono::nanoseconds(period);
			}
			else
			{
				frame(-1);
			}
		}
	}
};

void part_a()
{
	std::printf("Part A: the survivor counter collapses a clean link to the floor\n");

	// Exactly the old client: a survivor counter, and nothing reported for a frame that
	// did not survive. Ten seconds of a link doing nothing wrong, with one eye or the
	// other losing a frame about twice a second -- enough to desync the counters in the
	// first second, and not, by itself, anything the controller should react to, because
	// the controller is never told about it.
	harness h(numbering::survivor_counter, false);
	h.seconds(10, 45);

	std::printf("  survivor counter, no loss reports: %.1f -> %.1f Mbit/s (%zu changes)\n",
	            mbit(ceiling), mbit(h.current()), h.changes.size());

	// The bug, asserted so it cannot come back quietly. Not one lost frame was reported;
	// the numbering alone took a healthy link to the floor.
	CHECK(h.current() == floor_bps);
}

void part_b()
{
	std::printf("Part B: the wire frame id holds the ceiling on the same link\n");

	// The ONLY thing changed from Part A is the numbering: same holes, same eyes, same
	// silence about the losses, same control law.
	harness h(numbering::wire_frame_id, false);
	h.seconds(10, 45);

	std::printf("  wire frame id, no loss reports:    %.1f -> %.1f Mbit/s (%zu changes)\n",
	            mbit(ceiling), mbit(h.current()), h.changes.size());

	CHECK(h.current() == ceiling);
	CHECK(h.changes.empty());
}

void part_c()
{
	std::printf("Part C: reporting losses does not fire on a link that loses nothing\n");

	// The other change, on its own, on a link with no holes at all. A loss report that
	// fired spuriously would be a new way to collapse the bitrate, so this is the guard
	// on it: the controller must not move.
	harness h(numbering::wire_frame_id, true);
	h.seconds(10);

	std::printf("  loss reports on, no holes:         %.1f Mbit/s (%zu changes)\n",
	            mbit(h.current()), h.changes.size());

	CHECK(h.current() == ceiling);
	CHECK(h.changes.empty());
}

void part_d()
{
	std::printf("Part D: real loss is visible, backs off, and recovers\n");

	harness h(numbering::wire_frame_id, true);
	h.seconds(3);
	CHECK(h.current() == ceiling);

	// Both eyes losing frames steadily is a link in trouble. Before the fix this
	// produced no from_headset::feedback at all and the controller sat at the ceiling
	// through it -- NX Warp loss was invisible to the automatic bitrate.
	h.seconds_even_loss(6, 8);
	const uint32_t backed_off = h.current();
	std::printf("  under sustained loss:              %.1f Mbit/s\n", mbit(backed_off));
	CHECK(backed_off < ceiling);

	// And it comes back when the link does. The additive increase wants increase_hold
	// (5 s) of healthy window per step, so this takes a while by design.
	h.seconds(40);
	std::printf("  after 40 s of clean link:          %.1f Mbit/s\n", mbit(h.current()));
	CHECK(h.current() > backed_off);
}

void part_e()
{
	std::printf("Part E: how often NX Warp may lose a frame before the ceiling goes\n");

	// Now that the losses are reported, WiVRn's own thresholds decide what they cost,
	// and they are strict: lost_frames_decrease is ONE lost frame in the two second
	// window, and increase_hold wants five clean seconds before each step back up. So
	// the reassembler's hole rate is now a first-class constraint on the bitrate this
	// codec can hold, and this part pins where the boundary is rather than wishing it
	// somewhere else.
	//
	// Rare loss: the controller dips and climbs back, and does not end up at the floor.
	{
		harness h(numbering::wire_frame_id, true);
		h.seconds_even_loss(60, 900); // one lost frame every ten seconds
		std::printf("  one lost frame per 10 s:           %.1f Mbit/s\n", mbit(h.current()));
		CHECK(h.current() > floor_bps);
	}

	// Frequent loss: every two second window contains one, so there is never a five
	// second clean stretch to increase from, and the bitrate walks down and stays down.
	// This is WiVRn's policy working, not a fault in the reporting -- but it is the
	// number NX Warp's reassembler has to beat, and it is worth failing loudly if it
	// ever silently stops being true.
	{
		harness h(numbering::wire_frame_id, true);
		h.seconds_even_loss(20, 180); // one lost frame every two seconds
		std::printf("  one lost frame per 2 s:            %.1f Mbit/s (by design: a lost\n"
		            "                                     frame in every window means no\n"
		            "                                     clean stretch to increase from)\n",
		            mbit(h.current()));
		CHECK(h.current() < ceiling);
	}
}

} // namespace

// The controller logs through Monado's u_log; standing in for it is all it takes to run
// the policy on its own, with no server, no encoder and no headset.
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
	verbose = argc > 1 and std::string(argv[1]) == std::string("-v");
	part_a();
	part_b();
	part_c();
	part_d();
	part_e();

	std::printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
