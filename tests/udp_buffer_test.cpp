#include "wivrn_sockets.h"

#include <arpa/inet.h>
#include <array>
#include <cstdio>
#include <set>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
constexpr size_t buffer_pool_size = 32;
constexpr size_t batch_slot_size = 2048;

int failures = 0;

#define CHECK(condition)                                                                                 \
	do                                                                                                 \
	{                                                                                                  \
		if (!(condition))                                                                                \
		{                                                                                              \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                \
			++failures;                                                                                  \
		}                                                                                              \
	} while (false)

void send_datagram(int fd, const sockaddr_in & destination, std::span<const uint8_t> bytes)
{
	const auto sent = sendto(fd, bytes.data(), bytes.size(), 0,
	                        reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
	CHECK(sent == ssize_t(bytes.size()));
}

std::array<uint8_t, 16> message(uint8_t id)
{
	std::array<uint8_t, 16> bytes{};
	bytes.fill(id);
	return bytes;
}
} // namespace

int main()
{
	const int rx_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	const int tx_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	CHECK(rx_fd >= 0 and tx_fd >= 0);
	if (rx_fd < 0 or tx_fd < 0)
	{
		if (rx_fd >= 0)
			close(rx_fd);
		if (tx_fd >= 0)
			close(tx_fd);
		return 1;
	}

	sockaddr_in destination{};
	destination.sin_family = AF_INET;
	destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const bool bound = bind(rx_fd, reinterpret_cast<const sockaddr *>(&destination), sizeof(destination)) == 0;
	CHECK(bound);
	socklen_t destination_size = sizeof(destination);
	const bool named = getsockname(rx_fd, reinterpret_cast<sockaddr *>(&destination), &destination_size) == 0;
	CHECK(named);
	if (not bound or not named)
	{
		close(tx_fd);
		close(rx_fd);
		return 1;
	}
	wivrn::UDP receiver(rx_fd);

	// A recvmmsg batch remains ordered through receive_pending.
	for (uint8_t id = 1; id <= 3; ++id)
		send_datagram(tx_fd, destination, message(id));
	auto ordered = receiver.receive_raw();
	CHECK(ordered.initial_buffer.size() == 16 and ordered.initial_buffer[0] == 1);
	auto second = receiver.receive_pending();
	auto third = receiver.receive_pending();
	CHECK(second.initial_buffer.size() == 16 and second.initial_buffer[0] == 2);
	CHECK(third.initial_buffer.size() == 16 and third.initial_buffer[0] == 3);

	// Retain more batch owners than the fixed cache can hold; none may be reused
	// while a deserialization_packet still owns its bytes.
	std::vector<wivrn::deserialization_packet> held;
	std::set<const uint8_t *> addresses;
	for (uint8_t id = 10; id < 10 + buffer_pool_size + 8; ++id)
	{
		send_datagram(tx_fd, destination, message(id));
		auto packet = receiver.receive_raw();
		CHECK(packet.initial_buffer.size() == 16 and packet.initial_buffer[0] == id);
		addresses.insert(packet.initial_buffer.data());
		held.push_back(std::move(packet));
	}
	CHECK(addresses.size() == buffer_pool_size + 8);
	for (size_t i = 0; i < held.size(); ++i)
		CHECK(held[i].initial_buffer.size() == 16 and held[i].initial_buffer[0] == uint8_t(10 + i));

	// After release, the active buffer is reused first; while it is retained again,
	// the other cached batches must be recycled rather than newly allocated.
	const auto active_address = held.back().initial_buffer.data();
	held.clear();
	std::vector<wivrn::deserialization_packet> recycled;
	std::set<const uint8_t *> recycled_addresses;
	for (uint8_t id = 90; id < 94; ++id)
	{
		send_datagram(tx_fd, destination, message(id));
		auto packet = receiver.receive_raw();
		CHECK(packet.initial_buffer.size() == 16 and packet.initial_buffer[0] == id);
		CHECK(addresses.contains(packet.initial_buffer.data()));
		if (id == 90)
			CHECK(packet.initial_buffer.data() == active_address);
		else
			CHECK(packet.initial_buffer.data() != active_address and recycled_addresses.insert(packet.initial_buffer.data()).second);
		recycled.push_back(std::move(packet));
	}

	// Moving the socket preserves the active slot and shared batch owners.
	send_datagram(tx_fd, destination, message(95));
	auto before_move = receiver.receive_raw();
	wivrn::UDP moved_receiver(std::move(receiver));
	CHECK(before_move.initial_buffer.size() == 16 and before_move.initial_buffer[0] == 95);
	send_datagram(tx_fd, destination, message(96));
	auto after_move = moved_receiver.receive_raw();
	CHECK(after_move.initial_buffer.size() == 16 and after_move.initial_buffer[0] == 96);
	CHECK(addresses.contains(after_move.initial_buffer.data()));
	CHECK(before_move.initial_buffer[0] == 95);
	recycled.clear();

	// Oversized and too-short encrypted datagrams remain counted drops.
	std::array<uint8_t, batch_slot_size + 1> oversized{};
	send_datagram(tx_fd, destination, oversized);
	CHECK(moved_receiver.receive_raw().empty());
	CHECK(moved_receiver.dropped_datagrams() == 1);

	std::array<uint8_t, 16> key{};
	std::array<uint8_t, 8> recv_iv{}, send_iv{};
	moved_receiver.set_aes_key_and_ivs(key, recv_iv, send_iv);
	std::array<uint8_t, sizeof(uint64_t) - 1> tiny{};
	send_datagram(tx_fd, destination, tiny);
	CHECK(moved_receiver.receive_raw().empty());
	CHECK(moved_receiver.dropped_datagrams() == 2);

	close(tx_fd);
	if (failures)
		std::fprintf(stderr, "%d UDP buffer test(s) failed\n", failures);
	else
		std::puts("UDP receive buffer tests passed");
	return failures ? 1 : 0;
}
