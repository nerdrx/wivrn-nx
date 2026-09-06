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
#include <utility>
#include <vector>

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

bool wivrn::nxwarp_spans_fit(std::span<const nxwarp_tile_desc> descs, size_t max_tile_bytes)
{
	if (descs.empty() or max_tile_bytes == 0)
		return false;
	// A transport slot has to hold the tile's own bytes AND whatever non-tile bytes
	// precede it -- the frame header before the first, a row header before the first
	// of each row. Those are absorbed into the following tile's slot so that the
	// concatenation in index order is the frame byte for byte, with no gaps and no
	// slot spent on a header.
	size_t prev_end = 0;
	int64_t prev_index = -1;
	for (const auto & d: descs)
	{
		if (d.length == 0)
			continue;
		// The client concatenates the tiles it received IN INDEX ORDER, so the frame
		// comes back byte for byte only if the byte order and the index order are the
		// same order. They are, for every layout this codec produces -- raster within
		// an eye, eyes side by side, and under stereo the pair grid is what both the
		// index and the bitstream are laid out in (common/nxwarp_stream_grid.h) -- but
		// "they are" is a claim about another component, so it is checked rather than
		// assumed. A layout that ever broke it falls back to the chunk mapping, which
		// is slower to lose from and always correct.
		if (int64_t(d.index) <= prev_index)
			return false;
		const size_t end = size_t(d.offset) + d.length;
		if (end < prev_end)
			return false; // spans must be ascending and disjoint
		if (end - prev_end + kFrameLenBytes > max_tile_bytes)
			return false;
		prev_end = end;
		prev_index = int64_t(d.index);
	}
	return prev_end > 0;
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
                                                    uint16_t enc_us,
                                                    bool tile_spans)
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

	// The byte range each tile index carries. Under real spans a tile carries its
	// own bytes plus the non-tile bytes that precede it (frame header, row header),
	// so the concatenation in index order is the frame with no gaps; under the chunk
	// mapping it carries a fixed slice. Both are built here so the send loop below
	// does not care which it is.
	std::vector<std::pair<size_t, size_t>> range;
	if (tile_spans)
	{
		range.assign(cfg.tiles_per_frame(), {0, 0});
		size_t prev_end = 0;
		uint32_t last = 0;
		bool any_span = false;
		for (const auto & d: descs)
		{
			if (d.length == 0 or d.index >= range.size())
				continue;
			// +kFrameLenBytes: an offset is into the bitstream and the payload
			// has the length prefix in front of it.
			const size_t end = kFrameLenBytes + size_t(d.offset) + d.length;
			if (end > payload.size() or end < prev_end)
			{
				return out; // malformed spans: refuse rather than mis-send
			}
			range[d.index] = {prev_end, end - prev_end};
			prev_end = end;
			last = d.index;
			any_span = true;
		}
		if (not any_span)
			return out;
		// Anything after the last coded tile rides the last tile, so that the
		// concatenation is the whole payload and not a prefix of it.
		if (prev_end < payload.size())
			range[last].second += payload.size() - prev_end;
	}

	const size_t chunks = (payload.size() + chunk_bytes - 1) / chunk_bytes;
	if (not tile_spans and chunks > cfg.tiles_per_frame())
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
				size_t off = 0, len = 0;
				if (tile_spans)
				{
					off = range[t].first;
					len = range[t].second;
					if (len == 0)
						continue; // a tile this frame did not code
				}
				else
				{
					if (t >= chunks)
						continue;
					off = size_t(t) * chunk_bytes;
					len = std::min(chunk_bytes, payload.size() - off);
				}

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
		// No per-tile check here, deliberately, and it serves both mappings.
		//
		// It used to reject an empty tile as a hole and a short tile as
		// malformed. Both were the chunk mapping's way of noticing a loss, and
		// the length prefix below notices the same loss by the same evidence:
		// bytes that did not arrive make the total fall short of what the frame
		// declared itself to be. Under real per-tile spans the old checks are
		// not merely redundant but WRONG -- a skipped tile carries no bytes and
		// is, from the tile list alone, indistinguishable from a lost one, and a
		// coded tile is short or long according to its own content.
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
