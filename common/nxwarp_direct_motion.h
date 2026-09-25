// NX direct native-pixel motion residuals. Motion is lossless and separate
// from display warping; unsupported frames remain eligible for normal coding.
#pragma once

#include "nxwarp_direct.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace wivrn::nxwarp_direct
{
constexpr uint32_t motion_magic = 0x564d584e; // NXMV
constexpr size_t motion_header_bytes = 24;
constexpr int motion_search_radius = 16;
constexpr size_t motion_cache_slots = 16;
constexpr size_t motion_cache_entry_limit = 4u * 1024u * 1024u;

struct motion_native_info
{
	std::vector<int32_t> pixel; // byte offset of native tile pixels, or -1
	size_t count = 0;
};

struct motion_vector
{
	int dx = 0, dy = 0;
	double sad = 255.0;
	size_t hits = 0;
};

struct motion_header
{
	uint16_t reference = 0;
	int dx = 0, dy = 0;
	std::span<const uint8_t> body;
};

inline size_t motion_tile_index(layout l, unsigned eye, int x, int y)
{
	return size_t(y / 32) * (l.width / 32 * l.eyes) + size_t(eye) * (l.width / 32) + size_t(x / 32);
}

inline bool motion_native_layout(layout l)
{
	return l.valid() && l.motion && l.native_center && l.eyes == 2 && l.native_side == 256 && !l.packed_native && l.zstd && l.predictor && !l.checkerboard;
}

inline bool build_motion_native(layout l, std::span<const uint8_t> raw, motion_native_info & out)
{
	if (!motion_native_layout(l))
		return false;
	const auto frame = parse_frame(l, raw);
	if (!frame || checker_frame(raw))
		return false;
	const size_t tiles = l.tile_count();
	if (tiles > size_t(std::numeric_limits<int32_t>::max()) || frame->descriptors.size() != tiles * 4)
		return false;
	motion_native_info parsed;
	parsed.pixel.assign(tiles, -1);
	std::vector<std::pair<size_t, size_t>> ranges;
	const size_t payload = frame_header_bytes + frame->descriptors.size();
	for (size_t i = 0; i < tiles; ++i)
	{
		const uint32_t d = read32(frame->descriptors, i * 4);
		if (!(d & 0x20000000u))
			continue;
		if ((d & 0x10000000u) || (d >> 30) != 0)
			return false; // RGB888 only
		const size_t offset = size_t(d & 0x0fffffffu) * 4;
		if (offset > raw.size() || payload > raw.size() - offset || raw.size() - payload - offset < 4100)
			return false;
		const size_t begin = payload + offset, end = begin + 4100;
		ranges.emplace_back(begin, end);
		parsed.pixel[i] = int32_t(begin);
		++parsed.count;
	}
	if (!parsed.count)
		return false;
	std::sort(ranges.begin(), ranges.end());
	for (size_t i = 1; i < ranges.size(); ++i)
		if (ranges[i].first < ranges[i - 1].second)
			return false;
	out = std::move(parsed);
	return true;
}

inline bool motion_pixel(layout l, std::span<const uint8_t> raw, const motion_native_info & info,
	                        unsigned eye, int x, int y, const uint8_t *& pixel)
{
	if (x < 0 || y < 0 || x >= int(l.width) || y >= int(l.height) || eye >= l.eyes || info.pixel.size() != l.tile_count())
		return false;
	const int32_t base = info.pixel[motion_tile_index(l, eye, x, y)];
	if (base < 0 || size_t(base) > raw.size() || raw.size() - size_t(base) < 4096)
		return false;
	pixel = raw.data() + base + (size_t(y & 31) * 32 + size_t(x & 31)) * 4;
	return true;
}

inline motion_vector estimate_motion(layout l, std::span<const uint8_t> old, const motion_native_info & old_info,
	                                  std::span<const uint8_t> now, const motion_native_info & now_info)
{
	motion_vector best;
	best.sad = std::numeric_limits<double>::max();
	if (!motion_native_layout(l) || old_info.pixel.size() != l.tile_count() || now_info.pixel.size() != l.tile_count())
		return best;
	struct sample { uint8_t eye; int x, y; const uint8_t * current; };
	std::array<sample, 512> samples{};
	size_t count = 0;
	uint64_t best_cost = std::numeric_limits<uint64_t>::max();
	size_t best_hits = 0;
	const int rx = std::min(64, int(l.width / 4)), ry = std::min(64, int(l.height / 4));
	for (unsigned eye = 0; eye < l.eyes; ++eye)
		for (int y = int(l.height / 2) - ry; y < int(l.height / 2) + ry; y += 8)
			for (int x = int(l.width / 2) - rx; x < int(l.width / 2) + rx; x += 8)
			{
				const uint8_t *a, *b;
				if (count < samples.size() && motion_pixel(l, now, now_info, eye, x, y, a) && motion_pixel(l, old, old_info, eye, x, y, b))
					samples[count++] = {uint8_t(eye), x, y, a};
			}
	auto test = [&](int dx, int dy) {
		uint64_t cost = 0;
		size_t hits = 0;
		for (size_t i = 0; i < count; ++i)
		{
			const auto & s = samples[i];
			const uint8_t *b;
			if (!motion_pixel(l, old, old_info, s.eye, s.x + dx, s.y + dy, b))
				continue;
			cost += unsigned(std::abs(int(s.current[0]) - int(b[0]))) + unsigned(std::abs(int(s.current[1]) - int(b[1]))) + unsigned(std::abs(int(s.current[2]) - int(b[2])));
			++hits;
			// Even if every remaining sample matched at zero cost, this candidate
			// cannot beat or tie the incumbent. Keep equality for tie-break logic.
			if (best_hits && cost * best_hits > best_cost * count)
				return;
		}
		if (!count || hits < (count + 1) / 2)
			return;
		const double score = double(cost) / double(hits * 3);
		if (score < best.sad || (score == best.sad && std::abs(dx) + std::abs(dy) < std::abs(best.dx) + std::abs(best.dy)))
		{
			best = {dx, dy, score, hits};
			best_cost = cost;
			best_hits = hits;
		}
	};
	for (int dy = -motion_search_radius; dy <= motion_search_radius; dy += 4)
		for (int dx = -motion_search_radius; dx <= motion_search_radius; dx += 4)
			test(dx, dy);
	const int cx = best.dx, cy = best.dy;
	for (int dy = std::max(-motion_search_radius, cy - 3); dy <= std::min(motion_search_radius, cy + 3); ++dy)
		for (int dx = std::max(-motion_search_radius, cx - 3); dx <= std::min(motion_search_radius, cx + 3); ++dx)
			test(dx, dy);
	return best;
}

inline void motion_sub_bytes(uint8_t * dst, const uint8_t * current, const uint8_t * previous, size_t n);

inline std::vector<uint8_t> motion_residual(layout l, std::span<const uint8_t> old, const motion_native_info & old_info,
	                                        std::span<const uint8_t> now, const motion_native_info & now_info, int dx, int dy)
{
	if (std::abs(dx) > motion_search_radius || std::abs(dy) > motion_search_radius || !parse_frame(l, old) ||
	    old_info.pixel.size() != l.tile_count() || now_info.pixel.size() != l.tile_count())
		return {};
	motion_native_info validated_now;
	if (!build_motion_native(l, now, validated_now) || validated_now.pixel != now_info.pixel)
		return {};
	std::vector<uint8_t> residual(now.begin(), now.end());
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const int32_t dst = now_info.pixel[tile];
		if (dst < 0)
			continue;
		const int ty = int(tile / (l.width / 32 * l.eyes)), rem = int(tile % (l.width / 32 * l.eyes));
		const unsigned eye = rem / int(l.width / 32);
		const int tx = rem % int(l.width / 32);
		for (int y = 0; y < 32; ++y)
		{
			const int sy = ty * 32 + y + dy;
			if (sy < 0 || sy >= int(l.height))
				continue;
			int x = 0;
			while (x < 32)
			{
				const int sx = tx * 32 + x + dx;
				if (sx < 0 || sx >= int(l.width))
				{
					++x;
					continue;
				}
				const uint8_t *src = nullptr;
				const bool has_source = motion_pixel(l, old, old_info, eye, sx, sy, src);
				const int end = std::min(32, x + 32 - (sx & 31));
				if (has_source)
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

inline void motion_add_bytes(uint8_t * dst, const uint8_t * src, size_t n)
{
	size_t i = 0;
#ifdef __aarch64__
	for (; i + 16 <= n; i += 16)
		vst1q_u8(dst + i, vaddq_u8(vld1q_u8(dst + i), vld1q_u8(src + i)));
#endif
	for (; i < n; ++i)
		dst[i] = uint8_t(dst[i] + src[i]);
}

inline void motion_sub_bytes(uint8_t * dst, const uint8_t * current, const uint8_t * previous, size_t n)
{
	size_t i = 0;
#ifdef __aarch64__
	for (; i + 16 <= n; i += 16)
		vst1q_u8(dst + i, vsubq_u8(vld1q_u8(current + i), vld1q_u8(previous + i)));
#endif
	for (; i < n; ++i)
		dst[i] = uint8_t(current[i] - previous[i]);
}

inline bool restore_motion(layout l, std::span<const uint8_t> old, const motion_native_info & old_info,
	                       std::vector<uint8_t> & residual, int dx, int dy)
{
	if (!motion_native_layout(l) || std::abs(dx) > motion_search_radius || std::abs(dy) > motion_search_radius ||
	    !parse_frame(l, old) || old_info.pixel.size() != l.tile_count())
		return false;
	motion_native_info current;
	if (!build_motion_native(l, residual, current))
		return false;
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const int32_t dst = current.pixel[tile];
		if (dst < 0)
			continue;
		const int ty = int(tile / (l.width / 32 * l.eyes)), rem = int(tile % (l.width / 32 * l.eyes));
		const unsigned eye = rem / int(l.width / 32);
		const int tx = rem % int(l.width / 32);
		for (int y = 0; y < 32; ++y)
		{
			const int sy = ty * 32 + y + dy;
			if (sy < 0 || sy >= int(l.height))
				continue;
			int x = 0;
			while (x < 32)
			{
				const int sx = tx * 32 + x + dx;
				if (sx < 0 || sx >= int(l.width))
				{
					++x;
					continue;
				}
				const size_t source_tile = motion_tile_index(l, eye, sx, sy);
				const int32_t src = old_info.pixel[source_tile];
				const int end = std::min(32, x + 32 - (sx & 31));
				if (src >= 0)
				{
					const size_t source = size_t(src) + (size_t(sy & 31) * 32 + size_t(sx & 31)) * 4;
					const size_t dest = size_t(dst) + (size_t(y) * 32 + size_t(x)) * 4;
					if (source > old.size() || old.size() - source < size_t(end - x) * 4)
						return false;
					motion_add_bytes(residual.data() + dest, old.data() + source, size_t(end - x) * 4);
				}
				x = end;
			}
		}
	}
	return true;
}

inline std::vector<uint8_t> make_motion_wire(int dx, int dy, uint16_t reference, std::span<const uint8_t> body)
{
	if (std::abs(dx) > motion_search_radius || std::abs(dy) > motion_search_radius || body.size() > UINT32_MAX - motion_header_bytes)
		return {};
	std::vector<uint8_t> wire;
	wire.reserve(motion_header_bytes + body.size());
	for (uint32_t v: {motion_magic, 1u, uint32_t(reference), uint32_t(uint16_t(int16_t(dx))), uint32_t(uint16_t(int16_t(dy))), uint32_t(body.size())})
		append32(wire, v);
	wire.insert(wire.end(), body.begin(), body.end());
	return wire;
}

inline std::optional<motion_header> parse_motion_wire(std::span<const uint8_t> wire)
{
	if (wire.size() < motion_header_bytes || read32(wire, 0) != motion_magic || read32(wire, 4) != 1 ||
	    (read32(wire, 8) & 0xffff0000u) || (read32(wire, 12) & 0xffff0000u) || (read32(wire, 16) & 0xffff0000u) ||
	    read32(wire, 20) != wire.size() - motion_header_bytes)
		return {};
	const int dx = int16_t(read32(wire, 12)), dy = int16_t(read32(wire, 16));
	if (std::abs(dx) > motion_search_radius || std::abs(dy) > motion_search_radius)
		return {};
	return motion_header{uint16_t(read32(wire, 8)), dx, dy, wire.subspan(motion_header_bytes)};
}

inline bool is_motion(std::span<const uint8_t> wire)
{
	return wire.size() >= 4 && read32(wire, 0) == motion_magic;
}

struct motion_reference
{
	uint16_t reference = 0;
	layout frame_layout;
	std::vector<uint8_t> raw;
	motion_native_info native;
};

class motion_reference_cache
{
	std::array<std::optional<motion_reference>, motion_cache_slots> entries_{};
	size_t next_ = 0;
	std::optional<uint16_t> newest_;

	static bool newer(uint16_t a, uint16_t b)
	{
		const uint16_t distance = uint16_t(a - b);
		return distance && distance < 0x8000u;
	}

	bool advance(uint16_t reference)
	{
		if (newest_ && !newer(reference, *newest_))
			return false;
		newest_ = reference;
		for (auto & entry: entries_)
			if (entry)
			{
				const uint16_t age = uint16_t(reference - entry->reference);
				if (age >= motion_cache_slots && age < 0x8000u)
					entry.reset();
			}
		return true;
	}

public:
	bool put(layout l, uint16_t reference, std::span<const uint8_t> raw)
	{
		try
		{
			for (const auto & entry: entries_)
				if (entry && entry->reference == reference)
					return entry->frame_layout.width == l.width && entry->frame_layout.height == l.height && entry->frame_layout.eyes == l.eyes &&
					       entry->raw.size() == raw.size() && std::equal(raw.begin(), raw.end(), entry->raw.begin());
			if (!advance(reference))
				return false;
			if (raw.size() > motion_cache_entry_limit || !motion_native_layout(l))
				return false;
			motion_reference entry;
			entry.reference = reference;
			entry.frame_layout = l;
			if (!build_motion_native(l, raw, entry.native))
				return false;
			entry.raw.assign(raw.begin(), raw.end());
			entries_[next_] = std::move(entry);
			next_ = (next_ + 1) % entries_.size();
			return true;
		}
		catch (const std::bad_alloc &)
		{
			return false;
		}
	}

	const motion_reference * find(uint16_t reference) const
	{
		for (const auto & entry: entries_)
			if (entry && entry->reference == reference)
				return &*entry;
		return nullptr;
	}

	bool erase(uint16_t reference)
	{
		for (auto & entry: entries_)
			if (entry && entry->reference == reference)
			{
				entry.reset();
				return true;
			}
		return false;
	}

	void clear()
	{
		for (auto & entry: entries_)
			entry.reset();
		next_ = 0;
		newest_.reset();
	}
};
} // namespace wivrn::nxwarp_direct
