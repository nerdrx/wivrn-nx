// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxwarp_atlas_receipt.h"
#include <array>
#include <cassert>

struct Tile
{
	uint32_t index;
	uint8_t mode;
};
int main()
{
	using namespace wivrn;
	// Deliberately sparse, unordered, and including an out-of-range descriptor.
	const std::array<Tile, 4> tiles{{{3, 0}, {1, 3}, {0, 0}, {99, 0}}};
	std::vector<uint8_t> skips;
	nxwarp_record_intentional_skips(skips, 4, true, std::span<const Tile>(tiles));
	assert(nxwarp_tile_received(true, skips, 3));  // intentional absence
	assert(!nxwarp_tile_received(true, skips, 1)); // coded packet lost
	assert(!nxwarp_tile_received(true, skips, 2)); // unknown descriptor
	assert(nxwarp_tile_received(false, skips, 1)); // coded packet received
	assert(!nxwarp_tile_received(true, skips, 99));
	nxwarp_record_intentional_skips(skips, 4, false, std::span<const Tile>(tiles));
	assert(skips.empty()); // PICTURE with skip descriptors is not ATLAS.
	assert(!nxwarp_tile_received(true, skips, 3));
	// A reconnect/reset bypasses this helper and supplies an all-zero map;
	// no test here claims to exercise that separate server branch.
}
