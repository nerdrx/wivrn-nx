#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wivrn
{
struct fresh_frame_sample
{
	uint64_t index = 0;
	int64_t received_at = 0;
	bool valid = false;
};

constexpr bool is_recent_frame(int64_t received_at, int64_t now, int64_t max_age)
{
	return received_at > 0 && received_at <= now && now - received_at <= max_age;
}

template <std::size_t Views, std::size_t Slots>
constexpr bool has_recent_common_frame(
        const std::array<std::array<fresh_frame_sample, Slots>, Views> & frames,
        std::size_t view_count,
        int64_t now,
        int64_t max_age)
{
	if (view_count == 0 || view_count > Views)
		return false;

	for (const auto & candidate: frames[0])
	{
		if (not candidate.valid || not is_recent_frame(candidate.received_at, now, max_age))
			continue;

		bool shared = true;
		for (std::size_t view = 1; view < view_count && shared; ++view)
		{
			shared = false;
			for (const auto & frame: frames[view])
				if (frame.valid && frame.index == candidate.index &&
				    is_recent_frame(frame.received_at, now, max_age))
				{
					shared = true;
					break;
				}
		}
		if (shared)
			return true;
	}
	return false;
}
} // namespace wivrn
