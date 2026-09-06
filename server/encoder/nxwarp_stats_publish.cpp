/*
 * WiVRn VR streaming
 * Copyright (C) 2025  WiVRn contributors
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

// The monado-process half of the NX Warp statistics hop. Kept in its own file so that
// video_encoder_nxwarp.cpp needs nothing but the plain struct to report, and so the e2e harness --
// which links the encoder with no IPC socket at all -- has one symbol to stub.

#include "nxwarp_stats.h"

#include "wivrn_ipc.h"

namespace wivrn
{

void publish_nxwarp_stats(const nxwarp_stream_stats & stats)
{
	// Best effort by design. This is a report for a status page: losing one on a full socket
	// costs two seconds of staleness, and the next one is already on its way. It must never
	// be allowed to disturb the encode thread it is called from.
	try
	{
		send_to_main(stats);
	}
	catch (...)
	{
	}
}

} // namespace wivrn
