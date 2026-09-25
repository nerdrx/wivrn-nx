// NX direct residuals with four fixed native-centre motion regions.
#pragma once

#include "nxwarp_direct_motion.h"

namespace wivrn::nxwarp_direct
{
constexpr size_t motion_regions_header_bytes = 40;
constexpr uint32_t motion_regions_version = 2;
constexpr uint32_t motion_regions_mode = 3;
constexpr uint32_t motion_regions_count = 4;

using motion_region_vectors = std::array<motion_vector, motion_regions_count>;

struct motion_regions_header
{
	uint16_t reference = 0;
	motion_region_vectors vectors{};
	std::span<const uint8_t> body;
};

inline bool motion_regions_layout(layout l)
{
	return motion_native_layout(l) && l.motion_regions && l.width >= l.native_side && l.height >= l.native_side &&
	       l.native_side >= 64 && l.native_side % 64 == 0;
}

inline unsigned motion_region_index(layout l, int x, int y)
{
	const int split_x = int(l.width / 64 * 32), split_y = int(l.height / 64 * 32);
	return unsigned(x >= split_x) + 2u * unsigned(y >= split_y);
}

inline motion_region_vectors estimate_motion_regions(layout l, std::span<const uint8_t> old,
		const motion_native_info & old_info, std::span<const uint8_t> now,
		const motion_native_info & now_info)
{
	motion_region_vectors result{};
	for (auto & v: result) v.sad = std::numeric_limits<double>::max();
	if (!motion_regions_layout(l) || old_info.pixel.size() != l.tile_count() || now_info.pixel.size() != l.tile_count())
		return result;
	struct sample { uint8_t eye; int x, y; const uint8_t * current; };
	const int x0 = int((l.width - l.native_side) / 2) & ~31, y0 = int((l.height - l.native_side) / 2) & ~31;
	const int split_x = int(l.width / 64 * 32), split_y = int(l.height / 64 * 32);
	const int edge_x[3]{x0, split_x, x0 + int(l.native_side)};
	const int edge_y[3]{y0, split_y, y0 + int(l.native_side)};
	for (unsigned region = 0; region < motion_regions_count; ++region)
	{
		const int rx = int(region & 1u), ry = int(region >> 1u);
		std::array<sample, 512> samples{};
		size_t sample_count = 0;
		for (unsigned eye = 0; eye < l.eyes; ++eye)
			for (int y = edge_y[ry] + 16; y < edge_y[ry + 1] - 16; y += 8)
				for (int x = edge_x[rx] + 16; x < edge_x[rx + 1] - 16; x += 8)
				{
					const uint8_t *a, *b;
					if (motion_pixel(l, now, now_info, eye, x, y, a) && motion_pixel(l, old, old_info, eye, x, y, b))
					{
						if (sample_count == samples.size()) return result;
						samples[sample_count++] = {uint8_t(eye), x, y, a};
					}
				}
		auto & best = result[region];
		uint64_t best_cost = std::numeric_limits<uint64_t>::max();
		size_t best_hits = 0;
		auto test = [&](int dx, int dy) {
			uint64_t cost = 0;
			size_t hits = 0;
			for (size_t i = 0; i < sample_count; ++i)
			{
				const auto & s = samples[i];
				const uint8_t *b;
				if (!motion_pixel(l, old, old_info, s.eye, s.x + dx, s.y + dy, b)) continue;
				cost += unsigned(std::abs(int(s.current[0]) - int(b[0]))) +
				        unsigned(std::abs(int(s.current[1]) - int(b[1]))) +
				        unsigned(std::abs(int(s.current[2]) - int(b[2])));
				++hits;
				if (best_hits && cost * best_hits > best_cost * sample_count) return;
				if (hits + (sample_count - i - 1) < (sample_count + 1) / 2) return;
			}
			if (!sample_count || hits < (sample_count + 1) / 2) return;
			const double sad = double(cost) / double(hits * 3);
			if (!best_hits || cost * best_hits < best_cost * hits ||
			    (cost * best_hits == best_cost * hits && std::abs(dx) + std::abs(dy) < std::abs(best.dx) + std::abs(best.dy)))
			{
				best = {dx, dy, sad, hits};
				best_cost = cost;
				best_hits = hits;
			}
		};
		for (int dy = -motion_search_radius; dy <= motion_search_radius; dy += 4)
			for (int dx = -motion_search_radius; dx <= motion_search_radius; dx += 4) test(dx, dy);
		const int cx = best.dx, cy = best.dy;
		for (int dy = std::max(-motion_search_radius, cy - 3); dy <= std::min(motion_search_radius, cy + 3); ++dy)
			for (int dx = std::max(-motion_search_radius, cx - 3); dx <= std::min(motion_search_radius, cx + 3); ++dx) test(dx, dy);
	}
	return result;
}

inline std::vector<uint8_t> motion_regions_residual(layout l, std::span<const uint8_t> old,
		const motion_native_info & old_info, std::span<const uint8_t> now,
		const motion_native_info & now_info, const motion_region_vectors & vectors)
{
	if (!motion_regions_layout(l) || !parse_frame(l, old) || old_info.pixel.size() != l.tile_count() ||
	    old_info.count == 0 || now_info.pixel.size() != l.tile_count()) return {};
	for (const auto & v: vectors)
		if (v.dx < -motion_search_radius || v.dx > motion_search_radius ||
		    v.dy < -motion_search_radius || v.dy > motion_search_radius) return {};
	motion_native_info validated;
	if (!build_motion_native(l, now, validated) || validated.pixel != now_info.pixel) return {};
	std::vector<uint8_t> residual(now.begin(), now.end());
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const int32_t dst = now_info.pixel[tile];
		if (dst < 0) continue;
		const int ty = int(tile / (l.width / 32 * l.eyes)), rem = int(tile % (l.width / 32 * l.eyes));
		const unsigned eye = unsigned(rem / int(l.width / 32));
		const int tx = rem % int(l.width / 32);
		const auto & vector = vectors[motion_region_index(l, tx * 32 + 16, ty * 32 + 16)];
		for (int y = 0; y < 32; ++y)
		{
			const int sy = ty * 32 + y + vector.dy;
			if (sy < 0 || sy >= int(l.height)) continue;
			int x = 0;
			while (x < 32)
			{
				const int sx = tx * 32 + x + vector.dx;
				if (sx < 0 || sx >= int(l.width)) { ++x; continue; }
				const uint8_t *src = nullptr;
				const bool present = motion_pixel(l, old, old_info, eye, sx, sy, src);
				const int end = std::min(32, x + 32 - (sx & 31));
				if (present)
				{
					const size_t dest = size_t(dst) + (size_t(y) * 32 + size_t(x)) * 4;
					motion_sub_bytes(residual.data() + dest, now.data() + dest, src, size_t(end - x) * 4);
				}
				x = end;
			}
		}
	}
	return residual;
}

inline bool restore_motion_regions(layout l, std::span<const uint8_t> old,
		const motion_native_info & old_info, std::vector<uint8_t> & residual,
		const motion_region_vectors & vectors)
{
	if (!motion_regions_layout(l) || !parse_frame(l, old) || old_info.pixel.size() != l.tile_count() || old_info.count == 0)
		return false;
	for (const auto & v: vectors)
		if (v.dx < -motion_search_radius || v.dx > motion_search_radius ||
		    v.dy < -motion_search_radius || v.dy > motion_search_radius) return false;
	motion_native_info current;
	if (!build_motion_native(l, residual, current)) return false;
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const int32_t dst = current.pixel[tile];
		if (dst < 0) continue;
		const int ty = int(tile / (l.width / 32 * l.eyes)), rem = int(tile % (l.width / 32 * l.eyes));
		const unsigned eye = unsigned(rem / int(l.width / 32));
		const int tx = rem % int(l.width / 32);
		const auto & vector = vectors[motion_region_index(l, tx * 32 + 16, ty * 32 + 16)];
		for (int y = 0; y < 32; ++y)
		{
			const int sy = ty * 32 + y + vector.dy;
			if (sy < 0 || sy >= int(l.height)) continue;
			int x = 0;
			while (x < 32)
			{
				const int sx = tx * 32 + x + vector.dx;
				if (sx < 0 || sx >= int(l.width)) { ++x; continue; }
				const size_t source_tile = motion_tile_index(l, eye, sx, sy);
				const int32_t src = old_info.pixel[source_tile];
				const int end = std::min(32, x + 32 - (sx & 31));
				if (src >= 0)
				{
					const size_t source = size_t(src) + (size_t(sy & 31) * 32 + size_t(sx & 31)) * 4;
					const size_t dest = size_t(dst) + (size_t(y) * 32 + size_t(x)) * 4;
					const size_t n = size_t(end - x) * 4;
					if (source > old.size() || old.size() - source < n) return false;
					motion_add_bytes(residual.data() + dest, old.data() + source, n);
				}
				x = end;
			}
		}
	}
	return true;
}

inline std::vector<uint8_t> make_motion_regions_wire(const motion_region_vectors & vectors,
		uint16_t reference, std::span<const uint8_t> body)
{
	if (body.size() > UINT32_MAX - motion_regions_header_bytes) return {};
	for (const auto & v: vectors)
		if (v.dx < -motion_search_radius || v.dx > motion_search_radius ||
		    v.dy < -motion_search_radius || v.dy > motion_search_radius) return {};
	std::vector<uint8_t> wire;
	wire.reserve(motion_regions_header_bytes + body.size());
	for (uint32_t v: {motion_magic, motion_regions_version, uint32_t(reference), motion_regions_mode,
	                   motion_regions_count, uint32_t(body.size())}) append32(wire, v);
	for (const auto & v: vectors)
		append32(wire, uint16_t(int16_t(v.dx)) | (uint32_t(uint16_t(int16_t(v.dy))) << 16));
	wire.insert(wire.end(), body.begin(), body.end());
	return wire;
}

inline std::optional<motion_regions_header> parse_motion_regions_wire(std::span<const uint8_t> wire)
{
	if (wire.size() < motion_regions_header_bytes || read32(wire, 0) != motion_magic ||
	    read32(wire, 4) != motion_regions_version || (read32(wire, 8) & 0xffff0000u) ||
	    read32(wire, 12) != motion_regions_mode || read32(wire, 16) != motion_regions_count ||
	    read32(wire, 20) != wire.size() - motion_regions_header_bytes) return {};
	motion_regions_header result;
	result.reference = uint16_t(read32(wire, 8));
	for (size_t i = 0; i < motion_regions_count; ++i)
	{
		const uint32_t packed = read32(wire, 24 + i * 4);
		const int dx = int16_t(packed & 0xffffu), dy = int16_t(packed >> 16);
		if (dx < -motion_search_radius || dx > motion_search_radius ||
		    dy < -motion_search_radius || dy > motion_search_radius) return {};
		result.vectors[i] = {dx, dy, 0.0, 0};
	}
	result.body = wire.subspan(motion_regions_header_bytes);
	return result;
}
} // namespace wivrn::nxwarp_direct
