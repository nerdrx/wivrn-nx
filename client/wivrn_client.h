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
#include "path_selector.h"
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

	// --- Path selector ------------------------------------------------------
	// Mirror of the server side selector, for the headset -> server direction:
	// tracking, inputs and feedback go over the secondary path while the primary
	// is down. The two selectors are independent, an asymmetric state is fine.
	// Any packet received on the primary path proves it is alive in the
	// server -> headset direction; while the server has flipped its own output,
	// the path_pong echo of our keepalive is the only such packet.
	wivrn::path_selector selector;
	uint64_t reported_stream_send_errors = 0;
	std::chrono::steady_clock::time_point next_stream_error_report{};

	// Current state of the QoS marks, so that set_qos only logs real changes
	std::optional<bool> qos_enabled;

	void on_primary_received(bool from_control);
	// Returns true if the failure was absorbed (a usable secondary path exists)
	bool on_control_send_error(const std::exception &);
	void on_stream_send_error(const std::exception &);

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
		if (selector.on_secondary() and secondary_up)
		{
			send_secondary(std::forward<T>(packet));
			return;
		}

		try
		{
			bytes_sent_ += control.send(std::forward<T>(packet));
		}
		catch (const std::exception & e)
		{
			if (on_control_send_error(e))
				return;
			throw;
		}
	}

	template <typename T>
	void send_stream(T && packet)
	{
		// The secondary path is TCP: the stream/control split does not exist
		// there, everything goes over the one socket
		if (selector.on_secondary() and secondary_up)
		{
			send_secondary(std::forward<T>(packet));
			return;
		}

		try
		{
			if (stream)
			{
				bytes_sent_ += stream.send(std::forward<T>(packet));
				selector.on_stream_send_ok();
			}
			else
			{
				bytes_sent_ += control.send(std::forward<T>(packet));
			}
		}
		catch (const std::exception & e)
		{
			// A connected UDP socket latches at most one error (an ICMP
			// unreachable while the access point is gone, ENETUNREACH during a
			// Wi-Fi reassociation) and reports it to the next operation; the
			// datagram is lost but the socket works again as soon as the link
			// is back. Losing datagrams is what UDP does: never fatal.
			if (stream)
			{
				on_stream_send_error(e);
				return;
			}

			if (on_control_send_error(e))
				return;
			throw;
		}
	}

	// Keepalive on the primary path, sent by the path manager every 250 ms so
	// that the server keeps seeing the primary path even while we send
	// everything else over the secondary one. Never throws.
	void send_primary_ping();

	// Apply or clear the DSCP marks on the headset's own sockets. Live: the
	// setting can be flipped from inside the stream. The secondary path is
	// deliberately left alone, it rides a USB tunnel where there is no radio to
	// prioritise anything on.
	//
	// Both of the headset's sockets get EF: what it sends is tracking, inputs,
	// feedback and settings, all small and all latency critical. The bulk video
	// only exists in the other direction, where the server marks it AF41.
	void set_qos(bool enabled);

	// True while tracking and input go over the secondary path
	bool sending_on_secondary() const
	{
		return selector.on_secondary();
	}

	// The primary path already goes through the loopback, i.e. through the USB
	// tunnel: a secondary path over that same tunnel could not survive it
	bool primary_is_loopback() const;

	// Evaluate the liveness of both paths and switch if needed. Network thread
	// only, called at the end of poll().
	void update_paths();

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
		// A control socket that is known broken reports POLLERR on every poll:
		// leaving it in would spin the network thread
		fds[1].fd = selector.control_up() ? control.get_fd() : -1;

		// Malformed datagrams are dropped, only the control socket is fatal
		while (auto packet = stream.receive_pending_lossy(&bytes_received_))
		{
			on_primary_received(false);
			std::visit(std::forward<T>(visitor), std::move(*packet));
		}
		while (selector.control_up())
		{
			std::optional<to_headset::packets> packet;
			try
			{
				packet = control.receive_pending(&bytes_received_);
			}
			catch (const std::exception & e)
			{
				if (not on_control_send_error(e))
					throw;
				break;
			}

			if (not packet)
				break;

			on_primary_received(true);
			std::visit(std::forward<T>(visitor), std::move(*packet));
		}
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
		{
			// A connected UDP socket raises POLLERR for a latched error, most
			// often an ICMP unreachable while the link is down. Draining it
			// makes the socket usable again the moment the link is back, so
			// this must never be a session teardown.
			on_stream_send_error(std::runtime_error("poll reported an error"));
		}

		if (fds[1].revents & (POLLHUP | POLLERR))
		{
			if (not on_control_send_error(std::runtime_error("Error on control socket")))
				throw std::runtime_error("Error on control socket");
		}

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
			std::optional<to_headset::packets> packet;
			try
			{
				packet = stream.receive_lossy(&bytes_received_);
			}
			catch (const std::exception & e)
			{
				// Same as above: recvmmsg reports the latched error
				on_stream_send_error(e);
			}

			if (packet)
			{
				on_primary_received(false);
				std::visit(std::forward<T>(visitor), std::move(*packet));
			}
		}

		if (fds[1].revents & POLLIN)
		{
			std::optional<to_headset::packets> packet;
			try
			{
				packet = control.receive(&bytes_received_);
			}
			catch (const std::exception & e)
			{
				if (not on_control_send_error(e))
					throw;
			}

			if (packet)
			{
				on_primary_received(true);
				std::visit(std::forward<T>(visitor), std::move(*packet));
			}
		}

		if (uint64_t dropped = stream.take_dropped_datagrams())
			spdlog::warn("Dropped {} invalid datagram(s) on the stream socket ({} total)", dropped, stream.dropped_datagrams());

		update_paths();

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
