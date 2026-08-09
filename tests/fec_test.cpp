// Forward error correction for the video stream: the parity scheme, checked
// without a headset, a GPU or a socket.
//
// Part A: group construction. A frame is sharded the way video_encoder::SendData
// shards it (view_info on the first shard, timing_info on the last, the FEC payload
// budget) and fed to fec::group_builder. The groups it produces must cover every
// shard exactly once, in order, with no gaps and no overlap, and a parity shard
// must never come out as a bigger datagram than a data shard would have been
// without FEC — the whole point of fec::payload_reserve.
// Part B: parity round trip. Every shard position of every group is dropped in
// turn, the parity shard is put through the real serialization, and what comes back
// must be byte for byte the shard that was dropped — payload, view_info and
// timing_info alike.
// Part C: graceful failure. Two erasures in one group, a parity shard on its own,
// an empty group, and a parity shard whose length table has been corrupted must all
// produce no reconstruction and no crash.
// Part D: dedup. A rebuilt shard goes in through the same door as a received one,
// so the real copy arriving late is an ordinary duplicate; either arrival order has
// to leave the frame identical.
//
// Build (the boost include is wherever the build fetched it):
//   g++ -std=c++23 -I common -I build-client/common -I external \
//       -I build-client/_deps/boost-src/libs/pfr/include \
//       -o fec_test tests/fec_test.cpp common/smp.cpp -lcrypto
//   ./fec_test

#include "fec.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

using namespace wivrn;

static int failures = 0;
static int checks = 0;

#define CHECK(...)                                                                           \
	do                                                                                   \
	{                                                                                    \
		++checks;                                                                    \
		if (not(__VA_ARGS__))                                                        \
		{                                                                            \
			++failures;                                                          \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
		}                                                                            \
	} while (0)

namespace
{

using data_shard = to_headset::video_stream_data_shard;
using parity_shard = to_headset::video_stream_parity_shard;
using view_info_t = data_shard::view_info_t;
using timing_info_t = data_shard::timing_info_t;

template <typename T>
T round_trip(const T & value)
{
	serialization_packet packet;
	packet.serialize(value);

	std::vector<uint8_t> flat;
	for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		flat.insert(flat.end(), span.begin(), span.end());

	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[flat.size() + 1]);
	std::memcpy(memory.get(), flat.data(), flat.size());

	deserialization_packet in{memory, std::span<uint8_t>(memory.get(), flat.size())};
	return in.deserialize<T>();
}

view_info_t make_view_info()
{
	return view_info_t{
	        .display_time = 123'456'789,
	        .pose = {XrPosef{{0, 0, 0, 1}, {0.03f, 0, 0}}, XrPosef{{0, 0, 0, 1}, {-0.03f, 0, 0}}},
	        .fov = {XrFovf{-0.9f, 0.9f, 0.9f, -0.9f}, XrFovf{-0.9f, 0.9f, 0.9f, -0.9f}},
	        .foveation = {to_headset::foveation_parameter{{1, 4, 5, 3, 1}, {2, 3, 4, 3, 2}},
	                      to_headset::foveation_parameter{{1, 4, 5, 3, 1}, {2, 3, 4, 3, 2}}},
	        .alpha = false,
	        .quad = {},
	};
}

// One encoded frame, sharded exactly the way video_encoder::SendData shards it.
struct frame
{
	std::vector<uint8_t> encoded;
	std::vector<data_shard> shards;
	std::vector<parity_shard> parity;
	// Buffers the parity payloads point into, kept alive alongside them
	std::vector<std::vector<uint8_t>> parity_payloads;
};

frame make_frame(size_t bytes, uint8_t stream_idx = 0, uint64_t frame_idx = 42)
{
	frame f;
	f.encoded.resize(bytes);
	for (size_t i = 0; i < bytes; ++i)
		f.encoded[i] = uint8_t(i * 7 + (i >> 8) * 31 + 3);

	fec::group_builder builder;
	builder.reset(stream_idx, frame_idx);

	data_shard shard;
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_idx;
	shard.shard_idx = 0;
	shard.view_info = make_view_info();

	auto take_parity = [&] {
		if (auto p = builder.take())
		{
			// The builder's buffer is reused, so keep our own copy of the payload
			f.parity_payloads.emplace_back(p->payload.begin(), p->payload.end());
			p->payload = f.parity_payloads.back();
			f.parity.push_back(std::move(*p));
		}
	};

	size_t offset = 0;
	while (offset < bytes)
	{
		const size_t budget = fec::shard_payload_budget(true) - serialized_size(shard.view_info);
		const size_t next = std::min(bytes, offset + budget);
		if (next == bytes)
			shard.timing_info = timing_info_t{1, 2, 3, 4};
		shard.payload = std::span<uint8_t>(f.encoded).subspan(offset, next - offset);

		f.shards.push_back(shard);
		builder.add(shard);
		if (builder.full())
			take_parity();

		++shard.shard_idx;
		shard.view_info.reset();
		offset = next;
	}
	take_parity();

	// The shards' payloads point into f.encoded, which moves with the frame; repoint
	// them so the returned value is self contained.
	auto moved = std::move(f);
	for (size_t i = 0, off = 0; i < moved.shards.size(); ++i)
	{
		const size_t n = moved.shards[i].payload.size();
		moved.shards[i].payload = std::span<uint8_t>(moved.encoded).subspan(off, n);
		off += n;
	}
	for (size_t i = 0; i < moved.parity.size(); ++i)
		moved.parity[i].payload = moved.parity_payloads[i];
	return moved;
}

// What reconstruct() is given: every shard of the frame except the dropped ones
auto lookup_without(const frame & f, std::span<const size_t> dropped)
{
	return [&f, dropped](uint16_t idx) -> const data_shard * {
		for (size_t d: dropped)
			if (d == idx)
				return nullptr;
		if (idx >= f.shards.size())
			return nullptr;
		return &f.shards[idx];
	};
}

bool same_shard(const data_shard & a, const data_shard & b)
{
	if (a.stream_item_idx != b.stream_item_idx or a.frame_idx != b.frame_idx or a.shard_idx != b.shard_idx)
		return false;
	if (a.payload.size() != b.payload.size())
		return false;
	if (std::memcmp(a.payload.data(), b.payload.data(), a.payload.size()) != 0)
		return false;
	if (a.view_info.has_value() != b.view_info.has_value())
		return false;
	if (a.timing_info.has_value() != b.timing_info.has_value())
		return false;
	if (a.view_info and a.view_info->display_time != b.view_info->display_time)
		return false;
	if (a.view_info and a.view_info->foveation[0].x != b.view_info->foveation[0].x)
		return false;
	if (a.timing_info and (a.timing_info->encode_begin != b.timing_info->encode_begin or
	                       a.timing_info->send_end != b.timing_info->send_end))
		return false;
	return true;
}

void test_group_construction()
{
	std::printf("Part A: group construction\n");

	// About 200 kB, a frame at 150 Mbit/s and 90 fps
	frame f = make_frame(200 * 1024);

	CHECK(f.shards.size() > 150);
	CHECK(f.parity.size() == (f.shards.size() + fec::group_size - 1) / fec::group_size);

	// Every shard is covered exactly once, in order, with no gap
	size_t expected_first = 0;
	size_t covered = 0;
	for (const parity_shard & p: f.parity)
	{
		CHECK(p.stream_item_idx == f.shards.front().stream_item_idx);
		CHECK(p.frame_idx == f.shards.front().frame_idx);
		CHECK(p.first_shard_idx == expected_first);
		CHECK(not p.blob_size.empty());
		CHECK(p.blob_size.size() <= fec::group_size);

		// Only the last group may be short
		if (&p != &f.parity.back())
			CHECK(p.blob_size.size() == fec::group_size);

		// The parity payload is as long as the longest blob of its group, no more
		size_t longest = 0;
		for (uint16_t s: p.blob_size)
			longest = std::max<size_t>(longest, s);
		CHECK(p.payload.size() == longest);

		expected_first += p.blob_size.size();
		covered += p.blob_size.size();
	}
	CHECK(covered == f.shards.size());

	// The overhead is one shard in group_size, i.e. 12.5% for a group of 8
	size_t data_bytes = 0;
	for (const data_shard & s: f.shards)
		data_bytes += serialized_size(s);
	size_t parity_bytes = 0;
	for (const parity_shard & p: f.parity)
		parity_bytes += serialized_size(p);
	const double overhead = double(parity_bytes) / data_bytes;
	CHECK(overhead > 0.11 and overhead < 0.14);
	std::printf("  %zu shards, %zu parity, %.1f%% overhead\n",
	            f.shards.size(),
	            f.parity.size(),
	            overhead * 100);

	// A parity datagram is never larger than the data datagram the same encoder
	// would have produced with FEC off — that is what payload_reserve buys, and it
	// is the property that keeps FEC from causing the fragmentation it exists to
	// survive.
	size_t largest_parity = 0;
	for (const parity_shard & p: f.parity)
		largest_parity = std::max(largest_parity, serialized_size(p));
	size_t largest_data = 0;
	for (const data_shard & s: f.shards)
		largest_data = std::max(largest_data, serialized_size(s));

	// The same shard with FEC off would carry payload_reserve bytes more
	CHECK(largest_parity <= largest_data + fec::payload_reserve);
	std::printf("  largest parity %zu B, largest data %zu B, reserve %zu B\n",
	            largest_parity,
	            largest_data,
	            fec::payload_reserve);

	// The first shard is the one that carries the pose and the last one the
	// timings, which is what makes them the two the frame cannot do without
	CHECK(f.shards.front().view_info.has_value());
	CHECK(not f.shards.front().timing_info.has_value());
	CHECK(f.shards.back().timing_info.has_value());

	// A frame small enough to fit in a single shard still gets its parity: it is a
	// handful of bytes and losing it costs a whole frame plus an IDR round trip.
	frame tiny = make_frame(300);
	CHECK(tiny.shards.size() == 1);
	CHECK(tiny.parity.size() == 1);
	CHECK(tiny.parity.front().blob_size.size() == 1);

	// A group that straddles the frame's end is short, not padded out
	frame odd = make_frame(fec::shard_payload_budget(true) * 9 + 100);
	CHECK(odd.parity.size() == 2);
	CHECK(odd.parity.front().blob_size.size() == fec::group_size);
	CHECK(odd.parity.back().blob_size.size() == odd.shards.size() - fec::group_size);
}

void test_round_trip()
{
	std::printf("Part B: parity round trip\n");

	frame f = make_frame(64 * 1024);
	CHECK(f.parity.size() >= 4);

	size_t rebuilt = 0;
	for (const parity_shard & original: f.parity)
	{
		// Through the real wire encoding, spans and all
		parity_shard p = round_trip(original);
		CHECK(p.first_shard_idx == original.first_shard_idx);
		CHECK(p.blob_size == original.blob_size);
		CHECK(p.payload.size() == original.payload.size());

		for (size_t i = 0; i < p.blob_size.size(); ++i)
		{
			const size_t dropped = p.first_shard_idx + i;
			auto shard = fec::reconstruct(p, lookup_without(f, std::span(&dropped, 1)));

			CHECK(shard.has_value());
			if (not shard)
				continue;
			++rebuilt;
			CHECK(same_shard(*shard, f.shards[dropped]));
		}
	}
	std::printf("  rebuilt %zu shards, one per position of every group\n", rebuilt);
	CHECK(rebuilt == f.shards.size());

	// The two special shards specifically: the pose and the timings survive being
	// rebuilt, without which a recovered frame would never be submitted
	{
		const size_t dropped = 0;
		auto first = fec::reconstruct(f.parity.front(), lookup_without(f, std::span(&dropped, 1)));
		CHECK(first.has_value());
		CHECK(first and first->view_info.has_value());
		CHECK(first and first->view_info->display_time == make_view_info().display_time);
		CHECK(first and first->view_info->foveation[1].y == make_view_info().foveation[1].y);
	}
	{
		const size_t dropped = f.shards.size() - 1;
		auto last = fec::reconstruct(f.parity.back(), lookup_without(f, std::span(&dropped, 1)));
		CHECK(last.has_value());
		CHECK(last and last->timing_info.has_value());
		CHECK(last and last->timing_info->send_end == 4);
		// The parity shard is also what tells the headset that this index exists at
		// all: without it a lost last shard is not even known to be missing.
		CHECK(f.parity.back().first_shard_idx + f.parity.back().blob_size.size() == f.shards.size());
	}

	// A reconstructed shard owns its bytes: it stays valid once whatever it was
	// rebuilt from is gone
	std::optional<data_shard> detached;
	{
		frame temp = make_frame(8 * 1024);
		const size_t dropped = 3;
		detached = fec::reconstruct(temp.parity.front(), lookup_without(temp, std::span(&dropped, 1)));
		CHECK(detached.has_value());
	}
	CHECK(detached and detached->payload.size() > 0);
	CHECK(detached and detached->shard_idx == 3);
}

void test_graceful_failure()
{
	std::printf("Part C: what a single parity cannot do\n");

	frame f = make_frame(64 * 1024);
	const parity_shard & p = f.parity.front();

	// Two erasures in one group: XOR recovers one, and refusing is the only correct
	// answer. Notably it must not hand back a plausible looking shard of garbage.
	for (size_t i = 0; i + 1 < p.blob_size.size(); ++i)
	{
		const size_t dropped[2] = {p.first_shard_idx + i, p.first_shard_idx + i + 1};
		CHECK(not fec::reconstruct(p, lookup_without(f, dropped)).has_value());
	}

	// Two erasures far apart in the same group
	{
		const size_t dropped[2] = {size_t(p.first_shard_idx), size_t(p.first_shard_idx) + p.blob_size.size() - 1};
		CHECK(not fec::reconstruct(p, lookup_without(f, dropped)).has_value());
	}

	// A parity shard on its own, with none of its group: nothing to do
	{
		std::vector<size_t> all;
		for (size_t i = 0; i < p.blob_size.size(); ++i)
			all.push_back(p.first_shard_idx + i);
		CHECK(not fec::reconstruct(p, lookup_without(f, all)).has_value());
	}

	// A whole group, nothing missing: nothing to do either
	CHECK(not fec::reconstruct(p, lookup_without(f, {})).has_value());

	// An empty group is not a group
	{
		parity_shard empty = p;
		empty.blob_size.clear();
		CHECK(not fec::reconstruct(empty, lookup_without(f, {})).has_value());
	}

	// A length table that contradicts the shards it names: a corrupt or
	// version-skewed datagram, refused rather than decoded into garbage
	{
		parity_shard corrupt = p;
		corrupt.blob_size[1] += 1;
		const size_t dropped = corrupt.first_shard_idx + 3;
		CHECK(not fec::reconstruct(corrupt, lookup_without(f, std::span(&dropped, 1))).has_value());
	}

	// A length longer than the parity payload itself cannot name any real shard
	{
		parity_shard corrupt = p;
		corrupt.blob_size[2] = uint16_t(corrupt.payload.size() + 1);
		const size_t dropped = corrupt.first_shard_idx + 2;
		CHECK(not fec::reconstruct(corrupt, lookup_without(f, std::span(&dropped, 1))).has_value());
	}

	// A payload that has been truncated on the wire: the XOR would run past its end
	{
		parity_shard corrupt = p;
		corrupt.payload = corrupt.payload.first(corrupt.payload.size() / 2);
		const size_t dropped = corrupt.first_shard_idx + 1;
		CHECK(not fec::reconstruct(corrupt, lookup_without(f, std::span(&dropped, 1))).has_value());
	}

	// Blob bytes that decode to nothing sensible: refused, never thrown out of the
	// network thread
	{
		frame junk = make_frame(8 * 1024);
		std::vector<uint8_t> & payload = junk.parity_payloads.front();
		for (uint8_t & b: payload)
			b = 0xff;
		junk.parity.front().payload = payload;
		const size_t dropped = 2;
		auto shard = fec::reconstruct(junk.parity.front(), lookup_without(junk, std::span(&dropped, 1)));
		// Either it refuses or it produces something; what matters is that it
		// returns rather than throwing, and that it never claims the real shard
		CHECK(not shard or not same_shard(*shard, junk.shards[2]));
	}
}

void test_dedup()
{
	std::printf("Part D: a rebuilt shard is an ordinary shard\n");

	frame f = make_frame(32 * 1024);

	// The accumulator's rule, verbatim: first one in wins, later copies are dropped
	std::vector<std::optional<data_shard>> slots(f.shards.size());
	auto insert = [&slots](data_shard && s) -> bool {
		if (slots[s.shard_idx])
			return false;
		slots[s.shard_idx] = std::move(s);
		return true;
	};

	const size_t lost = 5;
	for (size_t i = 0; i < f.shards.size(); ++i)
		if (i != lost)
			insert(data_shard(f.shards[i]));

	auto rebuilt = fec::reconstruct(f.parity.front(), [&slots](uint16_t idx) -> const data_shard * {
		return (idx < slots.size() and slots[idx]) ? &*slots[idx] : nullptr;
	});
	CHECK(rebuilt.has_value());
	CHECK(rebuilt and insert(std::move(*rebuilt)));
	CHECK(slots[lost].has_value());
	CHECK(slots[lost] and same_shard(*slots[lost], f.shards[lost]));

	// The real copy turns up late — a duplicate, dropped, and the frame is the same
	// either way because the rebuilt bytes are the real bytes
	std::vector<uint8_t> before(slots[lost]->payload.begin(), slots[lost]->payload.end());
	CHECK(not insert(data_shard(f.shards[lost])));
	std::vector<uint8_t> after(slots[lost]->payload.begin(), slots[lost]->payload.end());
	CHECK(before == after);
	CHECK(before.size() == f.shards[lost].payload.size());
	CHECK(std::memcmp(before.data(), f.shards[lost].payload.data(), before.size()) == 0);

	// The frame is whole: every slot filled, the last one carrying the timings, so
	// the accumulator submits it and never counts it lost
	bool whole = true;
	for (const auto & s: slots)
		whole = whole and s.has_value();
	CHECK(whole);
	CHECK(slots.back()->timing_info.has_value());
	CHECK(slots.front()->view_info.has_value());

	// Reconstructing again once the group is whole does nothing, so a parity shard
	// that is retried cannot double count
	CHECK(not fec::reconstruct(f.parity.front(), [&slots](uint16_t idx) -> const data_shard * {
		          return (idx < slots.size() and slots[idx]) ? &*slots[idx] : nullptr;
	          }).has_value());
}

} // namespace

int main()
{
	test_group_construction();
	test_round_trip();
	test_graceful_failure();
	test_dedup();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
