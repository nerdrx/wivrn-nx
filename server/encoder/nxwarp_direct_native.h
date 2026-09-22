// Bake a circular full-resolution colour transition into packed RGB565 on the host.
#pragma once
#include "nxwarp_direct.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>
namespace wivrn::nxwarp_direct
{
// Scale detail with bytes available per refresh, after the safety allocation.
inline float native_center_radius(uint32_t bitrate, float refresh)
{
	const float equivalent = float(bitrate) * 90.f / std::max(refresh, 1.f);
	return 127.f * std::sqrt(std::clamp((equivalent - 80'000'000.f) / 600'000'000.f, 0.f, 1.f));
}
inline bool native_center_tile(uint32_t x, uint32_t y, float radius)
{
	const float dx = std::max({float(x) - 127.5f, 127.5f - float(x + 31), 0.f});
	const float dy = std::max({float(y) - 127.5f, 127.5f - float(y + 31), 0.f});
	return dx * dx + dy * dy < radius * radius;
}
inline size_t native_center_extra(float radius, bool packed = true)
{
	size_t tiles = 0;
	for (uint32_t y = 0; y < 256; y += 32)
		for (uint32_t x = 0; x < 256; x += 32)
			tiles += native_center_tile(x, y, radius);
	return tiles * (packed ? 515u : 1025u) * 4u * 2u;
}
// Continuous circular fade; the full-budget core has a 128-pixel diameter.
inline float native_center_weight(uint32_t x, uint32_t y, float radius = 127.f)
{
	if (radius <= 0.f)
		return 0.f;
	const float dx = float(x) - 127.5f, dy = float(y) - 127.5f;
	const float core = radius * 64.f / 127.f;
	const float t = std::clamp((std::sqrt(dx * dx + dy * dy) - core) / (radius - core), 0.f, 1.f);
	// Fade the tiny core in, avoiding a sudden full-strength birth at low budgets.
	const float strength = std::clamp(radius / 16.f, 0.f, 1.f);
	return (1.f - t * t * t * (t * (t * 6.f - 15.f) + 10.f)) * strength * strength * (3.f - 2.f * strength);
}
inline uint32_t native_base_pixel(std::span<const uint8_t> blocks, uint32_t d, uint32_t x, uint32_t y)
{
	const uint32_t mode = d >> 30;
	if (mode == 3)
		return d & 0xffffffu;
	const uint32_t qx = x >> mode, qy = y >> mode;
	const uint32_t off = (d & 0x3fffffffu) + 5 * ((qy / 8) * (4u >> mode) + qx / 8);
	const uint32_t endpoints = read32(blocks, off * 4), i = (qy % 8) * 8 + qx % 8;
	const uint32_t selector = (read32(blocks, (off + 1 + i / 16) * 4) >> (2 * (i % 16))) & 3;
	auto expand = [](uint32_t v) {
		const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
		return ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | (b << 3) | (b >> 2);
	};
	const uint32_t a = expand(endpoints & 65535), b = expand(endpoints >> 16);
	uint32_t c = 0;
	for (unsigned shift: {0u, 8u, 16u})
		c |= (((3 - selector) * ((a >> shift) & 255) + selector * ((b >> shift) & 255) + 1) / 3) << shift;
	return c;
}
inline std::span<const uint8_t> native_center_frame(layout l, std::span<const uint8_t> raw, std::span<const uint32_t> rgb, std::vector<uint8_t> & out, float radius = 127.f)
{
	constexpr uint32_t side = 256, native_flag = 1u << 29;
	if (radius <= 0.f)
		return raw;
	thread_local std::array<float, side * side> weights{};
	thread_local float cached_radius = -1.f;
	if (cached_radius != radius)
	{
		for (uint32_t y = 0; y < side; ++y)
			for (uint32_t x = 0; x < side; ++x)
				weights[y * side + x] = native_center_weight(x, y, radius);
		cached_radius = radius;
	}
	const auto f = parse_frame(l, raw);
	if (!l.native_center || l.native_side != side || !f || read32(raw, 4) != 1 || l.eyes != 2 || l.width < 256 || l.height < 256 || rgb.size() != 2 * side * side)
		return raw;
	const uint32_t ox = ((l.width - side) / 2) & ~31u, oy = ((l.height - side) / 2) & ~31u;
	const uint32_t cols = l.width / 32, rows = l.height / 32;
	out.clear();
	out.reserve(raw.size() + 2 * side * side * 4 + 128);
	for (uint32_t v: {frame_magic, 2u, l.tile_count(), 0u})
		append32(out, v);
	out.resize(16 + l.tile_count() * 4);
	uint32_t words = 0;
	for (uint32_t ty = 0; ty < rows; ++ty)
		for (uint32_t tx = 0; tx < cols * l.eyes; ++tx)
		{
			const uint32_t tile = ty * cols * l.eyes + tx, eye = tx / cols, x = (tx % cols) * 32, y = ty * 32;
			const uint32_t old = read32(f->descriptors, tile * 4), mode = old >> 30;
			uint32_t d = old;
			if (x >= ox && x < ox + side && y >= oy && y < oy + side && native_center_tile(x - ox, y - oy, radius))
			{
				d = native_flag | (l.packed_native ? 0x10000000u : 0u) | words;
				uint32_t pair = 0;
				for (uint32_t dy = 0; dy < 32; ++dy)
					for (uint32_t dx = 0; dx < 32; ++dx)
					{
						const uint32_t px = x - ox + dx, py = y - oy + dy;
						const float weight = weights[py * side + px];
						const uint32_t source = rgb[eye * side * side + py * side + px] & 0xffffffu;
						const uint32_t base = native_base_pixel(f->blocks, old, dx, dy);
						uint32_t colour = 0;
						for (unsigned shift: {0u, 8u, 16u})
						{
							const float a = float((base >> shift) & 255), b = float((source >> shift) & 255);
							colour |= uint32_t(std::lround(a + weight * (b - a))) << shift;
						}
						if (!l.packed_native)
						{
							append32(out, colour);
							continue;
						}
						const uint32_t r = (colour >> 16) & 255, g = (colour >> 8) & 255, b = colour & 255;
						const uint32_t packed = ((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255);
						if (dx % 2 == 0)
							pair = packed;
						else
							append32(out, pair | (packed << 16));
					}
				append32(out, 0);
				if (l.packed_native)
				{
					append32(out, 0);
					append32(out, 0);
				}
				words += l.packed_native ? 515 : 1025;
			}
			else if (mode != 3)
			{
				const uint32_t n = 80u >> (2 * mode), off = old & 0x3fffffffu;
				d = (mode << 30) | words;
				out.insert(out.end(), f->blocks.begin() + off * 4, f->blocks.begin() + (off + n) * 4);
				words += n;
			}
			for (unsigned b = 0; b < 4; ++b)
				out[16 + tile * 4 + b] = uint8_t(d >> (b * 8));
		}
	for (unsigned b = 0; b < 4; ++b)
		out[12 + b] = uint8_t(words >> (b * 8));
	return out;
}
} // namespace wivrn::nxwarp_direct
