// Direct-block rate layouts. Spatial detail drops before frame admission does.
#pragma once
#include "nxwarp_direct.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace wivrn::nxwarp_direct
{
struct plan
{
	std::vector<uint32_t> descriptors;
	std::vector<std::array<uint32_t, 4>> jobs;
	uint32_t words = 0;
	size_t bytes() const
	{
		return 16 + 4 * (descriptors.size() + words);
	}
};
inline plan make_plan(layout l, unsigned quality)
{
	plan p;
	const uint32_t cols = l.width / 32 * l.eyes, per_eye = l.width / 32;
	p.descriptors.resize(l.tile_count());
	for (uint32_t ty = 0; ty < l.height / 32; ty++)
		for (uint32_t tx = 0; tx < cols; tx++)
		{
			float x = (float(tx % per_eye) + 0.5f) / per_eye * 2 - 1;
			float y = (float(ty) + 0.5f) / (l.height / 32) * 2 - 1;
			float r = std::sqrt(x * x + y * y);
			uint32_t mode;
			if (quality < 16)
			{
				float scale = 1.0f - float(quality) / 16;
				mode = r < 0.42f * scale ? 0 : r < 0.72f * scale ? 1
				                                                 : 2;
			}
			else
			{
				float radius = (32 - std::min(quality, 32u)) / 16.0f * 1.5f;
				mode = r < radius ? 2 : 3;
			}
			uint32_t tile = ty * cols + tx;
			p.descriptors[tile] = (mode << 30) | p.words;
			if (mode == 3)
			{
				p.descriptors[tile] = 0xc0000000u;
				p.jobs.push_back({tx * 32, ty * 32, 0, 4 + tile});
			}
			else
			{
				uint32_t step = 1u << mode, blocks = 4u >> mode;
				for (uint32_t by = 0; by < blocks; by++)
					for (uint32_t bx = 0; bx < blocks; bx++)
					{
						p.jobs.push_back({tx * 32 + bx * 8 * step, ty * 32 + by * 8 * step, step, 4 + l.tile_count() + p.words});
						p.words += 5;
					}
			}
		}
	return p;
}
// Includes 25% headroom for FEC, packet headers and control traffic. Actual
// transport counters remain the authority; this is a conservative admission budget.
inline plan select_plan(layout l, uint32_t bitrate, float fps)
{
	double budget = std::max(1u, bitrate) / (8.0 * std::clamp(double(fps), 1.0, 240.0) * 1.25);
	for (unsigned q = 0; q <= 32; q++)
	{
		auto p = make_plan(l, q);
		if (p.bytes() <= budget || q == 32)
			return p;
	}
	return {};
}
} // namespace wivrn::nxwarp_direct
