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

#include "wivrn_ipc.h"
#include "driver/wivrn_connection.h"

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

std::unique_ptr<wivrn::wivrn_connection> connection;

int wivrn_path_socket_main_loop = -1;
int wivrn_path_socket_monado = -1;

std::optional<to_monado::packets> receive_from_main()
{
	return wivrn_ipc_socket_monado->receive();
}

bool send_path_to_monado(int fd, const wivrn::path_secrets & secrets)
{
	if (wivrn_path_socket_main_loop < 0 or fd < 0)
		return false;

	iovec iov{
	        .iov_base = const_cast<wivrn::path_secrets *>(&secrets),
	        .iov_len = sizeof(secrets),
	};

	union
	{
		cmsghdr align;
		char buffer[CMSG_SPACE(sizeof(int))];
	} control{};

	msghdr msg{
	        .msg_iov = &iov,
	        .msg_iovlen = 1,
	        .msg_control = control.buffer,
	        .msg_controllen = sizeof(control.buffer),
	};

	cmsghdr * cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

	while (sendmsg(wivrn_path_socket_main_loop, &msg, MSG_NOSIGNAL) < 0)
	{
		if (errno == EINTR)
			continue;
		return false;
	}

	return true;
}

std::optional<std::pair<int, wivrn::path_secrets>> receive_path_from_main()
{
	if (wivrn_path_socket_monado < 0)
		return {};

	wivrn::path_secrets secrets;
	iovec iov{
	        .iov_base = &secrets,
	        .iov_len = sizeof(secrets),
	};

	union
	{
		cmsghdr align;
		char buffer[CMSG_SPACE(sizeof(int))];
	} control{};

	msghdr msg{
	        .msg_iov = &iov,
	        .msg_iovlen = 1,
	        .msg_control = control.buffer,
	        .msg_controllen = sizeof(control.buffer),
	};

	ssize_t received;
	while ((received = recvmsg(wivrn_path_socket_monado, &msg, MSG_DONTWAIT)) < 0)
	{
		if (errno == EINTR)
			continue;
		return {};
	}

	int fd = -1;
	for (cmsghdr * cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
	{
		if (cmsg->cmsg_level == SOL_SOCKET and cmsg->cmsg_type == SCM_RIGHTS and cmsg->cmsg_len == CMSG_LEN(sizeof(int)))
			memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
	}

	if (fd < 0)
		return {};

	if (received != sizeof(secrets) or (msg.msg_flags & MSG_TRUNC))
	{
		::close(fd);
		return {};
	}

	return std::make_pair(fd, secrets);
}
