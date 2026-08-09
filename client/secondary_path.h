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

#pragma once

#include "crypto.h"
#include "wivrn_client.h"

#include <functional>
#include <thread>

// Keeps a secondary (multipath) path attached to the running session.
//
// The only transport considered for now is the USB tunnel: the dashboard sets up
// `adb reverse tcp:9757 tcp:9757` while a session is running, which makes
// 127.0.0.1:9757 reachable on the headset exactly when the cable is usable. The
// manager probes that address from its own low priority thread, runs the attach
// handshake on success and then holds the path, sending keepalives.
//
// Nothing here may disturb the primary path: all errors are handled by closing
// the secondary path and going back to probing.
class secondary_path_manager
{
	static const inline int port = 9757;
	static const inline uint8_t path_id = 1;
	static const inline std::chrono::milliseconds probe_interval{5000};
	static const inline std::chrono::milliseconds keepalive_interval{250};

	wivrn_session & session;

	// Whether a secondary path is wanted right now (streaming and enabled in
	// the settings), evaluated on the manager thread
	std::function<bool()> wanted;

	crypto::key keypair;

	// Avoid logging the same failure every probe
	std::string last_failure;

	std::jthread thread;

	void run(std::stop_token stop);
	void attach();
	bool sleep_for(std::stop_token & stop, std::chrono::milliseconds duration);

public:
	secondary_path_manager(wivrn_session & session, std::function<bool()> wanted);
	~secondary_path_manager();

	secondary_path_manager(const secondary_path_manager &) = delete;
	secondary_path_manager & operator=(const secondary_path_manager &) = delete;
};
