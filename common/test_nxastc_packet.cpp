#include "nxastc_packet.h"
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
	bool rejected = false;
	try { (void)make_header(UINT32_MAX, UINT32_MAX, 1, true); }
	catch (const std::invalid_argument &) { rejected = true; }
	assert(rejected);
}
