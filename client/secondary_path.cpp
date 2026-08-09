/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "secondary_path.h"

#include "application.h"
#include "protocol_version.h"
#include "secrets.h"
#include "wivrn_packets.h"

#include <arpa/inet.h>
#include <fstream>
#include <poll.h>
#include <pthread.h>
#include <spdlog/spdlog.h>
#include <sys/resource.h>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace wivrn;

static int64_t now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	               std::chrono::steady_clock::now().time_since_epoch())
	        .count();
}

secondary_path_manager::secondary_path_manager(wivrn_session & session, std::function<bool()> wanted) :
        session(session),
        wanted(std::move(wanted))
{
	thread = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

secondary_path_manager::~secondary_path_manager()
{
	thread.request_stop();
	if (thread.joinable())
		thread.join();

	session.drop_secondary("path manager stopped");
}

bool secondary_path_manager::sleep_for(std::stop_token & stop, std::chrono::milliseconds duration)
{
	auto deadline = std::chrono::steady_clock::now() + duration;

	while (not stop.stop_requested())
	{
		auto left = deadline - std::chrono::steady_clock::now();
		if (left <= 0ms)
			return true;

		std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(left, 50ms));
	}

	return false;
}

void secondary_path_manager::run(std::stop_token stop)
{
	pthread_setname_np(pthread_self(), "secondary_path");

	// Probing and attaching must never compete with the render, network or
	// tracking threads
	setpriority(PRIO_PROCESS, 0, 10);

#ifdef __ANDROID__
	application::instance().setup_jni();
#endif

	auto next_probe = std::chrono::steady_clock::now();

	// A secondary path over the USB tunnel cannot back up a primary that already
	// goes through that same tunnel: both would fail together
	if (session.primary_is_loopback())
	{
		spdlog::info("No secondary path: the primary connection already goes through the loopback");
		return;
	}

	while (not stop.stop_requested())
	{
		bool want = wanted();

		// Keepalive on the primary path too: it is what tells the server that
		// the primary is back after a failover, and it is the only traffic left
		// on it once we have flipped our own output to the secondary
		if (want)
			session.send_primary_ping();

		if (session.has_secondary())
		{
			if (not want)
			{
				session.drop_secondary("no longer wanted");
			}
			else
			{
				// Errors here only mark the secondary path down
				session.send_secondary(from_headset::path_ping{
				        .path_id = path_id,
				        .timestamp = now_ns(),
				});
			}

			sleep_for(stop, keepalive_interval);
			continue;
		}

		if (want and std::chrono::steady_clock::now() >= next_probe)
		{
			next_probe = std::chrono::steady_clock::now() + probe_interval;

			try
			{
				attach();
			}
			catch (std::exception & e)
			{
				// 127.0.0.1:9757 is only reachable while the USB tunnel is up,
				// a failure here is the normal case
				if (last_failure != e.what())
				{
					last_failure = e.what();
					spdlog::debug("No secondary path: {}", e.what());
				}
			}
		}

		sleep_for(stop, keepalive_interval);
	}

	session.drop_secondary("path manager stopped");
}

void secondary_path_manager::attach()
{
	if (not keypair)
	{
		const auto keypair_path = application::get_config_path() / "private_key.pem";
		std::ifstream f{keypair_path};
		keypair = crypto::key::from_private_key(std::string{(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()});
	}

	in_addr loopback{};
	loopback.s_addr = htonl(INADDR_LOOPBACK);

	// Connecting to the loopback either succeeds at once or fails at once
	wivrn_session::control_socket_t socket(loopback, port);

	socket.send(from_headset::attach_path{
	        .protocol_version = wivrn::protocol_version,
	        .public_key = keypair.public_key(),
	        .session_token = session.session_token(),
	        .path_id = path_id,
	});

	auto deadline = std::chrono::steady_clock::now() + 5s;
	std::optional<to_headset::packets> packet;
	while (not packet)
	{
		pollfd fds{.fd = socket.get_fd(), .events = POLLIN};

		int r = ::poll(&fds, 1, 100);
		if (r < 0)
			throw std::system_error(errno, std::system_category());

		if (fds.revents & (POLLHUP | POLLERR))
			throw std::runtime_error("connection closed");

		if (fds.revents & POLLIN)
			packet = socket.receive();

		if (not packet and std::chrono::steady_clock::now() > deadline)
			throw std::runtime_error("no response to the attach request");
	}

	auto * response = std::get_if<to_headset::attach_path_response>(&*packet);
	if (not response)
		throw std::runtime_error("unexpected response to the attach request");

	switch (response->state)
	{
		case to_headset::attach_path_response::attach_state::accepted:
			break;

		case to_headset::attach_path_response::attach_state::rejected:
			throw std::runtime_error("attach request rejected by the server");

		case to_headset::attach_path_response::attach_state::incompatible_version:
			throw std::runtime_error("incompatible server version");
	}

	if (not response->public_key.empty())
	{
		// Fresh ephemeral key: this path has its own key stream. The headset is
		// already paired, so there is no PIN to check.
		crypto::key server_key = crypto::key::from_public_key(response->public_key);
		auto s = secrets::for_additional_path(keypair, server_key, "000000");
		socket.set_aes_key_and_ivs(s.control_key, s.control_iv_to_headset, s.control_iv_from_headset);
	}

	last_failure.clear();
	spdlog::info("Secondary path {} attached over 127.0.0.1:{}{}",
	             int(path_id),
	             port,
	             response->public_key.empty() ? " (unencrypted)" : "");

	session.set_secondary(std::move(socket));
}
