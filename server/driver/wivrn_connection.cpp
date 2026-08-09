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

#include "wivrn_connection.h"
#include "configuration.h"
#include "protocol_version.h"
#include "secrets.h"
#include "smp.h"
#include "wivrn_ipc.h"
#include "wivrn_packets.h"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <random>
#include <regex>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <variant>

using namespace std::chrono_literals;

// Ignored until connection is established
static void handle_event_from_main_loop(to_monado::stop) {}
static void handle_event_from_main_loop(to_monado::disconnect) {}
static void handle_event_from_main_loop(to_monado::set_bitrate) {}
static void handle_event_from_main_loop(wivrn::to_headset::stream_tab_change) {}

static std::string clean_key(std::string key)
{
	static const std::regex header{"^-+BEGIN .*-+$", std::regex_constants::multiline};
	static const std::regex footer{"^-+END .*-+$", std::regex_constants::multiline};
	static const std::regex whitespace{"[[:space:]]"};

	key = std::regex_replace(key, header, "");
	key = std::regex_replace(key, footer, "");
	key = std::regex_replace(key, whitespace, "");

	return key;
}

wivrn::incorrect_pin::incorrect_pin() :
        std::runtime_error("Incorrect PIN") {}

static std::array<uint8_t, 16> random_session_token()
{
	std::array<uint8_t, 16> token;

	std::random_device rd;
	std::uniform_int_distribution<int> distrib(0, 255);
	for (uint8_t & i: token)
		i = distrib(rd);

	return token;
}

// Timeout on sends over a secondary path while it only carries keepalives and
// duplicated tracking: a stalled USB tunnel must not block the thread that
// duplicates tracking onto it
static const timeval secondary_send_timeout{.tv_sec = 0, .tv_usec = 50'000};

// Once the secondary path carries video, 50 ms is far too tight: a whole frame
// may legitimately take longer than that to hand to the kernel. A send that
// times out half way through a packet loses the TCP framing and costs the path.
static const timeval secondary_active_send_timeout{.tv_sec = 1, .tv_usec = 0};

// True if both sockets are connected to the same peer address, which is how a
// secondary path over the USB tunnel looks when the primary already goes
// through that same tunnel (both peers are the loopback, via adb reverse)
static bool same_peer(int a, int b)
{
	sockaddr_in6 addr_a{};
	sockaddr_in6 addr_b{};
	socklen_t len_a = sizeof(addr_a);
	socklen_t len_b = sizeof(addr_b);

	if (getpeername(a, (sockaddr *)&addr_a, &len_a) < 0)
		return false;
	if (getpeername(b, (sockaddr *)&addr_b, &len_b) < 0)
		return false;

	return memcmp(&addr_a.sin6_addr, &addr_b.sin6_addr, sizeof(addr_a.sin6_addr)) == 0;
}

void wivrn::wivrn_connection::drain_socket_error(int fd, const char * what)
{
	int error = 0;
	socklen_t len = sizeof(error);

	// Reading SO_ERROR clears it
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 and error)
		U_LOG_D("Cleared pending error on the %s: %s", what, strerror(error));
}

wivrn::wivrn_connection::wivrn_connection(std::stop_token stop_token, encryption_state state, std::string pin, TCP && tcp) :
        control(std::move(tcp)),
        stream(-1),
        pin(pin),
        state(state),
        token(random_session_token())
{
	init(stop_token);
}

void wivrn::wivrn_connection::attach_secondary(int fd, const path_secrets & secrets)
{
	drop_secondary("replaced by a new path");

	std::unique_lock lock(secondary_mutex);

	try
	{
		secondary = decltype(secondary)(fd);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &secondary_send_timeout, sizeof(secondary_send_timeout));

		if (secrets.encrypted)
		{
			auto key = secrets.key;
			auto iv_from = secrets.iv_from_headset;
			auto iv_to = secrets.iv_to_headset;
			secondary.set_aes_key_and_ivs(key, iv_from, iv_to);
		}

		secondary_path_id = secrets.path_id;
		secondary_received = 0;
		secondary_last_receive = std::chrono::steady_clock::now();
		secondary_next_report = secondary_last_receive + std::chrono::seconds(5);

		// A path that shares the primary's peer address rides the same link and
		// would fail with it: it is attached (the headset asked for it) but the
		// selector will never pick it
		secondary_can_failover = not same_peer(secondary.get_fd(), control.get_fd());

		U_LOG_I("Secondary path %d attached%s%s",
		        (int)secondary_path_id,
		        secrets.encrypted ? "" : " (unencrypted)",
		        secondary_can_failover ? "" : " (same peer as the primary, not usable for failover)");
	}
	catch (const std::exception & e)
	{
		U_LOG_W("Failed to attach secondary path: %s", e.what());
		secondary = decltype(secondary)(-1);
	}
}

void wivrn::wivrn_connection::drop_secondary(std::string_view reason)
{
	{
		std::unique_lock lock(secondary_mutex);
		if (not secondary)
			return;

		::shutdown(secondary.get_fd(), SHUT_RDWR);
		secondary = decltype(secondary)(-1);
		secondary_received = 0;
	}

	secondary_can_failover = false;

	// Nothing left to send on, whatever the state of the primary. The switch
	// itself is decided by update_paths, on the network thread.
	selector.set_secondary_usable(false);

	U_LOG_I("Secondary path %d detached: %.*s", (int)secondary_path_id, (int)reason.size(), reason.data());
}

void wivrn::wivrn_connection::on_secondary_received()
{
	++secondary_received;
	secondary_last_receive = std::chrono::steady_clock::now();
}

void wivrn::wivrn_connection::report_secondary_status()
{
	if (not has_secondary())
		return;

	auto now = std::chrono::steady_clock::now();
	auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - secondary_last_receive);

	// The headset sends a keepalive every 250ms
	if (since_last > 3s)
	{
		drop_secondary("no keepalive received");
		return;
	}

	if (now >= secondary_next_report)
	{
		secondary_next_report = now + 5s;
		U_LOG_I("Secondary path %d alive, %lu packets received, last %ld ms ago%s",
		        (int)secondary_path_id,
		        (unsigned long)secondary_received,
		        (long)since_last.count(),
		        selector.on_secondary() ? ", carrying video" : "");
	}
}

void wivrn::wivrn_connection::on_primary_received(bool from_control)
{
	selector.on_primary_received(from_control, std::chrono::steady_clock::now());
}

bool wivrn::wivrn_connection::on_control_send_error(const std::exception & e)
{
	const bool can_fail_over = has_secondary() and secondary_can_failover;

	if (selector.on_control_send_error())
		U_LOG_W("Primary control socket failed: %s%s", e.what(), can_fail_over ? ", failing over" : "");

	if (not can_fail_over)
		return false;

	selector.request(true, "the primary control socket failed");
	return true;
}

void wivrn::wivrn_connection::on_stream_send_error(const std::exception & e)
{
	if (selector.on_stream_send_error(std::chrono::steady_clock::now()))
		U_LOG_D("Send error on the primary stream socket: %s", e.what());

	drain_socket_error(stream.get_fd(), "stream socket");
}

void wivrn::wivrn_connection::update_paths()
{
	auto now = std::chrono::steady_clock::now();
	selector.set_secondary_usable(has_secondary() and secondary_can_failover);

	auto event = selector.update(now);
	if (event)
	{
		U_LOG_I("Path switch: video and control now on the %s path (%s), %ld ms since the last switch",
		        event->on_secondary ? "secondary" : "primary",
		        event->reason.c_str(),
		        (long)event->since_previous.count());

		{
			// The timeout that is right for keepalives is not the one that is
			// right for video
			std::shared_lock lock(secondary_mutex);
			if (secondary)
			{
				const timeval & timeout = event->on_secondary ? secondary_active_send_timeout : secondary_send_timeout;
				setsockopt(secondary.get_fd(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
			}
		}

		if (switch_callback)
			switch_callback(event->on_secondary, event->reason);

		// Back on a primary whose control socket is broken for good and with
		// nothing left to fall back to: let the session pause and reconnect
		if (not event->on_secondary and not selector.control_up())
			throw std::runtime_error("Both paths are down");
	}

	if (uint64_t errors = selector.stream_send_errors(); errors != reported_stream_send_errors and now >= next_stream_error_report)
	{
		next_stream_error_report = now + 5s;
		U_LOG_W("Send errors on the primary stream socket: %lu new, %lu total",
		        (unsigned long)(errors - reported_stream_send_errors),
		        (unsigned long)errors);
		reported_stream_send_errors = errors;
	}
}

void wivrn::wivrn_connection::init(std::stop_token stop_token, std::function<void()> tick)
{
	active = false;
	selector.reset(std::chrono::steady_clock::now());

	sockaddr_in6 server_address;
	socklen_t len = sizeof(server_address);
	if (getsockname(control.get_fd(), (sockaddr *)&server_address, &len) < 0)
	{
		throw std::system_error(errno, std::system_category(), "Cannot get socket port");
	}
	int port = ntohs(((struct sockaddr_in6 *)&server_address)->sin6_port);

	sockaddr_in6 client_address;
	len = sizeof(client_address);
	if (getpeername(control.get_fd(), (sockaddr *)&client_address, &len) < 0)
	{
		throw std::system_error(errno, std::system_category(), "Cannot get client address");
	}

	if (configuration().tcp_only)
	{
		port = -1;
	}
	else
	{
		stream = decltype(stream)();
		stream.bind(server_address);
	}

	auto receive = [&](std::optional<std::chrono::seconds> timeout = std::nullopt, bool allow_stream_socket = false) {
		// Returns the packet and the port (on the stream socket) or -1 (on the control socket)

		std::chrono::steady_clock::time_point timeout_abs{};
		if (timeout)
			timeout_abs = std::chrono::steady_clock::now() + *timeout;

		while (true)
		{
			if (stop_token.stop_requested())
				throw std::runtime_error("Connection cancelled");

			tick();

			pollfd fds[3] = {};
			if (allow_stream_socket)
			{
				fds[0].events = POLLIN;
				fds[0].fd = stream.get_fd();
			}
			fds[1].events = POLLIN;
			fds[1].fd = control.get_fd();
			fds[2].events = POLLIN;
			fds[2].fd = wivrn_ipc_socket_monado->get_fd();

			// Make sure tick() is called at least every 100ms
			int r = ::poll(fds, std::size(fds), 100);
			if (r < 0)
				throw std::system_error(errno, std::system_category());

			if (allow_stream_socket and (fds[0].revents & (POLLHUP | POLLERR)))
				throw std::runtime_error("Error on stream socket");

			if (fds[1].revents & (POLLHUP | POLLERR))
				throw std::runtime_error("Error on control socket");

			if (fds[2].revents & (POLLHUP | POLLERR))
				throw std::runtime_error("Error on IPC socket");

			if (allow_stream_socket and (fds[0].revents & POLLIN))
			{
				auto [raw_packet, peer_addr] = stream.receive_from_raw();

				// Ignore packets sent from the wrong address
				if (not raw_packet.empty() and memcmp(&peer_addr.sin6_addr, &client_address.sin6_addr, sizeof(peer_addr.sin6_addr)) == 0)
				{
					// A malformed datagram must not abort the handshake
					try
					{
						return std::make_pair(raw_packet.deserialize<from_headset::packets>(), (int)htons(peer_addr.sin6_port));
					}
					catch (const std::exception &)
					{
						stream.count_dropped_datagram();
					}
				}
			}

			if (fds[1].revents & POLLIN)
			{
				std::optional<from_headset::packets> packet = control.receive();
				if (packet)
					return std::make_pair(std::move(*packet), -1);
			}

			if (fds[2].revents & POLLIN)
			{
				auto packet = receive_from_main();
				if (packet)
					std::visit([](auto && x) { handle_event_from_main_loop(x); }, *packet);
			}

			if (timeout and std::chrono::steady_clock::now() > timeout_abs)
			{
				throw std::runtime_error("No handshake received from client");
			}
		}
	};

	// Wait for client to send handshake packet
	auto crypto_handshake = std::get<from_headset::crypto_handshake>(receive(10s).first);

	if (crypto_handshake.protocol_version != wivrn::protocol_version)
	{
		control.send(to_headset::crypto_handshake{
		        .state = to_headset::crypto_handshake::crypto_state::incompatible_version,
		});
		throw std::runtime_error("Incompatible protocol version");
	}

	crypto::key headset_key = crypto::key::from_public_key(crypto_handshake.public_key);
	bool is_public_key_known = std::ranges::any_of(
	        wivrn::known_keys(),
	        [key = clean_key(crypto_handshake.public_key)](const wivrn::headset_key & k) {
		        return k.public_key == key;
	        });

	switch (state)
	{
		case encryption_state::disabled:
			// Encryption and authentication are disabled
			control.send(to_headset::crypto_handshake{
			        .state = to_headset::crypto_handshake::crypto_state::encryption_disabled,
			});
			break;

		case encryption_state::enabled:
			if (not is_public_key_known)
			{
				control.send(to_headset::crypto_handshake{
				        .state = to_headset::crypto_handshake::crypto_state::pairing_disabled,
				});
				throw std::runtime_error("Client not known and pairing is disabled");
			}

			[[fallthrough]];

		case encryption_state::pairing:
			// Generate an ephemeral key pair just for exchanging the AES key
			crypto::key server_key = crypto::key::generate_x448_keypair();

			control.send(to_headset::crypto_handshake{
			        .public_key = server_key.public_key(),
			        .state = is_public_key_known
			                         ? to_headset::crypto_handshake::crypto_state::client_already_paired
			                         : to_headset::crypto_handshake::crypto_state::pin_needed,
			});

			if (not is_public_key_known)
			{
				try
				{
					// Check the PIN
					crypto::smp pin_check;

					auto msg1 = std::get<from_headset::pin_check_1>(receive(2min).first).message;

					auto msg2 = pin_check.step2(msg1, pin);
					control.send(to_headset::pin_check_2{msg2});

					auto msg3 = std::get<from_headset::pin_check_3>(receive(10s).first).message;

					auto [msg4, pin_match] = pin_check.step4(msg3);
					control.send(to_headset::pin_check_4{msg4});

					if (not pin_match)
						throw incorrect_pin{};
				}
				catch (crypto::smp_cheated &)
				{
					throw std::runtime_error("Unable to check PIN");
				}
			}

			secrets s{server_key, headset_key, is_public_key_known ? "000000" : pin};
			control.set_aes_key_and_ivs(s.control_key, s.control_iv_from_headset, s.control_iv_to_headset);
			stream.set_aes_key_and_ivs(s.stream_key, s.stream_iv_header_from_headset, s.stream_iv_header_to_headset);
			break;
	}

	// Wait for confirmation that the client has set up encryption
	if (not std::holds_alternative<from_headset::crypto_handshake>(receive().first))
		throw std::runtime_error("No handshake received from client");

	control.send(to_headset::handshake{.stream_port = port, .session_token = token});

	auto [stream_handshake, client_port] = receive(10s, true);

	// Check the packet type
	if (not std::holds_alternative<from_headset::handshake>(stream_handshake))
	{
		throw std::runtime_error("No handshake received from client");
	}

	if (client_port >= 0)
	{
		stream.connect(client_address.sin6_addr, client_port);
		stream.set_send_buffer_size(1024 * 1024 * 5);
	}
	else
	{
		// No stream socket
		stream = decltype(stream)(-1);
	}

	control.send(to_headset::handshake{.stream_port = port, .session_token = token});

	info_packet = std::get<from_headset::headset_info_packet>(receive(10s).first);

	// Fresh primary path
	selector.reset(std::chrono::steady_clock::now());

	active = true;

	if (state == encryption_state::pairing and not is_public_key_known)
		wivrn::add_known_key({
		        .public_key = clean_key(headset_key.public_key()),
		        .name = crypto_handshake.name,
		});
	else if (state != encryption_state::disabled)
		wivrn::update_last_connection_timestamp(clean_key(headset_key.public_key()));
}

void wivrn::wivrn_connection::set_qos(bool enabled)
{
	qos_wanted = enabled;
	apply_qos();
}

void wivrn::wivrn_connection::apply_qos()
{
	if (qos_applied == qos_wanted)
		return;

	// Video is the bulk traffic and goes to the video access category; the
	// control socket carries the small latency-critical things (stream
	// description, IDRs and their parameter sets, audio) and gets the voice one
	// so that it is never stuck behind a frame.
	bool ok = true;
	if (stream)
		ok = stream.set_tos(qos_wanted ? tos::dscp_af41 : tos::best_effort) and ok;
	if (control)
		ok = control.set_tos(qos_wanted ? tos::dscp_ef : tos::best_effort) and ok;

	if (not ok)
	{
		U_LOG_W("Could not %s the Wi-Fi QoS marks: %s", qos_wanted ? "set" : "clear", strerror(errno));
		return;
	}

	qos_applied = qos_wanted;
	U_LOG_I("Wi-Fi QoS marks %s (video AF41 / AC_VI, control EF / AC_VO)", qos_wanted ? "enabled" : "disabled");
}

void wivrn::wivrn_connection::reset(std::stop_token stop, TCP && tcp, std::function<void()> tick)
{
	if (stream)
		stream = decltype(stream)();

	// The secondary path belonged to the previous connection, its keys are gone
	drop_secondary("primary connection was reset");

	control = std::move(tcp);
	init(stop, tick);

	// Fresh sockets, they carry no mark yet
	qos_applied.reset();
	apply_qos();
}

void wivrn::wivrn_connection::shutdown()
{
	drop_secondary("session shutting down");

	if (stream)
		::shutdown(stream.get_fd(), SHUT_RDWR);
	if (control)
		::shutdown(control.get_fd(), SHUT_RDWR);
}

std::optional<wivrn::from_headset::packets> wivrn::wivrn_connection::poll_control(int timeout)
{
	pollfd fds{};
	fds.events = POLLIN;
	fds.fd = control.get_fd();

	int r = ::poll(&fds, 1, timeout);
	if (r < 0)
		throw std::system_error(errno, std::system_category());

	if (r > 0 && (fds.revents & POLLIN))
	{
		return control.receive();
	}

	return {};
}
