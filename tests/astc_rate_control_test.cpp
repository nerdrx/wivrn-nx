#include "server/encoder/astc_rate_control.h"

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
	constexpr std::array<uint32_t, 9> complex{44000, 138000, 259000, 390000, 427000, 463000, 501000, 850000, 1700000};
	constexpr std::array<uint32_t, 9> simple{40000, 60000, 80000, 100000, 120000, 140000, 160000, 280000, 500000};
	wivrn::astc_rate_control c;
	uint32_t q = 6;
	for (unsigned i = 0; i < 8; ++i)
	{
		q = c.update(q, complex[q], 250000);
		if (q == 2)
			break;
	}
	assert(q == 2); // lower quality until the observed frame budget fits
	for (unsigned i = 0; i < 40; ++i)
		q = c.update(q, complex[q], 250000);
	assert(q == 2); // no q0/q1 or adjacent-rung oscillation at a steady budget
	for (unsigned i = 0; i < 45; ++i)
		q = c.update(q, simple[q], 250000);
	assert(q >= 5); // stale complex-scene estimates expire and quality recovers
	q = c.update(q, simple[q], 100000); // bitrate drop
	assert(q < 6);
	for (unsigned i = 0; i < 40; ++i)
		q = c.update(q, simple[q], 600000); // bitrate rise
	assert(q == 7); // 6x6 fits; unknown 4x4 needs more headroom to probe
	for (unsigned i = 0; i < 40; ++i)
		q = c.update(q, simple[q], 1200000);
	assert(q == 8); // high budget buys the smallest 4x4 footprint

	// A sudden scene change must use the current sample, not let a warm low
	// q6 EMA hide several 2x-budget frames. Unknown rungs skip two on a >1.5x
	// overshoot; q2 is within the controller's existing 10% budget tolerance.
	wivrn::astc_rate_control scene_change;
	q = 6;
	for (unsigned i = 0; i < 30; ++i)
		q = scene_change.update(q, 190000, 250000);
	assert(q == 6);
	unsigned emitted_over_budget = 0;
	for (unsigned i = 0; i < 3 && q != 2; ++i)
	{
		const uint32_t bytes = complex[q];
		if (bytes > 250000)
			++emitted_over_budget;
		q = scene_change.update(q, bytes, 250000);
	}
	assert(q == 2);
	const uint32_t q2_bytes = complex[q];
	if (q2_bytes > 250000)
		++emitted_over_budget;
	q = scene_change.update(q, q2_bytes, 250000);
	assert(q == 2 && emitted_over_budget <= 3); // includes q2's tolerated 3.6% overrun

	// Lower-rung measurements from a simple scene can still be younger than the
	// 30-frame expiry when complexity changes. A >1.5x target and >1.5x prior-q6
	// sample marks that abrupt change and invalidates those stale alternatives.
	wivrn::astc_rate_control stale_scene;
	for (uint32_t rung = 0; rung < 7; ++rung)
		stale_scene.update(rung, simple[rung], 250000); // populate every rung
	q = 6;
	stale_scene.bytes[6] = 190000;
	for (unsigned i = 0; i < 10; ++i)
		q = stale_scene.update(q, 190000, 250000);
	assert(stale_scene.bytes[5] == simple[5]);
	emitted_over_budget = 0;
	uint32_t bytes = complex[q];
	if (bytes > 250000)
		++emitted_over_budget;
	q = stale_scene.update(q, bytes, 250000);
	assert(q == 4 && stale_scene.bytes[5] == 0); // stale q5 estimate discarded
	bytes = complex[q];
	if (bytes > 250000)
		++emitted_over_budget;
	q = stale_scene.update(q, bytes, 250000);
	assert(q == 2);
	bytes = complex[q];
	if (bytes > 250000)
		++emitted_over_budget;
	q = stale_scene.update(q, bytes, 250000);
	assert(q == 2 && emitted_over_budget <= 3); // counts the tolerated q2 packet

	// A moderate spike only steps one rung; it does not invoke the severe-spike
	// skip. A previously measured fitting rung takes priority over skipping.
	wivrn::astc_rate_control moderate_spike;
	q = 6;
	for (unsigned i = 0; i < 30; ++i)
		q = moderate_spike.update(q, 190000, 250000);
	q = moderate_spike.update(q, 300000, 250000);
	assert(q == 5);

	wivrn::astc_rate_control no_bad_probe;
	assert(no_bad_probe.update(7, 140000, 282745) == 7);
	assert(no_bad_probe.update(7, 140000, 400000) == 8);

	// Measured smaller footprints can win without an arbitrary 30% surplus.
	wivrn::astc_rate_control footprints;
	footprints.bytes[8] = 230000;
	assert(footprints.update(7, 220000, 250000) == 8);
	// A changed complex scene must not immediately retry a bad old estimate.
	assert(footprints.update(8, 300000, 250000) == 7);
	assert(footprints.update(7, 220000, 250000) == 7);
	assert(wivrn::astc_rate_control::block(6) == 8);
	assert(wivrn::astc_rate_control::block(7) == 6);
	assert(wivrn::astc_rate_control::block(8) == 4);

	wivrn::astc_rate_control measured_fit;
	measured_fit.bytes[6] = 400000; // same scene: this sample is not a >1.5x jump
	measured_fit.bytes[5] = 240000;
	q = measured_fit.update(6, 501000, 250000);
	assert(q == 5);
}
