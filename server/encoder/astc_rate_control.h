#pragma once

#include <array>
#include <cstdint>

namespace wivrn
{
struct astc_rate_control
{
	static constexpr uint32_t rungs = 9;
	// Rungs 0..6 tune 8x8; 7 and 8 keep q6 detail in 6x6 and 4x4 blocks.
	static constexpr uint32_t block(uint32_t quality) { return quality == 8 ? 4 : quality == 7 ? 6 : 8; }
	static constexpr uint8_t expiry_frames = 30;
	std::array<uint32_t, rungs> bytes{};
	std::array<uint8_t, rungs> age{};

	uint32_t update(uint32_t quality, uint32_t sample_bytes, uint32_t target_bytes)
	{
		const uint32_t previous_bytes = bytes[quality];
		const bool abrupt_scene_change =
		        uint64_t(sample_bytes) * 100 > uint64_t(target_bytes) * 150 &&
		        uint64_t(sample_bytes) * 100 > uint64_t(previous_bytes) * 150;
		if (abrupt_scene_change)
			for (uint32_t i = 0; i < rungs; ++i)
				if (i != quality)
				{
					bytes[i] = 0;
					age[i] = 0;
				}

		for (uint32_t i = 0; i < rungs; ++i)
			if (bytes[i] && i != quality)
			{
				if (age[i] < expiry_frames)
					++age[i];
				if (age[i] >= expiry_frames)
					bytes[i] = 0;
			}

		bytes[quality] = bytes[quality]
	                         ? uint32_t((uint64_t(bytes[quality]) * 7 + sample_bytes) / 8)
	                         : sample_bytes;
		age[quality] = 0;
		// Do not let an old low EMA immediately re-select a footprint whose
		// latest packet exceeded this budget.
		if (sample_bytes > target_bytes && quality >= 7)
			bytes[quality] = sample_bytes;
		uint32_t next = quality;
		const uint64_t downshift_limit = uint64_t(target_bytes) * (quality >= 7 ? 100 : 110) / 100;
		const bool measured_over_budget = bytes[quality] > downshift_limit;
		const bool current_frame_over_budget = sample_bytes > downshift_limit;
		if ((measured_over_budget || current_frame_over_budget) && quality > 0)
		{
			for (uint32_t candidate = quality; candidate-- > 0;)
				if (bytes[candidate] && uint64_t(bytes[candidate]) * 100 <= uint64_t(target_bytes) * 105)
				{
					next = candidate;
					break;
				}
			if (next == quality)
			{
				// If no measured rung can be trusted to fit, react to a large
				// current-frame overshoot faster than the EMA can converge.
				const uint32_t steps = uint64_t(sample_bytes) * 100 > uint64_t(target_bytes) * 150 ? 2 : 1;
				next = quality > steps ? quality - steps : 0;
			}
		}
		else if (quality >= 6)
		{
			// Prefer the smallest recently measured footprint that fits, even
			// when the current footprint has less than 30% spare budget.
			for (uint32_t candidate = rungs; candidate-- > quality + 1;)
				if (bytes[candidate] && uint64_t(bytes[candidate]) * 100 <= uint64_t(target_bytes) * 95)
					return candidate;
			const uint32_t candidate = quality + 1;
			if (candidate < rungs && uint64_t(bytes[quality]) * 100 < uint64_t(target_bytes) * 70 &&
			    (!bytes[candidate] || uint64_t(bytes[candidate]) * 100 <= uint64_t(target_bytes) * 95))
				next = candidate;
		}
		else if (uint64_t(bytes[quality]) * 100 < uint64_t(target_bytes) * 70 && quality + 1 < rungs)
		{
			const uint32_t candidate = quality + 1;
			if (!bytes[candidate] || uint64_t(bytes[candidate]) * 100 <= uint64_t(target_bytes) * 105)
				next = candidate;
		}
		return next;
	}
};
} // namespace wivrn
