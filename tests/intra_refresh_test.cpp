// Intra refresh loss recovery: the IDR handler's decision core, driven frame by frame
// with no encoder, no GPU and no headset.
//
// wivrn::default_idr_handler decides three things that matter here, and the whole point
// of the feature is that they stopped being the same decision:
//
//   * a keyframe the headset needs because it has nothing to predict from at all — the
//     first frame of a session, and every reset() (a reconnect, a rate reconfiguration,
//     a failover swap) — which must stay a real IDR whatever the setting says;
//   * a keyframe asked for because a frame was lost, which is the one that used to spike
//     the bitrate exactly when the link could least carry it, and which now becomes a
//     rolling intra refresh;
//   * whether frames may be skipped while recovery is in flight — true while waiting for
//     an IDR to be acknowledged, and necessarily false during a sweep, because the sweep
//     is carried by the frames.
//
// Part A: session start and reset() are real IDRs, with or without intra refresh.
// Part B: intra refresh off is the behaviour that was always there — loss asks for an IDR
//         and frames are skipped until it is acknowledged.
// Part C: intra refresh on — loss starts a sweep, nothing is skipped, and the stream comes
//         back to plain P frames on its own.
// Part D: loss inside a sweep never restarts it, but does condemn it: one more sweep runs.
// Part E: sweeps that keep losing frames escalate to a real IDR rather than looping.
// Part F: a clean sweep clears the tally, so the next loss is gentle again.
// Part G: the live switch. Off before a sweep starts falls back to an IDR; off during one
//         leaves it to finish, because it is already repairing the picture.
// Part H: non-reference frames are not evidence of anything, refresh or not.
//
// Reference invalidation turned the two-rung choice above into a three-rung ladder:
// invalidate (one ordinary P frame, predicted from an older acknowledged reference), then
// refresh, then IDR. The parts below are that ladder.
//
// Part I: invalidation is the first rung, and it names the frame to invalidate. Nothing is
//         skipped, and the stream is back to plain P frames as soon as the repair is
//         acknowledged. With invalidation off, the ladder is exactly Parts A-H again.
// Part J: the DPB bound. A loss older than the encoder's reference buffer cannot be repaired
//         by invalidating it — there would be nothing older left — so it skips the rung.
// Part K: a spoiled invalidation escalates one rung, to a sweep, or to an IDR when there is
//         no sweep to escalate to.
// Part L: escalation is per recovery, not permanent. A fresh, independent loss starts at the
//         cheapest rung again.
// Part M: the live switch, same shape as Part G: off before the invalidation goes out falls
//         down the ladder; off after it has gone out leaves it to be judged.
//
// Build:
//   g++ -std=c++23 -I server -I common -I build-server/common \
//       -I build-server/_deps/monado-src/src/xrt/include \
//       -I build-server/_deps/monado-src/src/xrt/auxiliary \
//       -I build-server/_deps/monado-src/src/external/openxr_includes \
//       -isystem external -isystem build-server/_deps/boost-src/libs/pfr/include \
//       -o intra_refresh_test tests/intra_refresh_test.cpp \
//       server/encoder/idr_handler.cpp common/smp.cpp -lcrypto
//   ./intra_refresh_test

#include "encoder/idr_handler.h"
#include "util/u_logging.h"

#include <cstdarg>
#include <cstdio>
#include <string>

using wivrn::default_idr_handler;
using wivrn::intra_refresh_sweep_frames;
using frame_type = default_idr_handler::frame_type;

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

// The handler waits a little longer than the nominal sweep before judging one, because the
// encoders round the refresh column up to whole macroblock columns and overshoot. How much
// longer is its own business, so nothing here counts frames to find the end of a sweep: the
// harness drives frames until the handler asks for something, which is the only thing an
// encoder can observe about it either.
constexpr uint64_t max_sweep = 4 * intra_refresh_sweep_frames;

const char * name(frame_type t)
{
	switch (t)
	{
		case frame_type::i:
			return "I";
		case frame_type::p:
			return "P";
		case frame_type::refresh:
			return "R";
		case frame_type::invalidate:
			return "V";
	}
	return "?";
}

// Reference frames the invalidation tests tell the handler the encoder keeps. Any small
// number will do — what is under test is that the bound exists and is applied to the age of
// the loss, not the value NVENC happens to be configured with.
constexpr uint32_t test_dpb = 4;

// One frame through the handler, exactly as an encoder drives it: ask whether to skip it,
// and if not ask what kind of frame it is. Returns nullopt for a skipped frame.
struct step_result
{
	bool skipped;
	frame_type type;
};

step_result step(default_idr_handler & h, uint64_t frame)
{
	if (h.should_skip(frame))
		return {.skipped = true, .type = frame_type::p};
	auto t = h.get_type(frame);
	if (verbose)
		std::printf("    frame %3llu -> %s\n", (unsigned long long)frame, name(t));
	return {.skipped = false, .type = t};
}

// The headset's report for a frame that arrived whole, or one that did not. Only the three
// fields the handler reads are filled in; everything else is timing the encoder never looks at.
wivrn::from_headset::feedback report(uint64_t frame, bool delivered)
{
	wivrn::from_headset::feedback f{};
	f.frame_index = frame;
	f.stream_index = 0;
	f.sent_to_decoder = delivered ? 1 : 0;
	return f;
}

// Bring a handler up to a running stream: the opening IDR, its acknowledgement, and a few
// good P frames. Returns the next frame index.
uint64_t start_stream(default_idr_handler & h, uint64_t frame = 0)
{
	auto first = step(h, frame);
	CHECK(not first.skipped);
	CHECK(first.type == frame_type::i);
	h.on_feedback(report(frame, true));
	++frame;

	for (int i = 0; i < 5; ++i, ++frame)
	{
		auto s = step(h, frame);
		CHECK(not s.skipped);
		CHECK(s.type == frame_type::p);
		h.on_feedback(report(frame, true));
	}
	return frame;
}

// Run `count` frames that all arrive, from `frame`, checking none is skipped and none is a
// keyframe. Returns the next frame index.
uint64_t run_clean(default_idr_handler & h, uint64_t frame, uint64_t count)
{
	for (uint64_t i = 0; i < count; ++i, ++frame)
	{
		auto s = step(h, frame);
		CHECK(not s.skipped);
		CHECK(s.type != frame_type::i);
		h.on_feedback(report(frame, true));
	}
	return frame;
}

// What the handler asked for next, and how many ordinary frames went by before it did.
struct next_ask
{
	frame_type type;
	uint64_t plain_frames;
	bool skipped_any;
};

// Drive frames from `frame` until the handler asks for something other than an ordinary P
// frame, or `cap` frames have gone by. `lost` says which frames the headset failed to
// receive. `frame` is left on the one after whatever it asked for.
//
// This is how an encoder sees the handler — it never knows how long a sweep is, only that
// one day it is told to start another one — so driving the tests this way keeps them from
// depending on a number that belongs to the encoders rather than to the policy.
template <typename Lost>
next_ask advance(default_idr_handler & h, uint64_t & frame, Lost lost, uint64_t cap = max_sweep)
{
	next_ask r{.type = frame_type::p, .plain_frames = 0, .skipped_any = false};
	for (uint64_t i = 0; i < cap; ++i)
	{
		if (h.should_skip(frame))
		{
			r.skipped_any = true;
			++frame;
			continue;
		}
		auto t = h.get_type(frame);
		if (verbose)
			std::printf("    frame %3llu -> %s\n", (unsigned long long)frame, name(t));
		h.on_feedback(report(frame, not lost(frame)));
		++frame;
		if (t != frame_type::p)
		{
			r.type = t;
			return r;
		}
		++r.plain_frames;
	}
	return r;
}

auto nothing_lost = [](uint64_t) { return false; };

// --- Part A -----------------------------------------------------------------
// The keyframes that are not loss recovery are the ones the headset's decoder cannot do
// without: it holds nothing this encoder produced, so a refresh sweep predicted from that
// nothing would decode to nothing. They stay real IDRs whether intra refresh is on or off.
void part_a()
{
	std::printf("Part A: session start and reset stay real IDRs\n");

	for (bool refresh: {false, true})
	{
		default_idr_handler h;
		h.set_intra_refresh(refresh, intra_refresh_sweep_frames);

		// Session start
		auto first = step(h, 0);
		CHECK(not first.skipped);
		CHECK(first.type == frame_type::i);
		h.on_feedback(report(0, true));

		uint64_t frame = run_clean(h, 1, 3);

		// A reconnect, a rate reconfiguration or a failover swap — all of them reset()
		h.reset();
		auto after = step(h, frame);
		CHECK(not after.skipped);
		CHECK(after.type == frame_type::i);
	}
}

// --- Part B -----------------------------------------------------------------
// Without intra refresh nothing changed: a lost frame asks for an IDR, and the stream is
// held silent until the headset says that IDR arrived. That silence is the cost the feature
// exists to remove, so it is worth pinning down that it is still exactly what happens when
// the switch is off.
void part_b()
{
	std::printf("Part B: intra refresh off asks for an IDR and skips until it lands\n");

	default_idr_handler h;
	uint64_t frame = start_stream(h);

	// A frame does not make it
	h.on_feedback(report(frame, false));
	++frame;

	auto idr = step(h, frame);
	CHECK(not idr.skipped);
	CHECK(idr.type == frame_type::i);
	const uint64_t idr_frame = frame;
	++frame;

	// Everything behind it is skipped while the handler waits
	for (int i = 0; i < 4; ++i, ++frame)
		CHECK(step(h, frame).skipped);

	h.on_feedback(report(idr_frame, true));

	auto resumed = step(h, frame);
	CHECK(not resumed.skipped);
	CHECK(resumed.type == frame_type::p);
}

// --- Part C -----------------------------------------------------------------
// The feature itself. The same loss now starts a sweep, and — the part that matters most —
// not one frame is skipped while it runs: the intra blocks that repair the picture ride
// those frames, so skipping them would be skipping the repair. When the sweep is over the
// stream is back to ordinary P frames with no keyframe having been sent at all.
void part_c()
{
	std::printf("Part C: intra refresh on repairs without a keyframe and without skipping\n");

	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	uint64_t frame = start_stream(h);

	h.on_feedback(report(frame, false));
	++frame;

	auto sweep = step(h, frame);
	CHECK(not sweep.skipped);
	CHECK(sweep.type == frame_type::refresh);
	h.on_feedback(report(frame, true));
	++frame;

	// The sweep is carried by ordinary frames, none of them skipped, none a keyframe, and
	// it takes roughly the sweep length to cross the picture
	const uint64_t sweep_start = frame;
	frame = run_clean(h, frame, max_sweep);
	CHECK(frame - sweep_start >= intra_refresh_sweep_frames);

	// And it is over: no second sweep, no IDR, ever
	auto after = advance(h, frame, nothing_lost);
	CHECK(not after.skipped_any);
	CHECK(after.type == frame_type::p);
	CHECK(after.plain_frames == max_sweep);
}

// --- Part D -----------------------------------------------------------------
// A link that drops a frame in the middle of a sweep took a slice of intra blocks with it,
// so that column was never refreshed and the sweep has to be redone. What must NOT happen
// is a restart on the report itself: restarting on every loss report from a link that is
// still dropping frames would keep moving the column back to the left edge and the picture
// would never come whole. The verdict is passed once, at the end.
void part_d()
{
	std::printf("Part D: loss inside a sweep condemns it but does not restart it\n");

	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	uint64_t frame = start_stream(h);

	h.on_feedback(report(frame, false));
	++frame;

	CHECK(step(h, frame).type == frame_type::refresh);
	h.on_feedback(report(frame, true));
	const uint64_t sweep_start = frame;
	++frame;

	// Two losses well inside the sweep. Nothing may happen on either of them: the next
	// thing the handler asks for has to be a whole sweep away, not a frame away.
	auto lost_twice = [sweep_start](uint64_t f) {
		return f == sweep_start + 5 or f == sweep_start + 12;
	};
	auto again = advance(h, frame, lost_twice);

	// The verdict, once, at the end — and still not a keyframe
	CHECK(not again.skipped_any);
	CHECK(again.type == frame_type::refresh);
	CHECK(again.plain_frames >= intra_refresh_sweep_frames);
}

// --- Part E -----------------------------------------------------------------
// A link bad enough to spoil sweep after sweep is one a gentle repair cannot fix, and
// sweeping forever would leave the user looking at a permanently broken picture. After a
// few tries the handler gives up on being gentle and sends the keyframe after all.
void part_e()
{
	std::printf("Part E: repeated failures escalate to a real IDR\n");

	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	uint64_t frame = start_stream(h);

	h.on_feedback(report(frame, false));
	++frame;

	// Every sweep loses one frame a little way in, which is enough to condemn it
	uint64_t sweep_start = frame;
	auto always_spoiled = [&sweep_start](uint64_t f) { return f == sweep_start + 7; };

	int sweeps = 0;
	int idrs = 0;
	for (int attempt = 0; attempt < 8 and idrs == 0; ++attempt)
	{
		sweep_start = frame;
		auto s = advance(h, frame, always_spoiled);
		if (s.type == frame_type::refresh)
			++sweeps;
		else if (s.type == frame_type::i)
			++idrs;
		else
			break; // ran out of frames without a verdict, which is its own failure
	}

	CHECK(idrs == 1);
	// More than one attempt at the gentle path, but a bounded number of them
	CHECK(sweeps >= 2);
	CHECK(sweeps <= 4);
}

// --- Part F -----------------------------------------------------------------
// The failure tally is about a link that is bad right now, not about the session. A sweep
// that completes cleanly says the link is fine again, so the next loss — whenever it comes —
// gets the gentle path from a clean slate rather than one attempt away from an IDR.
void part_f()
{
	std::printf("Part F: a clean sweep clears the failure tally\n");

	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	uint64_t frame = start_stream(h);

	// One frame of every sweep goes missing, which is enough to condemn it. The lambda is
	// rebased on each sweep, so `ask` below both runs a sweep out and reports the verdict
	// passed on it — the first call returns straight away, on the frame that starts the
	// first sweep, because that is the frame the handler speaks on.
	uint64_t sweep_start = frame;
	auto spoiled = [&sweep_start](uint64_t f) { return f == sweep_start + 7; };
	auto ask = [&](auto lost) {
		sweep_start = frame;
		return advance(h, frame, lost).type;
	};

	// One spoiled sweep, then a clean one that finishes the repair
	h.on_feedback(report(frame, false));
	++frame;
	CHECK(ask(spoiled) == frame_type::refresh); // the first sweep starts
	CHECK(ask(spoiled) == frame_type::refresh); // it was spoiled, so a second one
	CHECK(ask(nothing_lost) == frame_type::p);  // that one was clean, nothing more

	// A fresh loss much later gets the gentle path from a clean slate: the full allowance
	// of sweeps before an IDR, which it would not have if the tally had carried over
	h.on_feedback(report(frame, false));
	++frame;
	CHECK(ask(spoiled) == frame_type::refresh);
	CHECK(ask(spoiled) == frame_type::refresh);
	CHECK(ask(spoiled) == frame_type::refresh);
	CHECK(ask(spoiled) == frame_type::i);
}

// --- Part G -----------------------------------------------------------------
// The headset can turn the feature off mid-session. Before a sweep has started that has to
// take effect at once — the user asked for keyframe recovery and a keyframe is what the next
// loss should produce. A sweep already in flight is a different matter: it is repairing the
// picture right now, and an IDR on top of it would only spend bandwidth to arrive at the
// same place.
void part_g()
{
	std::printf("Part G: the live switch, before and during a sweep\n");

	{
		default_idr_handler h;
		h.set_intra_refresh(true, intra_refresh_sweep_frames);
		uint64_t frame = start_stream(h);

		h.on_feedback(report(frame, false));
		++frame;

		// Turned off between the loss and the frame that would have carried the sweep
		h.set_intra_refresh(false, intra_refresh_sweep_frames);

		auto s = step(h, frame);
		CHECK(not s.skipped);
		CHECK(s.type == frame_type::i);
	}

	{
		default_idr_handler h;
		h.set_intra_refresh(true, intra_refresh_sweep_frames);
		uint64_t frame = start_stream(h);

		h.on_feedback(report(frame, false));
		++frame;
		CHECK(step(h, frame).type == frame_type::refresh);
		h.on_feedback(report(frame, true));
		++frame;

		// Turned off with the column a quarter of the way across
		frame = run_clean(h, frame, intra_refresh_sweep_frames / 4);
		h.set_intra_refresh(false, intra_refresh_sweep_frames);

		// It finishes on its own, and no keyframe comes out of it
		auto after = advance(h, frame, nothing_lost);
		CHECK(not after.skipped_any);
		CHECK(after.type == frame_type::p);

		// But the next loss is a keyframe again
		h.on_feedback(report(frame, false));
		++frame;
		CHECK(step(h, frame).type == frame_type::i);
	}
}

// --- Part H -----------------------------------------------------------------
// A frame nothing predicts from can be lost without costing anything: no later frame
// references it, so there is nothing to repair. That was true of the keyframe path and has
// to stay true of the refresh one, or every dropped non-reference frame would start a sweep
// and the stream would carry intra blocks forever.
void part_h()
{
	std::printf("Part H: a lost non-reference frame starts nothing\n");

	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	uint64_t frame = start_stream(h);

	h.set_non_ref(frame);
	CHECK(h.is_non_ref_frame(frame));
	h.on_feedback(report(frame, false));
	++frame;

	auto s = step(h, frame);
	CHECK(not s.skipped);
	CHECK(s.type == frame_type::p);
}

// Encode one ordinary P frame at `frame`, tell the handler it never arrived, and leave
// `frame` on the next index. Returns the index of the frame that was lost.
uint64_t lose_one(default_idr_handler & h, uint64_t & frame)
{
	const uint64_t lost = frame;
	auto s = step(h, frame);
	CHECK(not s.skipped);
	CHECK(s.type == frame_type::p);
	++frame;
	h.on_feedback(report(lost, false));
	return lost;
}

// --- Part I -----------------------------------------------------------------
// The cheapest rung. A lost frame becomes one ordinary P frame that predicts from an older
// reference the headset acknowledged, instead of half a second of intra blocks or a
// keyframe. The handler has to name the frame the encoder must strike out, because the
// encoder has no idea which one went missing — it only ever hears about frames it sent.
void part_i()
{
	std::printf("Part I: a lost frame is repaired by invalidating it\n");

	// The rung has to be asked for. Without set_ref_invalidation the handler behaves exactly
	// as Parts A-H describe, which is what makes the feature's "off" switch honest.
	{
		default_idr_handler h;
		h.set_intra_refresh(true, intra_refresh_sweep_frames);
		uint64_t frame = start_stream(h);
		lose_one(h, frame);
		CHECK(step(h, frame).type == frame_type::refresh);
	}

	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	h.set_ref_invalidation(true, test_dpb);
	uint64_t frame = start_stream(h);

	const uint64_t lost = lose_one(h, frame);

	auto s = step(h, frame);
	CHECK(not s.skipped);
	CHECK(s.type == frame_type::invalidate);
	// The whole point: the encoder is told *which* reference is unusable
	CHECK(h.invalidate_target() == lost);

	const uint64_t repair = frame;
	++frame;

	// The repair arrives. That ends the wait at once — there is nothing else to wait for,
	// the picture is whole again the moment that one frame decodes.
	h.on_feedback(report(repair, true));

	for (int i = 0; i < 20; ++i, ++frame)
	{
		auto p = step(h, frame);
		CHECK(not p.skipped);
		CHECK(p.type == frame_type::p);
		h.on_feedback(report(frame, true));
	}
}

// --- Part J -----------------------------------------------------------------
// Invalidating a frame invalidates everything predicted from it, so it only helps while
// something older survives. Past the encoder's DPB depth nothing does, and asking for it
// anyway would force the encoder to produce the keyframe this ladder exists to avoid. So
// that rung is skipped rather than attempted.
void part_j()
{
	std::printf("Part J: a loss older than the DPB skips the invalidation rung\n");

	for (bool refresh: {true, false})
	{
		default_idr_handler h;
		if (refresh)
			h.set_intra_refresh(true, intra_refresh_sweep_frames);
		h.set_ref_invalidation(true, test_dpb);
		uint64_t frame = start_stream(h);

		// The loss happens, but the report of it takes its time: by the time it lands, the
		// encoder has moved a whole DPB's worth of frames past it.
		const uint64_t lost = frame;
		for (uint64_t i = 0; i <= test_dpb; ++i, ++frame)
			CHECK(step(h, frame).type == frame_type::p);
		h.on_feedback(report(lost, false));

		auto s = step(h, frame);
		CHECK(not s.skipped);
		CHECK(s.type == (refresh ? frame_type::refresh : frame_type::i));
	}

	// And the boundary the other way: one frame younger is still in reach
	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	h.set_ref_invalidation(true, test_dpb);
	uint64_t frame = start_stream(h);

	const uint64_t lost = frame;
	for (uint64_t i = 0; i < test_dpb; ++i, ++frame)
		CHECK(step(h, frame).type == frame_type::p);
	h.on_feedback(report(lost, false));

	CHECK(step(h, frame).type == frame_type::invalidate);
	CHECK(h.invalidate_target() == lost);
}

// --- Part K -----------------------------------------------------------------
// The repair is one frame, and one frame can be lost like any other. When it is, predicting
// from an older reference plainly did not get the picture back, so the handler climbs: a
// sweep if the encoder has one, a keyframe if it does not.
void part_k()
{
	std::printf("Part K: a spoiled invalidation escalates one rung\n");

	for (bool refresh: {true, false})
	{
		default_idr_handler h;
		if (refresh)
			h.set_intra_refresh(true, intra_refresh_sweep_frames);
		h.set_ref_invalidation(true, test_dpb);
		uint64_t frame = start_stream(h);

		lose_one(h, frame);
		CHECK(step(h, frame).type == frame_type::invalidate);
		const uint64_t repair = frame;
		++frame;

		// The frame carrying the repair never arrived either
		h.on_feedback(report(repair, false));

		auto ask = advance(h, frame, nothing_lost);
		CHECK(ask.type == (refresh ? frame_type::refresh : frame_type::i));
		// Nothing is skipped on the way there: the frames in between are ordinary ones and
		// the headset is better off with them than with silence.
		CHECK(not ask.skipped_any);
	}

	// A report that overtakes the acknowledgement must not be buried by it. Feedback does not
	// arrive in frame order, so a "frame N+1 was lost" that lands before "frame N arrived"
	// would, if the acknowledgement were checked first, end the wait and leave the hole N+1
	// opened with nothing at all scheduled to repair it.
	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	h.set_ref_invalidation(true, test_dpb);
	uint64_t frame = start_stream(h);

	lose_one(h, frame);
	CHECK(step(h, frame).type == frame_type::invalidate);
	const uint64_t repair = frame;
	++frame;

	// One more frame goes out, and its loss is reported before the repair's success is
	CHECK(step(h, frame).type == frame_type::p);
	h.on_feedback(report(frame, false));
	++frame;
	h.on_feedback(report(repair, true));

	auto ask = advance(h, frame, nothing_lost);
	CHECK(ask.type == frame_type::refresh);
}

// --- Part L -----------------------------------------------------------------
// Escalation belongs to one recovery, not to the stream. A link that drops a frame every few
// seconds should get the free repair every time and never climb — otherwise the first
// unlucky moment of a session would leave it paying for sweeps for the rest of it.
void part_l()
{
	std::printf("Part L: a fresh loss starts at the cheapest rung again\n");

	default_idr_handler h;
	h.set_intra_refresh(true, intra_refresh_sweep_frames);
	h.set_ref_invalidation(true, test_dpb);
	uint64_t frame = start_stream(h);

	for (int round = 0; round < 4; ++round)
	{
		lose_one(h, frame);

		auto s = step(h, frame);
		CHECK(s.type == frame_type::invalidate);
		const uint64_t repair = frame;
		++frame;
		h.on_feedback(report(repair, true));

		frame = run_clean(h, frame, 10);
	}

	// The same across an escalation: one spoiled repair costs one sweep, and the loss after
	// that sweep is cheap again.
	lose_one(h, frame);
	CHECK(step(h, frame).type == frame_type::invalidate);
	const uint64_t repair = frame;
	++frame;
	h.on_feedback(report(repair, false));

	auto ask = advance(h, frame, nothing_lost);
	CHECK(ask.type == frame_type::refresh);

	// Let that sweep finish cleanly, then lose another frame
	auto after = advance(h, frame, nothing_lost);
	CHECK(after.type == frame_type::p);
	frame = run_clean(h, frame, 5);

	lose_one(h, frame);
	CHECK(step(h, frame).type == frame_type::invalidate);
}

// --- Part M -----------------------------------------------------------------
// The live switch, exactly the shape Part G gives the refresh: turning the rung off before
// its frame has gone out falls down the ladder, turning it off afterwards leaves the repair
// that is already in the encoder's hands to be judged on its merits.
void part_m()
{
	std::printf("Part M: turning reference invalidation off, before and after\n");

	// Off before it goes out, with a sweep below it to fall to
	{
		default_idr_handler h;
		h.set_intra_refresh(true, intra_refresh_sweep_frames);
		h.set_ref_invalidation(true, test_dpb);
		uint64_t frame = start_stream(h);

		lose_one(h, frame);
		h.set_ref_invalidation(false, test_dpb);

		auto s = step(h, frame);
		CHECK(not s.skipped);
		CHECK(s.type == frame_type::refresh);
	}

	// Off before it goes out, with nothing below it but the keyframe
	{
		default_idr_handler h;
		h.set_ref_invalidation(true, test_dpb);
		uint64_t frame = start_stream(h);

		lose_one(h, frame);
		h.set_ref_invalidation(false, test_dpb);

		auto s = step(h, frame);
		CHECK(not s.skipped);
		CHECK(s.type == frame_type::i);
	}

	// Off after it has gone out: the frame is encoded, the repair may well have worked, and
	// throwing a keyframe on top of it would cost bandwidth for nothing.
	{
		default_idr_handler h;
		h.set_intra_refresh(true, intra_refresh_sweep_frames);
		h.set_ref_invalidation(true, test_dpb);
		uint64_t frame = start_stream(h);

		lose_one(h, frame);
		CHECK(step(h, frame).type == frame_type::invalidate);
		const uint64_t repair = frame;
		++frame;

		h.set_ref_invalidation(false, test_dpb);
		h.on_feedback(report(repair, true));

		auto after = advance(h, frame, nothing_lost);
		CHECK(after.type == frame_type::p);

		// From here on the cheap rung is gone, so the next loss is a sweep
		lose_one(h, frame);
		CHECK(step(h, frame).type == frame_type::refresh);
	}
}

} // namespace

// The handler logs through Monado's u_log; standing in for it is all it takes to run the
// policy on its own, with no encoder, no compositor and no headset.
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
	part_j();
	part_k();
	part_l();
	part_m();

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
