/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "nxwarp_codec.h"

#include <cstdint>
#include <span>
#include <vector>

#include <nxvc/transport/common.h>
#include <nxvc/transport/receiver.h>
#include <nxvc/transport/sender.h>

namespace wivrn
{

// How an NX Warp frame is laid onto the transport's tile grid, and how it comes
// back off it. Both halves live here because they are one contract: the client's
// decoder has to implement the reassembly side exactly, and a comment in two
// repositories is not a contract.
//
// THE MAPPING, and why it is not one-tile-per-tile yet
// ----------------------------------------------------
// The transport carries tiles: opaque blobs with a directory entry each, packed
// into runs that fill an MTU. The codec is supposed to say where each tile's
// bytes start and end. The CPU reference codec's C ABI reports a tile's payload
// *length* but not its offset (nxvc_tile_info has no offset field, and
// nx-warp's own transport_loopback example fills synthetic bytes for exactly
// this reason), so this backend cannot hand out the real per-tile spans.
//
// So: the frame bitstream is cut into chunks of `chunk_bytes` and chunk i is
// placed at tile index i of the grid, in raster order. Everything else is real —
// the runs, the class-A parity, the pose header, the band deadlines, the
// feedback, the client shadow — and the bytes round-trip exactly. What is lost
// is per-tile independence: a chunk that never arrives costs the frame rather
// than one tile.
//
// This is a property of the reference codec backend, not of the wire format.
// When the Vulkan encoder lands behind nxwarp_codec it produces segments that
// are already datagram-sized and knows where every tile begins, and the mapping
// becomes the identity it was always meant to be. The client side does not
// change when that happens: it still reassembles tiles in index order.
//
// WHAT THE CLIENT MUST DO
// -----------------------
//   1. Parse the codec stream header out of the to_headset::nxwarp_datagram
//      whose path_id is 0xFF (nxvc_decoder_parse_stream_header). It arrives on
//      the control socket and is repeated every 90 frames.
//   2. Feed every other datagram to nxt::Receiver::on_datagram with its path_id.
//   3. Run nxt::Receiver::band_deadline per band and send the bytes it returns
//      back as from_headset::nxwarp_feedback.
//   4. Concatenate the delivered tiles of a frame in tile-index order, strip the
//      4-byte little-endian length prefix that chunk 0 carries and check the
//      frame really is that long — which is all nxwarp_reassemble below does —
//      then hand the result to nxvc_decoder_decode_frame. A frame with a hole,
//      or one shorter than its own prefix says, is not decodable by this backend
//      and must be dropped; the feedback still reports the hole, which is how the
//      encoder learns to refresh.

// The frame's byte length, little endian, in front of chunk 0. See
// nxwarp_send_frame: it is how the receiving side tells a complete frame from
// one whose tail chunks were lost.
inline constexpr size_t kFrameLenBytes = 4;

// Bytes of the frame carried by one transport tile.
//
// It is the run payload budget less one directory entry AND less the pose
// header: the first datagram of every band replicates the 26-byte pose header
// (kCapPoseHdr), and the packetizer counts that against the same budget. A tile
// sized to max_tile_bytes() therefore cannot start a band — the run comes back
// empty and the whole band is rejected as bad input — which is a trap worth
// having in one place rather than in two.
size_t nxwarp_chunk_bytes(const nxt::StreamConfig & cfg);

// The parity ladder this integration runs, per INTEGRATION-DECISIONS 5: a
// blended overhead target of 14.5 percent and a hard cap of 20.
//
// Every chunk is class A (see nxwarp_send_frame), so the blended overhead is
// just the class A ratio and 15 percent is what meets the target. The
// transport's own default re-derives the ladder each frame from measured
// headroom and loss and will happily spend more than the cap, so the caller also
// turns auto_fec off. Escalating to the decision's 4/2/1 under measured loss
// above 3 percent is a follow-up, and it needs the loss signal wired from
// WiVRn's own feedback first.
nxt::FecPolicy nxwarp_fec_policy();

// Cut `bitstream` onto the grid and hand each band to `sender`. Returns every
// datagram the sender produced, in transmission order. `descs` are the codec's
// per-tile records, attached to the directory entries in tile order.
std::vector<nxt::Datagram> nxwarp_send_frame(nxt::Sender & sender,
                                             const nxt::StreamConfig & cfg,
                                             uint16_t frame_id,
                                             const nxt::PoseHeader & pose,
                                             std::span<const uint8_t> bitstream,
                                             std::span<const nxwarp_tile_desc> descs,
                                             size_t chunk_bytes,
                                             uint32_t base_qp,
                                             uint32_t now_us,
                                             uint16_t enc_us);

// The inverse. `tiles` are everything Receiver::on_datagram delivered for one
// frame, in any order. Returns the frame's bytes with the length prefix already
// stripped, or an empty vector if the run of tile indices from 0 has a hole in
// it, if a tile other than the last is short, or if fewer bytes arrived than the
// prefix says the frame is — the three shapes a lost chunk can take.
std::vector<uint8_t> nxwarp_reassemble(const nxt::StreamConfig & cfg,
                                       std::span<const nxt::TileOutput> tiles,
                                       size_t chunk_bytes);

} // namespace wivrn
