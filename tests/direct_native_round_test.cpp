#include "nxwarp_direct_native.h"

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
	assert(d & (1u << 29));
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

int main()
{
	constexpr uint32_t width = 256, height = 256, eyes = 2, side = 128;
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

	// Circular weight: flat core, zero outside radius 63, four-way symmetry,
	// and monotonic rise toward the centre.
	assert(native_center_weight(63, 32) == 1.f);
	assert(native_center_weight(0, 0) == 0.f);
	for (uint32_t y = 0; y < side; ++y)
	{
		assert(native_center_weight(63, y) == native_center_weight(64, y));
		assert(native_center_weight(63, y) == native_center_weight(63, 127 - y));
		if (y && y <= 63)
			assert(native_center_weight(63, y) >= native_center_weight(63, y - 1));
		if (y > 64)
			assert(native_center_weight(63, y) <= native_center_weight(63, y - 1));
	}
	for (uint32_t y = 0; y < side; ++y)
		for (uint32_t x = 0; x < side; ++x)
			assert(native_center_weight(x, y) == native_center_weight(127 - x, y));

	constexpr uint32_t ox = 64, oy = 64;
	for (uint32_t eye = 0; eye < eyes; ++eye)
	{
		const uint32_t expected_source = source[eye * side * side + 64 * side + 64];
		assert(output_pixel(*parsed, width, eye, ox + 64, oy + 64) == expected_source);
		assert(output_pixel(*parsed, width, eye, ox, oy) == (old_solid & 0xffffffu));
		assert(output_pixel(*parsed, width, eye, ox + 127, oy + 127) == (old_solid & 0xffffffu));
		const uint32_t x = ox + 12, y = oy + 63;
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
