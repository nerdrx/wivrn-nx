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

#include "wivrn_client.h"
#include "application.h"
#include "protocol_version.h"
#include "secrets.h"
#include "smp.h"
#include "spdlog/common.h"
#include "utils/i18n.h"
#include "wivrn_packets.h"
#include <arpa/inet.h>
#include <cstring>
#include <format>
#include <ifaddrs.h>
#include <linux/ipv6.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

const char * handshake_error::what() const noexcept
{
	return message.c_str();
}

handshake_error::handshake_error(std::string_view message) :
        message(message) {}

namespace
{
template <typename T>
void init_stream(T & stream)
{
	stream.set_receive_buffer_size(1024 * 1024 * 5);
}
} // namespace

template <typename T>
void wivrn_session::handshake(T address, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter)
{
	// FIXME this comment
	// Wait for handshake on control socket,
	// then send ours on stream or control socket,
	// finally wait for second server handshake

	pollfd fds{};
	fds.events = POLLIN;
	fds.fd = control.get_fd();

	auto receive = [&](std::optional<std::chrono::seconds> timeout = std::nullopt) {
		std::chrono::steady_clock::time_point timeout_abs{};
		if (timeout)
			timeout_abs = std::chrono::steady_clock::now() + *timeout;

		while (true)
		{
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout_abs - std::chrono::steady_clock::now());
			int r = ::poll(&fds, 1, std::max<int>(ms.count(), 100));
			if (r < 0)
				throw std::system_error(errno, std::system_category());

			if (r > 0 && (fds.revents & POLLIN))
			{
				auto packet = control.receive();
				if (not packet)
					continue;

				return std::move(*packet);
			}

			if (std::chrono::steady_clock::now() >= timeout_abs)
				throw std::runtime_error(_("Timeout"));
		}
	};

	send_control(from_headset::crypto_handshake{
	        .protocol_version = wivrn::protocol_version,
	        .public_key = headset_keypair.public_key(),
	        .name = application::get_hmd_traits().model_name(),
	});

	to_headset::crypto_handshake crypto_handshake = std::get<to_headset::crypto_handshake>(receive(10s));

	std::string pin = "000000";
	switch (crypto_handshake.state)
	{
		case to_headset::crypto_handshake::crypto_state::encryption_disabled: {
			spdlog::info("Encryption is disabled on server");

			send_control(from_headset::crypto_handshake{});

			to_headset::handshake h{std::get<to_headset::handshake>(receive(10s))};
			session_token_ = h.session_token;
			if (h.stream_port > 0 && !tcp_only)
			{
				stream = decltype(stream)();

				stream.connect(address, h.stream_port);
				init_stream(stream);
			}
			break;
		}

		case to_headset::crypto_handshake::crypto_state::pin_needed:
			pin = pin_enter(control.get_fd());

			// Check the PIN
			try
			{
				crypto::smp pin_check;

				auto msg1 = pin_check.step1(pin);
				send_control(from_headset::pin_check_1{msg1});

				auto msg2 = std::get<to_headset::pin_check_2>(receive(10s)).message;

				auto msg3 = pin_check.step3(msg2);
				send_control(from_headset::pin_check_3{msg3});

				auto msg4 = std::get<to_headset::pin_check_4>(receive(10s)).message;
				bool pin_match = pin_check.step5(msg4);

				if (not pin_match)
					throw std::runtime_error(_("Incorrect PIN"));
			}
			catch (crypto::smp_cheated &)
			{
				throw std::runtime_error(_("Unable to check PIN"));
			}

			[[fallthrough]];

		case to_headset::crypto_handshake::crypto_state::client_already_paired: {
			spdlog::info("Using pin \"{}\"", pin);

			crypto::key server_key = crypto::key::from_public_key(crypto_handshake.public_key);
			secrets s{headset_keypair, server_key, pin};
			control.set_aes_key_and_ivs(s.control_key, s.control_iv_to_headset, s.control_iv_from_headset);

			// Confirm that encryption is set up
			send_control(from_headset::crypto_handshake{});

			to_headset::handshake h{std::get<to_headset::handshake>(receive(10s))};
			session_token_ = h.session_token;
			if (h.stream_port > 0 && !tcp_only)
			{
				stream = decltype(stream)();

				stream.set_aes_key_and_ivs(s.stream_key, s.stream_iv_header_to_headset, s.stream_iv_header_from_headset);
				stream.connect(address, h.stream_port);
				init_stream(stream);
			}
			break;
		}

		case to_headset::crypto_handshake::crypto_state::pairing_disabled:
			spdlog::info("Pairing is disabled on server");
			throw std::runtime_error(_("Pairing is disabled on server"));

		case to_headset::crypto_handshake::crypto_state::incompatible_version:
			spdlog::error("Incompatible protocol versions");
			throw std::runtime_error(_("Incompatible server version"));
	}

	// may be on control socket if forced TCP
	send_stream(from_headset::handshake{});

	// Wait for second handshake
	auto timeout = std::chrono::steady_clock::now() + 10s;
	while (true)
	{
		if (poll([](const auto && packet) { return std::is_same_v<std::remove_cvref_t<decltype(packet)>, to_headset::handshake>; }, 100ms))
		{
			return;
		}

		if (std::chrono::steady_clock::now() >= timeout)
			throw std::runtime_error(_("Timeout"));

		// If using stream socket, the handshake might be lost
		if (stream)
		{
			stream.send(from_headset::handshake{});
		}
	}
}

namespace
{
int64_t steady_now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	               std::chrono::steady_clock::now().time_since_epoch())
	        .count();
}

// While the secondary path only carries keepalives and duplicated tracking a
// stalled send must never hold the tracking thread; once it carries everything
// a tight timeout would cost the path on the first hiccup (a send that times out
// half way through a packet loses the TCP framing).
void set_send_timeout(int fd, bool active)
{
	timeval timeout = active
	                          ? timeval{.tv_sec = 1, .tv_usec = 0}
	                          : timeval{.tv_sec = 0, .tv_usec = 20'000};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
} // namespace

void wivrn_session::set_secondary(control_socket_t && socket)
{
	{
		std::unique_lock lock(secondary_mutex);

		set_send_timeout(socket.get_fd(), selector.on_secondary());

		secondary = std::move(socket);
		++secondary_generation;
		secondary_up = true;
	}

	spdlog::info("Secondary path attached");
}

void wivrn_session::set_qos(bool enabled)
{
	if (qos_enabled == enabled)
		return;

	const int mark = enabled ? wivrn::tos::dscp_ef : wivrn::tos::best_effort;

	bool ok = true;
	if (stream)
		ok = stream.set_tos(mark) and ok;
	if (control)
		ok = control.set_tos(mark) and ok;

	if (not ok)
	{
		spdlog::warn("Could not {} the Wi-Fi QoS marks: {}", enabled ? "set" : "clear", strerror(errno));
		return;
	}

	qos_enabled = enabled;
	spdlog::info("Wi-Fi QoS marks {} (uplink EF / AC_VO)", enabled ? "enabled" : "disabled");
}

void wivrn_session::drop_secondary(std::string_view reason)
{
	{
		std::unique_lock lock(secondary_mutex);
		if (not secondary_up)
			return;

		::shutdown(secondary.get_fd(), SHUT_RDWR);
		secondary = control_socket_t(-1);
		++secondary_generation;
		secondary_up = false;
	}

	spdlog::info("Secondary path detached: {}", reason);

	// Nothing left to send on, whatever the state of the primary. The switch
	// itself is decided by update_paths, on the network thread.
	selector.set_secondary_usable(false);
}

bool wivrn_session::primary_is_loopback() const
{
	if (auto * v4 = std::get_if<in_addr>(&address))
		return (ntohl(v4->s_addr) >> 24) == 127;

	if (auto * v6 = std::get_if<in6_addr>(&address))
	{
		if (IN6_IS_ADDR_LOOPBACK(v6))
			return true;
		if (IN6_IS_ADDR_V4MAPPED(v6))
			return v6->s6_addr[12] == 127;
	}

	return false;
}

void wivrn_session::on_primary_received(bool from_control)
{
	selector.on_primary_received(from_control, std::chrono::steady_clock::now());
}

bool wivrn_session::on_control_send_error(const std::exception & e)
{
	const bool can_fail_over = secondary_up;

	if (selector.on_control_send_error())
		spdlog::warn("Primary control socket failed: {}{}", e.what(), can_fail_over ? ", failing over" : "");

	if (not can_fail_over)
		return false;

	selector.request(true, "the primary control socket failed");
	return true;
}

void wivrn_session::on_stream_send_error(const std::exception & e)
{
	if (selector.on_stream_send_error(std::chrono::steady_clock::now()))
		spdlog::debug("Send error on the primary stream socket: {}", e.what());

	// Reading SO_ERROR clears the latched error, so the socket is usable again
	// as soon as the link is back. Recreating it is not an option: the server
	// pinned our source port with connect() during the handshake.
	int error = 0;
	socklen_t len = sizeof(error);
	if (getsockopt(stream.get_fd(), SOL_SOCKET, SO_ERROR, &error, &len) == 0 and error)
		spdlog::debug("Cleared pending error on the stream socket: {}", strerror(error));
}

void wivrn_session::send_primary_ping()
{
	auto packet = from_headset::path_ping{
	        .path_id = 0,
	        .timestamp = steady_now_ns(),
	};

	try
	{
		// The stream socket is the one video comes back on, and it never blocks.
		// It is also what makes the primary path recover: while everything else
		// goes over the secondary, this is the only send left on it, so it is
		// the only thing that can clear a run of send errors.
		if (stream)
		{
			bytes_sent_ += stream.send(std::move(packet));
			selector.on_stream_send_ok();
		}
		else
		{
			bytes_sent_ += control.send(std::move(packet));
		}
	}
	catch (const std::exception & e)
	{
		if (stream)
			on_stream_send_error(e);
		else
			on_control_send_error(e);
	}
}

void wivrn_session::update_paths()
{
	auto now = std::chrono::steady_clock::now();
	selector.set_secondary_usable(secondary_up);

	if (auto event = selector.update(now))
	{
		{
			std::shared_lock lock(secondary_mutex);
			if (secondary_up)
				set_send_timeout(secondary.get_fd(), event->on_secondary);
		}

		spdlog::info("Path switch: tracking and input now on the {} path ({}), {} ms since the last switch",
		             event->on_secondary ? "secondary" : "primary",
		             event->reason,
		             event->since_previous.count());
	}

	if (uint64_t errors = selector.stream_send_errors(); errors != reported_stream_send_errors and now >= next_stream_error_report)
	{
		next_stream_error_report = now + std::chrono::seconds(5);
		spdlog::warn("Send errors on the primary stream socket: {} new, {} total",
		             errors - reported_stream_send_errors,
		             errors);
		reported_stream_send_errors = errors;
	}
}

wivrn_session::wivrn_session(in6_addr address, int port, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter) :
        control(address, port), stream(-1), address(address)
{
	try
	{
		handshake(address, tcp_only, headset_keypair, pin_enter);
	}
	catch (std::exception & e)
	{
		throw handshake_error{e.what()};
	}

	set_qos(application::get_config().wifi_qos);
	selector.reset(std::chrono::steady_clock::now());
}

wivrn_session::wivrn_session(in_addr address, int port, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter) :
        control(address, port), stream(-1), address(address)
{
	try
	{
		handshake(address, tcp_only, headset_keypair, pin_enter);
	}
	catch (std::exception & e)
	{
		throw handshake_error{e.what()};
	}

	set_qos(application::get_config().wifi_qos);
	selector.reset(std::chrono::steady_clock::now());
}
