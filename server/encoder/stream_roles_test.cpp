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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Unit test for what each video stream says it IS on the wire.
//
// The bug this exists to prevent, in full, because it cost a day and read as four other
// things first: encoder_settings::role defaulted to stream_role::view and
// get_encoder_settings() never assigned it, so EVERY enabled stream went out labelled
// `view`. The passthrough alpha plane on stream 2 is enabled whenever the session has
// one, and the server never sends it a frame. The client's views_ready() gate waits for
// every stream labelled `view` to produce a picture, so it waited forever, the scene
// never reached state::streaming, and the lobby never pushed it. What that looks like
// from outside is a headset that connects, decodes 1000 frames, sends feedback, and
// renders at 90 fps -- while scenes::stream::render() is never called once. It was
// variously diagnosed as a dead log line, an unworn proximity sensor and a collapsed
// Wi-Fi link before it was diagnosed as this.
//
// Nothing here needs a GPU, a session or an encoder: check_stream_roles() and
// default_stream_role() are pure functions over the settings array, which is the whole
// reason they are shaped that way.
//
// What is checked:
//   - the positional convention is what the wire's own defaults say it is,
//   - a correctly built session passes,
//   - the exact historical bug -- alpha enabled and labelled `view` -- is REJECTED,
//   - an unassigned role is rejected rather than silently shipped,
//   - the one legitimate override (the hybrid base layer on stream 1) is accepted,
//   - and a base layer in the wrong slot, or not naming its enhancement stream, is not.

#include "encoder_settings.h"

#include <cstdio>
#include <string>

using namespace wivrn;

namespace
{
int failures = 0;

void check(bool ok, const char * what)
{
	std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (not ok)
		++failures;
}

// A session as get_encoder_settings() builds one: two eyes, and whichever extras are
// asked for, every slot carrying its positional role.
std::array<encoder_settings, num_streams> session(bool alpha, bool quad)
{
	std::array<encoder_settings, num_streams> res{};
	for (size_t i = 0; i < num_streams; ++i)
		res[i].role = default_stream_role(i);
	res[0].enabled = true;
	res[1].enabled = true;
	res[2].enabled = alpha;
	res[quad_stream_idx].enabled = quad;
	return res;
}
} // namespace

int main()
{
	std::printf("stream roles\n");

	// The convention itself, against the wire description's own defaults. The compiler
	// already refuses to build a disagreement (static_assert in encoder_settings.h);
	// this states it in a form a reader can see.
	{
		to_headset::video_stream_description d{};
		bool same = true;
		for (size_t i = 0; i < num_streams; ++i)
			same = same and d.role[i] == default_stream_role(i);
		check(same, "the positional convention matches the wire description's defaults");
		check(default_stream_role(0) == stream_role::view, "stream 0 is a view");
		check(default_stream_role(1) == stream_role::view, "stream 1 is a view");
		check(default_stream_role(2) == stream_role::alpha, "stream 2 is the alpha plane");
		check(default_stream_role(quad_stream_idx) == stream_role::quad, "stream 3 is the quad layer");
	}

	// Sessions that are built correctly.
	{
		check(check_stream_roles(session(false, false)).empty(), "two eyes alone: consistent");
		check(check_stream_roles(session(true, false)).empty(), "with the alpha plane: consistent");
		check(check_stream_roles(session(true, true)).empty(), "with alpha and a quad layer: consistent");
	}

	// THE bug. An enabled alpha stream labelled `view` must be rejected here, on the
	// server, and not by a headset that will not start.
	{
		auto s = session(true, false);
		s[2].role = stream_role::view;
		const auto bad = check_stream_roles(s);
		check(not bad.empty(), "alpha plane mislabelled as a view is rejected");
		check(bad.find("stream 2") != std::string::npos, "and the message names stream 2");
		if (not bad.empty())
			std::printf("       -> %s\n", bad.c_str());
	}

	// A stream nobody assigned a role to. This is what `unset` buys: forgetting is an
	// error, not a `view`.
	{
		auto s = session(true, false);
		s[2].role = stream_role::unset;
		const auto bad = check_stream_roles(s);
		check(not bad.empty(), "an unassigned role is rejected");
		check(bad.find("no role") != std::string::npos, "and says the role was never assigned");
	}

	// A disabled stream is never advertised -- the compositor skips it -- so its role
	// is nobody's business and must not be an error.
	{
		auto s = session(false, false);
		s[2].role = stream_role::unset;
		check(check_stream_roles(s).empty(), "a disabled stream's role is not checked");
	}

	// The one documented override: the hybrid base layer takes the stream-1 slot the
	// eye pairing vacated, and names the stream whose atlas it fills.
	{
		auto s = session(false, false);
		s[1].role = stream_role::base;
		s[1].serves_stream = 0;
		check(check_stream_roles(s).empty(), "the hybrid base layer on stream 1 is accepted");

		s[1].serves_stream = 0xff;
		check(not check_stream_roles(s).empty(), "a base layer that names no enhancement stream is rejected");
	}

	// ...and only in that slot. A base layer anywhere else is a routing bug.
	{
		auto s = session(true, false);
		s[2].role = stream_role::base;
		s[2].serves_stream = 0;
		check(not check_stream_roles(s).empty(), "a base layer outside the stream-1 slot is rejected");
	}

	std::printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nall checks passed\n", failures);
	return failures ? 1 : 0;
}
