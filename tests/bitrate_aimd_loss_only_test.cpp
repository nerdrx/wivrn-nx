#include "driver/bitrate_controller.h"
#include "util/u_logging.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

using controller = wivrn::bitrate_controller;
using time_point = controller::clock::time_point;

namespace
{
constexpr int64_t period_ns = 11'111'111;
constexpr uint32_t ceiling = 50'000'000;
constexpr XrTime client_start = 1'000'000'000;
constexpr uint64_t not_reached = std::numeric_limits<uint64_t>::max();

struct loss_trace
{
	uint32_t final_bitrate;
	uint64_t ns_to_full = not_reached;
};

[[noreturn]] void fail(const char *what)
{
	std::fprintf(stderr, "FAIL: %s\n", what);
	std::abort();
}

void check(bool value, const char *what)
{
	if (not value)
		fail(what);
}

void frame(controller &ctl, uint64_t index, time_point now, double span,
           bool lost = false, bool late = false)
{
	ctl.on_frame_bytes(index, 0, 100'000, now);
	wivrn::from_headset::feedback feedback{};
	feedback.frame_index = index;
	feedback.stream_index = 0;
	feedback.received_first_packet = client_start + XrTime(index) * period_ns;
	if (not lost)
	{
		feedback.sent_to_decoder = feedback.received_first_packet;
		feedback.received_last_packet = feedback.received_first_packet + XrTime(period_ns * span);
		feedback.received_from_decoder = feedback.received_last_packet;
		feedback.blitted = late ? 0 : feedback.received_last_packet + period_ns;
		feedback.times_displayed = late ? 0 : 1;
	}
	ctl.on_feedback(feedback, period_ns, true, now);
}

loss_trace after_loss(bool loss_only, double clean_span, bool late = false,
                      bool persistent_loss = false, uint32_t ceiling_bps = ceiling)
{
	if (loss_only)
		setenv("WIVRN_BITRATE_AIMD_LOSS_ONLY", "1", 1);
	else
		unsetenv("WIVRN_BITRATE_AIMD_LOSS_ONLY");

	controller ctl;
	ctl.configure({.enabled = true}, ceiling_bps, true, true, controller::mode::aimd);
	const time_point start = time_point{} + std::chrono::hours(1);
	const auto loss_time = start + std::chrono::nanoseconds(48 * period_ns);
	bool dropped = false;
	uint64_t ns_to_full = not_reached;
	for (uint64_t i = 0; i < 48; ++i)
		frame(ctl, i, start + std::chrono::nanoseconds(i * period_ns), 0.50);

	frame(ctl, 48, start + std::chrono::nanoseconds(48 * period_ns), 0, true);
	for (uint64_t i = 49; i < 240; ++i)
	{
		const bool loss = persistent_loss && (i == 110 || i == 172);
		const auto now = start + std::chrono::nanoseconds(i * period_ns);
		frame(ctl, i, now, clean_span, loss, late);
		if (ctl.current() < ceiling_bps)
			dropped = true;
		else if (dropped && ctl.current() == ceiling_bps && ns_to_full == not_reached)
			ns_to_full = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - loss_time).count());
	}
	return {ctl.current(), ns_to_full};
}

uint64_t deep_recovery()
{
	setenv("WIVRN_BITRATE_AIMD_LOSS_ONLY", "1", 1);
	controller ctl;
	ctl.configure({.enabled = true}, 1'000'000'000, true, true, controller::mode::aimd);
	const time_point start = time_point{} + std::chrono::hours(1);
	for (uint64_t i = 0; i < 270; ++i)
		frame(ctl, i, start + std::chrono::nanoseconds(i * period_ns), 0.96, true);
	const auto low = ctl.current();
	check(low < 400'000'000, "deep trace must start recovery below 400 Mbit/s");
	for (uint64_t i = 270; i < 5670; ++i)
	{
		frame(ctl, i, start + std::chrono::nanoseconds(i * period_ns), 0.96);
		check(ctl.current() <= 1'000'000'000, "deep recovery must respect ceiling");
		if (ctl.current() == 1'000'000'000)
		{
			const uint64_t elapsed = (i - 270) * period_ns;
			std::printf("deep_start=%u deep_ns_to_full=%llu\n", low,
			            static_cast<unsigned long long>(elapsed));
			return elapsed;
		}
	}
	return not_reached;
}

uint32_t after_loss_with_bad_probe(double span, bool late, bool loss)
{
	setenv("WIVRN_BITRATE_AIMD_LOSS_ONLY", "1", 1);
	controller ctl;
	ctl.configure({.enabled = true}, ceiling, true, true, controller::mode::aimd);
	const time_point start = time_point{} + std::chrono::hours(1);
	for (uint64_t i = 0; i < 48; ++i)
		frame(ctl, i, start + std::chrono::nanoseconds(i * period_ns), 0.50);
	frame(ctl, 48, start + std::chrono::nanoseconds(48 * period_ns), 0, true);
	for (uint64_t i = 49; i < 240; ++i)
		frame(ctl, i, start + std::chrono::nanoseconds(i * period_ns), span, loss, late);
	return ctl.current();
}
} // namespace

extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char *, ...)
{}
extern "C" enum u_logging_level u_log_get_global_level(void)
{
	return U_LOGGING_INFO;
}

int main()
{
	const auto deep_time = deep_recovery();
	check(deep_time < 10'000'000'000, "clean deep recovery must reach full ceiling within 10 seconds");
	const loss_trace default_trace = after_loss(false, 0.96);
	const loss_trace loss_only_trace = after_loss(true, 0.96);
	const loss_trace gigabit_trace = after_loss(true, 0.96, false, false, 1'000'000'000);
	const uint32_t over_threshold = after_loss_with_bad_probe(1.11, false, false);
	const uint32_t late_probe = after_loss_with_bad_probe(0.96, true, false);
	const uint32_t loss_probe = after_loss_with_bad_probe(0.96, false, true);
	const uint32_t persistent_loss = after_loss(true, 0.50, false, true).final_bitrate;
	std::printf("default_recovery=%u loss_only_recovery=%u default_ns_to_full=%llu loss_only_ns_to_full=%llu gigabit_recovery=%u gigabit_ns_to_full=%llu over_threshold=%u late_probe=%u loss_probe=%u persistent_loss=%u\n",
	            default_trace.final_bitrate, loss_only_trace.final_bitrate,
	            static_cast<unsigned long long>(default_trace.ns_to_full),
	            static_cast<unsigned long long>(loss_only_trace.ns_to_full),
	            gigabit_trace.final_bitrate,
	            static_cast<unsigned long long>(gigabit_trace.ns_to_full),
	            over_threshold, late_probe, loss_probe, persistent_loss);

	check(default_trace.final_bitrate <= 45'000'000, "default AIMD must not recover at 0.96 utilisation");
	check(loss_only_trace.final_bitrate > 45'000'000 && loss_only_trace.final_bitrate <= ceiling,
	      "loss-only mode must recover at 0.96 utilisation within ceiling");
	check(loss_only_trace.ns_to_full < default_trace.ns_to_full,
	      "loss-only recovery must reach full ceiling faster than default AIMD");
	check(loss_only_trace.ns_to_full <= 2'000'000'000,
	      "loss-only recovery must reach full ceiling within 2 seconds of loss");
	check(gigabit_trace.final_bitrate == 1'000'000'000,
	      "loss-only recovery must reach a 1 Gbit/s ceiling");
	check(gigabit_trace.ns_to_full <= 2'000'000'000,
	      "1 Gbit/s loss-only recovery must reach full ceiling within 2 seconds of loss");
	check(over_threshold == 45'000'000, "span above 1.10 must not drive recovery");
	check(late_probe == 45'000'000, "late frames must not drive recovery");
	check(loss_probe < 45'000'000, "lost frames must not drive recovery");
	check(persistent_loss < 45'000'000, "persistent loss must cut again");

	return 0;
}
