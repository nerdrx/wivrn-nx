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

#pragma once

#include "crypto.h"
#include "socket_tos.h"
#include "wivrn_serialization.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netinet/ip.h>
#include <span>
#include <utility>
#include <vector>

namespace wivrn
{
class socket_shutdown : public std::exception
{
public:
	const char * what() const noexcept override;
};

class invalid_packet : public std::exception
{
public:
	const char * what() const noexcept override;
};

template <typename T, typename... Ts>
struct index_of_type
{};

template <typename T, typename... Ts>
struct index_of_type<T, T, Ts...> : std::integral_constant<size_t, 0>
{};

template <typename T, typename T2, typename... Ts>
struct index_of_type<T, T2, Ts...> : std::integral_constant<size_t, 1 + index_of_type<T, Ts...>::value>
{};

static_assert(index_of_type<int, int, float>::value == 0);
static_assert(index_of_type<float, int, float>::value == 1);

class fd_base
{
protected:
	int fd = -1;

	fd_base(const fd_base &) = delete;

public:
	fd_base() = default;
	fd_base(int fd) :
	        fd{fd} {}
	fd_base(fd_base &&);
	fd_base & operator=(fd_base &&);
	~fd_base();

	int get_fd() const
	{
		return fd;
	}

	// See set_socket_tos. Both socket kinds are marked: the split between the
	// video and the control class is per socket, see namespace tos.
	bool set_tos(int type_of_service) const
	{
		return fd != -1 and set_socket_tos(fd, type_of_service);
	}

	operator bool() const
	{
		return fd != -1;
	}

	operator int() const
	{
		return fd;
	}
};

class UDP : public fd_base
{
	std::shared_ptr<uint8_t[]> buffer;
	std::vector<std::span<uint8_t>> messages;

	crypto::decrypt_context decrypter;
	static thread_local crypto::encrypt_context encrypter;
	static std::atomic<uint64_t> iv_counter;
	static_assert(sizeof(iv_counter) == 8);

	bool encrypted = false;
	std::array<uint8_t, 16> key;
	std::array<uint8_t, 16 - sizeof(iv_counter)> recv_iv_header;
	std::array<uint8_t, 16 - sizeof(iv_counter)> send_iv_header;

	// Malformed datagrams are dropped, they must not be fatal for the session.
	// Only accessed from the thread that receives on this socket.
	uint64_t dropped_datagrams_ = 0;
	uint64_t reported_dropped_datagrams = 0;
	std::chrono::steady_clock::time_point next_drop_report{};

public:
	UDP();
	explicit UDP(int fd);

	deserialization_packet receive_raw();
	deserialization_packet receive_pending();
	std::pair<wivrn::deserialization_packet, sockaddr_in6> receive_from_raw();
	size_t send_raw(serialization_packet && packet);
	size_t send_many_raw(std::span<serialization_packet> packets);

	void connect(in6_addr address, int port);
	void connect(in_addr address, int port);
	void bind(sockaddr_in6 address);
	void subscribe_multicast(in6_addr address);
	void unsubscribe_multicast(in6_addr address);
	void set_receive_buffer_size(int size);
	void set_send_buffer_size(int size);

	void set_aes_key_and_ivs(std::span<std::uint8_t, 16> key, std::span<std::uint8_t, 8> recv_iv_header, std::span<std::uint8_t, 8> send_iv_header);

	// Account for a datagram that was dropped because it could not be decoded
	void count_dropped_datagram()
	{
		++dropped_datagrams_;
	}

	// Total number of datagrams dropped on this socket
	uint64_t dropped_datagrams() const
	{
		return dropped_datagrams_;
	}

	// Number of datagrams dropped since the last report, rate limited to one
	// report per second; returns 0 if there is nothing to report
	uint64_t take_dropped_datagrams();
};

class TCP : public fd_base
{
	std::shared_ptr<uint8_t[]> buffer;
	ssize_t capacity_left = 0;
	std::span<uint8_t> data;
	std::unique_ptr<std::mutex> mutex;

	void init();

	crypto::decrypt_context decrypter;
	crypto::encrypt_context encrypter;

public:
	TCP() = default;
	TCP(in6_addr address, int port);
	TCP(in_addr address, int port);
	explicit TCP(int fd);

	deserialization_packet receive_raw();
	deserialization_packet receive_pending();
	size_t send_raw(serialization_packet && packet);
	size_t send_many_raw(std::span<serialization_packet> packets);

	void set_aes_key_and_ivs(std::span<std::uint8_t, 16> key, std::span<std::uint8_t, 16> recv_iv, std::span<std::uint8_t, 16> send_iv);
};

using UnixDatagram = UDP;

class TCPListener : public fd_base
{
public:
	TCPListener() = default;
	TCPListener(int port);

	template <typename T = TCP>
	std::pair<T, sockaddr_in6> accept()
	{
		assert(fd != -1);
		sockaddr_in6 addr{};
		socklen_t addrlen = sizeof(addr);

		int fd2 = ::accept(fd, (sockaddr *)&addr, &addrlen);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		if (fd2 < 0)
			throw std::system_error{errno, std::generic_category()};

		return {T{fd2}, addr};
	}
};
template <typename Socket, typename ReceivedType, typename SentType>
class typed_socket;

namespace details
{
template <class T, class Tuple>
struct Index;

template <class T, class... Types>
struct Index<T, std::tuple<T, Types...>>
{
	static const std::size_t value = 0;
};

template <class T, class U, class... Types>
struct Index<T, std::tuple<U, Types...>>
{
	static const std::size_t value = 1 + Index<T, std::tuple<Types...>>::value;
};

template <typename T>
concept not_lvalue_reference = !std::is_lvalue_reference_v<T>;

} // namespace details

template <typename Socket, typename ReceivedType, typename... VariantTypes>
class typed_socket<Socket, ReceivedType, std::variant<VariantTypes...>> : public Socket
{
public:
	template <typename... Args>
	typed_socket(Args &&... args) :
	        Socket(std::forward<Args>(args)...)
	{}

	std::optional<ReceivedType> receive_pending(std::atomic<uint64_t> * size = nullptr)
	{
		deserialization_packet packet = ((Socket *)this)->receive_pending();
		if (packet.empty())
			return {};
		if (size)
			size->fetch_add(packet.wire_size());

		return packet.deserialize<ReceivedType>();
	}

	std::optional<ReceivedType> receive(std::atomic<uint64_t> * size = nullptr)
	{
		deserialization_packet packet = this->receive_raw();
		if (packet.empty())
			return {};
		if (size)
			size->fetch_add(packet.wire_size());

		return packet.deserialize<ReceivedType>();
	}

	// Datagram sockets only: a packet that fails to deserialize is dropped and
	// counted instead of being reported to the caller, a single corrupt or
	// spoofed datagram must not tear the session down.
	std::optional<ReceivedType> receive_pending_lossy(std::atomic<uint64_t> * size = nullptr)
	{
		while (true)
		{
			deserialization_packet packet = ((Socket *)this)->receive_pending();
			if (packet.empty())
				return {};
			if (size)
				size->fetch_add(packet.wire_size());

			try
			{
				return packet.deserialize<ReceivedType>();
			}
			catch (const std::exception &)
			{
				this->count_dropped_datagram();
			}
		}
	}

	// See receive_pending_lossy
	std::optional<ReceivedType> receive_lossy(std::atomic<uint64_t> * size = nullptr)
	{
		deserialization_packet packet = this->receive_raw();
		while (not packet.empty())
		{
			if (size)
				size->fetch_add(packet.wire_size());

			try
			{
				return packet.deserialize<ReceivedType>();
			}
			catch (const std::exception &)
			{
				this->count_dropped_datagram();
			}

			// Other datagrams of the same batch may still be valid
			packet = ((Socket *)this)->receive_pending();
		}
		return {};
	}

	// WARNING: serialization packet keeps references to data
	template <typename T>
	static void serialize(serialization_packet & p, const T & data)
	{
		p.clear();
		uint8_t index = details::Index<std::decay_t<T>, std::tuple<VariantTypes...>>::value;
		p.serialize(index);
		p.serialize(data);
	}

	template <details::not_lvalue_reference T>
	size_t send(T && data)
	{
		thread_local serialization_packet p;
		serialize(p, data);
		return this->send_raw(std::move(p));
	}

	size_t send(std::span<serialization_packet> packets)
	{
		return this->send_many_raw(packets);
	}
};

} // namespace wivrn
