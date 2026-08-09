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

#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Forward error correction for the video stream.
//
// Both ends need the same two things and nothing else: the exact bytes a data
// shard contributes to the XOR (encode_blob), and the inverse (decode_blob). The
// server also uses group_builder to accumulate a group and hand out its parity
// shard; the headset needs no equivalent because a parity shard describes its own
// group. tests/fec_test.cpp drives all of it without a socket.
namespace wivrn::fec
{

using data_shard = to_headset::video_stream_data_shard;
using parity_shard = to_headset::video_stream_parity_shard;

// Data shards one parity shard covers. Eight data shards plus one parity is 12.5%
// on the wire, and a group is then about 11 kB — near enough the 12 kB micro-burst
// the packet pacer hands to the kernel between two sleeps that a group and a burst
// are the same order of thing, without being locked to each other: the two sizes
// drift against one another over a frame, which is what scatters the parity shards
// across the bursts instead of parking them all at one boundary.
//
// Not a protocol constant: the parity shard names the shards it covers, so the
// server may change this at will and old headsets keep up.
inline constexpr uint16_t group_size = 8;

// Fraction of the on-wire video budget that carries actual video while FEC is on.
// The encoder bitrate is scaled by this so the total, parity included, stays at
// the number the bitrate controller decided.
inline constexpr double data_share = double(group_size) / (group_size + 1);

// Bytes taken out of a data shard's payload budget while FEC is on.
//
// A parity shard must not be a bigger datagram than a data shard, or FEC would
// start causing the fragmentation it exists to survive. It is bigger by three
// things: the per-shard length table (2 bytes of length prefix plus 2 per covered
// shard, 18 for a group of 8), the recovery blob's own framing (the empty
// timing_info flag and the payload length prefix that a data shard does not repeat,
// 3 bytes), and the timing_info the last shard of a frame carries, which lands in
// the longest blob of the last group (33 bytes). That is 54; 64 leaves room for a
// larger group and for a timing_info that grows a field.
//
// The cost is 4.6% more datagrams per frame for the same bytes, i.e. 4.6% more
// UDP/IP headers — under 0.2% of the link. group_builder additionally refuses to
// emit a parity shard whose payload would exceed max_payload_size, so an unforeseen
// blob can only cost the group its protection, never fragment a datagram.
inline constexpr size_t payload_reserve = 64;

// Payload budget of a data shard, before the first shard's view_info is taken out
// of it, with and without FEC.
inline constexpr size_t shard_payload_budget(bool fec_enabled)
{
	return data_shard::max_payload_size - (fec_enabled ? payload_reserve : 0);
}

// The part of a data shard that a parity shard can rebuild: everything except the
// three fields (stream, frame, shard index) the parity shard already knows.
//
// Encoded exactly as the data shard encodes them, so a blob is the tail of a data
// shard's own serialization with the payload length made explicit. Appends to
// `out`, which is cleared first.
inline void encode_blob(const data_shard & shard, std::vector<uint8_t> & out)
{
	serialization_packet p;
	p.serialize(shard.view_info);
	p.serialize(shard.timing_info);
	p.serialize(shard.payload);

	out.clear();
	for (const std::span<uint8_t> & span: static_cast<std::vector<std::span<uint8_t>> &>(p))
		out.insert(out.end(), span.begin(), span.end());
}

// Inverse of encode_blob. Throws deserialization_error on a blob that does not
// decode; callers treat that as "no reconstruction" rather than as a fatal error,
// since a corrupt or mismatched parity shard must never take the session down.
//
// The returned shard owns a copy of the blob: its payload span points into it and
// stays valid for as long as the shard does, exactly like a received one.
inline data_shard decode_blob(uint8_t stream_item_idx,
                              uint64_t frame_idx,
                              uint16_t shard_idx,
                              std::span<const uint8_t> blob)
{
	auto memory = std::make_shared_for_overwrite<uint8_t[]>(blob.size() + 1);
	std::copy(blob.begin(), blob.end(), memory.get());
	deserialization_packet p{memory, std::span<uint8_t>(memory.get(), blob.size())};

	data_shard shard;
	shard.stream_item_idx = stream_item_idx;
	shard.frame_idx = frame_idx;
	shard.shard_idx = shard_idx;
	shard.view_info = p.deserialize<std::optional<data_shard::view_info_t>>();
	shard.timing_info = p.deserialize<std::optional<data_shard::timing_info_t>>();
	shard.payload = p.deserialize<std::span<uint8_t>>();
	shard.data.c = p.steal_buffer();
	return shard;
}

// Rebuild the one data shard of `parity`'s group that `present` does not hold.
//
// `present` is looked up by shard index and returns null for a shard that has not
// arrived; exactly one of the covered indices must return null. Returns nothing
// when that is not the case (nothing missing, or more than one — XOR recovers a
// single erasure and no more), or when the group's blobs contradict the lengths
// the parity shard recorded, which is what a corrupt datagram looks like.
template <typename lookup>
std::optional<data_shard> reconstruct(const parity_shard & parity, lookup && present)
{
	const size_t n = parity.blob_size.size();
	if (n == 0)
		return {};

	size_t missing = n;
	for (size_t i = 0; i < n; ++i)
	{
		if (present(uint16_t(parity.first_shard_idx + i)))
			continue;
		if (missing != n)
			return {}; // two or more erasures, out of reach of a single parity
		missing = i;
	}
	if (missing == n)
		return {}; // the group is whole, nothing to do

	// Undo the XOR with everything that did arrive. The parity payload is as long
	// as the longest blob of the group and every blob was zero padded to it, so a
	// present blob can never be longer than it.
	std::vector<uint8_t> recovered(parity.payload.begin(), parity.payload.end());
	std::vector<uint8_t> blob;
	for (size_t i = 0; i < n; ++i)
	{
		if (i == missing)
			continue;

		const data_shard * shard = present(uint16_t(parity.first_shard_idx + i));
		encode_blob(*shard, blob);
		if (blob.size() != parity.blob_size[i] or blob.size() > recovered.size())
			return {};

		for (size_t b = 0; b < blob.size(); ++b)
			recovered[b] ^= blob[b];
	}

	const size_t length = parity.blob_size[missing];
	if (length > recovered.size())
		return {};

	try
	{
		return decode_blob(parity.stream_item_idx,
		                   parity.frame_idx,
		                   uint16_t(parity.first_shard_idx + missing),
		                   std::span<const uint8_t>(recovered).first(length));
	}
	catch (...)
	{
		return {};
	}
}

// Server side: accumulates the recovery blobs of one group and hands out its
// parity shard.
//
// Shards are added in the order they go out. A shard whose index does not continue
// the open group starts a new one, which is what keeps a group contiguous when a
// frame's shards do not all travel the same way (a parameter set going out on the
// control socket in the middle of a frame, say).
class group_builder
{
public:
	void reset(uint8_t stream_item_idx, uint64_t frame_idx)
	{
		stream = stream_item_idx;
		frame = frame_idx;
		start_group(0);
	}

	bool empty() const
	{
		return sizes.empty();
	}

	bool full() const
	{
		return sizes.size() >= group_size;
	}

	void add(const data_shard & shard)
	{
		if (not sizes.empty() and shard.shard_idx != first + sizes.size())
			start_group(shard.shard_idx);
		if (sizes.empty())
			first = shard.shard_idx;

		encode_blob(shard, scratch);
		if (parity.size() < scratch.size())
			parity.resize(scratch.size(), 0);
		for (size_t i = 0; i < scratch.size(); ++i)
			parity[i] ^= scratch[i];

		sizes.push_back(uint16_t(scratch.size()));
	}

	// Parity shard for the shards added so far, and start a new group. Nothing when
	// the group is empty, or when its blobs came out longer than a datagram may
	// carry — see payload_reserve.
	//
	// The returned shard's payload points into a buffer of this builder that stays
	// untouched until the next take(), which is all the sender needs: it serializes
	// the shard into the socket before it adds anything else here.
	std::optional<parity_shard> take()
	{
		if (sizes.empty())
			return {};

		out.swap(parity);

		std::optional<parity_shard> result;
		if (out.size() <= data_shard::max_payload_size)
		{
			result = parity_shard{
			        .stream_item_idx = stream,
			        .frame_idx = frame,
			        .first_shard_idx = first,
			        .blob_size = sizes,
			        .payload = out,
			        .data = {},
			};
		}

		start_group(uint16_t(first + sizes.size()));
		return result;
	}

private:
	void start_group(uint16_t next_first)
	{
		first = next_first;
		sizes.clear();
		parity.clear();
	}

	uint8_t stream = 0;
	uint64_t frame = 0;
	uint16_t first = 0;
	std::vector<uint16_t> sizes;
	std::vector<uint8_t> parity;
	std::vector<uint8_t> out;
	std::vector<uint8_t> scratch;
};

} // namespace wivrn::fec
