#include "driver/bitrate_controller.h"
#include "util/u_logging.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

using controller = wivrn::bitrate_controller;
using controller_clock = controller::clock;
using time_point = controller_clock::time_point;

namespace
{
constexpr int64_t period_ns = 11'111'111;
constexpr uint32_t ceiling = 50'000'000;

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

uint32_t run(bool loss_only, bool losses)
{
	if (loss_only)
		setenv("WIVRN_BITRATE_AIMD_LOSS_ONLY", "1", 1);
	else
		unsetenv("WIVRN_BITRATE_AIMD_LOSS_ONLY");

	controller ctl;
	ctl.configure({.enabled = true}, ceiling, true, true, controller::mode::aimd);
	const time_point start = time_point{} + std::chrono::hours(1);
	const uint64_t bytes = 100'000;
	for (uint64_t frame = 0; frame < 90; ++frame)
	{
		const auto now = start + std::chrono::nanoseconds(frame * period_ns);
		ctl.on_frame_bytes(frame, 0, uint32_t(bytes), now);
		wivrn::from_headset::feedback feedback{};
		feedback.frame_index = frame;
		feedback.stream_index = 0;
		const XrTime base = 1'000'000'000 + XrTime(frame) * period_ns;
		feedback.received_first_packet = base;
		if (not losses)
		{
			feedback.received_last_packet = base + period_ns * 11 / 10;
			feedback.sent_to_decoder = feedback.received_last_packet;
			feedback.received_from_decoder = feedback.received_last_packet + period_ns;
			feedback.blitted = feedback.received_last_packet + 2 * period_ns;
			feedback.times_displayed = 1;
		}
		ctl.on_feedback(feedback, period_ns, true, now);
	}
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
	const uint32_t default_span = run(false, false);
	const uint32_t loss_only_span = run(true, false);
	const uint32_t loss_only_loss = run(true, true);
	check(default_span < ceiling, "default AIMD must back off on a congested receive span");
	check(loss_only_span == ceiling, "loss-only mode must hold on utilisation-only congestion");
	check(loss_only_loss < ceiling, "loss-only mode must still back off on actual loss");
	check(loss_only_loss <= ceiling, "loss-only mode must respect the ceiling");
	std::printf("default_span=%u loss_only_span=%u loss_only_loss=%u\n", default_span, loss_only_span, loss_only_loss);
	return 0;
}
