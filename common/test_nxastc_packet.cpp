#include "nxastc_packet.h"
#include <algorithm>
#include <cassert>
#include <vector>

int main()
{
	using namespace wivrn::nxastc_packet;
	for (bool compressed : {false, true})
	{
		const uint32_t raw = uint32_t(block_bytes(17, 9));
		assert(raw == 96);
		const uint32_t size = compressed ? raw / 2 : raw;
		auto bytes = make_header(17, 9, size, compressed);
		std::vector<uint8_t> packet(bytes.begin(), bytes.end());
		packet.resize(header_size + size);
		auto h = parse_packet(packet);
		assert(h && h->width == 17 && h->height == 9 && h->raw_bytes == raw &&
		       h->encoding == (compressed ? compression::lz4 : compression::none));
		packet.pop_back();
		assert(!parse_packet(packet));
		packet.push_back(0);
		packet[5] = 2;
		assert(!parse_packet(packet));
		packet[5] = compressed;
		packet[16] ^= 16;
		assert(!parse_packet(packet));
	}
	assert(!parse_packet({}));
	for (uint8_t block : {4, 6, 8})
	{
		const uint32_t raw = uint32_t(block_bytes(17, 9, block));
		auto bytes = make_header(17, 9, raw, compression::none, block);
		assert(bytes[4] == (block == 8 ? 1 : 3));
		assert(bytes[6] == (block == 8 ? 0 : block) && bytes[7] == 0);
		std::vector<uint8_t> packet(bytes.begin(), bytes.end());
		packet.resize(header_size + raw);
		auto h = parse_packet(packet);
		assert(h && h->block == block && h->raw_bytes == raw);
		packet[6] = 5;
		assert(!parse_packet(packet));
		packet[6] = block;
		packet[7] = 1;
		assert(!parse_packet(packet));
	}
	{
		const auto raw = uint32_t(block_bytes(17, 9, 4));
		auto bytes = make_header(17, 9, raw / 2, compression::zstd, 4);
		assert(bytes[4] == 3 && bytes[5] == 2 && bytes[6] == 4);
		std::vector<uint8_t> packet(bytes.begin(), bytes.end());
		packet.resize(header_size + raw / 2);
		auto h = parse_packet(packet);
		assert(h && h->block == 4 && h->encoding == compression::zstd);
	}
	{
		auto bytes = make_header(8, 8, uint32_t(block_bytes(8, 8)), compression::none);
		std::vector<uint8_t> packet(bytes.begin(), bytes.end());
		packet.resize(header_size + block_bytes(8, 8));
		assert(parse_packet(packet)->block == 8);
		bytes[4] = 2;
		bytes[5] = 2;
		std::copy(bytes.begin(), bytes.end(), packet.begin());
		assert(parse_packet(packet)->block == 8);
	}
	auto malformed = make_header(8, 8, uint32_t(block_bytes(8, 8)), compression::none);
	malformed[6] = 4;
	std::vector<uint8_t> malformed_packet(malformed.begin(), malformed.end());
	malformed_packet.resize(header_size + block_bytes(8, 8));
	assert(!parse_packet(malformed_packet));
	bool rejected = false;
	try { (void)make_header(UINT32_MAX, UINT32_MAX, 1, true); }
	catch (const std::invalid_argument &) { rejected = true; }
	assert(rejected);
}
