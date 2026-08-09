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

#include "util/u_logging.h"
#include "wivrn_ipc.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <stop_token>
#include <system_error>

namespace wivrn
{
class incorrect_pin : public std::runtime_error
{
public:
	incorrect_pin();
};

class wivrn_connection
{
public:
	enum class encryption_state
	{
		disabled,
		enabled,
		pairing,
	};

private:
	typed_socket<TCP, from_headset::packets, to_headset::packets> control;
	typed_socket<UDP, from_headset::packets, to_headset::packets> stream;
	std::atomic<bool> active = false;
	std::string pin;
	encryption_state state;

	from_headset::headset_info_packet info_packet;

	// Secondary (multipath) TCP path, attached at runtime by the main loop
	// process. It has its own liveness: an error here never affects the session.
	typed_socket<TCP, from_headset::packets, to_headset::packets> secondary{-1};
	uint8_t secondary_path_id = 0;
	uint64_t secondary_received = 0;
	std::chrono::steady_clock::time_point secondary_last_receive{};
	std::chrono::steady_clock::time_point secondary_next_report{};

	// Random, sent to the headset in to_headset::handshake, presented back by
	// the headset to attach a secondary path
	std::array<uint8_t, 16> token;

	void init(std::stop_token stop_token, std::function<void()> tick = []() {});

	// Read whatever the secondary path already has buffered, dropping the path
	// on error
	template <typename T>
	void poll_secondary_pending(T && visitor)
	{
		while (secondary)
		{
			std::optional<from_headset::packets> packet;
			try
			{
				packet = secondary.receive_pending();
			}
			catch (const std::exception & e)
			{
				drop_secondary(e.what());
				return;
			}

			if (not packet)
				return;

			on_secondary_received();
			std::visit(std::forward<T>(visitor), std::move(*packet));
		}
	}

	void on_secondary_received();
	void report_secondary_status();

public:
	wivrn_connection(std::stop_token stop_token, encryption_state state, std::string pin, TCP && tcp);
	wivrn_connection(const wivrn_connection &) = delete;
	wivrn_connection & operator=(const wivrn_connection &) = delete;

	bool has_stream() const
	{
		return stream;
	}

	bool is_active()
	{
		return active;
	}
	void reset(std::stop_token stop, TCP && tcp, std::function<void()> tick = {});
	void shutdown();

	const std::array<uint8_t, 16> & session_token() const
	{
		return token;
	}

	bool has_secondary() const
	{
		return bool(secondary);
	}

	// Take over a secondary connection accepted and authenticated by the main
	// loop process, fd ownership is transferred
	void attach_secondary(int fd, const path_secrets & secrets);

	// Close the secondary path, the session is not affected
	void drop_secondary(std::string_view reason);

	// Never throws, a failure only drops the secondary path
	template <typename T>
	void send_secondary(T && packet)
	{
		if (not secondary)
			return;

		try
		{
			secondary.send(std::forward<T>(packet));
		}
		catch (const std::exception & e)
		{
			drop_secondary(e.what());
		}
	}

	template <typename T>
	void send_control(T && packet)
	{
		try
		{
			if (active)
				control.send(std::forward<T>(packet));
		}
		catch (...)
		{
			active = false;
			throw;
		}
	}

	template <typename T>
	void send_stream(T && packet)
	{
		try
		{
			if (active)
			{
				if (stream)
					stream.send(std::forward<T>(packet));
				else
					control.send(std::forward<T>(packet));
			}
		}
		catch (...)
		{
			active = false;
			throw;
		}
	}

	std::optional<from_headset::packets> poll_control(int timeout);

	const from_headset::headset_info_packet & info()
	{
		return info_packet;
	}

	template <typename T>
	int poll(T && visitor, int timeout)
	{
		pollfd fds[5] = {};
		fds[0].events = POLLIN;
		fds[0].fd = stream.get_fd();
		fds[1].events = POLLIN;
		fds[1].fd = control.get_fd();
		fds[2].fd = wivrn_ipc_socket_monado->get_fd();
		fds[2].events = POLLIN;
		fds[3].fd = wivrn_path_socket_monado;
		fds[3].events = POLLIN;

		// Malformed datagrams are dropped, only the control socket is fatal
		while (auto packet = stream.receive_pending_lossy())
			std::visit(std::forward<T>(visitor), std::move(*packet));
		while (auto packet = control.receive_pending())
			std::visit(std::forward<T>(visitor), std::move(*packet));
		poll_secondary_pending(std::forward<T>(visitor));

		// After poll_secondary_pending, the path may have been dropped
		fds[4].fd = secondary ? secondary.get_fd() : -1;
		fds[4].events = POLLIN;

		int r = ::poll(fds, std::size(fds), timeout);
		if (r < 0)
			throw std::system_error(errno, std::system_category());

		if (fds[0].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Error on stream socket");

		if (fds[1].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Error on control socket");

		if (fds[2].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Error on IPC socket");

		if (fds[0].revents & POLLIN)
		{
			auto packet = stream.receive_lossy();
			if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		if (fds[1].revents & POLLIN)
		{
			auto packet = control.receive();
			if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		if (fds[2].revents & POLLIN)
		{
			auto packet = receive_from_main();
			if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		if (fds[3].revents & POLLIN)
		{
			if (auto path = receive_path_from_main())
				attach_secondary(path->first, path->second);
		}

		if (fds[4].revents & (POLLHUP | POLLERR | POLLNVAL))
		{
			drop_secondary("socket closed by peer");
		}
		else if (fds[4].revents & POLLIN)
		{
			std::optional<from_headset::packets> packet;
			try
			{
				packet = secondary.receive();
			}
			catch (const std::exception & e)
			{
				drop_secondary(e.what());
			}

			if (packet)
			{
				on_secondary_received();
				std::visit(std::forward<T>(visitor), std::move(*packet));
			}
		}

		report_secondary_status();

		if (uint64_t dropped = stream.take_dropped_datagrams())
			U_LOG_W("Dropped %lu invalid datagram(s) on the stream socket (%lu total)",
			        (unsigned long)dropped,
			        (unsigned long)stream.dropped_datagrams());

		return r;
	}
};
} // namespace wivrn
