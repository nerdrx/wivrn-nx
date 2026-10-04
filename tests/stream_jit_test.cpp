#include "../client/scenes/stream_jit.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main()
{
	using wivrn::jit_scheduler;
	constexpr int64_t period = 11'111'111;

	// Idle refreshes still feed deadline/report/cap accounting, but do not train
	// the pass cost or complete warmup.
	jit_scheduler idle;
	for (int i = 0; i < 90; ++i)
		idle.account(3'000'000, 0, 0, 50'000'000, false, period, false);
	assert(idle.frames_seen == 0);
	assert(idle.cost_peak_ns == 0);
	assert(idle.lead_n == 90);
	assert(idle.sleep_ns(true, 0, 50'000'000) == 0);

	jit_scheduler warm;
	for (int i = 0; i < 90; ++i)
		warm.account(3'000'000, 0, 0, 50'000'000, false, period);
	assert(warm.frames_seen == warm.warmup_frames);
	assert(warm.cost_peak_ns == 3'000'000);
	assert(warm.sleep_ns(true, 0, 50'000'000) > 0);

	// A cached refresh cannot decay an existing peak, but its attributable skip
	// still ratchets the sleep cap and increments the miss report.
	jit_scheduler cached;
	cached.cost_peak_ns = 10'000'000;
	cached.frames_seen = 90;
	const int64_t cap_before = cached.sleep_cap_ns;
	cached.account(1'000'000, 0, period, 50'000'000, true, period, false);
	assert(cached.cost_peak_ns == 10'000'000);
	assert(cached.frames_seen == 90);
	assert(cached.missed_skipped == 1);
	assert(cached.sleep_cap_ns == cap_before - period);

	// The default argument preserves the previous accounting behavior exactly.
	jit_scheduler default_arg, explicit_true;
	default_arg.account(4'000'000, 5'000'000, 2'000'000, 20'000'000, false, period);
	explicit_true.account(4'000'000, 5'000'000, 2'000'000, 20'000'000, false, period, true);
	assert(default_arg.frames_seen == explicit_true.frames_seen);
	assert(default_arg.cost_peak_ns == explicit_true.cost_peak_ns);
	assert(default_arg.sleep_cap_ns == explicit_true.sleep_cap_ns);
	assert(default_arg.lead_n == explicit_true.lead_n);

	// A 12 ms spike every 200 submitted passes is covered by peak hold plus the
	// existing miss-driven margin over a 1000-frame run.
	jit_scheduler spikes;
	unsigned underbudget_spikes = 0;
	for (int frame = 0; frame < 1000; ++frame)
	{
		const bool spike = frame % 200 == 0;
		const int64_t cost = spike ? 12'000'000 : 3'000'000;
		const int64_t budget = spikes.frames_seen >= spikes.warmup_frames ? spikes.budget_ns() : 0;
		const int64_t slept = spikes.sleep_ns(true, 0, 50'000'000);
		if (spike and spikes.frames_seen >= spikes.warmup_frames and budget < cost)
			++underbudget_spikes;
		const int64_t lead = 50'000'000 - slept - cost;
		spikes.account(cost, budget, slept, lead, false, period);
	}
	assert(spikes.frames_seen == 1000);
	assert(underbudget_spikes <= 1); // One first observed spike may widen the margin.

	std::cout << "PASS: idle warmup, actual-pass warmup, idle peak hold, cached miss/cap accounting, default API semantics, and periodic-spike budget (1000 passes)\n";
}
