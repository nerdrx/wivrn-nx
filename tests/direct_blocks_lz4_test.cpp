#include "nxwarp_direct_lz4.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <vector>

using namespace wivrn::nxwarp_direct;
using Bytes = std::vector<uint8_t>;

static void put(Bytes & b, uint32_t v)
{
	append32(b, v);
}
static void field(Bytes & b, size_t p, uint32_t v)
{
	for (unsigned k = 0; k < 4; ++k)
		b[p + k] = uint8_t(v >> (8 * k));
}

static Bytes frame(layout l, bool random_blocks)
{
	const uint32_t n = l.tile_count(), words_per_tile = 80;
	Bytes out = frame_header(n, n * words_per_tile);
	for (uint32_t i = 0; i < n; ++i)
		put(out, (0u << 30) | i * words_per_tile);
	std::mt19937 rng(7);
	for (uint32_t i = 0; i < n * words_per_tile; ++i)
		put(out, random_blocks ? rng() : 0);
	assert(parse_frame(l, out));
	return out;
}

static void expect_bad(layout l, Bytes b)
{
	Bytes out;
	assert(!decompress_lz4(l, b, out));
}

int main()
{
	const layout small{32, 32, 1}, big{1024, 1024, 1};
	const Bytes raw = frame(big, false);
	Bytes packed;
	auto encoded = compress_lz4(raw, packed);
	assert(encoded.data() == packed.data());
	Bytes round;
	assert(decompress_lz4(big, encoded, round) && round == raw);

	// Random payload is intentionally incompressible: compressor must return raw span.
	const Bytes noise = frame(big, true);
	Bytes noise_out;
	auto raw_fallback = compress_lz4(noise, noise_out);
	assert(raw_fallback.data() == noise.data() && raw_fallback.size() == noise.size());
	assert(!is_lz4(raw_fallback));

	// Two independent chunks exercise compressed first chunk plus raw later chunk.
	const layout mixed_layout{512, 512, 1};
	Bytes mixed_raw = frame(mixed_layout, false);
	std::mt19937 mixed_rng(99);
	for (size_t i = 65536; i < mixed_raw.size(); ++i)
		mixed_raw[i] = uint8_t(mixed_rng());
	Bytes mixed_container;
	auto mixed = compress_lz4(mixed_raw, mixed_container);
	assert(mixed.data() == mixed_container.data());
	bool saw_raw = false, saw_compressed = false;
	size_t cursor = 16;
	for (uint32_t i = 0; i < read32(mixed, 12); ++i)
	{
		uint32_t stored = read32(mixed, cursor + 4), flags = read32(mixed, cursor + 8);
		saw_raw |= flags == 0;
		saw_compressed |= flags == 1;
		cursor += 12 + stored;
	}
	assert(saw_raw && saw_compressed);
	Bytes mixed_round;
	assert(decompress_lz4(mixed_layout, mixed, mixed_round) && mixed_round == mixed_raw);

	for (size_t cut = 0; cut < encoded.size(); ++cut)
		expect_bad(big, Bytes(encoded.begin(), encoded.begin() + cut));
	Bytes trailing(mixed.begin(), mixed.end());
	trailing.push_back(0);
	expect_bad(mixed_layout, trailing);
	Bytes truncated(mixed.begin(), mixed.end() - 1);
	expect_bad(mixed_layout, truncated);
	Bytes count_bad(mixed.begin(), mixed.end());
	field(count_bad, 12, 99);
	expect_bad(mixed_layout, count_bad);
	Bytes flags_bad(mixed.begin(), mixed.end());
	field(flags_bad, 24, 2);
	expect_bad(mixed_layout, flags_bad);
	Bytes length_bad(mixed.begin(), mixed.end());
	field(length_bad, 20, 0);
	expect_bad(mixed_layout, length_bad);
	Bytes stored_bad(mixed.begin(), mixed.end());
	field(stored_bad, 24 - 4, 0xffffffffu);
	expect_bad(mixed_layout, stored_bad);
	Bytes invalid_lz4(mixed.begin(), mixed.end());
	if (read32(invalid_lz4, 24) < read32(invalid_lz4, 20))
		invalid_lz4[28] ^= 0x5a;
	expect_bad(mixed_layout, invalid_lz4);

	Bytes oversized;
	put(oversized, lz4_magic);
	put(oversized, 1);
	put(oversized, small.max_frame_bytes() + 1);
	put(oversized, 1);
	expect_bad(small, oversized);
	Bytes too_short{0x4e, 0x58, 0x44};
	Bytes out;
	assert(!decompress_lz4(small, too_short, out));

	for (uint32_t v: {3u, 4u})
	{
		Bytes h = stream_header(small, v == 4, true);
		assert(read32(h, 4) == v);
		assert(parse_stream(h));
	}
	Bytes version99 = stream_header(small);
	field(version99, 4, 99);
	assert(!parse_stream(version99));
	std::puts("direct LZ4 envelope: ok");
}
