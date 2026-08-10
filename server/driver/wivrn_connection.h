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

#include "path_selector.h"
#include "util/u_logging.h"
#include "wivrn_ipc.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <poll.h>
#include <shared_mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
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
	//
	// Once it carries video it is written by the encoder sender threads and by
	// the worker thread as well as the network thread, so every use takes the
	// mutex in shared mode; only replacing or closing the socket takes it
	// exclusively, and a slow send can then never hold up the network thread.
	mutable std::shared_mutex secondary_mutex;
	typed_socket<TCP, from_headset::packets, to_headset::packets> secondary{-1};
	uint8_t secondary_path_id = 0;
	uint64_t secondary_received = 0;
	std::chrono::steady_clock::time_point secondary_last_receive{};
	std::chrono::steady_clock::time_point secondary_next_report{};
	// False when the secondary shares the primary's peer address, i.e. both ride
	// the same USB tunnel: attaching it is harmless but it can never be a backup
	std::atomic<bool> secondary_can_failover = false;
	// What the headset asked for the secondary path. Combining additionally needs
	// secondary_can_failover: striping over two halves of the same USB tunnel
	// would only make both slower.
	std::atomic<multipath_mode> multipath = multipath_mode::backup;

	// --- Path selector -----------------------------------------------------
	// Decides where control packets and video go. Any packet received on the
	// control or the stream socket, whatever it is, proves the primary path is
	// alive in the headset -> server direction; the headset also sends a
	// path_ping every 250 ms so that this stays true while it has flipped its
	// own output to the secondary.
	path_selector selector;
	// Set before the threads start, read by the network thread
	std::function<void(path_selector::posture, std::string_view reason)> switch_callback;

	uint64_t reported_stream_send_errors = 0;
	std::chrono::steady_clock::time_point next_stream_error_report{};

	// Wi-Fi QoS marks: what the headset asked for, and what the current sockets
	// actually carry (the sockets are replaced on reconnect, the setting is not)
	bool qos_wanted = false;
	std::optional<bool> qos_applied;

	void apply_qos();

	// Random, sent to the headset in to_headset::handshake, presented back by
	// the headset to attach a secondary path
	std::array<uint8_t, 16> token;

	void init(std::stop_token stop_token, std::function<void()> tick = []() {});

	// Read whatever the secondary path already has buffered, dropping the path
	// on error
	template <typename T>
	void poll_secondary_pending(T && visitor)
	{
		while (true)
		{
			std::optional<from_headset::packets> packet;
			std::string error;
			{
				std::shared_lock lock(secondary_mutex);
				if (not secondary)
					return;

				try
				{
					packet = secondary.receive_pending();
				}
				catch (const std::exception & e)
				{
					error = e.what();
				}
			}

			if (not error.empty())
			{
				drop_secondary(error);
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

	// Any packet received on the primary path. from_control tells which socket it
	// came from.
	void on_primary_received(bool from_control);
	// A send failed on the primary control socket. Returns true if the session
	// survives it (there is a usable secondary path to fall back to).
	bool on_control_send_error(const std::exception &);
	// A send failed on the primary stream socket, always survivable
	void on_stream_send_error(const std::exception &);
	// Evaluate the liveness of both paths and switch if needed, network thread only
	void update_paths();

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
		std::shared_lock lock(secondary_mutex);
		return bool(secondary);
	}

	// True while the secondary path carries control and video
	bool video_on_secondary() const
	{
		return selector.on_secondary();
	}

	// True while video is striped over both paths at once (stage 3). The primary
	// still carries everything its pacing window has room for, so this is *not*
	// video_on_secondary and none of the "the stream socket is idle" reasoning
	// that goes with it applies.
	bool combining() const
	{
		return selector.combining();
	}

	// What the headset wants done with the secondary path. Live: the headset
	// sends its whole settings block on every change, and a switch away from
	// `combine` collapses the posture on the next update().
	void set_multipath_mode(multipath_mode mode)
	{
		multipath = mode;
		selector.set_combine_allowed(mode == multipath_mode::combine and secondary_can_failover);
	}

	// Apply or clear the DSCP marks on this end's sockets. Live, and idempotent:
	// the headset sends its whole settings block on every change. The secondary
	// path is deliberately left alone, it rides a USB tunnel through the
	// loopback where there is no radio to prioritise anything on.
	void set_qos(bool enabled);

	// Called on every posture change, from the network thread. Must be set before
	// the session threads start.
	void set_path_switch_callback(std::function<void(path_selector::posture, std::string_view reason)> cb)
	{
		switch_callback = std::move(cb);
	}

	// Take over a secondary connection accepted and authenticated by the main
	// loop process, fd ownership is transferred
	void attach_secondary(int fd, const path_secrets & secrets);

	// Close the secondary path, the session is not affected
	void drop_secondary(std::string_view reason);

	// Read and clear a socket's pending error. A connected datagram socket keeps
	// at most one, reports it to the next operation and is then usable again.
	static void drain_socket_error(int fd, const char * what);

	// Never throws, a failure only drops the secondary path. Returns false, with
	// the packet untouched, when there is no secondary path at all; a failed
	// send returns true, the packet was consumed by the serializer.
	template <typename T>
	bool send_secondary(T && packet)
	{
		std::string error;
		{
			std::shared_lock lock(secondary_mutex);
			if (not secondary)
				return false;

			try
			{
				secondary.send(std::forward<T>(packet));
			}
			catch (const std::exception & e)
			{
				error = e.what();
			}
		}

		if (not error.empty())
			drop_secondary(error);

		return true;
	}

	// Striping send (stage 3): put one video shard on the secondary path and say
	// whether it really went out, so that the caller can put the rest of the frame
	// back on the primary.
	//
	// Unlike send_secondary, which returns false only when there is no path at
	// all, this says whether the bytes really went out, so that a caller holding a
	// frame can put the rest of it on the primary. It is never retried on this
	// path: wivrn::TCP poisons a socket whose send threw (the AES-CTR keystream
	// has already moved past bytes that never reached the wire), and
	// drop_secondary closes it here and now, which makes the selector collapse out
	// of the combine posture on its next update.
	template <typename T>
	bool send_spill(T && packet)
	{
		std::string error;
		{
			std::shared_lock lock(secondary_mutex);
			if (not secondary)
				return false;

			try
			{
				secondary.send(std::forward<T>(packet));
			}
			catch (const std::exception & e)
			{
				error = e.what();
			}
		}

		if (error.empty())
			return true;

		drop_secondary(error);
		return false;
	}

	// Send on the primary control socket whatever the selector says, never
	// throws. Used for the keepalive echo, which must go back on the path it
	// came from even while that path is not the one carrying video.
	template <typename T>
	void send_primary_control(T && packet)
	{
		if (not active or not control)
			return;

		try
		{
			control.send(std::forward<T>(packet));
		}
		catch (const std::exception & e)
		{
			on_control_send_error(e);
		}
	}

	template <typename T>
	void send_control(T && packet)
	{
		if (not active)
			return;

		// Falls through to the primary if the secondary path died in between
		if (selector.on_secondary() and send_secondary(std::forward<T>(packet)))
			return;

		try
		{
			control.send(std::forward<T>(packet));
		}
		catch (const std::exception & e)
		{
			if (on_control_send_error(e))
				return;
			active = false;
			throw;
		}
	}

	template <typename T>
	void send_stream(T && packet)
	{
		if (not active)
			return;

		// The secondary path is TCP: the stream/control split does not exist
		// there, everything goes over the one socket. Falls through to the
		// primary if that path died in between.
		if (selector.on_secondary() and send_secondary(std::forward<T>(packet)))
			return;

		try
		{
			if (stream)
			{
				stream.send(std::forward<T>(packet));
				selector.on_stream_send_ok();
			}
			else
				control.send(std::forward<T>(packet));
		}
		catch (const std::exception & e)
		{
			// Losing a datagram is not an error worth a session teardown: the
			// socket is usable again right after and the path liveness decides
			if (stream)
			{
				on_stream_send_error(e);
				return;
			}

			if (on_control_send_error(e))
				return;
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
		// A control socket that is known broken reports POLLERR on every poll:
		// leaving it in would spin the network thread
		fds[1].fd = selector.control_up() ? control.get_fd() : -1;
		fds[2].fd = wivrn_ipc_socket_monado->get_fd();
		fds[2].events = POLLIN;
		fds[3].fd = wivrn_path_socket_monado;
		fds[3].events = POLLIN;

		// Malformed datagrams are dropped, only the control socket is fatal
		while (auto packet = stream.receive_pending_lossy())
		{
			on_primary_received(false);
			std::visit(std::forward<T>(visitor), std::move(*packet));
		}
		while (selector.control_up())
		{
			std::optional<from_headset::packets> packet;
			try
			{
				packet = control.receive_pending();
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

		// After poll_secondary_pending, the path may have been dropped
		{
			std::shared_lock lock(secondary_mutex);
			fds[4].fd = secondary ? secondary.get_fd() : -1;
		}
		fds[4].events = POLLIN;

		int r = ::poll(fds, std::size(fds), timeout);
		if (r < 0)
			throw std::system_error(errno, std::system_category());

		if (fds[0].revents & (POLLHUP | POLLERR))
		{
			// A connected UDP socket raises POLLERR for a latched error, most
			// often an ICMP unreachable while the link is down. Draining it
			// makes the socket usable again the moment the link is back, so
			// this must never be a session teardown.
			drain_socket_error(stream.get_fd(), "stream socket");
			on_stream_send_error(std::runtime_error("poll reported an error"));
		}

		if (fds[1].revents & (POLLHUP | POLLERR))
		{
			if (not on_control_send_error(std::runtime_error("Error on control socket")))
				throw std::runtime_error("Error on control socket");
		}

		if (fds[2].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Error on IPC socket");

		if (fds[0].revents & POLLIN)
		{
			std::optional<from_headset::packets> packet;
			try
			{
				packet = stream.receive_lossy();
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
			std::optional<from_headset::packets> packet;
			try
			{
				packet = control.receive();
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
			std::string error;
			{
				std::shared_lock lock(secondary_mutex);
				// The path may have been dropped by a sending thread while we
				// were polling
				if (secondary)
				{
					try
					{
						packet = secondary.receive();
					}
					catch (const std::exception & e)
					{
						error = e.what();
					}
				}
			}

			if (not error.empty())
			{
				drop_secondary(error);
			}
			else if (packet)
			{
				on_secondary_received();
				std::visit(std::forward<T>(visitor), std::move(*packet));
			}
		}

		report_secondary_status();
		update_paths();

		if (uint64_t dropped = stream.take_dropped_datagrams())
			U_LOG_W("Dropped %lu invalid datagram(s) on the stream socket (%lu total)",
			        (unsigned long)dropped,
			        (unsigned long)stream.dropped_datagrams());

		return r;
	}
};
} // namespace wivrn
