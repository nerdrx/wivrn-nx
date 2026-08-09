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
#include "wivrn_packets.h"
#include "wivrn_sockets.h"
#include <array>
#include <chrono>
#include <mutex>
#include <poll.h>
#include <shared_mutex>
#include <spdlog/spdlog.h>

using namespace wivrn;

class handshake_error : public std::exception
{
	std::string message;

public:
	const char * what() const noexcept override;
	handshake_error(std::string_view message);
};

class wivrn_session
{
public:
	using control_socket_t = typed_socket<TCP, to_headset::packets, from_headset::packets>;
	using stream_socket_t = typed_socket<UDP, to_headset::packets, from_headset::packets>;

private:
	control_socket_t control;
	stream_socket_t stream;

	std::atomic<uint64_t> bytes_sent_ = 0;
	std::atomic<uint64_t> bytes_received_ = 0;

	// Secondary (multipath) path, attached at runtime by the path manager.
	// Sending (tracking and keepalive threads) and receiving (network thread)
	// only take the mutex in shared mode, so that a slow send on the secondary
	// path can never hold up the network thread; replacing or closing the socket
	// takes it exclusively.
	mutable std::shared_mutex secondary_mutex;
	control_socket_t secondary{-1};
	uint64_t secondary_generation = 0;
	std::atomic<bool> secondary_up = false;

	// Token given by the server in the handshake, needed to attach a path
	std::array<uint8_t, 16> session_token_{};

	template <typename T>
	void handshake(T address, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);

public:
	std::variant<in_addr, in6_addr> address;

	wivrn_session(in6_addr address, int port, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);
	wivrn_session(in_addr address, int port, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);
	wivrn_session(const wivrn_session &) = delete;
	wivrn_session & operator=(const wivrn_session &) = delete;

	template <typename T>
	void send_control(T && packet)
	{
		bytes_sent_ += control.send(std::forward<T>(packet));
	}

	template <typename T>
	void send_stream(T && packet)
	{
		if (stream)
			bytes_sent_ += stream.send(std::forward<T>(packet));
		else
			bytes_sent_ += control.send(std::forward<T>(packet));
	}

	const std::array<uint8_t, 16> & session_token() const
	{
		return session_token_;
	}

	bool has_secondary() const
	{
		return secondary_up;
	}

	// Install an already attached (handshaken) secondary path
	void set_secondary(control_socket_t && socket);

	// Close the secondary path, the session is not affected
	void drop_secondary(std::string_view reason);

	// Duplicate a packet onto the secondary path, never throws: a failure only
	// drops that path
	template <typename T>
	void send_secondary(T && packet)
	{
		std::shared_lock lock(secondary_mutex);
		if (not secondary_up)
			return;

		try
		{
			secondary.send(std::forward<T>(packet));
		}
		catch (std::exception & e)
		{
			std::string reason = e.what();
			lock.unlock();
			drop_secondary(reason);
		}
	}

	// Read whatever the secondary path has buffered
	template <typename T>
	void poll_secondary_pending(T && visitor)
	{
		while (secondary_up)
		{
			std::optional<to_headset::packets> packet;
			{
				std::shared_lock lock(secondary_mutex);
				if (not secondary_up)
					return;

				try
				{
					packet = secondary.receive_pending(&bytes_received_);
				}
				catch (std::exception & e)
				{
					std::string reason = e.what();
					lock.unlock();
					drop_secondary(reason);
					return;
				}
			}

			if (not packet)
				return;

			std::visit(std::forward<T>(visitor), std::move(*packet));
		}
	}

	template <typename T>
	int poll(T && visitor, std::chrono::milliseconds timeout)
	{
		pollfd fds[3] = {};
		fds[0].events = POLLIN;
		fds[0].fd = stream.get_fd();
		fds[1].events = POLLIN;
		fds[1].fd = control.get_fd();

		// Malformed datagrams are dropped, only the control socket is fatal
		while (auto packet = stream.receive_pending_lossy(&bytes_received_))
			std::visit(std::forward<T>(visitor), std::move(*packet));
		while (auto packet = control.receive_pending(&bytes_received_))
			std::visit(std::forward<T>(visitor), std::move(*packet));
		poll_secondary_pending(std::forward<T>(visitor));

		uint64_t secondary_gen = 0;
		{
			std::shared_lock lock(secondary_mutex);
			fds[2].events = POLLIN;
			fds[2].fd = secondary_up ? secondary.get_fd() : -1;
			secondary_gen = secondary_generation;
		}

		int r = ::poll(fds, std::size(fds), timeout.count());
		if (r < 0)
			throw std::system_error(errno, std::system_category());

		if (fds[0].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Error on stream socket");

		if (fds[1].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Error on control socket");

		if (fds[2].revents & (POLLHUP | POLLERR | POLLNVAL))
		{
			drop_secondary("socket closed by peer");
		}
		else if (fds[2].revents & POLLIN)
		{
			std::optional<to_headset::packets> packet;
			std::string error;
			{
				std::shared_lock lock(secondary_mutex);
				// The path may have been replaced while polling
				if (secondary_up and secondary_generation == secondary_gen)
				{
					try
					{
						packet = secondary.receive(&bytes_received_);
					}
					catch (std::exception & e)
					{
						error = e.what();
					}
				}
			}

			if (not error.empty())
				drop_secondary(error);
			else if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		if (fds[0].revents & POLLIN)
		{
			auto packet = stream.receive_lossy(&bytes_received_);
			if (packet)
			{
				std::visit(std::forward<T>(visitor), std::move(*packet));
			}
		}

		if (fds[1].revents & POLLIN)
		{
			auto packet = control.receive(&bytes_received_);
			if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		if (uint64_t dropped = stream.take_dropped_datagrams())
			spdlog::warn("Dropped {} invalid datagram(s) on the stream socket ({} total)", dropped, stream.dropped_datagrams());

		return r;
	}

	uint64_t bytes_received() const
	{
		return bytes_received_;
	}

	uint64_t bytes_sent() const
	{
		return bytes_sent_;
	}
};
