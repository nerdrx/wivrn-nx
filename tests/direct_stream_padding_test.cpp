// Build with the project's common serialization dependencies and run offline.
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using namespace wivrn;

static to_headset::stream_padding round_trip(const to_headset::stream_padding & value)
{
	serialization_packet packet;
	packet.serialize(value);
	std::vector<uint8_t> bytes;
	for (auto span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		bytes.insert(bytes.end(), span.begin(), span.end());
	assert(bytes.size() == 15);
	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[bytes.size()]);
	std::memcpy(memory.get(), bytes.data(), bytes.size());
	deserialization_packet input{memory, std::span<uint8_t>(memory.get(), bytes.size())};
	return input.deserialize<to_headset::stream_padding>();
}

int main()
{
	to_headset::stream_padding value{};
	for (uint8_t i = 0; i < value.bytes.size(); ++i)
		value.bytes[i] = uint8_t(0xa0u + i);
	const auto decoded = round_trip(value);
	assert(decoded.bytes == value.bytes);
	to_headset::packets packet = value;
	serialization_packet encoded;
	encoded.serialize(packet);
	std::vector<uint8_t> bytes;
	for (auto span: static_cast<std::vector<std::span<uint8_t>> &>(encoded))
		bytes.insert(bytes.end(), span.begin(), span.end());
	assert(bytes.size() == 16); // one variant tag plus the 15-byte payload
	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[bytes.size()]);
	std::memcpy(memory.get(), bytes.data(), bytes.size());
	deserialization_packet input{memory, std::span<uint8_t>(memory.get(), bytes.size())};
	const auto decoded_packet = input.deserialize<to_headset::packets>();
	assert(std::get<to_headset::stream_padding>(decoded_packet).bytes == value.bytes);
	return 0;
}
