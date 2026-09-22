#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace wivrn::nxwarp_direct
{
class compression_credit
{
	static constexpr size_t window_size = 8;
	std::array<double, window_size> ratios{};
	size_t count = 0;
	double factor_ = 1.0;

	bool set(double value)
	{
		value = std::clamp(value, 1.0, 1.5);
		if (value > factor_)
			value = std::min(value, factor_ + 0.1);
		if (value == factor_)
			return false;
		factor_ = value;
		return true;
	}

	bool clear()
	{
		const bool changed = factor_ != 1.0;
		factor_ = 1.0;
		count = 0;
		return changed;
	}

public:
	bool observe(size_t raw_detail, size_t wire_detail, size_t complete_wire, double frame_budget)
	{
		if (!raw_detail || !wire_detail || !std::isfinite(frame_budget) || frame_budget <= 0.0)
			return clear();
		const double raw = double(raw_detail), wire = double(wire_detail), complete = double(complete_wire);
		if (!std::isfinite(raw) || !std::isfinite(wire) || !std::isfinite(complete) || wire >= raw || complete > frame_budget * 1.10)
			return clear();
		// Grow only with spare wire budget; do not pump quality at the limit.
		if (complete > frame_budget * 0.90)
		{
			count = 0;
			return false;
		}
		ratios[count++] = raw / wire;
		if (count != window_size)
			return false;
		const double worst = *std::min_element(ratios.begin(), ratios.end());
		count = 0;
		return set(0.8 * worst);
	}

	// Apply new network budgets immediately, but retain proven compression.
	// Old samples must not authorize growth under a different budget.
	void budget_changed()
	{
		count = 0;
	}

	void reset()
	{
		(void)clear();
	}
	double value() const
	{
		return factor_;
	}
};
} // namespace wivrn::nxwarp_direct
