#include "../common/nxastc_packet_decode.h"

#include <cassert>
#include <algorithm>
#include <cstdio>
#include <lz4.h>
#include <utility>
#include <vector>

using namespace wivrn::nxastc_packet;

static std::vector<uint8_t> packet_bytes(uint32_t width, uint32_t height, compression encoding, std::span<const uint8_t> payload)
{
	auto header = make_header(width, height, uint32_t(payload.size()), encoding);
	std::vector<uint8_t> packet(header.begin(), header.end());
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

int main()
{
	constexpr uint32_t width = 128, height = 128;
	const size_t raw_size = size_t(block_bytes(width, height));
	std::vector<uint8_t> raw(raw_size);
	for (size_t i = 0; i < raw.size(); ++i)
		raw[i] = uint8_t((i / 16) % 7);
	std::vector<uint8_t> output(raw_size);

	// The shared decoder scratch buffer is sized for 4x4, but each packet
	// must decode into exactly its own footprint-sized prefix.
	std::vector<uint8_t> scratch(block_bytes(width, height, 4));
	for (uint8_t block : {4, 6, 8})
	{
		std::vector<uint8_t> source(block_bytes(width, height, block), block);
		std::vector<uint8_t> packed(ZSTD_compressBound(source.size()));
		const size_t n = ZSTD_compress(packed.data(), packed.size(), source.data(), source.size(), 3);
		assert(!ZSTD_isError(n) && n < source.size());
		packed.resize(n);
		auto header = make_header(width, height, uint32_t(n), compression::zstd, block);
		std::vector<uint8_t> packet(header.begin(), header.end());
		packet.insert(packet.end(), packed.begin(), packed.end());
		auto h = parse_packet(packet);
		assert(h && h->block == block);
		auto output_prefix = std::span<uint8_t>(scratch).first(h->raw_bytes);
		assert(decode_payload(*h, packed, output_prefix) == decode_status::ok);
		assert(std::equal(source.begin(), source.end(), output_prefix.begin()));
		if (block != 4)
			assert(decode_payload(*h, packed, scratch) == decode_status::length_mismatch);
	}

	// v1 raw remains byte-for-byte parseable and decodable.
	auto raw_packet = packet_bytes(width, height, compression::none, raw);
	assert(raw_packet[4] == 1 && raw_packet[5] == 0);
	auto parsed = parse_packet(raw_packet);
	assert(parsed && parsed->encoding == compression::none);
	assert(decode_payload(*parsed, std::span<const uint8_t>(raw_packet).subspan(header_size), output) == decode_status::ok);
	assert(output == raw);
	assert(decode_payload(*parsed, std::span<const uint8_t>(raw_packet).subspan(header_size), std::span<uint8_t>(output).first(output.size()-1)) == decode_status::length_mismatch);
	auto truncated_raw = raw_packet;
	truncated_raw.pop_back();
	assert(!parse_packet(truncated_raw));

	// v1 LZ4 remains the legacy wire representation.
	std::vector<uint8_t> lz4(LZ4_compressBound(int(raw.size())));
	const int lz4_size = LZ4_compress_default(reinterpret_cast<const char *>(raw.data()), reinterpret_cast<char *>(lz4.data()), int(raw.size()), int(lz4.size()));
	assert(lz4_size > 0 && size_t(lz4_size) < raw.size());
	lz4.resize(size_t(lz4_size));
	auto lz4_packet = packet_bytes(width, height, compression::lz4, lz4);
	assert(lz4_packet[4] == 1 && lz4_packet[5] == 1);
	parsed = parse_packet(lz4_packet);
	assert(parsed && parsed->encoding == compression::lz4);
	assert(decode_payload(*parsed, std::span<const uint8_t>(lz4_packet).subspan(header_size), output) == decode_status::ok);
	assert(output == raw);
	auto bad_lz4_packet = lz4_packet;
	bad_lz4_packet[header_size] ^= 0xff;
	parsed = parse_packet(bad_lz4_packet);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(bad_lz4_packet).subspan(header_size), output) != decode_status::ok);

	// v2/flag 2 Zstd round trip.
	std::vector<uint8_t> zstd(ZSTD_compressBound(raw.size()));
	const size_t zstd_size = ZSTD_compress(zstd.data(), zstd.size(), raw.data(), raw.size(), 3);
	assert(!ZSTD_isError(zstd_size) && zstd_size < raw.size());
	zstd.resize(zstd_size);
	auto zstd_packet = packet_bytes(width, height, compression::zstd, zstd);
	assert(zstd_packet[4] == 2 && zstd_packet[5] == 2);
	parsed = parse_packet(zstd_packet);
	assert(parsed && parsed->encoding == compression::zstd);
	assert(decode_payload(*parsed, std::span<const uint8_t>(zstd_packet).subspan(header_size), output) == decode_status::ok);
	assert(output == raw);

	// Only v1 flags 0/1 and v2 flag 2 are accepted.
	for (auto [version, flag] : {std::pair<uint8_t,uint8_t>{1,2}, {2,0}, {2,1}, {3,0}})
	{
		auto bad = raw_packet;
		bad[4] = version;
		bad[5] = flag;
		assert(!parse_packet(bad));
	}

	// Reject truncation, trailing data, and concatenated frames.
	auto truncated = zstd;
	truncated.pop_back();
	auto truncated_packet = packet_bytes(width, height, compression::zstd, truncated);
	parsed = parse_packet(truncated_packet);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(truncated_packet).subspan(header_size), output) == decode_status::zstd_bad_frame);

	for (const auto & suffix : {std::vector<uint8_t>{0}, zstd})
	{
		auto trailing = zstd;
		trailing.insert(trailing.end(), suffix.begin(), suffix.end());
		auto trailing_packet = packet_bytes(width, height, compression::zstd, trailing);
		parsed = parse_packet(trailing_packet);
		assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(trailing_packet).subspan(header_size), output) == decode_status::zstd_trailing_data);
	}

	// Reject frame content sizes that are unknown or don't equal raw ASTC size.
	ZSTD_CCtx * context = ZSTD_createCCtx();
	assert(context);
	assert(!ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 0)));
	std::vector<uint8_t> unknown_size(ZSTD_compressBound(raw.size()));
	const size_t unknown_len = ZSTD_compress2(context, unknown_size.data(), unknown_size.size(), raw.data(), raw.size());
	assert(!ZSTD_isError(unknown_len));
	unknown_size.resize(unknown_len);
	ZSTD_freeCCtx(context);
	auto unknown_packet = packet_bytes(width, height, compression::zstd, unknown_size);
	parsed = parse_packet(unknown_packet);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(unknown_packet).subspan(header_size), output) == decode_status::zstd_unknown_content_size);

	std::vector<uint8_t> short_raw(raw.begin(), raw.end() - 1);
	std::vector<uint8_t> wrong_size(ZSTD_compressBound(short_raw.size()));
	const size_t wrong_size_len = ZSTD_compress(wrong_size.data(), wrong_size.size(), short_raw.data(), short_raw.size(), 1);
	assert(!ZSTD_isError(wrong_size_len));
	wrong_size.resize(wrong_size_len);
	auto wrong_size_packet = packet_bytes(width, height, compression::zstd, wrong_size);
	parsed = parse_packet(wrong_size_packet);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(wrong_size_packet).subspan(header_size), output) == decode_status::zstd_wrong_content_size);
	assert(decode_payload(*parsed, std::span<const uint8_t>(wrong_size_packet).subspan(header_size), std::span<uint8_t>(output).first(output.size()-1)) == decode_status::length_mismatch);

	// Old bool overload remains v1 raw/LZ4 compatible.
	auto legacy = make_header(width, height, uint32_t(raw.size()), false);
	assert(legacy[4] == 1 && legacy[5] == 0);
	legacy = make_header(width, height, uint32_t(lz4.size()), true);
	assert(legacy[4] == 1 && legacy[5] == 1);

	std::puts("NX ASTC v1/v2 raw, LZ4, Zstd, malformed, and frame-boundary tests: PASS");
}
