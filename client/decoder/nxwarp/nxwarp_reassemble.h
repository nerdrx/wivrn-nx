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
// The short version, and it is the same sentence under both mappings the server can send:
// the tiles that arrived, concatenated in tile-index order, are the frame's bytes, with a
// 4-byte little-endian length prefix in front. Reassembly is that concatenation and the
// length check, and nothing else.
//
// The two mappings differ only in where the tile boundaries fall. Under the CHUNK mapping
// the frame bitstream is cut into `chunk_bytes` pieces and piece i rides tile index i, so
// a frame is a prefix of the grid and a lost datagram costs the whole frame. Under the
// PER-TILE SPAN mapping a codec tile's own bytes ride its own index, so a lost datagram
// costs the tiles it carried. Nothing on the wire says which was used and nothing here
// needs to know: the length prefix is exact under both.
//
// What the span mapping DID change here, and it was not the algorithm:
//
//   * the prefix rides the lowest tile carrying bytes, not tile 0, because a frame that
//     did not code tile 0 puts its leading bytes on the first tile it did;
//   * the two per-tile loss tests — no hole in the index run from 0, no short tile but the
//     last — are gone. They were the chunk mapping's way of noticing a loss and are WRONG
//     under spans, where a tile the frame did not code carries nothing and is
//     indistinguishable from one that was lost;
//   * and the cost model. A frame used to be ~45 tiles of a 578-tile stereo grid and is
//     now all of it, so anything here that was O(grid) is now O(grid) per frame at the
//     headset's frame rate. There is one grid walk, it is in scan(), and both entry points
//     share it; the buffer is sized to the frame rather than to the grid. See the notes in
//     the .cpp — both of those were measured regressions on a Pico 4, not tidying.

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
