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

// The parts of the server that wivrn-nxwarp-e2e links but must never execute.
//
// video_encoder.cpp is one translation unit, so linking it for its base class also drags
// in the wivrn_session and wivrn_connection calls on the shard send path. The test never
// reaches them: it constructs video_encoder_nxwarp directly, and its packets leave through
// video_encoder::packet_sink, which returns before any of the session accounting.
//
// The encoder backends the factory names are linked for real, because they are cheap and
// already configured in this build. The session is not: linking it for real would mean
// linking monado's IPC server and the compositor into a test whose entire point is that it
// needs neither.
//
// Every one of them aborts. That is deliberate: if the test ever does reach one, the seam
// it relies on has moved and the right outcome is a loud crash naming the symbol, not a
// silent no-op that makes the test pass while measuring nothing.

#include "nxwarp_stats.h"

#include "driver/clock_offset.h"
#include "driver/wivrn_connection.h"
#include "driver/wivrn_session.h"
#include "encoder/encoder_settings.h"
#include "encoder/video_encoder.h"
#include "utils/wivrn_trace.h"
#include <cstdio>

#include "decoder/decoder.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
[[noreturn]] void unreachable(const char * what)
{
	std::fprintf(stderr,
	             "wivrn-nxwarp-e2e: reached %s, which this test is built on the assumption "
	             "it never calls. The encoder's packet_sink seam has moved.\n",
	             what);
	std::abort();
}
} // namespace

// --- the session and connection: the test's datagrams never reach them -----------------
void wivrn::wivrn_session::dump_time(const std::string &, uint64_t, int64_t, uint8_t, const char *)
{
	unreachable("wivrn_session::dump_time");
}

void wivrn::wivrn_session::on_frame_sent(uint64_t, uint8_t, uint32_t)
{
	unreachable("wivrn_session::on_frame_sent");
}

void wivrn::wivrn_session::on_frame_paths(uint32_t, uint32_t)
{
	unreachable("wivrn_session::on_frame_paths");
}

void wivrn::wivrn_connection::drop_secondary(std::string_view)
{
	unreachable("wivrn_connection::drop_secondary");
}

// --- the encoder factory ----------------------------------------------------------------
//
// video_encoder::create names every backend this build can produce. The test never calls
// it -- it constructs video_encoder_nxwarp directly -- but the reference still has to
// resolve. These declarations exist only to give the linker the symbols; the classes are
// deliberately not the real ones, which is safe precisely because none of this is ever
// instantiated or called. Linking the real backends instead pulls in the NVENC shared
// state, the VAAPI/FFmpeg stack and the GPU timestamp pool, none of which a codec test has
// any business owning.
namespace wivrn
{
struct vk_bundle;
struct encoder_settings;

class video_encoder_nvenc
{
public:
	video_encoder_nvenc(vk_bundle &, const encoder_settings &, uint8_t);
};
class video_encoder_va
{
public:
	video_encoder_va(vk_bundle &, const encoder_settings &, uint8_t);
};
class video_encoder_raw
{
public:
	video_encoder_raw(vk_bundle &, const encoder_settings &, uint8_t);
};
class video_encoder_x264
{
public:
	video_encoder_x264(vk_bundle &, const encoder_settings &, uint8_t);
};

video_encoder_nvenc::video_encoder_nvenc(vk_bundle &, const encoder_settings &, uint8_t)
{
	unreachable("video_encoder_nvenc");
}
video_encoder_va::video_encoder_va(vk_bundle &, const encoder_settings &, uint8_t)
{
	unreachable("video_encoder_va");
}
video_encoder_raw::video_encoder_raw(vk_bundle &, const encoder_settings &, uint8_t)
{
	unreachable("video_encoder_raw");
}
video_encoder_x264::video_encoder_x264(vk_bundle &, const encoder_settings &, uint8_t)
{
	unreachable("video_encoder_x264");
}
} // namespace wivrn

// --- the two Vulkan video backends, whose entry point is a static create ----------------
namespace wivrn
{
class video_encoder_vulkan_h264
{
public:
	static std::unique_ptr<video_encoder> create(vk_bundle &, const encoder_settings &, uint8_t);
};
class video_encoder_vulkan_h265
{
public:
	static std::unique_ptr<video_encoder> create(vk_bundle &, const encoder_settings &, uint8_t);
};

std::unique_ptr<video_encoder> video_encoder_vulkan_h264::create(vk_bundle &, const encoder_settings &, uint8_t)
{
	unreachable("video_encoder_vulkan_h264::create");
}
std::unique_ptr<video_encoder> video_encoder_vulkan_h265::create(vk_bundle &, const encoder_settings &, uint8_t)
{
	unreachable("video_encoder_vulkan_h265::create");
}
} // namespace wivrn

// --- the session's clock accessor -------------------------------------------------------
wivrn::clock_offset wivrn::wivrn_session::get_offset()
{
	unreachable("wivrn_session::get_offset");
}

bool wivrn::wivrn_connection::on_control_send_error(const std::exception &)
{
	unreachable("wivrn_connection::on_control_send_error");
}

void wivrn::wivrn_connection::on_stream_send_error(const std::exception &)
{
	unreachable("wivrn_connection::on_stream_send_error");
}

// --- the client decoder base class ------------------------------------------------------
//
// decoder.cpp holds only the out-of-line destructor and the factory that names every client
// decoder backend. The test needs the destructor and the typeinfo, not the factory.
wivrn::decoder::~decoder() = default;

// --- the server's compiled shaders ------------------------------------------------------
//
// wivrn_vk_bundle.cpp's load_shader indexes this map. The real one is generated into
// wivrn-server's own sources; nothing in this test loads a shader, since the NX Warp
// encoder does its pixel work on the CPU and the decoder brings its own SPIR-V.
extern const std::map<std::string, std::vector<uint32_t>> shaders;
const std::map<std::string, std::vector<uint32_t>> shaders;

// The e2e harness links the encoder with no IPC socket to the main process, so its two-second
// reports have nowhere to go. Counting them is still worth something: it proves the encoder
// actually emits one per window, which is what the D-Bus property depends on.
namespace wivrn
{
uint64_t nxwarp_stats_published = 0;
wivrn::nxwarp_stream_stats nxwarp_stats_last{};

void publish_nxwarp_stats(const nxwarp_stream_stats & stats)
{
	++nxwarp_stats_published;
	nxwarp_stats_last = stats;
	// Printed rather than only counted: a harness run long enough to cross a two-second
	// window is the only place the emit side of this can be watched end to end, since the
	// harness has no socket to the main process and no bus.
	std::fprintf(stderr,
	             "[stats] stream %u: %llu frames in %.1f s, %.0f B/frame, QP %.1f, paced %.1f fps, "
	             "%llu not sent, entropy %s, %ux%u\n",
	             unsigned(stats.stream_index),
	             (unsigned long long)stats.frames_encoded,
	             double(stats.window_seconds),
	             double(stats.bytes_per_frame),
	             double(stats.qp_mean),
	             double(stats.paced_fps),
	             (unsigned long long)stats.frames_not_sent,
	             stats.entropy.c_str(),
	             unsigned(stats.encoded_width),
	             unsigned(stats.encoded_height));
}
} // namespace wivrn
