#define ZSTD_STATIC_LINKING_ONLY
#include "../common/nxastc_packet_decode.h"
#include "../server/encoder/nxastc_zstd_jobs.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <lz4.h>
#include <span>
#include <string_view>
#include <vector>

using Bytes = std::vector<uint8_t>;
using wivrn::nxastc_packet::compression;

static Bytes encode(const Bytes & raw, uint32_t width, uint32_t height, bool jobs, int level = 3)
{
	ZSTD_CCtx * ctx = ZSTD_createCCtx();
	assert(ctx);
	if (jobs)
	{
		assert(!ZSTD_isError(wivrn::nxastc_zstd_jobs::configure(ctx)));
		int actual = -1;
		assert(!ZSTD_isError(ZSTD_CCtx_getParameter(ctx, ZSTD_c_nbWorkers, &actual)) && actual == 2);
		assert(!ZSTD_isError(ZSTD_CCtx_getParameter(ctx, ZSTD_c_jobSize, &actual)) && actual == 512 * 1024);
		assert(!ZSTD_isError(ZSTD_CCtx_getParameter(ctx, ZSTD_c_overlapLog, &actual)) && actual == 1);
	}
	Bytes zstd(ZSTD_compressBound(raw.size()));
	const size_t zsize = wivrn::nxastc_zstd_jobs::compress(ctx, zstd.data(), zstd.size(), raw.data(), raw.size(), jobs, level);
	assert(!ZSTD_isError(zsize) && zsize);
	Bytes lz4(size_t(LZ4_compressBound(int(raw.size()))));
	size_t fallback_size = raw.size();
	const uint8_t * fallback = raw.data();
	compression fallback_encoding = compression::none;
	if (zsize > raw.size() / 2)
	{
		const int lsize = LZ4_compress_default(reinterpret_cast<const char *>(raw.data()),
		                                       reinterpret_cast<char *>(lz4.data()),
		                                       int(raw.size()),
		                                       int(lz4.size()));
		if (lsize > 0 && size_t(lsize) < raw.size())
		{
			fallback_size = size_t(lsize);
			fallback = lz4.data();
			fallback_encoding = compression::lz4;
		}
	}
	const bool select_zstd = zsize > 0 && uint64_t(zsize) * 100 <= uint64_t(fallback_size) * 90;
	const size_t payload_size = select_zstd ? zsize : fallback_size;
	const auto encoding = select_zstd ? compression::zstd : fallback_encoding;
	const auto header = wivrn::nxastc_packet::make_header(width, height, uint32_t(payload_size), encoding);
	Bytes packet(header.begin(), header.end());
	const uint8_t * payload = select_zstd ? zstd.data() : fallback;
	packet.insert(packet.end(), payload, payload + payload_size);
	ZSTD_freeCCtx(ctx);
	return packet;
}

static void exact_decode(const Bytes & packet, const Bytes & raw)
{
	auto h = wivrn::nxastc_packet::parse_packet(packet);
	assert(h && h->header_bytes == 24);
	Bytes decoded(raw.size());
	assert(wivrn::nxastc_packet::decode_payload(*h, std::span(packet).subspan(24), decoded) ==
	       wivrn::nxastc_packet::decode_status::ok);
	assert(decoded == raw);
}

static void legacy_packet_matches_direct_zstd(const Bytes & raw,
                                              uint32_t width,
                                              uint32_t height,
                                              const Bytes & packet,
                                              int level)
{
	Bytes payload(ZSTD_compressBound(raw.size()));
	ZSTD_CCtx * ctx = ZSTD_createCCtx();
	assert(ctx);
	const size_t n = ZSTD_compressCCtx(ctx, payload.data(), payload.size(), raw.data(), raw.size(), level);
	ZSTD_freeCCtx(ctx);
	assert(!ZSTD_isError(n) && n && n <= raw.size() / 2 && uint64_t(n) * 100 <= uint64_t(raw.size()) * 90);
	const auto header = wivrn::nxastc_packet::make_header(width, height, uint32_t(n), compression::zstd);
	Bytes expected(header.begin(), header.end());
	expected.insert(expected.end(), payload.begin(), payload.begin() + n);
	assert(expected == packet);
}

int main(int argc, char ** argv)
{
	if (argc == 2 && std::string_view(argv[1]) == "--no-mt")
	{
		ZSTD_CCtx * context = ZSTD_createCCtx();
		assert(context);
		const size_t configured = wivrn::nxastc_zstd_jobs::configure(context);
		assert(ZSTD_isError(configured));
		Bytes source(8192);
		for (size_t i = 0; i < source.size(); ++i)
			source[i] = uint8_t(i % 251);
		Bytes packed(ZSTD_compressBound(source.size()));
		const size_t packed_size = wivrn::nxastc_zstd_jobs::compress(
		        context, packed.data(), packed.size(), source.data(), source.size(), false, 3);
		assert(!ZSTD_isError(packed_size));
		Bytes decoded(source.size());
		assert(ZSTD_decompress(decoded.data(), decoded.size(), packed.data(), packed_size) == source.size());
		assert(decoded == source);
		ZSTD_freeCCtx(context);
		std::cout << "No-MT configure rejection and legacy compression fallback passed\n";
		return 0;
	}
	assert(argc == 1);
	constexpr uint32_t big_w = 2176, big_h = 2176;
	Bytes astc(size_t(wivrn::nxastc_packet::block_bytes(big_w, big_h)));
	for (size_t i = 0; i < astc.size(); ++i)
		astc[i] = uint8_t(i % 251);
	const auto legacy = encode(astc, big_w, big_h, false);
	const auto jobs = encode(astc, big_w, big_h, true);
	const auto fast_legacy = encode(astc, big_w, big_h, false, 1);
	legacy_packet_matches_direct_zstd(astc, big_w, big_h, legacy, 3);
	legacy_packet_matches_direct_zstd(astc, big_w, big_h, fast_legacy, 1);
	exact_decode(legacy, astc);
	exact_decode(jobs, astc);
	exact_decode(fast_legacy, astc);
	assert(legacy[4] == 2 && legacy[5] == uint8_t(compression::zstd));
	assert(jobs[4] == 2 && jobs[5] == uint8_t(compression::zstd));
	assert(fast_legacy[4] == 2 && fast_legacy[5] == uint8_t(compression::zstd));

	ZSTD_CCtx * persistent = ZSTD_createCCtx();
	assert(persistent && !ZSTD_isError(wivrn::nxastc_zstd_jobs::configure(persistent)));
	auto repeated = [&] {
		Bytes output(ZSTD_compressBound(astc.size()));
		const size_t n = wivrn::nxastc_zstd_jobs::compress(
		        persistent, output.data(), output.size(), astc.data(), astc.size(), true, 3);
		assert(!ZSTD_isError(n) && n);
		output.resize(n);
		int actual = -1;
		assert(!ZSTD_isError(ZSTD_CCtx_getParameter(persistent, ZSTD_c_nbWorkers, &actual)) && actual == 2);
		assert(!ZSTD_isError(ZSTD_CCtx_getParameter(persistent, ZSTD_c_jobSize, &actual)) && actual == 512 * 1024);
		assert(!ZSTD_isError(ZSTD_CCtx_getParameter(persistent, ZSTD_c_overlapLog, &actual)) && actual == 1);
		return output;
	};
	const auto first = repeated();
	const auto second = repeated();
	assert(first == second);
	ZSTD_freeCCtx(persistent);

	// High-entropy input below the 512 KiB job floor exercises q6 raw/LZ4 fallback.
	constexpr uint32_t small_w = 1024, small_h = 1024;
	Bytes incompressible(size_t(wivrn::nxastc_packet::block_bytes(small_w, small_h)));
	uint32_t state = 0x9e3779b9u;
	for (auto & b: incompressible)
	{
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		b = uint8_t(state);
	}
	const auto fallback = encode(incompressible, small_w, small_h, true);
	exact_decode(fallback, incompressible);
	const auto h = wivrn::nxastc_packet::parse_packet(fallback);
	assert(h && h->encoding == compression::none && fallback[4] == 1);
	std::cout << "Zstd jobs config, repeated frames, legacy L3/L1 packets, and sub-job-size fallback passed\n";
}
