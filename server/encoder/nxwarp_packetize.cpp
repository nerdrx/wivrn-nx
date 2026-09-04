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

#include "nxwarp_packetize.h"

#include <algorithm>
#include <cmath>

size_t wivrn::nxwarp_chunk_bytes(const nxt::StreamConfig & cfg)
{
	const size_t budget = cfg.run_payload_budget();
	const size_t reserve = nxt::kDirEntryBytes + ((cfg.caps & nxt::kCapPoseHdr) ? nxt::kPoseHeaderBytes : 0);
	return budget > reserve ? budget - reserve : 0;
}

nxt::FecPolicy wivrn::nxwarp_fec_policy()
{
	nxt::FecPolicy p;
	p.ratio_pct[0] = 15;
	p.ratio_pct[1] = 0;
	p.ratio_pct[2] = 0;
	// One parity block even for a short group: a band of two datagrams is still a
	// whole frame's worth of bytes under the chunk mapping, and a group with no
	// parity at all is a group FEC cannot help. It does push a very small frame's
	// overhead above the cap; that is a low-resolution bring-up artefact, not the
	// shape at 2048 squared where a band is dozens of datagrams.
	p.min_parity[0] = 1;
	p.min_parity[1] = 0;
	p.min_parity[2] = 0;
	return p;
}

std::vector<nxt::Datagram> wivrn::nxwarp_send_frame(nxt::Sender & sender,
                                                    const nxt::StreamConfig & cfg,
                                                    uint16_t frame_id,
                                                    const nxt::PoseHeader & pose,
                                                    std::span<const uint8_t> bitstream,
                                                    std::span<const nxwarp_tile_desc> descs,
                                                    size_t chunk_bytes,
                                                    uint32_t base_qp,
                                                    uint32_t now_us,
                                                    uint16_t enc_us)
{
	std::vector<nxt::Datagram> out;
	if (bitstream.empty() or chunk_bytes <= kFrameLenBytes)
		return out;

	// A 4-byte little-endian length in front of the frame, so that the receiving
	// side can tell a complete frame from one whose tail chunks never arrived.
	// Without it a lost last chunk reassembles into a shorter byte string that
	// looks perfectly well formed and is silently wrong; with it the reassembly
	// knows how many bytes to expect and rejects the frame instead. It lives in
	// chunk 0, whose loss already costs the frame.
	std::vector<uint8_t> framed;
	framed.reserve(kFrameLenBytes + bitstream.size());
	const uint32_t n = uint32_t(bitstream.size());
	framed.push_back(uint8_t(n));
	framed.push_back(uint8_t(n >> 8));
	framed.push_back(uint8_t(n >> 16));
	framed.push_back(uint8_t(n >> 24));
	framed.insert(framed.end(), bitstream.begin(), bitstream.end());
	const std::span<const uint8_t> payload(framed);

	const size_t chunks = (payload.size() + chunk_bytes - 1) / chunk_bytes;
	if (chunks > cfg.tiles_per_frame())
		return out;

	sender.begin_frame(frame_id, pose, now_us, 0);

	const uint8_t nbands = cfg.bands();
	std::vector<nxt::TileInput> band_tiles;
	for (uint8_t band = 0; band < nbands; ++band)
	{
		band_tiles.clear();
		const uint16_t first_row = cfg.first_row_of_band(band);
		const uint16_t last_row = uint16_t(first_row + cfg.rows_in_band(band));
		for (uint16_t row = first_row; row < last_row; ++row)
		{
			for (uint16_t col = 0; col < cfg.cols; ++col)
			{
				const uint32_t t = cfg.tile_index(row, col);
				if (t >= chunks)
					continue;
				const size_t off = size_t(t) * chunk_bytes;
				const size_t len = std::min(chunk_bytes, payload.size() - off);

				nxt::TileInput ti;
				ti.frame_id = frame_id;
				ti.layer_id = 0;
				ti.row = row;
				ti.col = col;
				// Every chunk is class A, and that is not laziness.
				//
				// The eccentricity classes of TRANSPORT.md 1 (A fovea, B mid
				// ring, C periphery) rank tiles by how much their loss costs the
				// picture. Under the chunk mapping a tile is not a region of the
				// picture at all — it is a slice of the frame's byte stream, and
				// losing any one of them costs the whole frame. Grading them by
				// where they happen to land on the grid would put the frame's
				// only protection on whichever rows the chunk count reached,
				// which for a small frame is the top edge: class C, no parity,
				// and a FEC layer that never fires.
				//
				// So they are all class A and all get parity. The eccentricity
				// grading comes back with the real per-tile spans.
				ti.cls = nxt::TileClass::kA;
				ti.qp = t < descs.size() ? descs[t].qp : uint8_t(base_qp);
				ti.mode = t < descs.size() ? nxt::TileMode(descs[t].mode) : nxt::TileMode::kIntra;
				ti.res_level = t < descs.size() ? descs[t].res_level : uint8_t(0);
				ti.ref_delta = t < descs.size() ? descs[t].ref_delta : nxt::kRefIntra;
				ti.bytes = payload.subspan(off, len);
				band_tiles.push_back(ti);
			}
		}

		auto d = sender.send_band(band, band_tiles, now_us, enc_us, band + 1 == nbands);
		out.insert(out.end(), std::make_move_iterator(d.begin()), std::make_move_iterator(d.end()));
	}
	return out;
}

std::vector<uint8_t> wivrn::nxwarp_reassemble(const nxt::StreamConfig & cfg,
                                              std::span<const nxt::TileOutput> tiles,
                                              size_t chunk_bytes)
{
	std::vector<uint8_t> out;
	if (tiles.empty() or chunk_bytes == 0)
		return out;

	// Sparse by design: a frame is a prefix of the grid and everything past the
	// last chunk is simply not sent, so indexing by tile index is the cheapest
	// correct placement.
	std::vector<std::span<const uint8_t>> by_index(cfg.tiles_per_frame());
	uint32_t highest = 0;
	bool any = false;
	for (const auto & t: tiles)
	{
		if (t.layer_id != 0)
			continue;
		const uint32_t idx = cfg.tile_index(t.row, t.col);
		if (idx >= by_index.size())
			return {};
		by_index[idx] = t.bytes;
		highest = std::max(highest, idx);
		any = true;
	}
	if (not any)
		return out;

	out.reserve(size_t(highest + 1) * chunk_bytes);
	for (uint32_t i = 0; i <= highest; ++i)
	{
		if (by_index[i].empty())
			return {}; // a hole: this frame cannot be decoded by this backend
		// Only the last chunk sent may be short, and the sender fills chunks in
		// order, so a short one anywhere else is a malformed stream.
		if (i != highest and by_index[i].size() != chunk_bytes)
			return {};
		out.insert(out.end(), by_index[i].begin(), by_index[i].end());
	}

	// The length prefix is what turns "everything that arrived" into "the whole
	// frame": a frame whose tail chunks were lost reassembles into a prefix that
	// is otherwise indistinguishable from a complete small frame.
	if (out.size() < kFrameLenBytes)
		return {};
	const uint32_t declared = uint32_t(out[0]) | (uint32_t(out[1]) << 8) |
	                          (uint32_t(out[2]) << 16) | (uint32_t(out[3]) << 24);
	if (out.size() < size_t(kFrameLenBytes) + declared)
		return {};
	out.erase(out.begin(), out.begin() + kFrameLenBytes);
	out.resize(declared);
	return out;
}
