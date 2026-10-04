#include "utils/fresh_frame.h"

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
	constexpr int64_t grace = 250'000'000;
	assert(wivrn::is_recent_frame(750'000'000, 1'000'000'000, grace));
	assert(wivrn::is_recent_frame(1'000'000'000, 1'000'000'000, grace));
	assert(!wivrn::is_recent_frame(749'000'000, 1'000'000'000, grace));
	assert(!wivrn::is_recent_frame(0, 1'000'000'000, grace));
	assert(!wivrn::is_recent_frame(1'000'000'001, 1'000'000'000, grace));

	using sample = wivrn::fresh_frame_sample;
	using view = std::array<sample, 3>;
	using pair = std::array<view, 2>;
	constexpr int64_t now = 1'000'000'000;
	assert(!wivrn::has_recent_common_frame(pair{
	                                               view{sample{1, 990'000'000, true}, sample{2, 995'000'000, true}, {}},
	                                               view{sample{1, 700'000'000, true}, sample{3, 995'000'000, true}, {}},
	                                       },
	                                       2,
	                                       now,
	                                       grace)); // Fresh unmatched frames cannot resume stream.
	assert(!wivrn::has_recent_common_frame(pair{
	                                               view{sample{1, 700'000'000, true}, sample{2, 990'000'000, true}, {}},
	                                               view{sample{1, 700'000'000, true}, sample{3, 995'000'000, true}, {}},
	                                       },
	                                       2,
	                                       now,
	                                       grace)); // Old retained common pair cannot resume stream.
	assert(wivrn::has_recent_common_frame(pair{
	                                              view{sample{2, 990'000'000, true}, {}, {}},
	                                              view{sample{2, 995'000'000, true}, {}, {}},
	                                      },
	                                      2,
	                                      now,
	                                      grace));
	assert(wivrn::has_recent_common_frame(std::array<view, 1>{view{sample{4, 990'000'000, true}, {}, {}}}, 1, now, grace));
}
