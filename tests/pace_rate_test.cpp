#include "pace_rate.h"

#include <cmath>
#include <cstdio>
#include <cstdint>

using namespace wivrn;

namespace
{
int failures = 0;

void check(bool ok, const char * name)
{
	if (!ok)
	{
		++failures;
		std::printf("FAIL: %s\n", name);
	}
}

void near(double got, double expected, const char * name)
{
	check(std::abs(got - expected) < 1e-9, name);
}

double simulated_admitted_fps(double source_fps, double pace_fps, uint64_t ticks)
{
	const double period = 1.0 / source_fps;
	const double interval = 1.0 / pace_fps;
	const double tolerance = pace_admission_tolerance(source_fps);
	double last = 0.0;
	uint64_t admitted = 0;
	for (uint64_t tick = 0; tick < ticks; ++tick)
	{
		const double now = double(tick + 1) * period;
		if (admitted == 0 or now - last >= interval - tolerance)
		{
			last = now;
			++admitted;
		}
	}
	return double(admitted) / (double(ticks) * period);
}
} // namespace

int main()
{
	near(effective_admission_fps(90.0, 0.0), 90.0, "disabled pacing uses source rate");
	near(effective_admission_fps(90.0, 1.0 / 61.0), 90.0, "61 of 90 admits at most source rate");
	near(effective_admission_fps(90.0, 1.0 / 30.0), 36.0, "30 of 90 bound is 36");
	near(effective_admission_fps(90.0, 1e-12), 90.0, "near-zero interval uses source rate");
	near(effective_admission_fps(0.0, 1.0 / 30.0), 0.0, "invalid source stays invalid");
	const double nominal_target = 76.3e6 / 8.0 / 61.0;
	const double admitted = simulated_admitted_fps(90.0, 61.0, 900);
	check(admitted > 89.0, "61 fps admission tolerance admits nearly every 90 Hz tick");
	check(admitted * nominal_target * 8.0 > 76.3e6,
	      "nominal 61 fps budget exceeds allowance at admitted rate");
	check(admitted * (76.3e6 / 8.0 / effective_admission_fps(90.0, 1.0 / 61.0)) * 8.0 <= 76.3e6 * 1.001,
	      "effective admission budget stays within allowance");
	for (double target : {45., 60., 71., 75., 90.})
	{
		std::chrono::steady_clock::time_point phase{};
		unsigned accepted = 0;
		for (unsigned tick = 1; tick <= 9000; ++tick)
		{
			const auto now = std::chrono::steady_clock::time_point{} +
			        std::chrono::nanoseconds(uint64_t(tick) * 1000000000 / 90);
			if (pace_accumulated_admit(now, 1. / target, phase))
			{
				++accepted;
				check(!pace_accumulated_admit(now, 1. / target, phase), "no same-tick burst");
			}
		}
		check(std::abs(double(accepted) / 100. - target) <= .02, "fractional target retained");
		near(effective_admission_fps(90., 1. / target, true), target, "nominal accumulated budget");
		const auto resumed = phase + std::chrono::seconds(2);
		check(pace_accumulated_admit(resumed, 1. / target, phase), "resume after stall");
		check(!pace_accumulated_admit(resumed + std::chrono::microseconds(1), 1. / target, phase), "stall leaves no catchup burst");
	}
	return failures ? 1 : 0;
}
