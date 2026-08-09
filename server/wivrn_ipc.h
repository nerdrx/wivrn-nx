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

#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <array>
#include <memory>
#include <optional>
#include <stdint.h>
#include <utility>
#include <variant>

namespace wivrn
{
class wivrn_connection;
}

extern std::unique_ptr<wivrn::wivrn_connection> connection;

namespace from_monado
{
struct headset_connected
{};

struct headset_disconnected
{};

struct server_error
{
	std::string where;
	std::string message;
};

using packets = std::variant<
        wivrn::from_headset::headset_info_packet,
        wivrn::from_headset::settings_changed,
        wivrn::from_headset::start_app,
        wivrn::from_headset::stream_tab_changed,
        headset_connected,
        headset_disconnected,
        server_error>;
} // namespace from_monado

namespace to_monado
{
struct stop
{};

struct disconnect
{};

struct set_bitrate
{
	uint32_t bitrate_bps;
};

using packets = std::variant<stop, disconnect, set_bitrate, wivrn::to_headset::stream_tab_change>;
} // namespace to_monado

extern std::optional<wivrn::typed_socket<wivrn::UnixDatagram, to_monado::packets, from_monado::packets>> wivrn_ipc_socket_monado;

namespace wivrn
{
// Everything the monado process needs to talk on a secondary (multipath) TCP
// connection that the main loop process accepted and authenticated
struct path_secrets
{
	bool encrypted = false;
	uint8_t path_id = 0;
	std::array<uint8_t, 16> key{};
	std::array<uint8_t, 16> iv_from_headset{};
	std::array<uint8_t, 16> iv_to_headset{};
};
} // namespace wivrn

// Dedicated AF_UNIX SOCK_DGRAM socketpair, used only to pass secondary path
// sockets with SCM_RIGHTS. It is kept apart from the packet socketpair above
// because the latter is read with recvmmsg without ancillary data, which would
// silently drop the file descriptors.
extern int wivrn_path_socket_main_loop;
extern int wivrn_path_socket_monado;

// Main loop process: hand an accepted+authenticated secondary connection over
// to the monado process. Returns false on failure, the caller keeps the fd.
bool send_path_to_monado(int fd, const wivrn::path_secrets & secrets);

// Monado process: receive a secondary connection, the returned fd is owned by
// the caller
std::optional<std::pair<int, wivrn::path_secrets>> receive_path_from_main();

std::optional<to_monado::packets> receive_from_main();
template <typename T>
void send_to_main(T packet)
{
	wivrn_ipc_socket_monado->send(std::move(packet));
}
