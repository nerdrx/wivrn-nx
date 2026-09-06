/*
 * WiVRn VR streaming — NX Warp codec
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "nxwarp_reassemble.h"

#include <algorithm>
#include <cstring>

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
// The length prefix the frame's leading tile carries, once at least four bytes of it are
// here. Little endian, in front of the frame.
uint32_t declared_len(const uint8_t * p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
	       (uint32_t(p[3]) << 24);
}

// One pass over the grid, and the only one.
//
// It used to be three: is_complete() walked the grid for `highest` and then again from 0
// to `highest`, and reassemble() called is_complete() and then walked it a THIRD time for
// its own `highest`. Under the chunk mapping the grid was a 45-entry prefix of itself and
// that was invisible; under the per-tile span mapping the same frame spans the whole
// 578-entry grid, and the walk is per closed frame on the network thread.
struct extent
{
	uint32_t lowest = 0;   // first tile carrying bytes; NOT always 0 (see below)
	uint32_t highest = 0;  // last tile carrying bytes
	uint32_t present = 0;  // tiles between them that carry bytes
	size_t total = 0;      // bytes over all of them
	uint32_t declared = 0; // the frame's own length, from the prefix
	bool any = false;      // anything at all arrived
	bool have_len = false; // the leading tile carries the whole 4-byte prefix
};

extent scan(std::span<const std::vector<uint8_t>> by_index)
{
	extent e;
	uint32_t first_missing = UINT32_MAX;
	for (uint32_t i = 0; i < by_index.size(); ++i)
	{
		if (by_index[i].empty())
		{
			if (e.any and first_missing == UINT32_MAX)
				first_missing = i;
			continue;
		}
		if (not e.any)
		{
			e.any = true;
			e.lowest = i;
		}
		e.highest = i;
		++e.present;
		e.total += by_index[i].size();
	}
	if (not e.any)
	{
		g_last_report = reassemble_report{};
		return e;
	}

	// Account before judging, so a frame that is not here can say what it is missing --
	// which is the whole of the two-second "hole" line, and is just as useful for a frame
	// the window is still holding open as for one it gave up on.
	reassemble_report r;
	r.expected = e.highest + 1;
	r.present = e.present;
	r.first_missing = first_missing <= e.highest ? first_missing : UINT32_MAX;
	g_last_report = r;

	// The length prefix rides the LOWEST tile that carries bytes, which is not always
	// tile 0. Under the chunk mapping it is, because chunks fill the grid from the
	// start. Under the per-tile span mapping tile 0 is sent only if the frame coded tile
	// 0; when it did not, the frame's leading bytes -- header, row header and the prefix
	// in front of them -- ride the first tile it DID code. Reading index 0
	// unconditionally made every such frame permanently incomplete, which is most frames
	// on an inter stream.
	if (by_index[e.lowest].size() >= kFrameLenBytes)
	{
		e.have_len = true;
		e.declared = declared_len(by_index[e.lowest].data());
	}
	return e;
}

// No per-tile length check and no "every index from 0 must be present" check.
//
// Both were the CHUNK mapping's way of noticing a loss: under it every tile but the last
// is exactly `chunk` bytes and every index up to the last is sent, so a gap or a short one
// meant something went missing. Under the per-tile span mapping neither holds -- a coded
// tile is as long as its own content, and a tile the frame did not code carries nothing at
// all and is, from this side, identical to one that was lost.
//
// The length prefix notices the same loss by better evidence: bytes that did not arrive
// make the total fall short of what the frame declared itself to be. That test is exact
// under both mappings, which is why the wire needs no flag saying which one produced the
// frame. `first_missing` and `present`/`expected` are still filled, because the two-second
// "hole" line is diagnostics and a gap in the index run is still worth reporting even when
// it is not fatal.
bool complete(const extent & e)
{
	return e.any and e.have_len and e.total >= size_t(kFrameLenBytes) + e.declared;
}
} // namespace

bool is_complete(const nxt::StreamConfig & cfg,
                 std::span<const std::vector<uint8_t>> by_index,
                 size_t chunk)
{
	(void)cfg;
	(void)chunk;
	if (by_index.empty())
		return false;
	return complete(scan(by_index));
}

std::vector<uint8_t> reassemble(const nxt::StreamConfig & cfg,
                                std::span<const std::vector<uint8_t>> by_index,
                                size_t chunk)
{
	(void)cfg;
	(void)chunk;
	std::vector<uint8_t> out;
	if (by_index.empty())
		return out;
	const extent e = scan(by_index);
	if (not complete(e))
		return out;

	// Exactly the frame, and not a byte more.
	//
	// This used to reserve `(highest + 1) * chunk_bytes` -- the whole grid at one full
	// transport slot each. Under the chunk mapping that was the frame's own size to
	// within a slot, because the frame WAS a prefix of the grid; under the per-tile span
	// mapping the same 51 kB frame spans the whole 578-tile grid and the reserve became
	// 658 kB, thirteen times the bytes it would hold, allocated and freed for every frame
	// at the headset's frame rate. On a phone-class allocator an allocation that size is
	// an mmap and the free is an munmap, and the cost of that does not land on the thread
	// that paid it -- it lands wherever the TLB shootdown catches, which on the headset
	// was the decode worker waiting on the one queue lock this process submits through.
	out.reserve(e.declared);

	// Straight to the frame's bytes: the prefix is skipped as it is copied rather than
	// erased afterwards. The erase moved the whole frame down four bytes -- a memmove of
	// the entire unit, every frame, to drop four bytes off the front of it.
	size_t skip = kFrameLenBytes;
	for (uint32_t i = e.lowest; i <= e.highest; ++i)
	{
		const auto & t = by_index[i];
		if (t.empty())
			continue;
		if (skip >= t.size())
		{
			skip -= t.size();
			continue;
		}
		const size_t take = t.size() - skip;
		const size_t room = e.declared - out.size();
		out.insert(out.end(), t.begin() + long(skip), t.begin() + long(skip + std::min(take, room)));
		skip = 0;
		if (out.size() >= e.declared)
			break;
	}
	// The declared length is what the frame says it is; `complete` has already
	// established that at least that many bytes arrived.
	out.resize(e.declared);
	return out;
}

} // namespace wivrn::nxwarp_wire
