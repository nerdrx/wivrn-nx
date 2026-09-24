// Focused v2 checks for NX quality budgets versus bytes actually sent on wire.
// Build beside bitrate_bbr_test.cpp; all checks remain active with NDEBUG.

#include "driver/bitrate_controller.h"
#include "util/u_logging.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

using controller = wivrn::bitrate_controller;
using controller_clock = controller::clock;
using time_point = controller_clock::time_point;

namespace
{
constexpr int64_t period_ns = 11'111'111;
constexpr uint32_t quality_ceiling = 1'000'000'000;
constexpr XrTime client_start = 1'000'000'000;

int failures = 0;
int checks = 0;

#define CHECK(expr)                                                                 \
	do                                                                          \
	{                                                                           \
		++checks;                                                           \
		if (!(expr))                                                        \
		{                                                                   \
			++failures;                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		}                                                                   \
	} while (0)

void frame(controller & ctl, uint64_t index, time_point now, uint32_t actual_bytes, int64_t receive_span_ns, uint32_t quality_budget_bps, bool lost = false)
{
	ctl.on_frame_bytes(index, 0, actual_bytes, now, quality_budget_bps, period_ns);

	wivrn::from_headset::feedback feedback{};
	feedback.frame_index = index;
	feedback.stream_index = 0;
	feedback.received_first_packet = client_start + XrTime(index) * period_ns;
	if (!lost)
	{
		feedback.received_last_packet = feedback.received_first_packet + receive_span_ns;
		feedback.sent_to_decoder = feedback.received_last_packet;
		feedback.received_from_decoder = feedback.received_last_packet + period_ns;
		feedback.blitted = feedback.received_from_decoder + period_ns;
		feedback.times_displayed = 1;
	}
	ctl.on_feedback(feedback, period_ns, true, now);
}

struct trace
{
	controller ctl;
	time_point now = time_point{} + std::chrono::hours(1);
	uint64_t index = 0;

	trace()
	{
		ctl.configure({.enabled = true}, quality_ceiling, true, true, controller::mode::bbr);
	}

	void feed(uint32_t actual_bytes, int64_t span_ns, uint32_t budget = quality_ceiling, bool lost = false)
	{
		frame(ctl, index++, now, actual_bytes, span_ns, budget, lost);
		now += std::chrono::nanoseconds(period_ns);
	}
};

uint32_t bytes_for(uint32_t bps)
{
	return uint32_t((uint64_t(bps) * uint64_t(period_ns) + 8'000'000'000ULL - 1) /
	                8'000'000'000ULL);
}

int64_t wire_span(uint32_t actual_bytes, double physical_capacity_bps)
{
	const auto physical = int64_t(8e9 * double(actual_bytes) / physical_capacity_bps);
	return std::max<int64_t>(physical, int64_t(0.96 * double(period_ns)));
}

void clean_compressed_wire_does_not_collapse()
{
	for (double ratio: {0.25, 0.125, 0.5})
	{
		trace t;
		const uint32_t actual = bytes_for(uint32_t(double(quality_ceiling) * ratio));
		const int64_t span = wire_span(actual, 2'000'000'000.0);
		for (int i = 0; i < 90 * 30; ++i)
			t.feed(actual, span);
		CHECK(t.ctl.current() == quality_ceiling);
		// Coalescing floor must not turn quality budget into a fake high wire rate.
		const double expected = 8e9 * actual / double(span);
		CHECK(t.ctl.bandwidth_estimate() > expected * 0.99);
		CHECK(t.ctl.bandwidth_estimate() < expected * 1.01);
	}
}

void baseline_tag_zero_matches_wire_measurement()
{
	trace mapped;
	trace baseline;
	const uint32_t actual = bytes_for(250'000'000);
	const int64_t span = wire_span(actual, 2'000'000'000.0);
	for (int i = 0; i < 90 * 8; ++i)
	{
		mapped.feed(actual, span, quality_ceiling);
		baseline.feed(actual, span, 0);
	}
	std::printf("clean_25pct: untagged_budget=%u mapped_budget=%u wire_estimate=%u\n",
	            baseline.ctl.current(),
	            mapped.ctl.current(),
	            mapped.ctl.bandwidth_estimate());
	CHECK(baseline.ctl.current() < quality_ceiling / 2);
	CHECK(mapped.ctl.current() == quality_ceiling);
	CHECK(baseline.ctl.bandwidth_estimate() == mapped.ctl.bandwidth_estimate());
}

void content_changes_keep_clean_quality()
{
	trace t;
	for (double ratio: {0.25, 0.125, 0.5, 0.25})
	{
		for (int i = 0; i < 90 * 12; ++i)
		{
			const uint32_t budget = t.ctl.current();
			const uint32_t actual = bytes_for(uint32_t(budget * ratio));
			t.feed(actual, wire_span(actual, 2e9), budget);
			CHECK(t.ctl.current() == quality_ceiling);
		}
	}
}

void empty_and_mixed_stream_mapping_is_safe()
{
	trace empty;
	for (int i = 0; i < 90 * 2; ++i)
		empty.feed(0, int64_t(0.96 * period_ns));
	CHECK(empty.ctl.current() == quality_ceiling);
	CHECK(empty.ctl.bandwidth_estimate() == 0);

	trace mixed;
	const uint32_t actual = bytes_for(250'000'000);
	const int64_t span = wire_span(actual, 2'000'000'000.0);
	for (uint64_t i = 0; i < 90 * 4; ++i)
	{
		const auto now = mixed.now;
		mixed.ctl.on_frame_bytes(i, 0, actual, now, quality_ceiling, period_ns);
		mixed.ctl.on_frame_bytes(i, 1, actual, now, 0, 0);
		wivrn::from_headset::feedback feedback{};
		feedback.frame_index = i;
		feedback.stream_index = 0;
		feedback.received_first_packet = client_start + XrTime(i) * period_ns;
		feedback.received_last_packet = feedback.received_first_packet + span;
		feedback.sent_to_decoder = feedback.received_last_packet;
		feedback.received_from_decoder = feedback.received_last_packet + period_ns;
		feedback.blitted = feedback.received_from_decoder + period_ns;
		feedback.times_displayed = 1;
		mixed.ctl.on_feedback(feedback, period_ns, true, now);
		mixed.now += std::chrono::nanoseconds(period_ns);
	}
	CHECK(mixed.ctl.current() <= quality_ceiling);
	CHECK(mixed.ctl.bandwidth_estimate() > 0);
}

void timely_flow_tracks_physical_capacity()
{
	trace t;
	const double physical_capacity = 150'000'000.0;
	const uint32_t actual = bytes_for(250'000'000);
	const int64_t span = wire_span(actual, physical_capacity);
	for (int i = 0; i < 90 * 4; ++i)
		t.feed(actual, span);
	CHECK(t.ctl.bandwidth_estimate() > 120'000'000U);
	CHECK(t.ctl.bandwidth_estimate() < 180'000'000U);
}

void loss_and_slow_span_back_off_and_reset()
{
	trace loss;
	const uint32_t actual = bytes_for(quality_ceiling);
	for (int i = 0; i < 90 * 3; ++i)
		loss.feed(actual, int64_t(0.96 * period_ns));
	const uint32_t clean = loss.ctl.current();
	loss.feed(actual, int64_t(0.96 * period_ns), quality_ceiling, true);
	uint32_t minimum = loss.ctl.current();
	for (int i = 0; i < 90 * 2; ++i)
	{
		loss.feed(actual, int64_t(0.96 * period_ns));
		minimum = std::min(minimum, loss.ctl.current());
	}
	// Recovery can finish within this observation window; assert that the loss
	// caused a real cut, rather than requiring it to remain degraded afterwards.
	CHECK(minimum < clean);
	CHECK(loss.ctl.set_ceiling(quality_ceiling).value_or(0) == quality_ceiling);
	CHECK(loss.ctl.current() == quality_ceiling);

	trace slow;
	for (int i = 0; i < 90 * 8; ++i)
		slow.feed(actual, int64_t(1.30 * period_ns));
	CHECK(slow.ctl.current() < quality_ceiling);
	CHECK(slow.ctl.set_ceiling(quality_ceiling).value_or(0) == quality_ceiling);
	CHECK(slow.ctl.current() == quality_ceiling);
}

void missing_accounting_still_backs_off()
{
	trace t;
	const uint32_t actual = bytes_for(250'000'000);
	for (int i = 0; i < 90 * 3; ++i)
		t.feed(actual, int64_t(0.96 * period_ns));
	// No byte callbacks for long enough to expire both the ratio window and
	// the bandwidth estimate. Feedback still reports real losses.
	for (int i = 0; i < 90 * 12; ++i)
		t.feed(0, 0, quality_ceiling, true);
	CHECK(t.ctl.current() == 10'000'000);
	CHECK(t.ctl.bandwidth_estimate() == 0);
}
} // namespace

extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char *, ...)
{
}

extern "C" enum u_logging_level u_log_get_global_level(void)
{
	return U_LOGGING_INFO;
}

int main()
{
	clean_compressed_wire_does_not_collapse();
	baseline_tag_zero_matches_wire_measurement();
	content_changes_keep_clean_quality();
	empty_and_mixed_stream_mapping_is_safe();
	timely_flow_tracks_physical_capacity();
	loss_and_slow_span_back_off_and_reset();
	missing_accounting_still_backs_off();
	std::printf("%d checks, %d failure(s)\n", checks, failures);
	return failures != 0;
}
