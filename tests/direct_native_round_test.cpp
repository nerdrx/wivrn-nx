#include "nxwarp_direct_native.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace wivrn::nxwarp_direct;

static uint32_t output_pixel(const frame_view & frame, uint32_t width, uint32_t eye, uint32_t x, uint32_t y)
{
	const uint32_t tile = (y / 32) * (width / 32) * 2 + eye * (width / 32) + (x / 32);
	const uint32_t d = read32(frame.descriptors, tile * 4);
	if (!(d & (1u << 29)))
		return native_base_pixel(frame.blocks, d, x % 32, y % 32);
	return read32(frame.blocks, ((d & 0x1fffffffu) + (y % 32) * 32 + x % 32) * 4);
}

static uint32_t blend(uint32_t base, uint32_t source, float weight)
{
	uint32_t result = 0;
	for (unsigned shift: {0u, 8u, 16u})
		result |= uint32_t(std::lround(float((base >> shift) & 255) +
		                               weight * (float((source >> shift) & 255) - float((base >> shift) & 255))))
		          << shift;
	return result;
}

static uint32_t native_tiles(const frame_view & frame, uint32_t tiles)
{
	uint32_t count = 0;
	for (uint32_t i = 0; i < tiles; ++i)
		count += (read32(frame.descriptors, i * 4) & (1u << 29)) != 0;
	return count;
}

int main()
{
	constexpr uint32_t width = 512, height = 512, eyes = 2, side = 256;
	constexpr uint32_t old_solid = 0xc0123456u;
	const layout l{width, height, eyes, true};
	const uint32_t tiles = l.tile_count();
	auto raw = frame_header(tiles, 0);
	for (uint32_t i = 0; i < tiles; ++i)
		append32(raw, old_solid);
	assert(parse_frame(l, raw));

	std::vector<uint32_t> source(eyes * side * side);
	for (uint32_t eye = 0; eye < eyes; ++eye)
		for (uint32_t y = 0; y < side; ++y)
			for (uint32_t x = 0; x < side; ++x)
				source[eye * side * side + y * side + x] = eye ? 0xaabbccu : 0x112233u;
	std::vector<uint8_t> encoded;
	const auto native = native_center_frame(l, raw, source, encoded);
	assert(native.data() == encoded.data());
	const auto parsed = parse_frame(l, native);
	assert(parsed && read32(native, 4) == native_version);

	// Radius controls native tile count and payload budget.
	std::vector<uint8_t> radius0, radius20, radius63, radius127;
	const auto no_radius = native_center_frame(l, raw, source, radius0, 0.f);
	assert(no_radius.size() == raw.size());
	assert(std::equal(no_radius.begin(), no_radius.end(), raw.begin()));
	const auto small = native_center_frame(l, raw, source, radius20, 20.f);
	const auto medium = native_center_frame(l, raw, source, radius63, 63.f);
	const auto full = native_center_frame(l, raw, source, radius127, 127.f);
	const auto small_parsed = parse_frame(l, small);
	const auto medium_parsed = parse_frame(l, medium);
	const auto full_parsed = parse_frame(l, full);
	assert(small_parsed && medium_parsed && full_parsed);
	const auto small_tiles = native_tiles(*small_parsed, tiles);
	const auto medium_tiles = native_tiles(*medium_parsed, tiles);
	const auto full_tiles = native_tiles(*full_parsed, tiles);
	assert(small_tiles < medium_tiles);
	assert(small_tiles < full_tiles);
	assert(small.size() < medium.size() && medium.size() < full.size());
	assert(small.size() - raw.size() <= native_center_extra(20.f));
	assert(medium.size() - raw.size() <= native_center_extra(63.f));
	assert(full.size() - raw.size() <= native_center_extra(127.f));
	assert(native_center_extra(0.f) <= native_center_extra(20.f));
	assert(native_center_extra(20.f) <= native_center_extra(63.f));
	assert(native_center_extra(63.f) <= native_center_extra(127.f));
	assert(native_center_radius(80'000'000u, 90.f) == 0.f);
	assert(native_center_radius(680'000'000u, 90.f) == 127.f);
	assert(native_center_radius(240'000'000u, 120.f) < native_center_radius(240'000'000u, 90.f));

	// Circular weight: flat core, zero outside radius 127, four-way symmetry,
	// and monotonic rise toward the centre.
	assert(native_center_weight(127, 64) == 1.f);
	assert(native_center_weight(0, 0) == 0.f);
	for (uint32_t y = 0; y < side; ++y)
	{
		assert(native_center_weight(127, y) == native_center_weight(128, y));
		assert(native_center_weight(127, y) == native_center_weight(127, 255 - y));
		if (y && y <= 127)
			assert(native_center_weight(127, y) >= native_center_weight(127, y - 1));
		if (y > 128)
			assert(native_center_weight(127, y) <= native_center_weight(127, y - 1));
	}
	for (uint32_t y = 0; y < side; ++y)
		for (uint32_t x = 0; x < side; ++x)
			assert(native_center_weight(x, y) == native_center_weight(255 - x, y));

	constexpr uint32_t ox = 128, oy = 128;
	for (uint32_t eye = 0; eye < eyes; ++eye)
	{
		const uint32_t expected_source = source[eye * side * side + 128 * side + 128];
		assert(output_pixel(*parsed, width, eye, ox + 128, oy + 128) == expected_source);
		assert(output_pixel(*parsed, width, eye, ox, oy) == (old_solid & 0xffffffu));
		assert(output_pixel(*parsed, width, eye, ox + 255, oy + 255) == (old_solid & 0xffffffu));
		const uint32_t x = ox + 12, y = oy + 127;
		const auto weight = native_center_weight(x - ox, y - oy);
		assert(output_pixel(*parsed, width, eye, x, y) == blend(old_solid, expected_source, weight));
	}
	// All palette modes: exact RGB565 expansion and selector interpolation.
	for (uint32_t mode = 0; mode < 3; ++mode)
	{
		std::vector<uint8_t> blocks;
		for (uint32_t b = 0; b < (16u >> (2 * mode)); ++b)
		{
			append32(blocks, 0x001ff800u);
			for (int j = 0; j < 4; ++j)
				append32(blocks, 0xe4e4e4e4u);
		}
		assert(native_base_pixel(blocks, mode << 30, 0, 0) == 0xff0000u);
		assert(native_base_pixel(blocks, mode << 30, 1u << mode, 0) == 0xaa0055u);
		assert(native_base_pixel(blocks, mode << 30, 2u << mode, 0) == 0x5500aau);
		assert(native_base_pixel(blocks, mode << 30, 3u << mode, 0) == 0x0000ffu);
	}
	std::puts("native centre circular blend: ok");
}
