/*
 * WiVRn VR streaming — NX Warp codec
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

// The receiving half of the frame-to-tile-grid mapping.
//
// This is the exact inverse of `nxwarp_send_frame` on the server, and the two are one
// contract: the normative description is the header comment of
// `server/encoder/nxwarp_packetize.h`, and this file implements what it says the client
// must do. The two implementations are deliberately identical line for line — the server
// tree carries the same pair of functions — because a mapping that is described in two
// places and implemented once is a mapping that will drift.
//
// The short version: the codec's frame bitstream is cut into `chunk_bytes` pieces and
// chunk i is carried by tile index i of the transport's grid, in raster order. Chunk 0
// carries a 4-byte little-endian length prefix in front of the frame. Reassembly is a
// concatenation in tile-index order, a length check, and nothing else.
//
// Why it is not one tile per tile yet: the CPU reference codec's C ABI reports a tile's
// payload length but not its offset in the frame, so the server cannot hand the transport
// real per-tile spans. Everything else on the path is real — the runs, the class-A
// parity, the pose header, the band deadlines, the feedback, the client shadow — and the
// bytes round-trip exactly; what is lost is per-tile independence, so a chunk that never
// arrives costs the frame rather than one tile. When the Vulkan encoder lands behind the
// server's nxwarp_codec the mapping becomes the identity, and **this file does not
// change**: it still reassembles tiles in index order.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <nxvc/transport/common.h>

namespace wivrn::nxwarp_wire
{
// Why the last reassemble() on this thread returned empty, for the two-second report.
struct reassemble_report
{
	uint32_t expected = 0; // highest chunk index seen + 1
	uint32_t present = 0;
	uint32_t first_missing = UINT32_MAX;
	uint32_t out_of_range = 0; // tile indices past the grid
	bool short_chunk = false;  // a non-last chunk shorter than chunk_bytes
};
reassemble_report last_report();


// The frame's byte length, little endian, in front of chunk 0.
inline constexpr size_t kFrameLenBytes = 4;

// Bytes of the frame carried by one transport tile: the run payload budget less one
// directory entry and less the pose header, which the packetizer charges to the same
// budget on the first datagram of every band.
size_t chunk_bytes(const nxt::StreamConfig & cfg);

// True when `by_index` already holds a whole frame: the run of tile indices from 0 has no
// hole, no chunk before the last is short, and the length prefix on chunk 0 is covered by
// the bytes that arrived. Exactly the conditions reassemble() checks -- it calls this
// first -- but without building the frame. It fills last_report() either way, so a frame
// that is not here yet can still say what it is waiting for.
//
// The windowed reassembler in nxwarp_decoder needs this because "the last run of the frame
// arrived" is not the same statement as "the frame is here": on a link that reorders, the
// datagram carrying the last run routinely overtakes an earlier one, and a frame closed on
// the flag alone is closed with a hole it was about to fill.
bool is_complete(const nxt::StreamConfig & cfg,
                 std::span<const std::vector<uint8_t>> by_index,
                 size_t chunk);

// `tiles` is everything Receiver::on_datagram delivered for one frame, in any order.
// Returns the frame's bytes with the length prefix stripped, or an empty vector if the run
// of tile indices from 0 has a hole in it, if a tile other than the last is short, or if
// fewer bytes arrived than the prefix says the frame is.
std::vector<uint8_t> reassemble(const nxt::StreamConfig & cfg,
                                std::span<const std::vector<uint8_t>> by_index,
                                size_t chunk);

} // namespace wivrn::nxwarp_wire
