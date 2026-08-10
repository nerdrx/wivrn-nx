// TCP send-path hardening test harness.
//
// The TCP sockets encrypt with AES-CTR, a continuous keystream: every byte the
// encrypter moves past must reach the wire exactly once, in order, or every
// later packet on the connection decrypts to garbage. This harness drives
// wivrn::TCP over real loopback TCP connections and checks:
//
//  A. keystream and framing continuity: many packets of awkward sizes, single
//     and batched sends, multi-span packets, all decode byte-exact;
//  B. batches whose iovec count exceeds IOV_MAX are chunked instead of failing
//     with EMSGSIZE (which would advance the keystream with nothing on the
//     wire, the original desync bug);
//  C. a send that fails mid-frame (short write into a full socket buffer, then
//     a send timeout) poisons the socket: every later send or receive throws
//     socket_shutdown instead of putting undecryptable bytes on the wire, and
//     the receiving end never sees a complete (i.e. seemingly valid) frame;
//  D. the same holds for unencrypted sockets, where a partial frame on the
//     wire would desync the length-prefix framing instead of the keystream.
//
// Build:
//   g++ -std=c++23 -I common -I external -I build-server/common \
//       -I build-server/_deps/boost-src/libs/pfr/include \
//       -o tcp_desync_test tests/tcp_desync_test.cpp \
//       common/wivrn_sockets.cpp common/crypto.cpp common/smp.cpp -lcrypto
//   ./tcp_desync_test

#include "wivrn_sockets.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <vector>

using namespace std::chrono_literals;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
	do                                                                    \
	{                                                                     \
		++checks;                                                         \
		if (!(cond))                                                      \
		{                                                                 \
			++failures;                                                   \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
		}                                                                 \
	} while (0)

// Deterministic bytes, no global state
static uint8_t pattern(size_t packet, size_t offset)
{
	return uint8_t((packet * 131 + offset * 7 + 13) & 0xff);
}

static std::vector<uint8_t> make_payload(size_t packet, size_t size)
{
	std::vector<uint8_t> v(size);
	for (size_t i = 0; i < size; ++i)
		v[i] = pattern(packet, i);
	return v;
}

static wivrn::serialization_packet make_packet(const std::vector<uint8_t> & payload)
{
	wivrn::serialization_packet p;
	p.write(payload.data(), payload.size());
	return p;
}

// A connected pair of wivrn::TCP over the loopback
static std::pair<wivrn::TCP, wivrn::TCP> tcp_pair()
{
	wivrn::TCPListener listener(0);

	sockaddr_in6 addr{};
	socklen_t len = sizeof(addr);
	if (getsockname(listener.get_fd(), (sockaddr *)&addr, &len) < 0)
		throw std::system_error{errno, std::generic_category()};

	wivrn::TCP client(in6addr_loopback, ntohs(addr.sin6_port));
	auto [server, peer] = listener.accept<wivrn::TCP>();

	return {std::move(client), std::move(server)};
}

static void set_keys(wivrn::TCP & a, wivrn::TCP & b)
{
	std::array<uint8_t, 16> key;
	std::array<uint8_t, 16> iv_ab; // a sends, b receives
	std::array<uint8_t, 16> iv_ba;
	for (size_t i = 0; i < 16; ++i)
	{
		key[i] = uint8_t(0xa0 + i);
		iv_ab[i] = uint8_t(0x10 + i);
		iv_ba[i] = uint8_t(0x60 + i);
	}

	a.set_aes_key_and_ivs(key, iv_ba, iv_ab);
	b.set_aes_key_and_ivs(key, iv_ab, iv_ba);
}

// Receive one complete frame, waiting up to the deadline. Returns an empty
// vector on timeout.
static std::vector<uint8_t> recv_one(wivrn::TCP & sock, std::chrono::milliseconds timeout = 5s)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (true)
	{
		if (auto pkt = sock.receive_pending(); not pkt.empty())
			return {pkt.initial_buffer.begin(), pkt.initial_buffer.end()};

		if (std::chrono::steady_clock::now() > deadline)
			return {};

		pollfd fds{.fd = sock.get_fd(), .events = POLLIN, .revents = 0};
		if (::poll(&fds, 1, 100) < 0)
			throw std::system_error{errno, std::generic_category()};
		if (not(fds.revents & POLLIN))
			continue;

		if (auto pkt = sock.receive_raw(); not pkt.empty())
			return {pkt.initial_buffer.begin(), pkt.initial_buffer.end()};
	}
}

// --- A: keystream and framing continuity ------------------------------------

static void test_continuity()
{
	auto [a, b] = tcp_pair();
	set_keys(a, b);

	// Sizes crossing the AES block size in every phase, plus a large frame that
	// takes several recv() calls to reassemble
	std::vector<size_t> sizes;
	for (size_t s = 1; s <= 70; ++s)
		sizes.push_back(s);
	sizes.push_back(100 * 1024);
	for (size_t s = 71; s <= 90; ++s)
		sizes.push_back(s);

	size_t idx = 0;
	for (size_t size: sizes)
	{
		auto payload = make_payload(idx, size);
		size_t sent = a.send_raw(make_packet(payload));
		CHECK(sent == size + sizeof(uint32_t));

		auto got = recv_one(b);
		if (got != payload)
		{
			CHECK(false && "payload mismatch");
			return;
		}
		++idx;
	}

	// A multi-span packet: external spans are gathered into one frame
	{
		std::vector<std::vector<uint8_t>> chunks;
		std::vector<uint8_t> expected;
		wivrn::serialization_packet p;
		for (size_t i = 0; i < 10; ++i)
		{
			chunks.push_back(make_payload(1000 + i, 33 + i));
			expected.insert(expected.end(), chunks.back().begin(), chunks.back().end());
		}
		for (auto & c: chunks)
			p.write(std::span(c));

		a.send_raw(std::move(p));
		CHECK(recv_one(b) == expected);
	}

	// Both directions share nothing: send the other way too
	{
		auto payload = make_payload(2000, 1234);
		b.send_raw(make_packet(payload));
		CHECK(recv_one(a) == payload);
	}

	std::printf("continuity: ok\n");
}

// --- B: batches larger than IOV_MAX -----------------------------------------

static void test_iov_max_batch()
{
	auto [a, b] = tcp_pair();
	set_keys(a, b);

	// Each packet contributes 2 iovec entries (size header + payload):
	// comfortably past IOV_MAX in a single send_many_raw call
	const size_t count = IOV_MAX + 200;
	const size_t payload_size = 25;

	std::vector<std::vector<uint8_t>> payloads;
	std::vector<wivrn::serialization_packet> packets;
	payloads.reserve(count);
	packets.reserve(count);
	size_t expected_bytes = 0;
	for (size_t i = 0; i < count; ++i)
	{
		payloads.push_back(make_payload(i, payload_size));
		packets.push_back(make_packet(payloads.back()));
		expected_bytes += payload_size + sizeof(uint32_t);
	}

	size_t sent = a.send_many_raw(packets);
	CHECK(sent == expected_bytes);

	for (size_t i = 0; i < count; ++i)
	{
		if (recv_one(b) != payloads[i])
		{
			CHECK(false && "batched payload mismatch");
			return;
		}
	}
	CHECK(true);

	// A single packet with more spans than IOV_MAX must be chunked too
	{
		std::vector<std::vector<uint8_t>> chunks;
		std::vector<uint8_t> expected;
		wivrn::serialization_packet p;
		for (size_t i = 0; i < IOV_MAX + 50; ++i)
		{
			chunks.push_back(make_payload(3000 + i, 32));
			expected.insert(expected.end(), chunks.back().begin(), chunks.back().end());
		}
		for (auto & c: chunks)
			p.write(std::span(c));

		a.send_raw(std::move(p));
		CHECK(recv_one(b) == expected);
	}

	// The keystream is still aligned afterwards
	{
		auto payload = make_payload(4000, 99);
		a.send_raw(make_packet(payload));
		CHECK(recv_one(b) == payload);
	}

	std::printf("iov_max batch: ok\n");
}

// --- C/D: a failed send poisons the socket ----------------------------------

static void test_poison(bool encrypted)
{
	auto [a, b] = tcp_pair();
	if (encrypted)
		set_keys(a, b);

	// Small send buffer, nobody reading on the other end, and a send timeout:
	// sendmsg writes part of the frame, then fails with EAGAIN. This is the
	// mid-batch short write: the keystream (and the frame) are ahead of the wire.
	int sndbuf = 4096;
	setsockopt(a.get_fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	timeval tv{.tv_sec = 0, .tv_usec = 100'000};
	setsockopt(a.get_fd(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	auto big = make_payload(0, 8 * 1024 * 1024);

	bool threw = false;
	try
	{
		a.send_raw(make_packet(big));
	}
	catch (const std::system_error & e)
	{
		threw = true;
		CHECK(e.code().value() == EAGAIN || e.code().value() == EWOULDBLOCK);
	}
	catch (const wivrn::socket_shutdown &)
	{
		threw = true;
	}
	CHECK(threw);

	// Poisoned: no later send may reach the wire
	bool poisoned = false;
	try
	{
		a.send_raw(make_packet(make_payload(1, 8)));
	}
	catch (const wivrn::socket_shutdown &)
	{
		poisoned = true;
	}
	CHECK(poisoned);

	// ... and the path selectors assume a failed TCP socket never receives again
	poisoned = false;
	try
	{
		a.receive_raw();
	}
	catch (const wivrn::socket_shutdown &)
	{
		poisoned = true;
	}
	CHECK(poisoned);

	// The peer got a truncated frame: it must never assemble into a complete
	// packet, and nothing must follow it
	auto got = recv_one(b, 200ms);
	CHECK(got.empty());

	std::printf("poison (%s): ok\n", encrypted ? "encrypted" : "plaintext");
}

int main()
{
	test_continuity();
	test_iov_max_batch();
	test_poison(true);
	test_poison(false);

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
