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

std::vector<uint8_t> reassemble(const nxt::StreamConfig & cfg,
                                std::span<const std::vector<uint8_t>> by_index,
                                size_t chunk)
{
	std::vector<uint8_t> out;
	if (chunk == 0 or by_index.empty())
		return out;

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
		return out;

	// Account before judging, so a rejected frame can say what it was missing.
	{
		reassemble_report r;
		r.expected = highest + 1;
		for (uint32_t i = 0; i <= highest; ++i)
		{
			if (by_index[i].empty())
			{
				if (r.first_missing == UINT32_MAX)
					r.first_missing = i;
			}
			else
			{
				++r.present;
				if (i != highest and by_index[i].size() != chunk)
					r.short_chunk = true;
			}
		}
		g_last_report = r;
	}
	out.reserve(size_t(highest + 1) * chunk);
	for (uint32_t i = 0; i <= highest; ++i)
	{
		if (by_index[i].empty())
			return {}; // a hole: this frame cannot be decoded by this backend
		// Only the last chunk sent may be short, and the sender fills chunks in order,
		// so a short one anywhere else is a malformed stream.
		if (i != highest and by_index[i].size() != chunk)
			return {};
		out.insert(out.end(), by_index[i].begin(), by_index[i].end());
	}

	// The length prefix is what turns "everything that arrived" into "the whole frame":
	// a frame whose tail chunks were lost reassembles into a prefix that is otherwise
	// indistinguishable from a complete small frame.
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

} // namespace wivrn::nxwarp_wire
