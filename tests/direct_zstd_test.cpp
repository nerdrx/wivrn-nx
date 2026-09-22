#include "nxwarp_direct_zstd.h"

#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

using namespace wivrn::nxwarp_direct;

int main()
{
	const layout l{512, 512, 2, true};
	std::vector<uint8_t> raw(120000, 0x5a), packed, decoded;
	for (uint32_t i = 0; i < raw.size(); i += 4096)
		raw[i] = uint8_t(i);
	const auto wire = compress_zstd(raw, packed);
	assert(wire.data() == packed.data() && wire.size() < raw.size());
	assert(is_zstd(wire));
	assert(decompress_zstd(l, wire, decoded) && decoded == raw);

	std::mt19937 rng(7);
	std::vector<uint8_t> incompressible(120000);
	for (auto & byte : incompressible)
		byte = uint8_t(rng());
	const auto fallback = compress_zstd(incompressible, packed);
	assert(fallback.data() == incompressible.data() && fallback.size() == incompressible.size());

	std::vector<uint8_t> bad(wire.begin(), wire.end());
	bad.pop_back();
	assert(!decompress_zstd(l, bad, decoded));
	bad.assign(wire.begin(), wire.end());
	bad[12]++;
	assert(!decompress_zstd(l, bad, decoded));
	bad.assign(wire.begin(), wire.end());
	bad[20] ^= 1;
	assert(!decompress_zstd(l, bad, decoded));
	bad.assign(wire.begin(), wire.end());
	bad.push_back(0);
	bad[12]++;
	assert(!decompress_zstd(l, bad, decoded));
	bad.assign(wire.begin(), wire.end());
	for (unsigned k = 0; k < 4; ++k)
		bad[8 + k] = uint8_t((layout{4096, 4096, 2, true}.max_frame_bytes() + 1u) >> (8 * k));
	assert(!decompress_zstd(l, bad, decoded));
	bad.assign(wire.begin(), wire.end());
	bad[4] = 2;
	assert(!decompress_zstd(l, bad, decoded));
	return 0;
}
