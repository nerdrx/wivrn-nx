#include "nxwarp_direct_zstd.h"
#include "nxwarp_direct_safety.h"
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <vector>

using namespace wivrn::nxwarp_direct;

int main()
{
	auto require = [](bool ok, const char * what) {
		if (!ok)
		{
			std::fprintf(stderr, "FAIL: %s\n", what);
			std::exit(1);
		}
	};
	const layout predicted{256, 256, 2, true, 256, false, true, true};
	const layout legacy{256, 256, 2, true, 256, false, true, false};
	std::vector<uint8_t> raw(64 * 1024);
	for (size_t i = 0; i < raw.size(); ++i)
		raw[i] = uint8_t((i / 4) ^ (i >> 9));
	std::vector<uint8_t> packed, scratch, decoded;
	auto v2 = compress_zstd_predicted(raw, packed, scratch);
	require(is_zstd(v2) && read32(v2, 4) == 2, "predicted envelope");
	const std::vector<uint8_t> predicted_wire(v2.begin(), v2.end());
	require(decompress_zstd(predicted, v2, decoded) && decoded == raw, "predicted roundtrip");
	require(!decompress_zstd(legacy, v2, decoded), "legacy rejects predicted envelope");
	for (size_t n = 0; n < predicted_wire.size(); ++n)
		require(!decompress_zstd(predicted, std::span<const uint8_t>(predicted_wire).first(n), decoded), "truncated predicted envelope");
	std::mt19937 rng(7);
	std::vector<uint8_t> random(raw.size());
	for (auto & b : random) b = uint8_t(rng());
	packed.clear(); scratch.clear();
	const auto fallback = compress_zstd_predicted(random, packed, scratch);
	require(!is_zstd(fallback), "incompressible predictor fallback");
	require(fallback.size() == random.size(), "fallback preserves raw size");
	packed.clear();
	const auto v1 = compress_zstd(raw, packed);
	require(is_zstd(v1) && read32(v1, 4) == 1, "legacy envelope");
	require(decompress_zstd(legacy, v1, decoded) && decoded == raw, "legacy roundtrip");
	// Exercise the actual safety wrapper path around a predicted detail unit.
	const layout full{256, 256, 2, true, 256, false, true, true};
	const layout low{64, 64, 2};
	std::vector<uint8_t> safety_wire;
	for (uint32_t v : {safety_magic, 1u, low.width, low.height, low.eyes, 16u, uint32_t(v2.size()), 0u})
		append32(safety_wire, v);
	safety_wire.resize(safety_header_bytes + 16u, 0);
	safety_wire.insert(safety_wire.end(), predicted_wire.begin(), predicted_wire.end());
	const auto safety = parse_safety_header(full, safety_wire);
	require(safety && safety->prefix_bytes() == safety_header_bytes + 16u &&
	                safety->total_bytes() == safety_wire.size(),
	        "safety wrapper bounds");
	const auto detail = std::span<const uint8_t>(safety_wire).subspan(safety->prefix_bytes());
	require(decompress_zstd(full, detail, decoded) && decoded == raw, "safety predicted detail roundtrip");
	for (size_t n = 0; n < safety_header_bytes; ++n)
		require(!parse_safety_header(full, std::span<const uint8_t>(safety_wire).first(n)), "truncated safety wrapper rejected");
	const auto old_h = stream_header(legacy, false, true, true);
	require(read32(old_h, 4) == 13 && parse_stream(old_h) && !parse_stream(old_h)->predictor, "legacy stream version");
	const auto new_h = stream_header(predicted, false, true, true);
	require(read32(new_h, 4) == 17 && parse_stream(new_h) && parse_stream(new_h)->predictor && !parse_stream(new_h)->packed_native, "RGB888 predictor stream version");
	const layout packed_old{256, 256, 2, true, 256, true, true, false};
	const layout packed_new{256, 256, 2, true, 256, true, true, true};
	require(read32(stream_header(packed_old, false, true, true), 4) == 15, "RGB565 legacy stream version");
	const auto packed_new_h = stream_header(packed_new, false, true, true);
	require(read32(packed_new_h, 4) == 19 && parse_stream(packed_new_h) && parse_stream(packed_new_h)->packed_native, "RGB565 predictor stream version");
	auto unsupported = new_h;
	unsupported[4] = 21;
	require(!parse_stream(unsupported), "unsupported stream version");
	require(!parse_stream(std::vector<uint8_t>(32, 0)), "invalid stream header");
	for (uint32_t version = 1; version <= 6; ++version)
	{
		const layout l{256, 256, 2};
		auto h = stream_header(l, false, version >= 3, version >= 5);
		h[4] = uint8_t(version);
		require(parse_stream(h).has_value(), "legacy stream parse");
	}
	for (uint32_t version = 7; version <= 12; ++version)
	{
		const bool packed = version >= 11;
		const uint32_t side = version >= 9 ? 256u : 128u;
		const layout l{256, 256, 2, true, side, packed, false, false};
		auto h = stream_header(l, false, true, true);
		h[4] = uint8_t(version);
		require(parse_stream(h).has_value(), "native stream parse");
	}
	for (uint32_t version = 13; version <= 20; ++version)
	{
		const bool packed = version >= 15 && (version <= 16 || version >= 19);
		const bool predictor = version >= 17;
		const layout l{256, 256, 2, true, 256, packed, true, predictor};
		auto h = stream_header(l, false, true, true);
		h[4] = uint8_t(version);
		require(parse_stream(h).has_value(), "zstd predictor stream parse");
	}
	std::puts("NXDZ v2 predictor roundtrip/bounds/fallback: PASS");
}
