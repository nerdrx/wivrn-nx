#include "nxwarp_direct_flat.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace wivrn::nxwarp_direct;

static void put(std::vector<uint8_t> & b, uint32_t v)
{
	append32(b, v);
}

static void set32(std::vector<uint8_t> & b, size_t offset, uint32_t v)
{
	for (unsigned i = 0; i < 4; ++i)
		b[offset + i] = uint8_t(v >> (8 * i));
}

static std::vector<uint8_t> frame()
{
	const layout l{96, 32, 1};
	std::vector<uint8_t> blocks;
	blocks.reserve(105 * 4);
	auto add = [&](uint32_t endpoint) {
		put(blocks, endpoint);
		for (unsigned i = 0; i < 4; ++i)
			put(blocks, 0u);
	};
	// Mode 1 tile: four uniform blocks at offset 0.
	for (uint32_t block = 0; block < 4; ++block)
		add(0x7bef39e7u);
	// Mode 0 tile: sixteen uniform blocks at offset 20.
	for (uint32_t block = 0; block < 16; ++block)
		add(0xf800001fu);
	// Mode 2 tile: one non-flat block at offset 100.
	put(blocks, 0xf800001fu);
	put(blocks, 1u);
	put(blocks, 0u);
	put(blocks, 0u);
	put(blocks, 0u);
	std::vector<uint8_t> out = frame_header(l.tile_count(), 105);
	put(out, (1u << 30) | 0u);
	put(out, 20u);
	put(out, (2u << 30) | 100u);
	out.insert(out.end(), blocks.begin(), blocks.end());
	return out;
}

int main()
{
	const layout l{96, 32, 1};
	const auto raw = frame();
	std::vector<uint8_t> compacted;
	const auto result = compact_flat(l, raw, compacted);
	assert(result.data() == compacted.data());
	const auto parsed = parse_frame(l, result);
	assert(parsed);
	assert((read32(parsed->descriptors, 0) >> 30) == 3);
	assert((read32(parsed->descriptors, 4) >> 30) == 3);
	assert((read32(parsed->descriptors, 8) >> 30) == 2);
	assert((read32(parsed->descriptors, 8) & 0x3fffffffu) == 0);
	assert(result.size() == 16 + 12 + 5 * 4);
	std::vector<uint8_t> varied = raw;
	set32(varied, 16 + 12 + 5 * 4, 0x001f001fu);
	set32(varied, 16 + 12 + 20 * 4 + 5 * 4, 0xf800f800u);
	std::vector<uint8_t> varied_out;
	const auto varied_result = compact_flat(l, varied, varied_out);
	const auto varied_frame = parse_frame(l, varied_result);
	assert(varied_frame && (read32(varied_frame->descriptors, 0) >> 30) == 1);
	assert(varied_frame && (read32(varied_frame->descriptors, 4) >> 30) == 0);

	// No flat tiles: return original span and leave output untouched.
	std::vector<uint8_t> unchanged{7, 8, 9};
	const auto same = compact_flat(l, result, unchanged);
	assert(same.data() == result.data());
	assert((unchanged == std::vector<uint8_t>{7, 8, 9}));

	// Malformed frame: return original span without writing output.
	const auto malformed = result.first(result.size() - 1);
	const auto bad = compact_flat(l, malformed, unchanged);
	assert(bad.data() == malformed.data());
	assert((unchanged == std::vector<uint8_t>{7, 8, 9}));
	std::puts("direct blocks flat: ok");
}
