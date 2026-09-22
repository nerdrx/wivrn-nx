#include "nxwarp_direct.h"
#include "nxwarp_direct_layout.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

using namespace wivrn::nxwarp_direct;

static void check_plan(layout l, unsigned bitrate)
{
	const plan p = select_plan(l, bitrate, 90.0f);
	assert(p.descriptors.size() == l.tile_count());
	assert(p.bytes() <= l.max_frame_bytes());
	const uint32_t total_words = 4u + l.tile_count() + p.words;
	std::set<uint32_t> used;
	for (uint32_t i = 0; i < p.jobs.size(); ++i)
	{
		const auto j = p.jobs[i];
		assert(j[3] < total_words);
		const uint32_t count = j[2] == 0 ? 1u : 5u;
		assert(j[3] + count <= total_words);
		for (uint32_t w = 0; w < count; ++w)
			assert(used.insert(j[3] + w).second);
	}

	std::vector<uint8_t> bytes = frame_header(l.tile_count(), p.words);
	for (uint32_t d: p.descriptors)
		append32(bytes, d);
	bytes.resize(bytes.size() + size_t(p.words) * 4u, 0);

	// Simulate GPU output: solid jobs write their descriptor, block jobs write 5 words.
	auto put = [&](uint32_t word, uint32_t value) {
		assert(word < total_words);
		bytes[16u + size_t(word - 4u) * 4u + 0] = uint8_t(value);
		bytes[16u + size_t(word - 4u) * 4u + 1] = uint8_t(value >> 8);
		bytes[16u + size_t(word - 4u) * 4u + 2] = uint8_t(value >> 16);
		bytes[16u + size_t(word - 4u) * 4u + 3] = uint8_t(value >> 24);
	};
	for (const auto & j: p.jobs)
		if (j[2] == 0)
			put(j[3], 0xc0123456u);
		else
			for (uint32_t w = 0; w < 5; ++w)
				put(j[3] + w, 0);

	assert(bytes.size() == p.bytes());
	assert(parse_frame(l, bytes).has_value());
	assert(parse_stream(stream_header(l)).has_value());
}

int main()
{
	// Smaller full-detail centre, identical eye layouts, valid byte budgets.
	const layout stereo{2176, 2176, 2};
	const auto top = make_plan(stereo, 0);
	unsigned full = 0;
	for (unsigned y = 0; y < 68; ++y)
		for (unsigned x = 0; x < 68; ++x)
		{
			const auto left = top.descriptors[y * 136 + x] >> 30;
			assert(left == top.descriptors[y * 136 + x + 68] >> 30);
			full += left == 0;
		}
	assert(full > 380 && full < 460); // About 9% full detail at the top layout.
	for (unsigned rate: {80'000'000u, 160'000'000u, 200'000'000u, 500'000'000u})
	{
		auto chosen = select_plan(stereo, rate, 90);
		assert(chosen.bytes() <= rate / (8.0 * 90 * 1.25));
	}

	constexpr uint32_t sizes[] = {64, 1088, 2048, 4096};
	for (uint32_t width: sizes)
		for (uint32_t height: sizes)
			for (uint32_t eyes: {1u, 2u})
			{
				if (width != height && width != 64)
					continue;
				layout l{width, height, eyes};
				if (!l.valid())
					continue;
				size_t previous = 0;
				for (uint32_t bitrate = 1; bitrate <= 800'000'000; bitrate += 1'000'000)
				{
					const plan p = select_plan(l, bitrate, 90.0f);
					assert(p.bytes() >= previous);
					previous = p.bytes();
					if (bitrate == 1 || bitrate == 400'000'001 || bitrate > 799'000'000)
						check_plan(l, bitrate);
				}
			}
	std::puts("direct blocks layout: ok");
}
