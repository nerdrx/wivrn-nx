#pragma once
#include <algorithm>
#include <cstdint>

namespace wivrn::nxwarp_direct
{
// Reserve conservatively before encoding, then settle against actual wire cost.
// No more than one frame interval of scheduling credit can accumulate.
class frame_admission
{
	int64_t due = 0, previous_due = 0, admitted_at = 0;
	bool pending = false;
	static int64_t advance(int64_t previous, int64_t now, int64_t interval)
	{
		return std::max(previous, now - interval) + interval;
	}

public:
	bool admit(int64_t now, int64_t predicted_interval)
	{
		if (now < due)
			return false;
		previous_due = due;
		admitted_at = now;
		pending = true;
		due = advance(due, now, predicted_interval);
		return true;
	}
	void settle(int64_t actual_interval)
	{
		if (pending)
			due = advance(previous_due, admitted_at, actual_interval);
		pending = false;
	}
	void reset()
	{
		due = 0;
		pending = false;
	}
};
} // namespace wivrn::nxwarp_direct
