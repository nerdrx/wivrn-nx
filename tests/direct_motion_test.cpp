#include "common/nxwarp_direct_motion.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace wivrn::nxwarp_direct;

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

static std::vector<uint8_t> make_frame(layout l, int shift_x = 0, int shift_y = 0, bool checker = false, bool overlap = false)
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
	wire[4] |= 0x80;
	assert(!parse_stream(wire));
	auto old = l;
	old.motion = false;
	old.predictor = false;
	old.zstd = false;
	assert(parse_stream(stream_header(old, false, true, true)));
	old.checkerboard = true;
	auto checker_stream = parse_stream(stream_header(old, false, true, true));
	assert(checker_stream && checker_stream->checkerboard && !checker_stream->motion);
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
