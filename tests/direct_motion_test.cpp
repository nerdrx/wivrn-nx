#include "common/nxwarp_direct_motion.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace wivrn::nxwarp_direct;

static motion_vector scalar_estimate(layout l, std::span<const uint8_t> old, const motion_native_info & oi,
	                                std::span<const uint8_t> now, const motion_native_info & ni, bool * varying_hits = nullptr)
{
	motion_vector best;
	best.sad = std::numeric_limits<double>::max();
	struct sample { uint8_t eye; int x, y; };
	std::array<sample, 512> samples{};
	size_t count = 0;
	size_t first_hits = std::numeric_limits<size_t>::max();
	const int rx = std::min(64, int(l.width / 4)), ry = std::min(64, int(l.height / 4));
	for (unsigned eye = 0; eye < l.eyes; ++eye)
		for (int y = int(l.height / 2) - ry; y < int(l.height / 2) + ry; y += 8)
			for (int x = int(l.width / 2) - rx; x < int(l.width / 2) + rx; x += 8)
			{
				const uint8_t *a, *b;
				if (count < samples.size() && motion_pixel(l, now, ni, eye, x, y, a) && motion_pixel(l, old, oi, eye, x, y, b))
					samples[count++] = {uint8_t(eye), x, y};
			}
	auto test = [&](int dx, int dy) {
		uint64_t cost = 0;
		size_t hits = 0;
		for (size_t i = 0; i < count; ++i)
		{
			const auto & s = samples[i];
			const uint8_t *a, *b;
			if (!motion_pixel(l, now, ni, s.eye, s.x, s.y, a) || !motion_pixel(l, old, oi, s.eye, s.x + dx, s.y + dy, b))
				continue;
			cost += unsigned(std::abs(int(a[0]) - int(b[0]))) + unsigned(std::abs(int(a[1]) - int(b[1]))) + unsigned(std::abs(int(a[2]) - int(b[2])));
			++hits;
		}
		if (!count || hits < (count + 1) / 2)
			return;
		if (first_hits == std::numeric_limits<size_t>::max())
			first_hits = hits;
		else if (varying_hits && first_hits != hits)
			*varying_hits = true;
		const double score = double(cost) / double(hits * 3);
		if (score < best.sad || (score == best.sad && std::abs(dx) + std::abs(dy) < std::abs(best.dx) + std::abs(best.dy)))
			best = {dx, dy, score, hits};
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

static std::vector<uint8_t> scalar_residual(layout l, std::span<const uint8_t> old, const motion_native_info & oi,
	                                       std::span<const uint8_t> now, const motion_native_info & ni, int dx, int dy)
{
	std::vector<uint8_t> result(now.begin(), now.end());
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const int32_t dst = ni.pixel[tile];
		if (dst < 0)
			continue;
		const int ty = int(tile / (l.width / 32 * l.eyes)), rem = int(tile % (l.width / 32 * l.eyes));
		const unsigned eye = rem / int(l.width / 32);
		const int tx = rem % int(l.width / 32);
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x)
			{
				const uint8_t *src = nullptr;
				motion_pixel(l, old, oi, eye, tx * 32 + x + dx, ty * 32 + y + dy, src);
				const size_t p = size_t(dst) + (size_t(y) * 32 + x) * 4;
				for (unsigned c = 0; c < 4; ++c)
					result[p + c] = uint8_t(now[p + c] - (src ? src[c] : 0));
			}
	}
	return result;
}

static void sparsify(layout l, std::vector<uint8_t> & raw, unsigned phase)
{
	const auto frame = parse_frame(l, raw);
	assert(frame);
	unsigned native_index = 0;
	for (size_t i = 0; i < frame->descriptors.size() / 4; ++i)
		if (read32(frame->descriptors, i * 4) & 0x20000000u)
		{
			if (native_index++ % 4 == phase)
				for (unsigned b = 0; b < 4; ++b)
					raw[16 + i * 4 + b] = uint8_t(0xc0112233u >> (8 * b));
		}
}

static void invert_native(layout l, std::vector<uint8_t> & raw)
{
	motion_native_info info;
	assert(build_motion_native(l, raw, info));
	for (int32_t base: info.pixel)
		if (base >= 0)
			for (size_t i = 0; i < 4096; ++i)
				raw[size_t(base) + i] = uint8_t(255 - raw[size_t(base) + i]);
}

static std::vector<uint8_t> make_frame(layout l, int shift_x = 0, int shift_y = 0, bool checker = false, bool overlap = false);

static std::vector<uint8_t> make_flat_frame(layout l)
{
	auto raw = make_frame(l);
	motion_native_info info;
	assert(build_motion_native(l, raw, info));
	for (int32_t base: info.pixel)
		if (base >= 0)
			std::fill_n(raw.begin() + base, 4096, uint8_t(0x55));
	return raw;
}

static motion_vector compare_pair(layout l, const std::vector<uint8_t> & old, const std::vector<uint8_t> & now, bool * varying_hits = nullptr)
{
	motion_native_info oi, ni;
	assert(build_motion_native(l, old, oi) && build_motion_native(l, now, ni));
	const auto expected = scalar_estimate(l, old, oi, now, ni, varying_hits);
	const auto actual = estimate_motion(l, old, oi, now, ni);
	assert(actual.dx == expected.dx && actual.dy == expected.dy && actual.sad == expected.sad && actual.hits == expected.hits);
	const auto expected_residual = scalar_residual(l, old, oi, now, ni, actual.dx, actual.dy);
	auto residual = motion_residual(l, old, oi, now, ni, actual.dx, actual.dy);
	assert(residual == expected_residual);
	assert(restore_motion(l, old, oi, residual, actual.dx, actual.dy) && residual == now);
	return actual;
}

static double percentile(std::vector<double> values, size_t n)
{
	std::sort(values.begin(), values.end());
	return values[(values.size() * n + 99) / 100 - 1];
}

static void benchmark_motion(layout l, const std::vector<uint8_t> & old, const std::vector<uint8_t> & now)
{
	motion_native_info oi, ni;
	assert(build_motion_native(l, old, oi) && build_motion_native(l, now, ni));
	std::vector<double> old_search, new_search, old_residual, new_residual;
	for (unsigned i = 0; i < 30; ++i)
	{
		auto t = std::chrono::steady_clock::now();
		const auto baseline = scalar_estimate(l, old, oi, now, ni);
		double elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count();
		if (i >= 6) old_search.push_back(elapsed);
		t = std::chrono::steady_clock::now();
		const auto optimized = estimate_motion(l, old, oi, now, ni);
		elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count();
		assert(optimized.dx == baseline.dx && optimized.dy == baseline.dy && optimized.sad == baseline.sad && optimized.hits == baseline.hits);
		if (i >= 6) new_search.push_back(elapsed);
		t = std::chrono::steady_clock::now();
		const auto expected = scalar_residual(l, old, oi, now, ni, optimized.dx, optimized.dy);
		elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count();
		if (i >= 6) old_residual.push_back(elapsed);
		t = std::chrono::steady_clock::now();
		const auto actual = motion_residual(l, old, oi, now, ni, optimized.dx, optimized.dy);
		elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count();
		assert(actual == expected);
		if (i >= 6) new_residual.push_back(elapsed);
	}
	std::printf("motion CPU us p50/p95: search scalar %.1f/%.1f optimized %.1f/%.1f; residual scalar %.1f/%.1f spans %.1f/%.1f\n",
	            percentile(old_search, 50), percentile(old_search, 95), percentile(new_search, 50), percentile(new_search, 95),
	            percentile(old_residual, 50), percentile(old_residual, 95), percentile(new_residual, 50), percentile(new_residual, 95));
}

static layout motion_layout()
{
	return {512, 512, 2, true, 256, false, true, true, false, true};
}

static uint8_t pattern(unsigned eye, int x, int y, unsigned c)
{
	uint32_t v = uint32_t(x) * 0x9e3779b9u ^ uint32_t(y) * 0x85ebca6bu ^ eye * 0xc2b2ae35u ^ c * 0x27d4eb2fu;
	v ^= v >> 16;
	v *= 0x7feb352du;
	v ^= v >> 15;
	return uint8_t(v >> 24);
}

static std::vector<uint8_t> make_frame(layout l, int shift_x, int shift_y, bool checker, bool overlap)
{
	const uint32_t cols = l.width / 32, rows = l.height / 32, tile_count = l.tile_count();
	const int x0 = int(l.width - l.native_side) / 2, y0 = int(l.height - l.native_side) / 2;
	const uint32_t words_per_tile = checker ? 512 : native_rgb_words;
	uint32_t native_count = 0;
	for (uint32_t ty = 0; ty < rows; ++ty)
		for (uint32_t eye = 0; eye < l.eyes; ++eye)
			for (uint32_t tx = 0; tx < cols; ++tx)
				if (int(tx * 32) >= x0 && int(tx * 32 + 32) <= x0 + int(l.native_side) &&
				    int(ty * 32) >= y0 && int(ty * 32 + 32) <= y0 + int(l.native_side))
					++native_count;
	const uint32_t words = native_count * words_per_tile;
	std::vector<uint8_t> raw = frame_header(tile_count, words, native_version | (checker ? checker_flag : 0));
	uint32_t offset = 0, first_native_offset = 0, native_index = 0;
	for (uint32_t ty = 0; ty < rows; ++ty)
		for (uint32_t eye = 0; eye < l.eyes; ++eye)
			for (uint32_t tx = 0; tx < cols; ++tx)
			{
				const bool native = int(tx * 32) >= x0 && int(tx * 32 + 32) <= x0 + int(l.native_side) &&
				                    int(ty * 32) >= y0 && int(ty * 32 + 32) <= y0 + int(l.native_side);
				if (native)
				{
					if (!native_index)
						first_native_offset = offset;
					append32(raw, 0x20000000u | (overlap && native_index == 1 ? first_native_offset : offset));
					offset += words_per_tile;
					++native_index;
				}
				else
					append32(raw, 0xc0112233u);
			}
	raw.resize(16 + 4ull * (tile_count + words), 0);
	const size_t payload = 16 + size_t(tile_count) * 4;
	for (uint32_t ty = 0; ty < rows; ++ty)
		for (uint32_t eye = 0; eye < l.eyes; ++eye)
			for (uint32_t tx = 0; tx < cols; ++tx)
			{
				const size_t tile = size_t(ty) * (cols * l.eyes) + eye * cols + tx;
				const uint32_t d = read32(raw, 16 + tile * 4);
				if (!(d & 0x20000000u) || checker)
					continue;
				const size_t block = payload + size_t(d & 0x0fffffffu) * 4;
				for (int y = 0; y < 32; ++y)
					for (int x = 0; x < 32; ++x)
					{
						const int gx = int(tx * 32) + x + shift_x, gy = int(ty * 32) + y + shift_y;
						const bool inside = gx >= x0 && gy >= y0 && gx < x0 + int(l.native_side) && gy < y0 + int(l.native_side);
						const size_t p = block + (size_t(y) * 32 + x) * 4;
						for (unsigned c = 0; c < 4; ++c)
							raw[p + c] = inside ? pattern(eye, gx, gy, c) : 0;
					}
			}
	return raw;
}

static void test_wire()
{
	const std::array<uint8_t, 3> body{3, 7, 11};
	auto wire = make_motion_wire(-8, 16, 65535, body);
	assert(is_motion(wire));
	auto parsed = parse_motion_wire(wire);
	assert(parsed && parsed->reference == 65535 && parsed->dx == -8 && parsed->dy == 16);
	assert(std::equal(parsed->body.begin(), parsed->body.end(), body.begin(), body.end()));
	assert(make_motion_wire(17, 0, 1, body).empty());
	for (size_t n: {size_t(0), size_t(23), wire.size() - 1})
		assert(!parse_motion_wire(std::span<const uint8_t>(wire).first(n)));
	auto bad = wire;
	bad[0] ^= 1;
	assert(!parse_motion_wire(bad));
	bad = wire;
	bad[4] = 2;
	assert(!parse_motion_wire(bad));
	bad = wire;
	bad[10] = 1; // reference upper bits must be zero
	assert(!parse_motion_wire(bad));
	bad = wire;
	bad[14] = 1; // dx upper bits must be zero
	assert(!parse_motion_wire(bad));
	bad = wire;
	bad[18] = 1; // dy upper bits must be zero
	assert(!parse_motion_wire(bad));
	bad = wire;
	bad[20]++;
	assert(!parse_motion_wire(bad));
	bad = make_motion_wire(0, 0, 1, body);
	bad[12] = 17;
	assert(!parse_motion_wire(bad));
}

static void test_roundtrip_and_layout()
{
	const layout l = motion_layout();
	const auto old = make_frame(l);
	auto now = make_frame(l, 8, -4);
	motion_native_info old_info, now_info;
	assert(build_motion_native(l, old, old_info) && build_motion_native(l, now, now_info));
	auto estimate = estimate_motion(l, old, old_info, now, now_info);
	assert(estimate.dx == 8 && estimate.dy == -4 && estimate.hits > 0);
	auto residual = motion_residual(l, old, old_info, now, now_info, estimate.dx, estimate.dy);
	assert(!residual.empty() && residual != now);
	assert(restore_motion(l, old, old_info, residual, estimate.dx, estimate.dy) && residual == now);
	for (auto [dx, dy]: {std::pair{16, 16}, std::pair{-16, -16}})
	{
		now = make_frame(l, dx, dy);
		assert(build_motion_native(l, now, now_info));
		residual = motion_residual(l, old, old_info, now, now_info, dx, dy);
		assert(!residual.empty() && restore_motion(l, old, old_info, residual, dx, dy) && residual == now);
	}
	compare_pair(l, old, make_frame(l, 8, -4));
	compare_pair(l, old, old);
	compare_pair(l, old, make_frame(l, 16, 16));
	for (auto [dx, dy]: {std::pair{7, -3}, std::pair{-5, 11}, std::pair{13, 2}})
		compare_pair(l, old, make_frame(l, dx, dy));
	auto flat = make_flat_frame(l);
	const auto flat_vector = compare_pair(l, flat, flat);
	assert(flat_vector.dx == 0 && flat_vector.dy == 0 && flat_vector.sad == 0);
	auto sparse_reference = old, sparse_current = make_frame(l, 8, -4);
	sparsify(l, sparse_reference, 0);
	sparsify(l, sparse_current, 1);
	compare_pair(l, sparse_reference, make_frame(l, 8, -4));
	compare_pair(l, old, sparse_current);
	bool varying_hits = false;
	compare_pair(l, sparse_reference, sparse_current, &varying_hits);
	assert(varying_hits);
	auto scene_cut = make_frame(l);
	invert_native(l, scene_cut);
	compare_pair(l, old, scene_cut);
	benchmark_motion(l, old, make_frame(l, 8, -4));
	auto aliased = make_frame(l, 0, 0, false, true);
	assert(parse_frame(l, aliased));
	assert(!build_motion_native(l, aliased, now_info));
	auto checker_layout = l;
	checker_layout.checkerboard = true;
	auto checker = make_frame(checker_layout, 0, 0, true);
	assert(parse_frame(checker_layout, checker));
	assert(!build_motion_native(checker_layout, checker, now_info));
	auto rgb565 = l;
	rgb565.packed_native = true;
	rgb565.motion = false;
	assert(!build_motion_native(rgb565, old, now_info));
	auto wrong_dimensions = l;
	wrong_dimensions.width = 544;
	assert(!build_motion_native(wrong_dimensions, old, now_info));
}

static void test_stream_versions()
{
	auto l = motion_layout();
	auto wire = stream_header(l, false, true, true);
	auto parsed = parse_stream(wire);
	assert(parsed && parsed->motion && parsed->predictor && !parsed->checkerboard);
	wire[5] |= 0x01;
	assert(!parse_stream(wire));
	auto old = l;
	old.motion = false;
	old.predictor = false;
	old.zstd = false;
	assert(parse_stream(stream_header(old, false, true, true)));
	old.checkerboard = true;
	auto checker_stream = parse_stream(stream_header(old, false, true, true));
	assert(checker_stream && checker_stream->checkerboard && !checker_stream->motion);
	old = l;
	old.motion_regions = true;
	auto regional_wire = stream_header(old, false, true, true);
	auto regional = parse_stream(regional_wire);
	assert(regional && regional->motion && regional->motion_regions && regional->predictor && !regional->checkerboard);
	assert(read32(regional_wire, 4) & 128u);
	old.motion = false;
	assert(stream_header(old, false, true, true).empty());
	old.motion = true;
	old.zstd = false;
	assert(stream_header(old, false, true, true).empty());
	auto invalid = l;
	invalid.packed_native = true;
	assert(stream_header(invalid, false, true, true).empty());
	invalid = l;
	invalid.checkerboard = true;
	assert(stream_header(invalid, false, true, true).empty());
}

static void test_cache()
{
	const auto l = motion_layout();
	const auto raw = make_frame(l);
	motion_reference_cache cache;
	assert(cache.put(l, 65530, raw));
	assert(cache.find(65530) && cache.find(65530)->raw == raw);
	assert(cache.put(l, 65530, raw)); // identical duplicate is idempotent
	auto altered = raw;
	altered[20] ^= 1;
	assert(!cache.put(l, 65530, altered)); // reference IDs cannot be rebound
	for (uint32_t i = 1; i <= 16; ++i)
		assert(cache.put(l, uint16_t(65530u + i), raw));
	assert(!cache.find(65530)); // seventeenth reference evicts oldest slot
	assert(cache.find(10)); // uint16 sequence wraps
	assert(!cache.put(l, 65520, raw)); // stale insertion rejected
	assert(cache.erase(10) && !cache.find(10));
	assert(!cache.erase(10));
	assert(!cache.put(layout{544, 512, 2, true, 256, false, true, true, false, true}, 11, raw));
	auto too_big = raw;
	too_big.resize(motion_cache_entry_limit + 1);
	assert(!cache.put(l, 11, too_big));
	cache.clear();
	assert(!cache.find(9));
	assert(cache.put(l, 7, raw));
	assert(!cache.put(l, 6, raw)); // reject out-of-order insertion
	cache.clear();
	assert(cache.put(l, 42, raw));
	auto unsupported = l;
	unsupported.motion = false;
	for (uint32_t i = 1; i <= 65536; ++i)
		assert(!cache.put(unsupported, uint16_t(42u + i), raw));
	assert(!cache.find(42)); // unsupported-frame notifications still expire old IDs after wrap
}

int main()
{
	test_wire();
	test_roundtrip_and_layout();
	test_stream_versions();
	test_cache();
	std::puts("direct motion: ok");
}
