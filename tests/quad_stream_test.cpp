// Promoted quad layer stream: the two pieces of the wire format the feature adds,
// checked without a headset or a GPU.
//
// Part A: per stream geometry. The eye streams still derive their size from the one
// width/height pair, the alpha plane is still half height, and the quad stream has a
// size of its own that is zero exactly when the server is not promoting anything.
// Part B: the placement of the quad, carried on the first shard of its stream,
// survives a serialization round trip, and an empty one stays empty.
//
// Build (the boost include is wherever the build fetched it):
//   g++ -std=c++23 -I common -I build-client/common -I external \
//       -I build-client/_deps/boost-src/libs/pfr/include \
//       -o quad_stream_test tests/quad_stream_test.cpp common/smp.cpp -lcrypto
//   ./quad_stream_test

#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <cstdio>
#include <cstring>
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

void test_stream_geometry()
{
	std::printf("Part A: per stream geometry\n");

	to_headset::video_stream_description desc{
	        .width = 1920,
	        .height = 1832,
	        .codec = {h265, h265, h265, h265},
	        .frame_rate = 72,
	        .refresh_rate = 72,
	        .quad_width = 1024,
	        .quad_height = 1024,
	};

	CHECK(desc.stream_size(0) == std::pair<uint16_t, uint16_t>(1920, 1832));
	CHECK(desc.stream_size(1) == std::pair<uint16_t, uint16_t>(1920, 1832));
	// The alpha plane is half height, as it always was
	CHECK(desc.stream_size(2) == std::pair<uint16_t, uint16_t>(1920, 916));
	// The quad is the one stream that does not follow the eyes
	CHECK(desc.stream_size(3) == std::pair<uint16_t, uint16_t>(1024, 1024));

	auto back = round_trip(desc);
	CHECK(back == desc);
	CHECK(back.stream_size(3) == desc.stream_size(3));

	// No quad stream: zero size, which is how the headset knows not to create a
	// decoder for it. Everything else is untouched.
	to_headset::video_stream_description off = desc;
	off.quad_width = 0;
	off.quad_height = 0;
	CHECK(not(off == desc));
	CHECK(off.stream_size(3) == std::pair<uint16_t, uint16_t>(0, 0));
	CHECK(off.stream_size(0) == desc.stream_size(0));
	CHECK(round_trip(off) == off);
}

void test_quad_info()
{
	std::printf("Part B: quad placement on the wire\n");

	using view_info_t = to_headset::video_stream_data_shard::view_info_t;

	view_info_t info{
	        .display_time = 123456789,
	        .pose = {XrPosef{{0, 0, 0, 1}, {0.03f, 0, 0}}, XrPosef{{0, 0, 0, 1}, {-0.03f, 0, 0}}},
	        .fov = {},
	        .foveation = {},
	        .alpha = false,
	        .quad = view_info_t::quad_info_t{
	                .pose = {{0, 0.7071068f, 0, 0.7071068f}, {0.5f, 1.2f, -1.5f}},
	                .size = {.width = 1.6f, .height = 0.9f},
	                .head_locked = false,
	                .source = {.offset = {0, 0}, .extent = {1024, 576}},
	        },
	};

	auto back = round_trip(info);
	CHECK(back.quad.has_value());
	if (back.quad)
	{
		CHECK(back.quad->pose.position.x == info.quad->pose.position.x);
		CHECK(back.quad->pose.position.z == info.quad->pose.position.z);
		CHECK(back.quad->pose.orientation.y == info.quad->pose.orientation.y);
		CHECK(back.quad->size.width == 1.6f);
		CHECK(back.quad->size.height == 0.9f);
		CHECK(back.quad->head_locked == false);
		// The source rectangle is what keeps the panel's aspect ratio: the
		// encode image is square and only part of it holds picture.
		CHECK(back.quad->source.extent.width == 1024);
		CHECK(back.quad->source.extent.height == 576);
	}

	// Head locked layers say so, and the headset submits them in the view space
	info.quad->head_locked = true;
	CHECK(round_trip(info).quad->head_locked == true);

	// The eye streams carry no placement at all, and adding the field must not have
	// made their first shard any bigger.
	view_info_t plain = info;
	plain.quad.reset();
	auto plain_back = round_trip(plain);
	CHECK(not plain_back.quad.has_value());
	CHECK(serialized_size(std::optional<view_info_t>(plain)) < serialized_size(std::optional<view_info_t>(info)));
}

} // namespace

int main()
{
	test_stream_geometry();
	test_quad_info();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
