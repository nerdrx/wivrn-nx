/*
 * WiVRn VR streaming — NX Warp codec
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "nxwarp_reassemble.h"

namespace wivrn::nxwarp_wire
{
static thread_local reassemble_report g_last_report;
reassemble_report last_report() { return g_last_report; }


size_t chunk_bytes(const nxt::StreamConfig & cfg)
{
	const size_t budget = cfg.run_payload_budget();
	const size_t reserve = nxt::kDirEntryBytes + ((cfg.caps & nxt::kCapPoseHdr) ? nxt::kPoseHeaderBytes : 0);
	return budget > reserve ? budget - reserve : 0;
}

namespace
{
// The length prefix chunk 0 carries, once at least four bytes of it are here.
uint32_t declared_len(const std::vector<uint8_t> & chunk0)
{
	return uint32_t(chunk0[0]) | (uint32_t(chunk0[1]) << 8) |
	       (uint32_t(chunk0[2]) << 16) | (uint32_t(chunk0[3]) << 24);
}
} // namespace

bool is_complete(const nxt::StreamConfig & cfg,
                 std::span<const std::vector<uint8_t>> by_index,
                 size_t chunk)
{
	(void)cfg;
	if (chunk == 0 or by_index.empty())
		return false;

	// Sparse by design: a frame is a prefix of the grid and everything past the last
	// chunk is simply not sent.
	uint32_t highest = 0;
	bool any = false;
	for (uint32_t i = 0; i < by_index.size(); ++i)
	{
		if (not by_index[i].empty())
		{
			highest = i;
			any = true;
		}
	}
	if (not any)
	{
		g_last_report = reassemble_report{};
		return false;
	}

	// Account before judging, so a frame that is not here can say what it is missing --
	// which is the whole of the two-second "hole" line, and is just as useful for a frame
	// the window is still holding open as for one it gave up on.
	reassemble_report r;
	r.expected = highest + 1;
	size_t total = 0;
	for (uint32_t i = 0; i <= highest; ++i)
	{
		if (by_index[i].empty())
		{
			if (r.first_missing == UINT32_MAX)
				r.first_missing = i;
			continue;
		}
		++r.present;
		total += by_index[i].size();
		// Only the last chunk sent may be short, and the sender fills chunks in order,
		// so a short one anywhere else is a malformed stream.
		if (i != highest and by_index[i].size() != chunk)
			r.short_chunk = true;
	}
	g_last_report = r;

	if (r.present != r.expected or r.short_chunk)
		return false;

	// The length prefix is what turns "everything that arrived" into "the whole frame":
	// a frame whose tail chunks were lost reassembles into a prefix that is otherwise
	// indistinguishable from a complete small frame.
	if (by_index[0].size() < kFrameLenBytes or total < kFrameLenBytes)
		return false;
	return total >= size_t(kFrameLenBytes) + declared_len(by_index[0]);
}

std::vector<uint8_t> reassemble(const nxt::StreamConfig & cfg,
                                std::span<const std::vector<uint8_t>> by_index,
                                size_t chunk)
{
	std::vector<uint8_t> out;
	if (not is_complete(cfg, by_index, chunk))
		return out;

	uint32_t highest = 0;
	for (uint32_t i = 0; i < by_index.size(); ++i)
	{
		if (not by_index[i].empty())
			highest = i;
	}
	out.reserve(size_t(highest + 1) * chunk);
	for (uint32_t i = 0; i <= highest; ++i)
		out.insert(out.end(), by_index[i].begin(), by_index[i].end());

	const uint32_t declared = declared_len(by_index[0]);
	out.erase(out.begin(), out.begin() + kFrameLenBytes);
	out.resize(declared);
	return out;
}

} // namespace wivrn::nxwarp_wire
