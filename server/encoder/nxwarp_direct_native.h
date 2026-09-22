// Bake a circular native-colour transition into RGB888 on the host.
#pragma once
#include "nxwarp_direct.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>
namespace wivrn::nxwarp_direct
{
// Flat native core: 64-pixel diameter. No discrete resolution bands.
inline float native_center_weight(uint32_t x, uint32_t y)
{
	const float dx = float(x) - 63.5f, dy = float(y) - 63.5f;
	const float t = std::clamp((std::sqrt(dx * dx + dy * dy) - 32.f) / 31.f, 0.f, 1.f);
	return 1.f - t * t * t * (t * (t * 6.f - 15.f) + 10.f);
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
inline std::span<const uint8_t> native_center_frame(layout l, std::span<const uint8_t> raw, std::span<const uint32_t> rgb, std::vector<uint8_t> & out)
{
	constexpr uint32_t side = 128, native_flag = 1u << 29;
	static const auto weights = [] {
		std::array<float, side * side> result{};
		for (uint32_t y = 0; y < side; ++y)
			for (uint32_t x = 0; x < side; ++x)
				result[y * side + x] = native_center_weight(x, y);
		return result;
	}();
	const auto f = parse_frame(l, raw);
	if (!l.native_center || !f || read32(raw, 4) != 1 || l.eyes != 2 || l.width < 256 || l.height < 256 || rgb.size() != 2 * side * side)
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
			if (x >= ox && x < ox + side && y >= oy && y < oy + side)
			{
				d = native_flag | words;
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
						append32(out, colour);
					}
				append32(out, 0);
				words += 1025;
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
