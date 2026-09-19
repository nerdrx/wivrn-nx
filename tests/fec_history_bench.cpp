// Local recovery-work benchmark. No sockets, encryption, codec or headset.
// Build from repository root (set BOOST_INCLUDE and CONFIG_INCLUDE to your build):
// g++ -std=c++23 -O2 tests/fec_history_bench.cpp common/smp.cpp -Icommon \
//   -Iserver/encoder -Iexternal -I"$BOOST_INCLUDE" -I"$CONFIG_INCLUDE" \
//   -lcrypto -o /tmp/fec_history_bench
// /tmp/fec_history_bench

#include "fec.h"
#include "shard_history.h"
#include "wivrn_packets.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace wivrn;
using data_shard = to_headset::video_stream_data_shard;

template <typename T>
static std::vector<uint8_t> serialized(const T & value)
{
	serialization_packet packet;
	packet.serialize(value);
	std::vector<uint8_t> out;
	for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		out.insert(out.end(), span.begin(), span.end());
	return out;
}

static std::array<data_shard, 16> make_shards()
{
	static std::array<std::vector<uint8_t>, 16> storage;
	std::array<data_shard, 16> shards{};
	for (uint16_t i = 0; i < shards.size(); ++i)
	{
		storage[i].resize(1200);
		for (size_t j = 0; j < storage[i].size(); ++j)
			storage[i][j] = uint8_t(j * 17 + i * 3);
		shards[i].stream_item_idx = 0;
		shards[i].frame_idx = 0;
		shards[i].shard_idx = i;
		shards[i].payload = storage[i];
		if (i == 0)
			shards[i].view_info.emplace();
		if (i == 15)
			shards[i].timing_info = {1, 2, 3, 4};
	}
	return shards;
}

struct snapshot
{
	std::vector<std::vector<uint8_t>> parity;
	std::vector<std::vector<uint8_t>> history;
};

static snapshot one_frame(bool reuse, bool fec_active, bool mixed_secondary)
{
	const auto source = make_shards();
	fec::group_builder groups;
	groups.set_layout(8, 1);
	groups.reset(0, 0);
	shard_history history;
	history.set_enabled(true);
	std::vector<uint8_t> old_blob;
	snapshot out;
	for (uint16_t i = 0; i < source.size(); ++i)
	{
		auto shard = source[i];
		const bool primary = not mixed_secondary or i % 3 != 0;
		std::span<const uint8_t> fec_blob;
		if (fec_active)
			fec_blob = groups.add(shard, primary);
		if (fec_active and groups.block_full())
			while (auto parity = groups.take())
				out.parity.push_back(serialized(*parity));
		if (primary)
		{
			if (reuse and fec_active)
				history.push(0, i, fec_blob, true);
			else
			{
				fec::encode_blob(shard, old_blob);
				history.push(0, i, old_blob, true);
			}
		}
		else if (not reuse)
			fec::encode_blob(shard, old_blob);
	}
	if (fec_active)
		while (auto parity = groups.take())
			out.parity.push_back(serialized(*parity));
	std::vector<shard_history::hit> hits;
	const std::array<uint8_t, 2> all_shards{0xff, 0xff};
	history.collect(0, 0, all_shards, 16, hits);
	for (auto & hit: hits)
		out.history.push_back(std::move(hit.blob));
	return out;
}

static uint64_t timed(bool reuse, bool fec_active, bool mixed_secondary, size_t frames = 1000)
{
	const auto source = make_shards();
	fec::group_builder groups;
	groups.set_layout(8, 1);
	shard_history history;
	history.set_enabled(true);
	std::vector<uint8_t> old_blob;
	uint64_t checksum = 0;
	const auto start = std::chrono::steady_clock::now();
	for (uint64_t frame = 0; frame < frames; ++frame)
	{
		groups.reset(0, frame);
		for (uint16_t i = 0; i < source.size(); ++i)
		{
			auto shard = source[i];
			shard.frame_idx = frame;
			const bool primary = not mixed_secondary or i % 3 != 0;
			std::span<const uint8_t> fec_blob;
			if (fec_active)
				fec_blob = groups.add(shard, primary);
			if (fec_active and groups.block_full())
				while (auto parity = groups.take())
					checksum += parity->payload.size();
			if (primary)
			{
				if (reuse and fec_active)
					history.push(frame, i, fec_blob, true);
				else
				{
					fec::encode_blob(shard, old_blob);
					history.push(frame, i, old_blob, true);
				}
			}
			else if (not reuse)
			{
				fec::encode_blob(shard, old_blob);
				checksum += old_blob.size();
			}
		}
		if (fec_active)
			while (auto parity = groups.take())
				checksum += parity->payload.size();
	}
	const auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - start).count());
	std::printf("checksum=%llu\n", (unsigned long long)checksum);
	return ns;
}

static bool equal(const snapshot & a, const snapshot & b)
{
	return a.parity == b.parity and a.history == b.history;
}

int main()
{
	for (int round = 0; round < 5; ++round)
	for (bool mixed: {false, true})
		for (bool fec_active: {false, true})
		{
			const auto old = one_frame(false, fec_active, mixed);
			const auto fresh = one_frame(true, fec_active, mixed);
			if (not equal(old, fresh) or old.history.size() != (mixed ? 10u : 16u))
				return std::fprintf(stderr, "correctness failure fec=%d mixed=%d\n", fec_active, mixed), 1;
			(void)timed(false, fec_active, mixed, 100);
			(void)timed(true, fec_active, mixed, 100);
			const auto a = timed(false, fec_active, mixed);
			const auto b = timed(true, fec_active, mixed);
			const auto b2 = timed(true, fec_active, mixed);
			const auto a2 = timed(false, fec_active, mixed);
			std::printf("round=%d fec=%d mixed=%d old=%llu new=%llu new2=%llu old2=%llu history=%zu parity=%zu\n",
			            round, fec_active, mixed, (unsigned long long)a, (unsigned long long)b,
			            (unsigned long long)b2, (unsigned long long)a2,
			            fresh.history.size(), fresh.parity.size());
		}
}
