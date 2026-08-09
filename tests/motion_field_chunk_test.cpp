// Motion field chunking: a field is several kilobytes, more than a datagram carries
// and more than the headset's 2048 byte receive slots hold, so it travels as chunks
// of whole grid rows of one eye. This checks the two halves of that, without a GPU or
// a headset: split_motion_field / motion_field_assembler in common/motion_field.h, and
// the real serialization of the chunk packet.
//
// Part A: every chunk of a realistically sized field fits in a datagram the headset
// will actually accept.
// Part B: split, serialize every chunk, shuffle them, reassemble: the field that comes
// out is the one that went in.
// Part C: what the network does to it. A missing chunk never completes, duplicates are
// harmless, a newer frame throws away an incomplete older one, a straggler from an
// older frame is ignored, and malformed chunks are refused.
//
// Build (the boost include is wherever the build fetched it):
//   g++ -std=c++23 -I common -I build-client/common -I external \
//       -I build-client/_deps/boost-src/libs/pfr/include \
//       -o motion_field_chunk_test tests/motion_field_chunk_test.cpp common/smp.cpp -lcrypto
//   ./motion_field_chunk_test

#include "motion_field.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
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

// The buffer the headset reads datagrams into: anything longer is dropped whole
// (common/wivrn_sockets.cpp), which is what made an unchunked field never arrive.
constexpr size_t receive_slot = 2048;
// What a chunk should stay under to survive any path worth worrying about
constexpr size_t datagram_budget = 1200;

template <typename T>
T round_trip(const T & value)
{
	serialization_packet packet;
	packet.serialize(value);

	std::vector<uint8_t> flat;
	for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		flat.insert(flat.end(), span.begin(), span.end());

	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[flat.size()]);
	std::memcpy(memory.get(), flat.data(), flat.size());

	deserialization_packet in{memory, std::span<uint8_t>(memory.get(), flat.size())};
	return in.deserialize<T>();
}

// A field whose every value is a function of its position, so a chunk landing at the
// wrong offset cannot pass unnoticed
motion_field_data make_field(uint16_t width, uint16_t height, uint64_t frame_idx)
{
	motion_field_data field{
	        .frame_idx = frame_idx,
	        .span_ns = 33'000'000,
	        .width = width,
	        .height = height,
	        .scale = 0.125f,
	};
	field.vectors.resize(field.value_count());
	for (size_t i = 0; i < field.vectors.size(); ++i)
		field.vectors[i] = int8_t(int((i * 37 + frame_idx * 11) % 255) - 127);
	return field;
}

bool same_field(const motion_field_data & a, const motion_field_data & b)
{
	return a.frame_idx == b.frame_idx and a.span_ns == b.span_ns and a.width == b.width and
	       a.height == b.height and a.scale == b.scale and a.vectors == b.vectors;
}

void test_chunk_size()
{
	std::printf("Part A: chunks fit in a datagram\n");

	// The two ends of the range of grids real resolutions produce, plus a square one
	for (auto [w, h]: {std::pair<uint16_t, uint16_t>{27, 29}, {32, 35}, {40, 40}})
	{
		auto field = make_field(w, h, 100);
		auto chunks = split_motion_field(field);
		CHECK(not chunks.empty());

		size_t largest = 0;
		size_t rows = 0;
		for (const auto & chunk: chunks)
		{
			largest = std::max(largest, serialized_size(chunk));
			rows += chunk.row_count;
			CHECK(chunk.vectors.size() <= to_headset::motion_field::max_chunk_bytes);
		}

		// A whole field really is more than one datagram, which is the point
		const size_t whole = field.value_count();
		std::printf("  %2ux%2u: %zu B of vectors in %zu chunks, largest packet %zu B\n",
		            w,
		            h,
		            whole,
		            chunks.size(),
		            largest);

		CHECK(whole > receive_slot);
		CHECK(largest < datagram_budget);
		CHECK(largest < receive_slot);
		// Every row of both eyes is sent exactly once
		CHECK(rows == size_t(h) * 2);
	}
}

void test_round_trip()
{
	std::printf("Part B: split, serialize, shuffle, reassemble\n");

	auto field = make_field(32, 35, 4242);
	auto chunks = split_motion_field(field);

	// Every chunk goes through the wire format, as it would on the socket
	std::vector<to_headset::motion_field> received;
	for (const auto & chunk: chunks)
	{
		auto back = round_trip(chunk);
		CHECK(back.frame_idx == chunk.frame_idx);
		CHECK(back.view == chunk.view);
		CHECK(back.row_offset == chunk.row_offset);
		CHECK(back.row_count == chunk.row_count);
		CHECK(back.vectors == chunk.vectors);
		received.push_back(std::move(back));
	}

	// UDP keeps no order, and the chunks are self-describing precisely so that it
	// does not have to. Every permutation must give the same field back.
	std::mt19937 rng(12345);
	for (int attempt = 0; attempt < 20; ++attempt)
	{
		auto shuffled = received;
		std::shuffle(shuffled.begin(), shuffled.end(), rng);

		motion_field_assembler assembler;
		for (size_t i = 0; i < shuffled.size(); ++i)
		{
			// Nothing is usable until the last chunk lands
			CHECK(not assembler.complete());
			assembler.add(shuffled[i]);
		}

		CHECK(assembler.complete());
		CHECK(same_field(assembler.field(), field));
	}

	std::printf("  %zu chunks, 20 orders, all reassembled identically\n", received.size());
}

void test_losses_and_garbage()
{
	std::printf("Part C: losses, duplicates, stale and malformed chunks\n");

	auto field = make_field(27, 29, 7);
	auto chunks = split_motion_field(field);
	CHECK(chunks.size() >= 2);

	// A lost chunk means no smoothing this interval, not a half warped frame
	for (size_t missing = 0; missing < chunks.size(); ++missing)
	{
		motion_field_assembler assembler;
		for (size_t i = 0; i < chunks.size(); ++i)
		{
			if (i != missing)
				assembler.add(chunks[i]);
		}
		CHECK(not assembler.complete());
	}

	// Duplicates are just as harmless as they are for video shards
	{
		motion_field_assembler assembler;
		for (const auto & chunk: chunks)
		{
			assembler.add(chunk);
			assembler.add(chunk);
		}
		CHECK(assembler.complete());
		CHECK(same_field(assembler.field(), field));
	}

	// A newer frame drops whatever was still incomplete, and completes on its own
	{
		auto newer = make_field(27, 29, 8);
		auto newer_chunks = split_motion_field(newer);

		motion_field_assembler assembler;
		assembler.add(chunks[0]);
		for (const auto & chunk: newer_chunks)
			assembler.add(chunk);

		CHECK(assembler.complete());
		CHECK(assembler.field().frame_idx == 8);
		CHECK(same_field(assembler.field(), newer));

		// A straggler from the frame before must not corrupt the newer field
		for (const auto & chunk: chunks)
			assembler.add(chunk);
		CHECK(same_field(assembler.field(), newer));
	}

	// A grid that changed size (a resolution change) starts a new assembly
	{
		auto resized = make_field(32, 35, 9);
		auto resized_chunks = split_motion_field(resized);

		motion_field_assembler assembler;
		for (const auto & chunk: chunks)
			assembler.add(chunk);
		CHECK(assembler.complete());
		for (const auto & chunk: resized_chunks)
			assembler.add(chunk);
		CHECK(assembler.complete());
		CHECK(same_field(assembler.field(), resized));
	}

	// Nothing a hostile or broken sender puts on the wire may be trusted: a chunk
	// whose fields do not agree with each other is dropped, not indexed with.
	{
		motion_field_assembler assembler;
		for (size_t i = 1; i < chunks.size(); ++i)
			assembler.add(chunks[i]);

		auto bad = chunks[0];
		bad.view = 2;
		assembler.add(bad);
		CHECK(not assembler.complete());

		bad = chunks[0];
		bad.row_offset = bad.height;
		assembler.add(bad);
		CHECK(not assembler.complete());

		bad = chunks[0];
		bad.row_count = uint16_t(bad.height + 1);
		assembler.add(bad);
		CHECK(not assembler.complete());

		bad = chunks[0];
		bad.vectors.pop_back();
		assembler.add(bad);
		CHECK(not assembler.complete());

		bad = chunks[0];
		bad.width = 0;
		assembler.add(bad);
		CHECK(not assembler.complete());

		// And the honest one still finishes the job
		assembler.add(chunks[0]);
		CHECK(assembler.complete());
		CHECK(same_field(assembler.field(), field));
	}

	// An empty field is not something to divide by
	CHECK(split_motion_field({}).empty());
	CHECK(split_motion_field(motion_field_data{.width = 8, .height = 0}).empty());
	// A grid whose rows alone exceed the budget still sends one row per chunk
	CHECK(to_headset::motion_field::rows_per_chunk(0) == 1);
	CHECK(to_headset::motion_field::rows_per_chunk(4096) == 1);

	std::printf("  losses, duplicates, stale, resized and malformed chunks all handled\n");
}

} // namespace

int main()
{
	test_chunk_size();
	test_round_trip();
	test_losses_and_garbage();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
