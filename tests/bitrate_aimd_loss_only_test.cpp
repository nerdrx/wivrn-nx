#include "driver/bitrate_controller.h"
#include "util/u_logging.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using controller = wivrn::bitrate_controller;
using time_point = controller::clock::time_point;

namespace
{
constexpr int64_t period_ns = 11'111'111;
constexpr uint32_t ceiling = 50'000'000;
constexpr XrTime client_start = 1'000'000'000;

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

uint32_t after_loss(bool loss_only, double clean_span, bool late = false,
                   bool persistent_loss = false)
{
	if (loss_only)
		setenv("WIVRN_BITRATE_AIMD_LOSS_ONLY", "1", 1);
	else
		unsetenv("WIVRN_BITRATE_AIMD_LOSS_ONLY");

	controller ctl;
	ctl.configure({.enabled = true}, ceiling, true, true, controller::mode::aimd);
	const time_point start = time_point{} + std::chrono::hours(1);
	for (uint64_t i = 0; i < 48; ++i)
		frame(ctl, i, start + std::chrono::nanoseconds(i * period_ns), 0.50);

	frame(ctl, 48, start + std::chrono::nanoseconds(48 * period_ns), 0, true);
	for (uint64_t i = 49; i < 240; ++i)
	{
		const bool loss = persistent_loss && (i == 110 || i == 172);
		frame(ctl, i, start + std::chrono::nanoseconds(i * period_ns), clean_span, loss, late);
	}
	return ctl.current();
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
	const uint32_t default_recovery = after_loss(false, 0.96);
	const uint32_t loss_only_recovery = after_loss(true, 0.96);
	const uint32_t over_threshold = after_loss_with_bad_probe(1.11, false, false);
	const uint32_t late_probe = after_loss_with_bad_probe(0.96, true, false);
	const uint32_t loss_probe = after_loss_with_bad_probe(0.96, false, true);
	const uint32_t persistent_loss = after_loss(true, 0.50, false, true);
	std::printf("default_recovery=%u loss_only_recovery=%u over_threshold=%u late_probe=%u loss_probe=%u persistent_loss=%u\n",
	            default_recovery, loss_only_recovery, over_threshold, late_probe, loss_probe,
	            persistent_loss);

	check(default_recovery <= 45'000'000, "default AIMD must not recover at 0.96 utilisation");
	check(loss_only_recovery > 45'000'000 && loss_only_recovery <= ceiling,
	      "loss-only mode must recover at 0.96 utilisation within ceiling");
	check(over_threshold == 45'000'000, "span above 1.10 must not drive recovery");
	check(late_probe == 45'000'000, "late frames must not drive recovery");
	check(loss_probe < 45'000'000, "lost frames must not drive recovery");
	check(persistent_loss < 45'000'000, "persistent loss must cut again");

	return 0;
}
