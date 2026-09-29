/* Compile: c++ -std=c++20 -Wall -Wextra -Werror -Icommon tests/nxwarp_jpeg_packet_test.cpp -o /tmp/nxwarp-jpeg-packet-test && /tmp/nxwarp-jpeg-packet-test */

#include "nxwarp_jpeg_packet.h"

#include <algorithm>
#include <cassert>
#include <vector>

int main()
{
	std::vector<uint8_t> jpeg(25'001, 0x5a);
	const auto count = uint16_t((jpeg.size() + wivrn::nxwarp_jpeg_chunk_size - 1) / wivrn::nxwarp_jpeg_chunk_size);
	size_t sent = 0;
	for (uint16_t i = 0; i < count; ++i)
	{
		const size_t bytes = std::min(wivrn::nxwarp_jpeg_chunk_size, jpeg.size() - sent);
		auto packet = wivrn::nxwarp_jpeg_chunk_payload(0x12345678, 1, 1088, 1088, i, count,
		                                                  uint32_t(jpeg.size()), std::span(jpeg).subspan(sent, bytes));
		assert(!packet.empty());
		assert(packet.size() <= wivrn::nxwarp_jpeg_payload_limit);
		assert(packet[0] == 'N' && packet[1] == 'X' && packet[2] == 'J' && packet[3] == '2');
		assert(packet[4] == 0x12 && packet[7] == 0x78 && packet[8] == 1);
		assert(packet[13] == 0x04 && packet[14] == 0x40 && packet[15] == 0x04 && packet[16] == 0x40);
		sent += bytes;
	}
	assert(sent == jpeg.size());
	assert(wivrn::nxwarp_jpeg_chunk_payload(1, 0, 544, 544, 0, count, uint32_t(jpeg.size()),
	                                        std::span(jpeg).first(wivrn::nxwarp_jpeg_chunk_size)).empty());
}
