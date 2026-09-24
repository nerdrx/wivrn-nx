// Deterministic toy link, not a Wi-Fi/codec performance model.
#include "driver/bitrate_controller.h"
#include "util/u_logging.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string_view>

using controller = wivrn::bitrate_controller;
constexpr double hz = 90, period = 1 / hz;
constexpr int64_t period_ns = 11'111'111;
struct pending
{
	double at;
	wivrn::from_headset::feedback feedback;
};
extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char *, ...) {}
extern "C" enum u_logging_level u_log_get_global_level(void)
{
	return U_LOGGING_INFO;
}
template<typename C>
void account_frame(C & ctl, uint64_t index, uint32_t bytes, controller::clock::time_point now, bool tagged)
{
	if constexpr (requires { ctl.on_frame_bytes(index, 0, bytes, now, ctl.current(), period_ns); })
		ctl.on_frame_bytes(index, 0, bytes, now, tagged ? ctl.current() : 0, tagged ? period_ns : 0);
	else
		ctl.on_frame_bytes(index, 0, bytes, now);
}

int main(int argc, char ** argv)
{
	// Optional feedback delay and synthetic receive-span floor.
	const double delay = argc > 1 ? std::atof(argv[1]) / 1000 : 40.0 / 1000;
	const double span_floor = argc > 2 ? std::atof(argv[2]) : .96;
	const double weak_capacity = argc > 3 ? std::atof(argv[3]) * 1e6 : 5.5e8;
	if (delay < 0 || delay > 0.1 || span_floor < 0 || span_floor > 2 || weak_capacity < 1e8 || weak_capacity > 1e9)
		return 1;
	setenv("WIVRN_BITRATE_AIMD_LOSS_ONLY", "1", 1);
	const bool bbr = argc > 4 && std::string_view(argv[4]) == "bbr";
	const bool tagged = argc > 5 && std::string_view(argv[5]) == "tagged";
	controller ctl;
	ctl.configure({.enabled = true}, 1'000'000'000, true, true, bbr ? controller::mode::bbr : controller::mode::aimd);
	const auto start = controller::clock::time_point{} + std::chrono::hours(1);
	auto clock_at = [&](double s) { return start + std::chrono::nanoseconds(int64_t(s * 1e9)); };
	std::deque<pending> feedback;
	double queued_bits = 0;
	int losses = 0;
	std::puts("seconds,budget_mbps,equivalent_capacity_mbps,lost_frames,queue_ms");
	for (uint64_t i = 0; i < 9000; ++i)
	{
		double t = i * period;
		// Wire bytes are a fixed 25% of the quality budget in this model.
		double capacity = (t < 10 || (t >= 20 && t < 35) || t >= 70) ? 1e9 : (t < 20 ? 3e8 : weak_capacity);
		double wire_capacity = capacity * .25;
		queued_bits = std::max(0.0, queued_bits - wire_capacity * period);
		while (!feedback.empty() && feedback.front().at <= t)
		{
			ctl.on_feedback(feedback.front().feedback, period_ns, true, clock_at(t));
			feedback.pop_front();
		}
		uint32_t bytes = uint32_t(std::ceil(ctl.current() * .25 / hz / 8));
		account_frame(ctl, i, bytes, clock_at(t), tagged);
		const double bits = bytes * 8.0;
		bool lost = (queued_bits + bits) / wire_capacity > 2 * period;
		wivrn::from_headset::feedback f{};
		f.frame_index = i;
		f.stream_index = 0;
		f.received_first_packet = 1'000'000'000 + int64_t((t + queued_bits / wire_capacity) * 1e9);
		if (lost)
			++losses;
		else
		{
			queued_bits += bits;
			f.received_last_packet = f.received_first_packet + int64_t(std::max(bits / wire_capacity, span_floor * period) * 1e9);
			f.sent_to_decoder = f.received_last_packet;
			f.received_from_decoder = f.received_last_packet;
			f.blitted = f.received_last_packet + period_ns;
			f.times_displayed = 1;
		}
		feedback.push_back({t + delay + 3 * period, f});
		if (ctl.current() > 1'000'000'000 || ctl.current() < 10'000'000)
			return 2;
		if (i % 9 == 0)
			std::printf("%.3f,%.6f,%.0f,%d,%.4f\n", t, ctl.current() / 1e6, capacity / 1e6, losses, queued_bits / wire_capacity * 1000);
	}
}
