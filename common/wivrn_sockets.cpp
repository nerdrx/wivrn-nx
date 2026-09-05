/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_sockets.h"

#include "crypto.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <ctime>
#include <limits.h>
#include <memory>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>

thread_local crypto::encrypt_context wivrn::UDP::encrypter{EVP_aes_128_ctr()};
std::atomic<uint64_t> wivrn::UDP::iv_counter;

namespace
{
// A single UDP datagram to a connected socket, retrying the transient
// backpressure errors a burst provokes instead of dropping the datagram.
//
// The failure this exists for: an NX Warp intra frame is dozens of datagrams
// pushed onto the socket back to back with no spacing. On a real link the device
// queue (or, on a socket with a send timeout, the socket send buffer) fills part
// way through the burst and the kernel returns ENOBUFS / EAGAIN *synchronously*
// from writev — not a fatal error, just "come back in a moment". The old code let
// that throw, the caller swallowed it, and every datagram from that point to the
// end of the frame was silently dropped: the client saw the head of the frame,
// never the tail, and every large frame reassembled with a hole and decoded none.
// A small frame (a handful of datagrams) never fills the queue and was unaffected,
// which is exactly the size-dependent loss that was observed.
//
// The queue drains in microseconds, so a short bounded backoff recovers the whole
// frame. The retry re-issues the *same* writev: the payload was already encrypted
// in place under an IV that was already consumed, so re-sending the identical
// bytes keeps the CTR keystream and the wire in step (unlike TCP, where a partial
// send has already advanced the stream). EINTR is retried for free and does not
// count against the budget. Anything else, or exhausting the budget, throws as
// before so the path-liveness machinery still sees a genuinely dead link.
ssize_t writev_with_retry(int fd, const iovec * iov, int iovcnt)
{
	// ~64 tries at 60us is under 4ms of worst-case backoff — comfortably inside a
	// frame period even at a high refresh, and far cheaper than losing the frame.
	constexpr int max_retries = 64;
	constexpr long backoff_ns = 60'000;

	for (int transient = 0;;)
	{
		ssize_t sent = ::writev(fd, iov, iovcnt);
		if (sent >= 0)
			return sent;

		const int err = errno;
		if (err == EINTR)
			continue;

		const bool retryable = (err == EAGAIN or err == EWOULDBLOCK or err == ENOBUFS);
		if (not retryable or transient >= max_retries)
			return sent; // caller reads errno

		++transient;
		timespec ts{.tv_sec = 0, .tv_nsec = backoff_ns};
		::nanosleep(&ts, nullptr);
	}
}
} // namespace

const char * wivrn::invalid_packet::what() const noexcept
{
	return "Invalid packet";
}

const char * wivrn::socket_shutdown::what() const noexcept
{
	return "Socket shutdown";
}

wivrn::fd_base::fd_base(wivrn::fd_base && other) :
        fd(other.fd)
{
	other.fd = -1;
}

wivrn::fd_base & wivrn::fd_base::operator=(wivrn::fd_base && other)
{
	std::swap(fd, other.fd);
	return *this;
}

wivrn::fd_base::~fd_base()
{
	if (fd >= 0)
		::close(fd);
}

wivrn::UDP::UDP()
{
	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0)
		throw std::system_error{errno, std::generic_category()};
	fcntl(fd, F_SETFD, FD_CLOEXEC);
}

wivrn::UDP::UDP(int fd)
{
	this->fd = fd;
}

void wivrn::UDP::bind(sockaddr_in6 address)
{
	if (::bind(fd, (sockaddr *)&address, sizeof(address)) < 0)
		throw std::system_error{errno, std::generic_category()};
}

void wivrn::UDP::connect(in6_addr address, int port)
{
	sockaddr_in6 sa;
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = address;
	sa.sin6_port = htons(port);

	if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		throw std::system_error{errno, std::generic_category()};
}

void wivrn::UDP::connect(in_addr address, int port)
{
	sockaddr_in sa;
	sa.sin_family = AF_INET;
	sa.sin_addr = address;
	sa.sin_port = htons(port);

	if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		throw std::system_error{errno, std::generic_category()};
}

void wivrn::UDP::subscribe_multicast(in6_addr address)
{
	assert(IN6_IS_ADDR_MULTICAST(&address));

	ipv6_mreq subscribe{};
	subscribe.ipv6mr_multiaddr = address;

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &subscribe, sizeof(subscribe)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}
}

void wivrn::UDP::unsubscribe_multicast(in6_addr address)
{
	assert(IN6_IS_ADDR_MULTICAST(&address));

	ipv6_mreq subscribe{};
	subscribe.ipv6mr_multiaddr = address;

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, &subscribe, sizeof(subscribe)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}
}

void wivrn::UDP::set_receive_buffer_size(int size)
{
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

void wivrn::UDP::set_send_buffer_size(int size)
{
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

void wivrn::TCP::init()
{
	int nodelay = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	mutex = std::make_unique<std::mutex>();
}

wivrn::TCP::TCP(int fd)
{
	this->fd = fd;

	// fd == -1 is the "empty socket" sentinel (e.g. an unattached secondary
	// path): there is nothing to configure and setsockopt would fail EBADF.
	if (fd != -1)
		init();
}

wivrn::TCP::TCP(in6_addr address, int port)
{
	fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd < 0)
		throw std::system_error{errno, std::generic_category()};
	fcntl(fd, F_SETFD, FD_CLOEXEC);

	sockaddr_in6 sa;
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = address;
	sa.sin6_port = htons(port);

	if (connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	init();
}

wivrn::TCP::TCP(in_addr address, int port)
{
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		throw std::system_error{errno, std::generic_category()};
	fcntl(fd, F_SETFD, FD_CLOEXEC);

	sockaddr_in sa;
	sa.sin_family = AF_INET;
	sa.sin_addr = address;
	sa.sin_port = htons(port);

	if (connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	init();
}

wivrn::TCPListener::TCPListener(int port)
{
	fd = socket(AF_INET6, SOCK_STREAM, 0);

	if (fd < 0)
	{
		throw std::system_error{errno, std::generic_category()};
	}

	int reuse_addr = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_any;

	if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	int backlog = 1;
	if (listen(fd, backlog) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}
}

std::pair<wivrn::deserialization_packet, sockaddr_in6> wivrn::UDP::receive_from_raw()
{
	sockaddr_in6 addr;
	socklen_t addrlen = sizeof(addr);

	ssize_t peeked = recvfrom(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC, (sockaddr *)&addr, &addrlen);
	if (peeked < 0)
		throw std::system_error{errno, std::generic_category()};

	auto buffer = std::make_shared_for_overwrite<uint8_t[]>(peeked);
	ssize_t received = recvfrom(fd, buffer.get(), peeked, 0, (sockaddr *)&addr, &addrlen);
	if (received < 0)
		throw std::system_error{errno, std::generic_category()};

	std::span message{buffer.get(), (size_t)received};

	if (encrypted)
	{
		// Not big enough for the IV: drop the packet
		if (received < sizeof(uint64_t))
		{
			count_dropped_datagram();
			return {};
		}

		std::array<uint8_t, 16> full_iv;
		memcpy(full_iv.data(), buffer.get(), sizeof(uint64_t)); // TODO: endianness?
		memcpy(full_iv.data() + sizeof(uint64_t), recv_iv_header.data(), recv_iv_header.size());

		message = message.subspan(sizeof(uint64_t));

		decrypter.set_iv(full_iv);
		decrypter.decrypt_in_place(message);
	}

	return {deserialization_packet{std::move(buffer), message}, addr};
}

wivrn::deserialization_packet wivrn::UDP::receive_pending()
{
	if (messages.empty())
		return {};

	auto span = messages.back();
	messages.pop_back();
	return deserialization_packet{buffer, span};
}

wivrn::deserialization_packet wivrn::UDP::receive_raw()
{
	if (not messages.empty())
	{
		auto span = messages.back();
		messages.pop_back();
		return deserialization_packet{buffer, span};
	}

	static const size_t message_size = 2048;
	static const size_t num_messages = 20;
	if ((not buffer) or buffer.use_count() > 1)
	{
		buffer = std::make_shared_for_overwrite<uint8_t[]>(message_size * num_messages);
	}
	std::array<iovec, num_messages> iovecs;
	std::array<mmsghdr, num_messages> mmsgs;
	for (size_t i = 0; i < num_messages; ++i)
	{
		iovecs[i] = {
		        .iov_base = buffer.get() + message_size * i,
		        .iov_len = message_size,
		};

		mmsgs[i] = {
		        .msg_hdr = {
		                .msg_iov = &iovecs[i],
		                .msg_iovlen = 1,
		        },
		};
	}

	int received = recvmmsg(fd, mmsgs.data(), num_messages, MSG_DONTWAIT | MSG_TRUNC, nullptr);

	if (received < 0)
		throw std::system_error{errno, std::generic_category()};
	if (received == 0)
		throw socket_shutdown();

	messages.reserve(received);

	// Messages are pushed in reverse order, receive_pending pops from the back
	for (int i = received - 1; i >= 0; --i)
	{
		// With MSG_TRUNC msg_len is the size of the datagram, which may be
		// larger than the buffer: drop what we could not read entirely
		if (mmsgs[i].msg_len > message_size)
		{
			count_dropped_datagram();
			continue;
		}

		std::span<uint8_t> message{(uint8_t *)iovecs[i].iov_base, mmsgs[i].msg_len};
		assert(message.data() != nullptr);

		if (encrypted)
		{
			// Not big enough for the IV: drop the packet
			if (message.size() < sizeof(uint64_t))
			{
				count_dropped_datagram();
				continue;
			}

			std::array<uint8_t, 16> full_iv;
			memcpy(full_iv.data(), message.data(), sizeof(uint64_t)); // TODO: endianness?
			memcpy(full_iv.data() + sizeof(uint64_t), recv_iv_header.data(), recv_iv_header.size());

			message = message.subspan(sizeof(uint64_t));

			decrypter.set_iv(full_iv);
			decrypter.decrypt_in_place(message);
		}

		messages.push_back(message);
	}

	return receive_pending();
}

uint64_t wivrn::UDP::take_dropped_datagrams()
{
	if (dropped_datagrams_ == reported_dropped_datagrams)
		return 0;

	auto now = std::chrono::steady_clock::now();
	if (now < next_drop_report)
		return 0;
	next_drop_report = now + std::chrono::seconds(1);

	uint64_t dropped = dropped_datagrams_ - reported_dropped_datagrams;
	reported_dropped_datagrams = dropped_datagrams_;
	return dropped;
}

size_t wivrn::UDP::send_raw(serialization_packet && packet)
{
	thread_local std::vector<iovec> iovecs;
	iovecs.clear();

	std::vector<std::span<uint8_t>> & data = packet;

	uint64_t counter;
	if (encrypted)
	{
		counter = iv_counter.fetch_add(1);

		std::array<uint8_t, 16> full_iv;
		memcpy(full_iv.data(), &counter, sizeof(uint64_t)); // TODO: endianness?
		memcpy(full_iv.data() + sizeof(uint64_t), send_iv_header.data(), send_iv_header.size());

		iovecs.emplace_back(&counter, sizeof(uint64_t));

		encrypter.set_key_and_iv(key, full_iv);
		encrypter.encrypt_in_place(data);
	}

	for (const auto & span: data)
		iovecs.emplace_back(span.data(), span.size());

	if (ssize_t sent = writev_with_retry(fd, iovecs.data(), int(iovecs.size())); sent >= 0)
		return sent;
	throw std::system_error{errno, std::generic_category()};
}

size_t wivrn::UDP::send_many_raw(std::span<serialization_packet> packets)
{
	thread_local std::vector<iovec> iovecs;
	thread_local std::vector<mmsghdr> mmsgs;
	thread_local std::vector<uint64_t> iv_counters;

	if (packets.empty())
		return 0;

	iovecs.clear();
	mmsgs.clear();
	iv_counters.clear();

	iv_counters.reserve(packets.size());

	size_t sent = 0;
	for (serialization_packet & packet: packets)
	{
		std::vector<std::span<uint8_t>> & data = packet;

		if (encrypted)
		{
			iv_counters.push_back(iv_counter.fetch_add(1));

			std::array<uint8_t, 16> full_iv;
			memcpy(full_iv.data(), &iv_counters.back(), sizeof(uint64_t)); // TODO: endianness?
			memcpy(full_iv.data() + sizeof(uint64_t), send_iv_header.data(), send_iv_header.size());

			iovecs.emplace_back(&iv_counters.back(), sizeof(uint64_t));

			encrypter.set_key_and_iv(key, full_iv);
			encrypter.encrypt_in_place(data);
		}

		for (const auto & span: data)
		{
			iovecs.emplace_back(span.data(), span.size_bytes());
			sent += span.size();
		}

		if (encrypted)
			mmsgs.push_back({.msg_hdr = {.msg_iovlen = data.size() + 1}});
		else
			mmsgs.push_back({.msg_hdr = {.msg_iovlen = data.size()}});
	}

	for (size_t i = 0, j = 0; i < packets.size(); ++i)
	{
		mmsgs[i].msg_hdr.msg_iov = &iovecs[j];
		j += mmsgs[i].msg_hdr.msg_iovlen;
	}

	// sendmmsg may not send all messages, just consider them as lost for UDP
	if (sendmmsg(fd, mmsgs.data(), mmsgs.size(), 0) < 0)
		throw std::system_error{errno, std::generic_category()};
	return sent;
}

wivrn::deserialization_packet wivrn::TCP::receive_raw()
{
	// Single reader by design: the reassembly buffer and the receive decrypter
	// are deliberately used without taking the mutex, which only serializes
	// senders, so that a receive never waits behind a slow send. The receive
	// keystream is a separate cipher context from the send one.
	//
	// A socket whose send failed is dead in both directions: the path selectors
	// assume a failed TCP socket never comes back, and honouring that here is
	// what makes the assumption safe (see the broken flag).
	if (broken)
		throw socket_shutdown{};

	static constexpr size_t max_payload = 16 * 1024 * 1024;

	ssize_t expected_size;

	if (data.size_bytes() < sizeof(uint32_t))
	{
		expected_size = sizeof(uint32_t) - data.size_bytes();
	}
	else
	{
		uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
		if (payload_size > max_payload)
			throw std::runtime_error("Invalid packet: size " + std::to_string(payload_size));
		expected_size = payload_size + sizeof(uint32_t) - data.size_bytes();
	}

	if (expected_size > capacity_left)
	{
		size_t new_size = std::max<size_t>(data.size_bytes() + expected_size,
		                                   4096);
		auto old = std::move(buffer);
		buffer = std::make_shared_for_overwrite<uint8_t[]>(new_size);
		memcpy(buffer.get(), data.data(), data.size_bytes());
		data = std::span(buffer.get(), data.size());
		capacity_left = new_size - data.size_bytes();
	}

	if (capacity_left > 0)
	{
		ssize_t received_size = recv(fd, &*data.end(), capacity_left, MSG_DONTWAIT);

		if (received_size < 0)
			throw std::system_error{errno, std::generic_category()};

		if (received_size == 0)
			throw socket_shutdown{};

		if (decrypter)
		{
			std::span<uint8_t> received_data{&*data.end(), (size_t)received_size};
			decrypter.decrypt_in_place(received_data);
		}

		data = std::span(data.data(), data.size() + received_size);
		capacity_left -= received_size;
	}

	if (data.size_bytes() < sizeof(uint32_t))
		return {};

	uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
	if (payload_size == 0)
		throw std::runtime_error("Invalid packet: 0 size");

	if (data.size_bytes() < sizeof(uint32_t) + payload_size)
		return {};

	auto span = data.subspan(sizeof(uint32_t), payload_size);
	data = data.subspan(sizeof(uint32_t) + payload_size);
	return deserialization_packet{buffer, span};
}

wivrn::deserialization_packet wivrn::TCP::receive_pending()
{
	if (data.size_bytes() < sizeof(uint32_t))
		return {};

	uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
	if (payload_size == 0)
		throw std::runtime_error("Invalid packet: 0 size");

	if (data.size_bytes() < sizeof(uint32_t) + payload_size)
		return {};

	auto span = data.subspan(sizeof(uint32_t), payload_size);
	data = data.subspan(sizeof(uint32_t) + payload_size);
	return deserialization_packet{buffer, span};
}

// Send every iovec entry, chunking at IOV_MAX and retrying EINTR. Called with
// the send mutex held, after the payload was encrypted: any failure poisons the
// socket (see the broken flag), because the CTR keystream has already advanced
// past bytes that may never reach the wire.
size_t wivrn::TCP::send_iovecs(iovec * iov, size_t iovcnt)
{
	size_t total_sent = 0;

	while (iovcnt > 0)
	{
		msghdr hdr{
		        .msg_name = nullptr,
		        .msg_namelen = 0,
		        .msg_iov = iov,
		        .msg_iovlen = std::min<size_t>(iovcnt, IOV_MAX),
		        .msg_control = nullptr,
		        .msg_controllen = 0,
		        .msg_flags = 0,
		};

		ssize_t sent = ::sendmsg(fd, &hdr, MSG_NOSIGNAL);

		// A signal before the first byte was transferred: nothing was sent, the
		// keystream still matches the wire, this exact send can be retried.
		// (A signal after a partial transfer makes sendmsg return the count.)
		if (sent < 0 and errno == EINTR)
			continue;

		if (sent <= 0)
		{
			broken = true;
			if (sent == 0)
				throw socket_shutdown{};
			throw std::system_error{errno, std::generic_category()};
		}

		total_sent += sent;

		// iov fully consumed
		while (iovcnt > 0 and (size_t)sent >= iov->iov_len)
		{
			sent -= iov->iov_len;
			++iov;
			--iovcnt;
		}
		if (iovcnt > 0)
		{
			iov->iov_base = (void *)((uintptr_t)iov->iov_base + sent);
			iov->iov_len -= sent;
		}
	}

	return total_sent;
}

size_t wivrn::TCP::send_raw(serialization_packet && packet)
{
	thread_local std::vector<iovec> iovecs;
	iovecs.clear();

	std::vector<std::span<uint8_t>> & data = packet;

	uint32_t size = 0;
	iovecs.emplace_back(&size, sizeof(size));
	for (const auto & span: data)
	{
		size += span.size_bytes();
		iovecs.emplace_back(span.data(), span.size_bytes());
	}

	std::lock_guard lock(*mutex);
	if (broken)
		throw socket_shutdown{};

	if (encrypter)
	{
		data.insert(data.begin(), {(uint8_t *)&size, sizeof(size)});
		try
		{
			encrypter.encrypt_in_place(data);
		}
		catch (...)
		{
			// The keystream position is unknown, nothing sent on this socket
			// could ever be decrypted again
			broken = true;
			throw;
		}
	}

	return send_iovecs(iovecs.data(), iovecs.size());
}

size_t wivrn::TCP::send_many_raw(std::span<serialization_packet> packets)
{
	thread_local std::vector<iovec> iovecs;
	thread_local std::vector<uint32_t> sizes;
	thread_local std::vector<std::span<uint8_t>> spans;

	if (packets.empty())
		return 0;

	iovecs.clear();
	sizes.clear();
	spans.clear();

	sizes.reserve(packets.size());

	for (serialization_packet & packet: packets)
	{
		std::vector<std::span<uint8_t>> & data = packet;

		auto & size = sizes.emplace_back(0);
		iovecs.emplace_back(&size, sizeof(size));
		spans.emplace_back((uint8_t *)&size, sizeof(size));

		for (const auto & span: data)
		{
			size += span.size_bytes();
			iovecs.emplace_back(span.data(), span.size_bytes());
			spans.emplace_back(span.data(), span.size_bytes());
		}
	}

	std::lock_guard lock(*mutex);
	if (broken)
		throw socket_shutdown{};

	if (encrypter)
	{
		try
		{
			encrypter.encrypt_in_place(spans);
		}
		catch (...)
		{
			// The keystream position is unknown, nothing sent on this socket
			// could ever be decrypted again
			broken = true;
			throw;
		}
	}

	return send_iovecs(iovecs.data(), iovecs.size());
}

void wivrn::UDP::set_aes_key_and_ivs(std::span<std::uint8_t, 16> key_, std::span<std::uint8_t, 8> recv_iv_header_, std::span<std::uint8_t, 8> send_iv_header_)
{
	decrypter = crypto::decrypt_context{EVP_aes_128_ctr()};
	decrypter.set_key(key_);

	std::ranges::copy(key_, key.begin());
	std::ranges::copy(recv_iv_header_, recv_iv_header.begin());
	std::ranges::copy(send_iv_header_, send_iv_header.begin());
	encrypted = true;
}

void wivrn::TCP::set_aes_key_and_ivs(std::span<std::uint8_t, 16> key, std::span<std::uint8_t, 16> recv_iv, std::span<std::uint8_t, 16> send_iv)
{
	encrypter = crypto::encrypt_context{EVP_aes_128_ctr()};
	encrypter.set_key(key);
	encrypter.set_iv(send_iv);

	decrypter = crypto::decrypt_context{EVP_aes_128_ctr()};
	decrypter.set_key(key);
	decrypter.set_iv(recv_iv);
}
