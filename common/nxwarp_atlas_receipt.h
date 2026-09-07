// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace wivrn
{
// NX Warp and transport both assign WARP_SKIP mode zero. A skip retains an
// existing atlas position; the absence of its packet is not a lost coded tile.
// This does not confirm a source generation: the encoder still checks atlas
// validity and held acknowledgements before admitting a skip. Full resets
// bypass this map and continue to force every tile intra.
inline constexpr uint8_t nxwarp_warp_skip_mode = 0;

template <typename Tile>
void nxwarp_record_intentional_skips(std::vector<uint8_t> & skips, uint32_t count, bool atlas, std::span<const Tile> tiles)
{
	skips.clear();
	if (!atlas)
		return;         // PICTURE and ordinary streams retain receipt semantics.
	skips.resize(count, 0); // Missing descriptors are never assumed skipped.
	for (const auto & tile: tiles)
		if (tile.index < count)
			skips[tile.index] = tile.mode == nxwarp_warp_skip_mode;
}

inline bool nxwarp_tile_received(bool concealed, std::span<const uint8_t> skips, uint32_t tile)
{
	return !concealed || (tile < skips.size() && skips[tile]);
}
} // namespace wivrn
