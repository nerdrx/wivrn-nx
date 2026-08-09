/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
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

#include <netinet/in.h>
#include <sys/socket.h>

namespace wivrn
{

// DSCP code points in TOS-byte form (code point << 2), as set_socket_tos takes
// them.
//
// Access points map the top three bits of the byte to a WMM access category, so
// these decide which of the four hardware queues the traffic lands in. A mark
// only ever applies to what the marking end sends, and the two directions do
// not carry the same thing:
//
//   server -> headset  stream (UDP): video, hundreds of kB per frame  -> AF41 / AC_VI
//                      control (TCP): stream description, IDRs, audio -> EF   / AC_VO
//   headset -> server  stream (UDP): tracking, inputs, feedback       -> EF   / AC_VO
//                      control (TCP): settings, application control   -> EF   / AC_VO
//
// Video deliberately does not share the voice queue: a 200 kB frame sitting
// ahead of a tracking packet in the same queue is exactly the head-of-line
// blocking the split exists to avoid, and AC_VI is the category access points
// tune for video bursts anyway.
namespace tos
{
// Expedited Forwarding, DSCP 46. WMM AC_VO, the shortest contention window.
inline constexpr int dscp_ef = 46 << 2; // 0xb8
// Assured Forwarding 41, DSCP 34. WMM AC_VI.
inline constexpr int dscp_af41 = 34 << 2; // 0x88
// Unmarked, what the sockets carry without this feature.
inline constexpr int best_effort = 0;
} // namespace tos

// Set the DSCP/traffic class of everything sent on `fd`, whatever its address
// family. Returns false only if the kernel refused every applicable option; a
// socket with no mark is still a working socket, so callers only log it.
//
// Every socket this program creates is AF_INET6 except the explicitly IPv4 TCP
// constructor, and an AF_INET6 socket may still be carrying a v4-mapped
// connection. The traffic class lives in a different option per family, and for
// a v4-mapped peer Linux emits the IPv4 header from the IP_TOS value rather
// than from IPV6_TCLASS: set whatever applies and only complain if nothing took.
// IPV6_TCLASS on an AF_INET socket fails with ENOPROTOOPT, which is why the
// IPv4 option is tried first and independently.
inline bool set_socket_tos(int fd, int type_of_service)
{
	int domain = AF_INET6;
	socklen_t len = sizeof(domain);
	if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) < 0)
		domain = AF_INET6;

	bool ok = setsockopt(fd, IPPROTO_IP, IP_TOS, &type_of_service, sizeof(type_of_service)) == 0;

	if (domain == AF_INET6)
		ok = setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &type_of_service, sizeof(type_of_service)) == 0 or ok;

	return ok;
}

} // namespace wivrn
