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

#include <array>
#include <chrono>
#include <cstdint>
#include <magic_enum.hpp>
#include <netinet/in.h>
#include <openssl/aes.h>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <openxr/openxr.h>

#include "packed_quaternion.h"
#include "smp.h"
#include "wivrn_serialization_types.h"

namespace wivrn
{

static constexpr int protocol_revision = 1;

enum class device_id : uint8_t
{
	HEAD,                         // /user/head
	LEFT_CONTROLLER_HAPTIC,       // /user/hand/left/output/haptic
	RIGHT_CONTROLLER_HAPTIC,      // /user/hand/right/output/haptic
	LEFT_TRIGGER_HAPTIC,          // /user/hand/left/output/haptic_trigger
	RIGHT_TRIGGER_HAPTIC,         // /user/hand/right/output/haptic_trigger
	LEFT_THUMB_HAPTIC,            // /user/hand/left/output/haptic_thumb
	RIGHT_THUMB_HAPTIC,           // /user/hand/right/output/haptic_thumb
	GAMEPAD_HAPTIC_LEFT,          // /user/gamepad/output/haptic_left
	GAMEPAD_HAPTIC_RIGHT,         // /user/gamepad/output/haptic_right
	GAMEPAD_HAPTIC_LEFT_TRIGGER,  // /user/gamepad/output/haptic_left_trigger
	GAMEPAD_HAPTIC_RIGHT_TRIGGER, // /user/gamepad/output/haptic_right_trigger
	LEFT_GRIP,                    // /user/hand/left/input/grip/pose
	LEFT_AIM,                     // /user/hand/left/input/aim/pose
	LEFT_PALM,                    // /user/hand/left/palm_ext/pose
	RIGHT_GRIP,                   // /user/hand/right/input/grip/pose
	RIGHT_AIM,                    // /user/hand/right/input/aim/pose
	RIGHT_PALM,                   // /user/hand/right/palm_ext/pose
	X_CLICK,                      // /user/hand/left/input/x/click
	X_TOUCH,                      // /user/hand/left/input/x/touch
	Y_CLICK,                      // /user/hand/left/input/y/click
	Y_TOUCH,                      // /user/hand/left/input/y/touch
	MENU_CLICK,                   // /user/hand/left/input/menu/click
	LEFT_SQUEEZE_CLICK,           // /user/hand/left/input/squeeze/click
	LEFT_SQUEEZE_FORCE,           // /user/hand/left/input/squeeze/force
	LEFT_SQUEEZE_VALUE,           // /user/hand/left/input/squeeze/value
	LEFT_TRIGGER_CLICK,           // /user/hand/left/input/trigger/click
	LEFT_TRIGGER_VALUE,           // /user/hand/left/input/trigger/value
	LEFT_TRIGGER_TOUCH,           // /user/hand/left/input/trigger/touch
	LEFT_TRIGGER_PROXIMITY,       // /user/hand/left/input/trigger/proximity
	LEFT_TRIGGER_CURL,            // /user/hand/left/input/trigger/curl_fb
	LEFT_TRIGGER_SLIDE,           // /user/hand/left/input/trigger/slide_fb
	LEFT_TRIGGER_FORCE,           // /user/hand/left/input/trigger/force
	LEFT_THUMBSTICK_X,            // /user/hand/left/input/thumbstick/x
	LEFT_THUMBSTICK_Y,            // /user/hand/left/input/thumbstick/y
	LEFT_THUMBSTICK_CLICK,        // /user/hand/left/input/thumbstick/click
	LEFT_THUMBSTICK_TOUCH,        // /user/hand/left/input/thumbstick/touch
	LEFT_THUMBREST_TOUCH,         // /user/hand/left/input/thumbrest/touch
	LEFT_THUMBREST_FORCE,         // /user/hand/left/input/thumbrest/force
	LEFT_THUMB_PROXIMITY,         // /user/hand/left/input/thumb_resting_surfaces/proximity
	LEFT_TRACKPAD_X,              // /user/hand/left/input/trackpad/x
	LEFT_TRACKPAD_Y,              // /user/hand/left/input/trackpad/y
	LEFT_TRACKPAD_CLICK,          // /user/hand/left/input/trackpad/click
	LEFT_TRACKPAD_TOUCH,          // /user/hand/left/input/trackpad/touch
	LEFT_TRACKPAD_FORCE,          // /user/hand/left/input/trackpad/force
	LEFT_STYLUS_FORCE,            // /user/hand/left/input/stylus_fb/force
	LEFT_PINCH_POSE,              // /user/hand/left/input/pinch_ext/pose
	LEFT_PINCH_VALUE,             // /user/hand/left/input/pinch_ext/value
	LEFT_PINCH_READY,             // /user/hand/left/input/pinch_ext/ready_ext
	LEFT_POKE,                    // /user/hand/left/input/poke_ext/pose
	LEFT_AIM_ACTIVATE_VALUE,      // /user/hand/left/input/aim_activate_ext/value
	LEFT_AIM_ACTIVATE_READY,      // /user/hand/left/input/aim_activate_ext/ready_ext
	LEFT_GRASP_VALUE,             // /user/hand/left/input/grasp_ext/value
	LEFT_GRASP_READY,             // /user/hand/left/input/grasp_ext/ready_ext
	A_CLICK,                      // /user/hand/right/input/a/click
	A_TOUCH,                      // /user/hand/right/input/a/touch
	B_CLICK,                      // /user/hand/right/input/b/click
	B_TOUCH,                      // /user/hand/right/input/b/touch
	SYSTEM_CLICK,                 // /user/hand/right/input/system/click
	RIGHT_SQUEEZE_CLICK,          // /user/hand/right/input/squeeze/click
	RIGHT_SQUEEZE_FORCE,          // /user/hand/right/input/squeeze/force
	RIGHT_SQUEEZE_VALUE,          // /user/hand/right/input/squeeze/value
	RIGHT_TRIGGER_CLICK,          // /user/hand/right/input/trigger/click
	RIGHT_TRIGGER_VALUE,          // /user/hand/right/input/trigger/value
	RIGHT_TRIGGER_TOUCH,          // /user/hand/right/input/trigger/touch
	RIGHT_TRIGGER_PROXIMITY,      // /user/hand/right/input/trigger/proximity
	RIGHT_TRIGGER_CURL,           // /user/hand/right/input/trigger/curl_fb
	RIGHT_TRIGGER_SLIDE,          // /user/hand/right/input/trigger/slide_fb
	RIGHT_TRIGGER_FORCE,          // /user/hand/right/input/trigger/force
	RIGHT_THUMBSTICK_X,           // /user/hand/right/input/thumbstick/x
	RIGHT_THUMBSTICK_Y,           // /user/hand/right/input/thumbstick/y
	RIGHT_THUMBSTICK_CLICK,       // /user/hand/right/input/thumbstick/click
	RIGHT_THUMBSTICK_TOUCH,       // /user/hand/right/input/thumbstick/touch
	RIGHT_THUMBREST_TOUCH,        // /user/hand/right/input/thumbrest/touch
	RIGHT_THUMBREST_FORCE,        // /user/hand/right/input/thumbrest/force
	RIGHT_THUMB_PROXIMITY,        // /user/hand/right/input/thumb_resting_surfaces/proximity
	RIGHT_TRACKPAD_X,             // /user/hand/right/input/trackpad/x
	RIGHT_TRACKPAD_Y,             // /user/hand/right/input/trackpad/y
	RIGHT_TRACKPAD_CLICK,         // /user/hand/right/input/trackpad/click
	RIGHT_TRACKPAD_TOUCH,         // /user/hand/right/input/trackpad/touch
	RIGHT_TRACKPAD_FORCE,         // /user/hand/right/input/trackpad/force
	RIGHT_STYLUS_FORCE,           // /user/hand/right/input/stylus_fb/force
	RIGHT_PINCH_POSE,             // /user/hand/right/input/pinch_ext/pose
	RIGHT_PINCH_VALUE,            // /user/hand/right/input/pinch_ext/value
	RIGHT_PINCH_READY,            // /user/hand/right/input/pinch_ext/ready_ext
	RIGHT_POKE,                   // /user/hand/right/input/poke_ext/pose
	RIGHT_AIM_ACTIVATE_VALUE,     // /user/hand/right/input/aim_activate_ext/value
	RIGHT_AIM_ACTIVATE_READY,     // /user/hand/right/input/aim_activate_ext/ready_ext
	RIGHT_GRASP_VALUE,            // /user/hand/right/input/grasp_ext/value
	RIGHT_GRASP_READY,            // /user/hand/right/input/grasp_ext/ready_ext
	EYE_GAZE,                     // /user/eyes_ext/input/gaze_ext/pose
	LEFT_HAND,                    // identify hand tracking
	RIGHT_HAND,                   // identify hand tracking
	BODY,                         // identify body tracking
	FACE,                         // identify face tracking

	// Gamepad, microsoft/xbox_controller profile (/user/gamepad)
	GAMEPAD_MENU_CLICK,             // /user/gamepad/input/menu/click
	GAMEPAD_VIEW_CLICK,             // /user/gamepad/input/view/click
	GAMEPAD_A_CLICK,                // /user/gamepad/input/a/click
	GAMEPAD_B_CLICK,                // /user/gamepad/input/b/click
	GAMEPAD_X_CLICK,                // /user/gamepad/input/x/click
	GAMEPAD_Y_CLICK,                // /user/gamepad/input/y/click
	GAMEPAD_DPAD_DOWN_CLICK,        // /user/gamepad/input/dpad_down/click
	GAMEPAD_DPAD_RIGHT_CLICK,       // /user/gamepad/input/dpad_right/click
	GAMEPAD_DPAD_UP_CLICK,          // /user/gamepad/input/dpad_up/click
	GAMEPAD_DPAD_LEFT_CLICK,        // /user/gamepad/input/dpad_left/click
	GAMEPAD_SHOULDER_LEFT_CLICK,    // /user/gamepad/input/shoulder_left/click
	GAMEPAD_SHOULDER_RIGHT_CLICK,   // /user/gamepad/input/shoulder_right/click
	GAMEPAD_THUMBSTICK_LEFT_CLICK,  // /user/gamepad/input/thumbstick_left/click
	GAMEPAD_THUMBSTICK_RIGHT_CLICK, // /user/gamepad/input/thumbstick_right/click
	GAMEPAD_TRIGGER_LEFT_VALUE,     // /user/gamepad/input/trigger_left/value
	GAMEPAD_TRIGGER_RIGHT_VALUE,    // /user/gamepad/input/trigger_right/value
	GAMEPAD_THUMBSTICK_LEFT_X,      // /user/gamepad/input/thumbstick_left/x
	GAMEPAD_THUMBSTICK_LEFT_Y,      // /user/gamepad/input/thumbstick_left/y
	GAMEPAD_THUMBSTICK_RIGHT_X,     // /user/gamepad/input/thumbstick_right/x
	GAMEPAD_THUMBSTICK_RIGHT_Y,     // /user/gamepad/input/thumbstick_right/y
};

enum class interaction_profile : uint8_t
{
	none,
	khr_simple_controller,
	ext_hand_interaction_ext,
	bytedance_pico_neo3_controller,
	bytedance_pico4_controller,
	bytedance_pico4s_controller,
	bytedance_pico_g3_controller,
	google_daydream_controller,
	hp_mixed_reality_controller,
	htc_vive_controller,
	htc_vive_cosmos_controller,
	htc_vive_focus3_controller,
	htc_vive_pro,
	ml_ml2_controller,
	microsoft_motion_controller,
	microsoft_xbox_controller,
	oculus_go_controller,
	oculus_touch_controller,
	meta_touch_pro_controller,
	meta_touch_plus_controller,
	meta_touch_controller_rift_cv1,
	meta_touch_controller_quest_1_rift_s,
	meta_touch_controller_quest_2,
	yvr_touch_controller_yvr,
	samsung_odyssey_controller,
	valve_index_controller,
};

enum video_codec
{
	h264,
	h265,
	hevc = h265,
	av1,
	raw,
	// NX Warp, the pure-compute codec of nx-warp/. The bitstream is carried by its
	// own transport (see to_headset::nxwarp_datagram), not by the shard path.
	//
	// Adding an enumerator here changes protocol_version: serialization_traits for
	// an enum feeds every enumerator name and value into the type hash. That is
	// deliberate — a headset that does not know this codec must not negotiate with
	// a server that does — but it means client and server ship together.
	nxwarp,
};

// What a stream IS, said on the wire instead of inferred from its index.
//
// The client used to map index to compositing role by position: 0 and 1 are the
// views, 2 is alpha, 3 is the quad. That rule survived exactly as long as every
// stream was one of those three things. The hybrid base layer is not -- it is an
// atlas patch source, it is never composited on its own, and it lives on the
// stream-1 slot the eye pairing vacates -- so the positional rule would have to
// grow a second special case, and the next stream-shaped feature a third.
//
// Saying the role on the wire costs one byte per stream and one protocol hash
// (the hash is derived from the type, so adding this field moves it on its own),
// and it is what makes the NEXT such feature free.
enum class stream_role : uint8_t
{
	view = 0,  // a view's picture; `paired_eyes` says how many views it carries
	alpha = 1, // the passthrough alpha plane
	quad = 2,  // a promoted quad layer
	base = 3,  // an atlas patch source; never presented on its own

	// Server-side sentinel, never serialised. encoder_settings::role starts here so
	// that a stream nobody assigned a role to is a loud error at description-build
	// time instead of a silent `view` -- which is what shipped an alpha plane to the
	// client labelled as an eye and hung the stream scene before it ever started.
	// The compositor only ever copies roles out of ENABLED streams, and
	// check_stream_roles() refuses this value, so it cannot reach the wire.
	unset = 255,
};

enum class stream_tab : uint8_t
{
	hidden,
	overlay_only,
	compact,
	stats,
	// NX transport HUD: the state of every transport feature, live. Its own wire tab
	// rather than a client-side sub-page of stats, because the server has to know it is
	// open — see to_headset::transport_status.
	transport,
	settings,
	foveation_settings,
	applications,
	application_launcher,
};

// One span of PCM, interleaved signed 16 bit samples in the channel count and
// sample rate audio_stream_description named for that direction. Used both ways:
// server -> headset is the speaker, headset -> server the microphone.
//
// Two transports, chosen by the headset's "low latency audio path" setting:
//
// - the loss-tolerant path (send_stream, i.e. the UDP stream socket, falling back
//   to whatever the path selector currently routes video over). seq is set, and
//   the receiver tracks it: a hole in the numbering is concealed, a late or
//   repeated packet is dropped (common/audio_plc.h). A datagram the Wi-Fi drops
//   then costs one concealed packet instead of stalling every later audio packet
//   behind it in the control socket's byte stream.
// - the control socket (send_control), which is what upstream does. seq is absent
//   and the receiver plays exactly what it is given: ordering and delivery are
//   TCP's problem, and so is the head-of-line blocking.
//
// The packet says which of the two it is, so a receiver needs no setting of its
// own and the toggle can be flipped mid-session from either end.
struct audio_data
{
	XrTime timestamp;
	// Position of this packet in its direction's stream, incremented once per
	// packet and wrapping. Only ever set on the loss-tolerant path.
	std::optional<uint16_t> seq;
	std::span<uint8_t> payload;
	data_holder data;

	// Most PCM one packet may carry. A datagram has to stay under the MTU like
	// everything else on the stream socket, and under the 2048 byte slot the
	// receiver reads datagrams into; a capture buffer longer than this is cut
	// into several packets on whole frame boundaries (see audio_frames_per_packet).
	static constexpr size_t max_payload_size = 1200;
};

// Which control law the automatic bitrate runs, when it runs at all (bitrate_auto).
//
// aimd is the original one: a sliding window of per-frame link utilisation drives a
// multiplicative decrease and an additive probe upwards, with a deep drop and a fast
// rebound on an acute lag spike.
//
// bbr estimates the delivered bandwidth directly (frame bytes over the time the headset
// spent receiving that frame) and sets the bitrate to a gain times a windowed maximum of
// it, the way BBR sets a pacing rate from its bottleneck bandwidth estimate. Experimental.
enum class bitrate_mode : uint8_t
{
	aimd = 0,
	bbr = 1,
};

// Where, if anywhere, the frames the application did not produce are synthesized.
//
// off: nothing happens, a repeat refresh shows the last frame unchanged.
//
// headset: the server measures a motion field between real application frames and sends
// it (to_headset::motion_field); the headset warps the last decoded frame along it on the
// refreshes the application produced nothing for. The duplicate frames on the wire stay
// near-free, so the encoder spends its whole budget on the real ones.
//
// server: the server keeps the last real composited frame, warps it forward along the
// same field on every duplicate commit, and encodes the result. The headset gets a stream
// of genuinely different frames and does nothing special with them — at the price of the
// bitrate those synthesized frames cost. Experimental.
enum class motion_mode : uint8_t
{
	off = 0,
	headset = 1,
	server = 2,
};

// What the headset wants done with a secondary (USB) path, see docs/multipath.md.
//
// off: no secondary path is attached at all. The headset never probes the tunnel, so the
// server never sees one; it is listed here only so that the wire says what the headset's
// selector says.
//
// backup: the Stage 2 behaviour. The path is attached, kept alive and used as a spare —
// video and control flip to it when the Wi-Fi link fails, and flip back when it recovers.
//
// combine: the Stage 3 behaviour. While both paths are healthy the server puts the tail of
// every frame that does not fit the Wi-Fi path's pacing window on the USB path instead, so
// the two links add up. Collapses back to `backup` semantics the moment either path
// degrades. Never entered for a session whose primary already rides the same USB tunnel.
enum class multipath_mode : uint8_t
{
	off = 0,
	backup = 1,
	combine = 2,
};

// Cadence of the transport status feed: how often the server sends
// to_headset::transport_status while subscribed, how often the headset renews the
// subscription, and how long the server keeps sending without a renewal.
//
// The refresh is comfortably shorter than the timeout so that one lost renewal (it rides
// the control socket, so it takes a disconnect rather than a drop) does not blink the page.
inline constexpr std::chrono::milliseconds transport_status_interval{500};
inline constexpr std::chrono::milliseconds transport_status_refresh{2000};
inline constexpr std::chrono::milliseconds transport_status_timeout{5000};

namespace from_headset
{
struct crypto_handshake
{
	uint64_t protocol_version;
	std::string public_key; // In PEM format
	std::string name;
};

struct pin_check_1
{
	crypto::smp::msg1 message;
};

struct pin_check_3
{
	crypto::smp::msg3 message;
};

// First packet on a secondary (multipath) TCP connection: attach to an already
// running session instead of creating a new one. No PIN/SMP exchange, the key
// must already be paired; the token proves that the client owns the session.
struct attach_path
{
	uint64_t protocol_version;
	std::string public_key; // In PEM format, must be a known (paired) key
	std::array<uint8_t, 16> session_token;
	uint8_t path_id; // 1 for the first secondary path
};

// Keepalive, sent on secondary paths only, ~4/s. The server echoes it back as
// to_headset::path_pong so the client can measure the path RTT.
struct path_ping
{
	uint8_t path_id;
	int64_t timestamp; // client steady clock, in ns
};

struct visibility_mask_changed
{
	struct mask
	{
		std::vector<XrVector2f> vertices;
		std::vector<uint32_t> indices;
	};
	static const int num_types = 3; // XrVisibilityMaskTypeKHR values
	using masks = std::array<mask, num_types>;

	masks data;
	uint8_t view_index;
};

enum class face_type : uint8_t
{
	none,
	android,
	fb2,
	htc,
};

enum class body_type : uint8_t
{
	none,
	fb,
	meta,
	bd,
	htc,
};

enum class body_part_mask : uint32_t
{
	chest = 1 << 0,
	left_elbow = 1 << 1,
	right_elbow = 1 << 2,
	hip = 1 << 3,
	left_knee = 1 << 4,
	right_knee = 1 << 5,
	left_foot = 1 << 6,
	right_foot = 1 << 7,
	max,
};

struct settings_changed
{
	float preferred_refresh_rate;
	// for automatic
	float minimum_refresh_rate;

	uint32_t fps_divider = 1;
	uint32_t bitrate_bps;

	// Whether the headset lets the server adapt the bitrate to the link quality, with
	// bitrate_bps as the ceiling. The server also has its own switch, both must be enabled.
	bool bitrate_auto = true;
	// Which control law that adaptation uses. Empty means the headset expresses no
	// preference and the server's own `bitrate-auto`/`mode` configuration key decides; a
	// value set here always wins over it. The headset only fills it in once the user has
	// actually picked one of the adaptive entries in its bitrate selector, so a server
	// configured for one law keeps it for every headset that never touched the setting.
	std::optional<bitrate_mode> bitrate_control;
	// Whether the headset reports its Wi-Fi radio state (see from_headset::wifi_state) and
	// the server may step the bitrate down on a falling signal, before the loss it is about
	// to cause shows up in the frame timings. Only meaningful with bitrate_auto on.
	bool radio_aware = true;
	// Whether the video shards of a frame should be spread over a fraction of the frame
	// period instead of being handed to the socket as fast as it takes them. The burst is
	// what overflows an access point's buffer; the server also has its own switch, and the
	// fraction is a server configuration key.
	bool smooth_pacing = true;
	// Whether the server should send a parity shard per group of video shards, so that
	// the headset rebuilds a lost datagram instead of losing the frame. Costs about
	// 12% of the video bandwidth, which the server takes out of the encoder bitrate so
	// that the total on the wire stays where the bitrate controller put it.
	bool fec = true;
	// Whether the server may size the parity ratio to the loss it is measuring instead of
	// always sending one parity per eight shards, and interleave the groups so that a burst
	// of consecutive datagrams costs one erasure in several groups rather than several in
	// one. Extends the fec switch above rather than replacing it: with fec off this does
	// nothing, with fec on and this off the ratio is the fixed 8+1 it has always been.
	bool fec_adaptive = true;
	// Whether the headset may ask the server to send a video shard again when it notices one
	// missing that the parity cannot rebuild, and the server keep a short history of what it
	// sent so that it can. On a LAN the round trip is a couple of milliseconds against an
	// 11 ms frame budget, so a shard asked for still arrives in time to finish the frame; off,
	// the server keeps no history at all and the headset sends nothing.
	bool shard_retransmit = true;
	// Whether both ends should mark their sockets with a DSCP class, which maps to the WMM
	// access categories on Wi-Fi. Each end applies it to its own sockets; some networks
	// mangle or drop marked traffic, hence the switch.
	bool wifi_qos = true;
	// Whether the encoders should be biased towards keeping fine detail (text, UI) rather
	// than a smooth image. Taken into account when the encoders are created.
	bool sharp_text = false;
	// Whether a stream whose hardware encoder dies or stops answering mid-session should
	// be handed to the software encoder instead of freezing for the rest of the session.
	// Only possible within one codec (the headset's decoder cannot be changed without a
	// reconnect), so it applies to H.264 streams. The server also has its own switch.
	bool encoder_failover = true;
	// Whether loss the forward error correction and the retransmissions could not repair
	// should be recovered from with a rolling column of intra coded blocks sweeping across
	// the next few dozen frames, rather than with a full keyframe. A keyframe is a huge
	// frame that arrives exactly when the link is least able to carry it, and losing it in
	// turn asks for another one; the sweep repairs the picture over about half a second at
	// a near constant bitrate instead. The keyframes that are not loss recovery — the first
	// of a session, and the one after a reconnect — stay keyframes either way. Needs an
	// encoder that has the mechanism (x264 and NVENC do), and the server also has its own
	// switch. Read when the encoders are created, so turning it on takes effect on the next
	// connection; turning it off applies immediately.
	bool intra_refresh = true;
	// Whether a lost frame should first be repaired by telling the encoder to stop predicting
	// from it — the next P frame then references an older frame the headset acknowledged, and
	// the repair costs one ordinary P frame instead of a sweep or a keyframe. The cheapest rung
	// of the recovery ladder, tried before intra_refresh above; a loss older than the encoder's
	// reference buffer, or a repair that is itself lost, falls through to it. Needs an encoder
	// with an invalidation call (NVENC has one; the Vulkan encoders do this inherently and
	// always have; x264 and VAAPI have no such control), and the server also has its own
	// switch. Read when the encoders are created, so turning it on takes effect on the next
	// connection; turning it off applies immediately.
	bool ref_invalidation = true;
	// Whether, as the last automatic resort below the bitrate floor, the server may halve the
	// stream framerate to halve bandwidth when the automatic bitrate is already pinned at its
	// minimum and the link is still losing frames, restoring the full rate once it recovers.
	// Independent of the manual "Half framerate mode". The server also has its own switch.
	bool emergency_framerate = true;
	// Whether the server should estimate a motion field between consecutive application
	// frames so that the headset can warp the last decoded frame on repeat refreshes.
	// The server only does the work while the application is actually below the stream
	// rate; the headset only warps while it has a field for the frame it is displaying.
	//
	// Kept in step with motion_smoothing_mode below — false for off, true for either of
	// the two active modes — so that anything reading the plain switch (the dashboard,
	// an older log) still sees whether the feature is on at all.
	bool motion_smoothing = false;
	// Which end synthesizes the frames the application did not produce. Empty means the
	// headset says nothing about it, in which case motion_smoothing alone decides and
	// the answer is the headset-side warp the switch has always meant.
	std::optional<motion_mode> motion_smoothing_mode;
	// What the headset wants done with the secondary (USB) path. Empty means the headset
	// says nothing about it — an older client, for which the Stage 2 backup behaviour is
	// the whole story and combining must never be entered behind its back.
	std::optional<multipath_mode> multipath;
	// Whether the server may pull an overlay quad layer out of the composited eye
	// images and stream it on its own, for the headset to submit as a real quad
	// layer. Read when the encoders are created (the stream needs its own encoder
	// and its share of the bitrate), and again on every frame: turning it off while
	// streaming immediately puts the layer back into the eye images.
	bool quad_layers = true;
	// Whether the audio streams should ride the same loss-tolerant path as the video
	// instead of sharing the control socket with everything else. Each end applies it
	// to what it sends (the server to the speaker, the headset to the microphone); a
	// receiver needs no setting, an audio_data says which path it came from.
	bool low_latency_audio = true;
	// Whether a device component that has ever been tracked should hold its last tracked
	// pose while the runtime reports it valid but no longer tracked, instead of following
	// whatever pose the runtime keeps emitting. On is the NX behaviour, which stops a
	// controller entering standby from teleporting; off is upstream's, where any valid
	// pose is served and flagged tracked. Components whose runtime never sets the tracked
	// flag at all (estimated body joints) are unaffected either way.
	bool standby_freeze = true;
	// Whether the server should mirror the gamepad to a virtual uinput device;
	// gamepad inputs are always forwarded for the OpenXR path
	bool mirror_gamepad = false;
	// which virtual trackers should be enabled for body tracking
	std::underlying_type_t<body_part_mask> enabled_body_parts;
	// Fraction of the stream eye size the server should actually encode the eye images at,
	// in ]0, 1]. 1.0 is the normal full-resolution stream; below that the encoder, the
	// foveation target and therefore the decoded image shrink by this factor, and the
	// headset reconstructs the full defoveated resolution when sampling the decoded image
	// (best paired with the headset's FSR upscaling). Changing the encoded size cannot be
	// done live, so like the stream/render eye sizes it only takes effect on the next
	// connection: the server reads it when it builds the encoders.
	float render_scale = 1.0;

	// Fixed-foveation "sharper center". Reshapes the foveation resample curve at a FIXED
	// encode size so the central region stays 1:1 over a wider plateau and the periphery
	// falls off more steeply — more central pixels for the same encoded size and bit budget.
	// 0 is the neutral curve WiVRn has always used (behaviour unchanged); 1 is the widest
	// plateau / steepest periphery. Unlike render_scale it never changes the encode size, so
	// the server recomputes it per frame and it applies live, no reconnect. In ]0, 1].
	float foveation_strength = 0.0;
	// Let the server steepen that curve on its own as the link degrades, so the periphery
	// compresses under a Wi-Fi dip instead of the whole image losing quality. Only the curve
	// SHAPE moves (recomputed per frame), never the encode size, so this too is live. Read on
	// every frame and gated on bitrate_auto being active.
	bool foveation_adaptive = false;
	// Bias the encoder QP down over the (now static, gaze-independent) foveal rectangle where
	// the encoder exposes a per-region QP map. Only NVENC (qpDeltaMap) and x264 (quant_offsets)
	// have such a path; VAAPI and Vulkan ignore it. Read when the encoders are created.
	bool foveation_foveal_qp = false;
};

// The motion smoothing mode a settings packet asks for. A headset that names one always
// wins; one that names none is an older or simpler client for which the plain switch is
// the whole story, and the switch has always meant the headset-side warp.
inline motion_mode effective_motion_mode(const settings_changed & settings)
{
	if (settings.motion_smoothing_mode)
		return *settings.motion_smoothing_mode;
	return settings.motion_smoothing ? motion_mode::headset : motion_mode::off;
}

// What a settings packet asks for the secondary path. A headset that names a mode always
// wins; one that names none is an older client, and for it the path can only ever be a
// backup — striping needs a reassembly window that client does not have.
inline multipath_mode effective_multipath_mode(const settings_changed & settings)
{
	return settings.multipath.value_or(multipath_mode::backup);
}

struct headset_info_packet
{
	uint16_t render_eye_width;
	uint16_t render_eye_height;
	uint16_t stream_eye_width;
	uint16_t stream_eye_height;
	std::vector<float> available_refresh_rates;

	// runtime configurable settings
	settings_changed settings;

	struct audio_description
	{
		uint8_t num_channels;
		uint32_t sample_rate;
	};
	std::optional<audio_description> speaker;
	std::optional<audio_description> microphone;
	std::array<XrFovf, 2> fov;
	bool hand_tracking;
	bool eye_gaze;
	bool palm_pose;
	bool user_presence;
	bool passthrough;
	face_type face_tracking;
	body_type body_tracking;
	// htc body only
	uint32_t num_generic_trackers;
	std::vector<video_codec> supported_codecs; // from preferred to least preferred
	// The nxvc tool bits this headset's decoder will accept (docs/SYNTAX.md 2.3), for
	// video_codec::nxwarp only. Zero means "this headset did not report one" -- an old
	// client, or a build with no NX Warp decoder in it -- and the server must read that
	// as "no information", never as "supports nothing", or every stream would be refused.
	//
	// It is the PER-DEVICE mask (nxvc_vk_decoder_tools) and not the build-wide superset
	// (nxvc_vk_decoder_tools_supported), because those differ and the difference is the
	// point: a decoder on an Adreno 650 clears XFORM_LARGE (bit 27), which it decodes
	// wrong and, on the 4:4:4 32x32 vector, wedges on. A capability handshake that sent
	// the superset would invite exactly the stream that kills the session.
	//
	// What it buys: ENTROPY_LITE (bit 30) is a NEGOTIATED tool -- it spends bytes to buy
	// the headset's Pass A time, so only the decoder can say whether it is worth it, and
	// only this field lets the server find out. Before it the server had to be told by
	// hand and a wrong answer was a black screen.
	uint64_t nxvc_tools = 0;
	std::optional<uint8_t> bit_depth;
	std::string system_name;

	// Used for the application list
	std::string language;
	std::string country;
	std::string variant;
};

struct handshake
{
	// Sending this on TCP means connection will be TCP only
};

enum pose_flags : uint8_t
{
	orientation_valid = 1 << 0,
	position_valid = 1 << 1,
	linear_velocity_valid = 1 << 2,
	angular_velocity_valid = 1 << 3,
	orientation_tracked = 1 << 4,
	position_tracked = 1 << 5
};

inline uint8_t to_pose_flags(XrSpaceLocationFlags location_flags, XrSpaceVelocityFlags velocity_flags = 0)
{
	uint8_t flags{};
	if (location_flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
		flags |= pose_flags::orientation_valid;
	if (location_flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		flags |= pose_flags::position_valid;
	if (location_flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)
		flags |= pose_flags::orientation_tracked;
	if (location_flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
		flags |= pose_flags::position_tracked;

	if (velocity_flags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
		flags |= pose_flags::linear_velocity_valid;
	if (velocity_flags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
		flags |= pose_flags::angular_velocity_valid;
	return flags;
}

struct tracking
{
	enum state_flags : uint8_t
	{
		recentered = 1 << 0,
	};

	struct pose
	{
		XrPosef pose;
		XrVector3f linear_velocity;
		XrVector3f angular_velocity;
		device_id device;
		uint8_t flags;
	};

	struct view
	{
		// Relative to XR_REFERENCE_SPACE_TYPE_VIEW
		XrPosef pose;
		XrFovf fov;
	};

	// /user/hand/left, /user/hand/right and /user/gamepad
	std::array<interaction_profile, 3> interaction_profiles;

	XrTime production_timestamp;
	XrTime timestamp;
	XrViewStateFlags view_flags;

	uint8_t state_flags;

	std::array<view, 2> views;
	std::vector<pose> device_poses;

	struct android_face
	{
		std::array<float, XR_FACE_PARAMETER_COUNT_ANDROID> parameters;
		std::array<float, XR_FACE_REGION_CONFIDENCE_COUNT_ANDROID> confidences;
		XrFaceTrackingStateANDROID state;
		XrTime sample_time;
		bool is_calibrated;
		bool is_valid;
	};

	struct fb_face2
	{
		XrTime time;
		std::array<float, XR_FACE_EXPRESSION2_COUNT_FB> weights;
		std::array<float, XR_FACE_CONFIDENCE2_COUNT_FB> confidences;
		bool is_valid;
		bool is_eye_following_blendshapes_valid;
	};

	struct htc_face
	{
		XrTime eye_sample_time;
		XrTime lip_sample_time;
		std::array<float, XR_FACIAL_EXPRESSION_EYE_COUNT_HTC> eye;
		std::array<float, XR_FACIAL_EXPRESSION_LIP_COUNT_HTC> lip;
		bool eye_active;
		bool lip_active;
	};

	std::variant<std::monostate, android_face, fb_face2, htc_face> face;
};

struct derived_pose
{
	device_id source;
	device_id target;
	XrPosef relation;
};

struct hand_tracking
{
	enum hand_id : uint8_t
	{
		left,
		right,
	};
	struct pose
	{
		XrVector3f position;
		packed_quaternion orientation;
		XrVector3f linear_velocity;
		XrVector3f angular_velocity;
		// In order to avoid packet fragmentation
		// use 2 less bytes for radius
		uint16_t radius; // 10th of mm
		uint8_t flags;
	};

	XrTime production_timestamp;
	XrTime timestamp;
	hand_id hand;
	std::optional<std::array<pose, XR_HAND_JOINT_COUNT_EXT>> joints;
};

struct meta_body
{
	struct pose
	{
		XrVector3f position;
		packed_quaternion orientation;
		uint8_t flags;
	};
	struct packed_pose
	{
		struct
		{
			int16_t x, y, z; // 10th of mm relative to root
		} position;
		packed_quaternion orientation;
		uint8_t flags;
	};

	XrTime production_timestamp;
	XrTime timestamp;
	float confidence;

	struct fb_joints
	{
		pose root;
		// excluding root
		std::array<packed_pose, XR_BODY_JOINT_COUNT_FB - 1> joints;
	};
	struct meta_joints
	{
		pose root;
		// excluding root
		std::array<packed_pose, XR_FULL_BODY_JOINT_COUNT_META - 1> joints;
	};
	std::variant<std::monostate, fb_joints, meta_joints> joints;
};

struct meta_body_skeleton
{
	struct fb_skeleton
	{
		std::array<XrBodySkeletonJointFB, XR_BODY_JOINT_COUNT_FB> joints;
	};
	struct meta_skeleton
	{
		std::array<XrBodySkeletonJointFB, XR_FULL_BODY_JOINT_COUNT_META> joints;
	};
	std::variant<fb_skeleton, meta_skeleton> skeleton;
};

struct bd_body
{
	struct pose
	{
		XrVector3f position;
		packed_quaternion orientation;
		uint8_t flags;
	};

	bool all_tracked;
	XrTime production_timestamp;
	XrTime timestamp;
	std::array<pose, XR_BODY_JOINT_COUNT_BD> joints;
};

struct htc_body
{
	static constexpr size_t max_tracked_poses = 16;
	struct pose
	{
		XrPosef pose;
		XrVector3f linear_velocity;
		XrVector3f angular_velocity;
		uint8_t flags;
	};

	XrTime production_timestamp;
	XrTime timestamp;
	std::array<pose, max_tracked_poses> poses;
};

struct inputs
{
	struct input_value
	{
		device_id id;
		float value;
		XrTime last_change_time;
	};
	std::vector<input_value> values;
};

struct hid
{
	struct button_down
	{
		uint8_t button;
	};

	struct button_up
	{
		uint8_t button;
	};

	struct mouse_move
	{
		float x;
		float y;
	};

	struct mouse_scroll
	{
		float h;
		float v;
	};

	struct key_down
	{
		uint8_t key;
	};

	struct key_up
	{
		uint8_t key;
	};

	using input_t = std::variant<button_down, button_up, mouse_move, mouse_scroll, key_down, key_up>;
	struct input
	{
		input_t input_data;
	};
};

struct timesync_response
{
	XrTime query;
	XrTime response;
};

struct feedback
{
	uint64_t frame_index;
	uint8_t stream_index;

	// Timestamps
	XrTime encode_begin;
	XrTime encode_end;
	XrTime send_begin;
	XrTime send_end;
	XrTime received_first_packet;
	XrTime received_last_packet;
	XrTime sent_to_decoder;
	XrTime received_from_decoder;
	XrTime blitted;
	XrTime displayed;

	uint8_t times_displayed;

	// Data shards of this frame that were rebuilt from a parity shard rather than
	// received (see to_headset::video_stream_parity_shard). Nonzero means the link
	// dropped datagrams and forward error correction absorbed them: the frame is
	// complete, sent_to_decoder is set, and it must not be counted as lost.
	uint8_t reconstructed_shards;
};

// Data shards of one video frame the headset is missing and would like sent again.
//
// Sent on the stream (UDP) socket, because a request that arrives after the frame's
// display deadline is worth nothing and the control socket's head-of-line blocking is
// exactly what would make it late. Losing one costs a retransmission round, no more:
// the headset gives up cleanly after a couple of rounds and the frame then takes the
// incomplete-frame path it always did.
//
// Only ever describes shards the parity cannot already rebuild. A group with one hole
// and its parity in hand repairs itself, so those indices are left out; a group with
// two holes has all but one of them nacked, since repairing the rest is what puts the
// parity back in reach.
struct nack
{
	uint8_t stream_index;
	uint64_t frame_idx;
	// Lowest missing shard index this request covers, and always the first bit set
	// in the bitmap below.
	uint16_t first_shard_idx;
	// Bit (8 * b + i) of bitmap, LSB first within a byte, asks for shard
	// first_shard_idx + 8 * b + i. Capped at max_nack_bitmap_bytes, i.e. a window of
	// 256 shards from the first missing one — past that the frame is too far gone
	// for a retransmission to save it anyway.
	std::vector<uint8_t> bitmap;
};

// Longest bitmap a nack may carry, and therefore the widest window of shard indices
// one request can name.
inline constexpr size_t max_nack_bitmap_bytes = 32;

struct battery
{
	float charge;
	bool present;
	bool charging;
};

// State of the headset's Wi-Fi radio, sampled about once a second and sent on the control
// socket (tiny, and a lost sample is a hole in a trend rather than one stale frame).
//
// The server uses the *trend* only: absolute RSSI is not comparable across houses, headsets
// or even antenna orientations, but a signal falling several dB over a few seconds is the
// user walking away from the access point, and it precedes the packet loss by a second or
// two. valid is false whenever the platform gave no usable reading — a non-Android client,
// no WifiManager (wired or unknown transport), or the sentinel values Android returns when
// it will not answer (RSSI -127, link speed -1). The other fields are then meaningless.
struct wifi_state
{
	bool valid;
	// Received signal strength, negative dBm, e.g. -55 close to the AP, -75 far from it
	int8_t rssi_dbm;
	// Negotiated PHY rate, Mbit/s. Nominal: aggregation, contention and the uplink all take
	// their share of it, so the usable throughput is a fraction of this.
	uint16_t link_speed_mbps;
	// Headset clock, for logging and ordering only; the server ages samples on its own clock
	XrTime timestamp;
};

struct refresh_rate_changed
{
	float from;
	float to;
};

struct session_state_changed
{
	XrSessionState state;
};

struct user_presence_changed
{
	bool present;
	XrTime change_time;
};

struct stream_tab_changed
{
	stream_tab tab;
};

// Subscription to to_headset::transport_status, the feed behind the headset's Transport
// page. Nothing else consumes it, so it is not streamed while nobody is looking: the page
// sends this with active true when it opens and every transport_status_refresh while it
// stays open, and with active false when it closes.
//
// The repeat is not redundancy, it is the lease: the server stops after
// transport_status_timeout without one. A headset that crashes, disconnects mid-frame or
// simply forgets to unsubscribe therefore costs at most one timeout of chatter, and the
// unsubscribe is only an optimisation on top of it.
struct transport_status_subscribe
{
	bool active;
};

struct override_foveation_center
{
	bool enabled;
	float pitch;
	float distance;
};

struct get_application_list
{
	std::string language;
	std::string country;
	std::string variant;
};

struct start_app
{
	std::string app_id;
};

struct get_running_applications
{};

struct set_active_application
{
	uint32_t id;
};

struct stop_application
{
	uint32_t id;
};

// Per-band feedback for an NX Warp stream: the client's receiver builds it
// (nxt::Receiver::band_deadline) and the server hands it straight back to that
// stream's nxt::Sender, which folds it into the client shadow. Opaque here on
// purpose — the wire format is normative in nx-warp/docs/TRANSPORT.md 8 and this
// packet is only an envelope, so a transport revision does not touch WiVRn.
//
// Sent on the control (TCP) socket: it is small, it is cumulative, and a lost
// feedback packet costs the encoder a frame of shadow knowledge.
struct nxwarp_feedback
{
	// Same stream numbering as video_stream_data_shard
	uint8_t stream_item_idx;
	// Path the datagrams it reports on arrived over (nxt path_id, 0 or 1)
	uint8_t path_id;
	std::vector<uint8_t> payload;

	// What one frame of this stream costs this headset to decode, in microseconds, or
	// 0 before it has decoded anything.
	//
	// It rides here rather than in a packet of its own because it is the same kind of
	// statement as the payload above -- a periodic report from the receiving end about
	// what it is managing -- and it goes out at the same cadence, several times a
	// frame, which is exactly the rate the server's pace controller wants to read it
	// at. A packet of its own would cost a control-socket write per report for two
	// bytes.
	//
	// The server uses it for one thing: video_encoder_nxwarp's send pacing. A headset
	// that decodes in 31 ms cannot be sent 90 frames a second, and until this field
	// existed the server had no way to know that other than by counting the frames the
	// headset threw away -- each of which cost an all-intra frame, which made the
	// decode slower, which made it throw away more. See the pace controller in
	// video_encoder_nxwarp.cpp.
	//
	// Saturates at 65535 us. A decode slower than 65 ms is already below the pace
	// floor, so the exact figure past that point changes nothing.
	uint16_t decode_us = 0;

	// The frames this headset HAS reconstructed: `held_base` is the newest, and bit k
	// of `held_mask` says the frame `held_base - k` was reconstructed too. Bit 0 is
	// therefore always set when the field means anything, so `held_mask == 0` is "this
	// headset has reconstructed nothing yet" and needs no separate flag.
	//
	// It is the POSITIVE half of nxwarp_frame_not_held below, and the encoder needs
	// both for different reasons. The negative report is a correction and arrives on
	// its own message as soon as the headset knows; it is fast, and it is what stops
	// the encoder spending another frame predicting from something already lost. But
	// it is negative, so silence means "held" -- and silence is also exactly what a
	// frame dropped a moment ago produces. The encoder codes frame N believing N-1 is
	// held and only learns otherwise after N has already been refused, and no amount of
	// reasoning on the encoder closes that window, because the information does not
	// exist there yet.
	//
	// This closes it. A frame the headset says it reconstructed is one it can predict
	// from, whatever happened before or after, so an inter frame that references only
	// confirmed frames cannot be refused. nxvc takes it through
	// nxvc_vk_encoder_set_frame_held(enc, f, 1) and picks the newest confirmed frame
	// within ref_sel's three-frame reach, falling to an all-intra frame -- which is
	// decodable -- when there is none.
	//
	// It rides this packet rather than one of its own for the reason decode_us does:
	// it is the same kind of statement, it wants the same cadence, and a packet of its
	// own would cost a control-socket write per report for six bytes. The cadence
	// MATTERS and is not free to change: measured at 1088x1088 over a client
	// reconstructing every other frame, a confirmation that arrives within the frame
	// costs 3 % more bytes than a client that drops nothing, one frame of latency costs
	// 31 %, and two frames cost 3.9x, because the encoder must reach further back for a
	// confirmed reference and runs out of ring at three. This packet goes out once per
	// transport band -- several times a frame -- which is why it is the right one.
	//
	// 32 bits of history is about a third of a second at 90 Hz, far more than the three
	// frames ref_sel can reach; the surplus is what makes a lost feedback packet cost
	// nothing, since the next one repeats the same window.
	uint16_t held_base = 0;
	uint32_t held_mask = 0;
};

// Where one eye's GPU decode time went, over the decoder's own two-second window.
//
// A SEPARATE packet, and the cadence is the whole reason. nxwarp_feedback carries
// decode_us and the held mask because those want to arrive several times a frame --
// the pace controller and the reference selector both read them that often, and the
// packet's own comment explains why a packet of its own would be wasteful for two
// bytes. This is the opposite case: it is a profile window, republished about every
// two seconds, and putting it on the feedback packet would send the same unchanged
// numbers several hundred times per window to no purpose.
//
// It exists so the dashboard can show what the in-headset HUD shows. Nothing on the
// server READS it to make a decision -- it is reported, not acted on -- which is why
// losing one costs a stale card and nothing else, and why it goes out on the control
// socket without any retry of its own.
//
// `pass_b_ms` is an ENVELOPE: nxvc measures it from the end of Pass A to the end of
// Pass B, so it contains the predictor dispatch (`pass_w_ms`) and all three
// reconstruction segments. The parts therefore do NOT sum to it, and the remainder is
// not slack -- the segments are timestamps taken around dispatches and the pipeline
// drain between them belongs to none of them. Both ends report that remainder as
// "other" rather than distributing it.
//
// Milliseconds as floats because that is what nxvc reports and what is displayed; the
// packet is sent twice a second, so there is nothing to gain by packing it smaller.
struct nxwarp_decode_profile
{
	// Same stream numbering as video_stream_data_shard: 0 and 1 are the eyes.
	uint8_t stream_item_idx = 0;
	// True when this build's nxvc breaks Pass B up at all. False means every segment
	// below is zero because it was never measured, which a reader must be able to tell
	// from a decode that genuinely cost nothing.
	bool segments_known = false;

	float pass_a_ms = 0;
	float pass_b_ms = 0; // the envelope, which contains everything below
	float pass_w_ms = 0;
	float pass_b_skip_ms = 0;  // WARP_SKIP tiles: the normative integer pose warp
	float pass_b_coded_ms = 0; // every other non-INTRA tile
	float pass_b_dir_ms = 0;   // INTRA tiles, directional-intra wavefront

	// Mean tiles per frame in each segment, eye pass 0 -- what makes a zero segment
	// legible: no tiles means empty, tiles with no time means the device cannot
	// timestamp it.
	float tiles_skip = 0;
	float tiles_coded = 0;
	float tiles_dir = 0;
};

// A frame the headset will NOT reconstruct, named by the stream's own 16-bit frame id.
//
// It is a CORRECTION, and that is the whole reason it has to exist separately from
// nxwarp_feedback above. That one is the transport's report and it is about the wire: it
// says which tiles arrived, it is assembled on the network thread as the datagrams land,
// and it goes out on the band deadline -- before the decoder has done anything with them.
// Everything that happens to a frame after that point is invisible to it. And plenty
// does: the shared decode stride skips frames this device cannot decode at the rate they
// arrive, the bounded worker queue discards a frame that is already stale by the time the
// worker reaches it, the codec can refuse a unit, and a frame can close with a hole. In
// every one of those the transport has already said "these tiles arrived", truthfully,
// and the headset then does not reconstruct the picture.
//
// With an all-intra stream that costs one dropped frame and nothing else. With inter
// prediction it is a silent corruption: the encoder's receipt map says the client holds
// frame N, so it codes N+1 as a warp of it, and the headset warps from a reference that
// was never built. What the user sees is a few blocks of real picture and the rest grey.
//
// So this packet says the thing the transport cannot: the frame arrived and was not
// reconstructed. The server's answer is the blunt and correct one -- an all-zero receipt
// map, which is nxvc's documented way to say "the client holds nothing", and which makes
// the next frame code every tile INTRA so the headset resynchronises from it.
//
// It rides the CONTROL socket. A lost one is the corruption it exists to prevent.
struct nxwarp_frame_not_held
{
	// Same stream numbering as video_stream_data_shard
	uint8_t stream_item_idx;
	// The stream's own 16-bit frame id, as it arrived on the wire
	uint16_t frame_id;
	// Why, for the server's log only. Never a control input: every reason has the
	// same consequence, and a server that treated them differently would be trusting
	// a headset's account of its own scheduling.
	enum class reason : uint8_t
	{
		hole,      // closed incomplete: a chunk never arrived
		stride,    // skipped by the shared decode stride
		backlog,   // discarded by the bounded worker queue
		refused,   // the codec would not decode the unit
	} why;
};

// when changing this, also make sure there are handlers in wivrn_session, etc. or compilation will fail
using packets = std::variant<
        crypto_handshake,
        pin_check_1,
        pin_check_3,
        attach_path,
        path_ping,
        headset_info_packet,
        settings_changed,
        feedback,
        audio_data,
        handshake,
        tracking,
        derived_pose,
        hand_tracking,
        meta_body,
        meta_body_skeleton,
        bd_body,
        htc_body,
        inputs,
        timesync_response,
        nack,
        battery,
        wifi_state,
        visibility_mask_changed,
        refresh_rate_changed,
        session_state_changed,
        user_presence_changed,
        stream_tab_changed,
        transport_status_subscribe,
        nxwarp_feedback,
        nxwarp_frame_not_held,
        nxwarp_decode_profile,
        override_foveation_center,
        get_application_list,
        start_app,
        get_running_applications,
        set_active_application,
        hid::input,
        stop_application>;
} // namespace from_headset

namespace to_headset
{

struct crypto_handshake
{
	enum class crypto_state : uint8_t
	{
		encryption_disabled,
		pin_needed,
		client_already_paired,
		pairing_disabled,
		incompatible_version,
	};

	std::string public_key; // In PEM format
	crypto_state state;
};

struct pin_check_2
{
	crypto::smp::msg2 message;
};

struct pin_check_4
{
	crypto::smp::msg4 message;
};

struct handshake
{
	// -1 if stream socket should not be used
	int stream_port;

	// Random, generated once per session. A secondary path is attached to this
	// session by presenting it back in from_headset::attach_path.
	std::array<uint8_t, 16> session_token;
};

// Reply to from_headset::attach_path
struct attach_path_response
{
	enum class attach_state : uint8_t
	{
		accepted,
		rejected,
		incompatible_version,
	};

	// Ephemeral X448 public key in PEM format, empty if encryption is disabled
	std::string public_key;
	attach_state state;
	uint8_t path_id;
};

// Reply to from_headset::path_ping, echoes it back
struct path_pong
{
	uint8_t path_id;
	int64_t timestamp;
};

struct server_message
{
	enum class kind : uint8_t
	{
		// in-stream toasts (not buffered)
		toast,
		toast_urgent,

		// displayed in lobby after disconnect (buffered)
		error,
	};

	kind kind;
	std::string msg;
};

struct foveation_parameter
{
	// The number of source pixels for each ratio,
	// the middle one is 1:1
	//
	// how to read it:
	// 1, 4, 5, 3, 1 means:
	// the first output pixel has 3 source pixels
	// the 4 that come after have 2 source pixels
	// then 5 with 1 source pixel
	// 3 with 2 source pixels
	// 1 with 3 source pixels
	std::vector<uint16_t> x;
	std::vector<uint16_t> y;
};

struct audio_stream_description
{
	struct device
	{
		uint8_t num_channels;
		uint32_t sample_rate;
	};
	std::optional<device> speaker;
	std::optional<device> microphone;
};

struct video_stream_description
{
	// dimensions of the video stream per eye
	// alpha is half resolution
	uint16_t width;
	uint16_t height;
	std::array<video_codec, 4> codec; // left, right, alpha, quad
	float frame_rate;
	float refresh_rate;

	// Encoded size of the promoted quad layer stream (index 3). Zero when the
	// server is not streaming quad layers separately, which is also how the
	// headset knows not to create a decoder for that stream.
	//
	// This is the one stream whose geometry is not derived from width/height:
	// it carries an overlay panel, capped to its own resolution, and only a
	// sub-rectangle of it holds picture (see view_info_t::quad_info_t::source).
	uint16_t quad_width = 0;
	uint16_t quad_height = 0;

	// Eyes carried by stream 0. 2 when the NX Warp encoder codes both eyes of a
	// frame as ONE nxvc stereo frame, in which case stream 1 has no encoder and
	// never receives a datagram -- so stream_size() reports it as zero and the
	// headset does not build a decoder for it, the same way quad_width does for
	// the quad stream. Without this the client allocates a full second decoder
	// (about 35 MB, its own images and reassembly window) that nothing ever
	// feeds.
	//
	// It has to be here rather than inferred from the codec's own stream header,
	// which also carries the eye count: the decoders are created when this packet
	// arrives, and the .nxv header does not turn up until the first frame.
	//
	// Stream 1 keeps its `codec` entry so the view-to-stream mapping is unchanged;
	// what it loses is its size, and therefore its decoder.
	uint8_t paired_eyes = 1;

	// What each stream is, by index. The default is the historical positional
	// rule, so a description built without touching this behaves exactly as
	// before; the server overwrites it from the encoder settings.
	std::array<stream_role, 4> role = {
	        stream_role::view,
	        stream_role::view,
	        stream_role::alpha,
	        stream_role::quad,
	};

	// For a `base` stream: the index of the stream whose atlas it fills. 0xff
	// where it does not apply. This is what lets a base stream NAME its
	// enhancement stream, rather than the client knowing "stream 1 fills stream 0
	// because it is stream 1".
	std::array<uint8_t, 4> serves_stream = {0xff, 0xff, 0xff, 0xff};

	// Edge bleed. Sent once with the rest of the stream geometry because it is decided
	// once, at connection, in the same place: the overscan is baked into the FOVs the
	// application is already rendering by the time the first frame is encoded, so a
	// mid-session change would mean a mismatched frame in flight for nothing.
	//
	// `edge_bleed_overscan` is what the SERVER did -- the fraction of each side's tangent
	// the rendered FOV was widened by, 0 for none. The headset does not need it to draw
	// (the widened FOV arrives per frame in view_info like any other), it needs it to know
	// whether the margin is already real pixels, in which case it invents nothing.
	//
	// `edge_bleed_fallback` and `edge_bleed_extension` are what the headset should do when
	// it is not: widen its own projection layer by the fallback margin and fill the
	// invented ring by stretching the image edge over it, fading toward that edge's own
	// colour past `edge_bleed_fade` of the ring. Extension 0 is none, 1 clamp, 2 fade;
	// it is view_geometry::edge_extension, sent as its underlying type so the packet does
	// not depend on the header.
	float edge_bleed_overscan = 0;
	float edge_bleed_fallback = 0;
	float edge_bleed_fade = 0;
	uint8_t edge_bleed_extension = 0;

	bool operator==(const video_stream_description &) const = default;

	constexpr stream_role role_of(uint8_t stream_index) const
	{
		return stream_index < role.size() ? role[stream_index] : stream_role::view;
	}

	// Encoded size of one stream, the size the decoder for it must be created with.
	// Zero means "this stream does not exist this session" and no decoder is built.
	constexpr std::pair<uint16_t, uint16_t> stream_size(uint8_t stream_index) const
	{
		switch (role_of(stream_index))
		{
			case stream_role::alpha: // half height
				return {width, uint16_t(height / 2)};
			case stream_role::quad:
				return {quad_width, quad_height};
			case stream_role::base:
				// The base is coded as ONE side-by-side picture over the eye
				// pair, and unlike the nxvc stream it carries no header of its
				// own that says so -- an HEVC SPS just states a width. So the
				// size reported here is the CODED size, pair-wide, and not the
				// per-eye size the view streams report.
				return {uint16_t(width * (paired_eyes ? paired_eyes : 1)), height};
			case stream_role::view:
			default:
				// The right-eye view stream is absent when stream 0 carries both
				// eyes; every other view stream reports one eye.
				return (stream_index == 1 and paired_eyes > 1)
				               ? std::pair<uint16_t, uint16_t>{0, 0}
				               : std::pair<uint16_t, uint16_t>{width, height};
		}
	}
};

class video_stream_data_shard
{
public:
	inline static const size_t max_payload_size = 1400;
	// Identifier of stream:
	// 0 left
	// 1 right
	// 2 alpha
	// 3 promoted quad layer
	uint8_t stream_item_idx;
	// Counter increased for each frame
	uint64_t frame_idx;
	// Identifier of the shard within the frame
	uint16_t shard_idx;

	// Position information, must be present on first video shard
	struct view_info_t
	{
		// ns in headset time referential
		XrTime display_time;

		std::array<XrPosef, 2> pose;
		std::array<XrFovf, 2> fov;
		std::array<foveation_parameter, 2> foveation;
		// True when the frame contains an alpha channel
		bool alpha;

		// Where the headset must put the picture carried by stream 3. Only ever
		// present on that stream; the eye streams leave it empty.
		struct quad_info_t
		{
			// Pose of the centre of the quad, in the space named by head_locked:
			// the same world space the eye poses above are expressed in, or the
			// view (head) space for a layer the application asked to be head
			// locked. The headset submits the quad in that space so that the
			// runtime, not the server, is what holds it still.
			XrPosef pose;
			// Size of the quad in meters
			XrExtent2Df size;
			// True when pose is relative to the view space rather than the world
			bool head_locked;
			// Part of the encoded image that holds picture. The rest is padding:
			// the encoded size is fixed for the session while a panel may change
			// size or aspect ratio at any time.
			XrRect2Di source;
		};
		std::optional<quad_info_t> quad;
	};
	std::optional<view_info_t> view_info;

	// Information about timing, on last video shard
	struct timing_info_t
	{
		XrTime encode_begin;
		XrTime encode_end;
		XrTime send_begin;
		XrTime send_end;
	};
	std::optional<timing_info_t> timing_info;
	// Actual video data, may contain multiple NAL units
	std::span<uint8_t> payload;

	// Container for the data, read payload instead
	data_holder data;
};

// Forward error correction for the video stream: one parity shard per group of
// consecutive data shards of a frame, so that a single datagram lost out of the
// group is rebuilt on the headset instead of costing the whole frame and the IDR
// round trip that follows it.
//
// The scheme is a plain XOR over the group, which recovers exactly one erasure.
// That is the dominant loss on a Wi-Fi link: an access point drops the odd
// datagram far more often than it drops a run of them. A code that survives a
// burst (Reed-Solomon over the same groups) is a v2 item; it costs the same
// bandwidth but a great deal more arithmetic.
//
// What is XOR'd is not the payload alone but a *recovery blob* per data shard:
// its view_info, its timing_info and its payload, in the same encoding the data
// shard itself uses (see common/fec.h). Doing it that way makes the group uniform
// — the first shard of a frame, which carries the pose, and the last one, which
// carries the timings and without which the headset cannot even tell the frame is
// finished, are recovered like any other. Blobs shorter than the longest in the
// group are zero padded before the XOR, and blob_size records the true length of
// each so the padding can be cut back off.
//
// Only ever sent on the UDP stream socket. On the TCP paths (control socket,
// secondary path) nothing is lost in the first place and parity would be pure
// waste.
class video_stream_parity_shard
{
public:
	// Same stream numbering as video_stream_data_shard
	uint8_t stream_item_idx;
	// Frame the covered data shards belong to
	uint64_t frame_idx;
	// shard_idx of the first data shard of the group. With shard_stride below, the
	// group is first_shard_idx, first_shard_idx + shard_stride, and so on for
	// blob_size.size() entries.
	uint16_t first_shard_idx;
	// Gap between two consecutive shards of the group. 1 is the contiguous group,
	// which is what the fixed 8+1 scheme emits; the adaptive scheme interleaves,
	// so that a burst of consecutive datagrams lost on the air lands one erasure in
	// each of `shard_stride` different groups instead of killing one of them
	// outright. 0 never appears on the wire and is read as 1.
	uint16_t shard_stride;
	// Recovery blob length of each covered data shard, in shard_idx order. Its
	// size is the group size: the configured K for a full group, less for the
	// last group of a frame. Also tells the headset that those shard indices
	// exist at all, which is how the last shard of a frame can be missed and
	// still be rebuilt.
	std::vector<uint16_t> blob_size;
	// XOR of the covered recovery blobs, each zero padded to the longest of them.
	// Its length is therefore max(blob_size).
	std::span<uint8_t> payload;

	// Container for the data, read payload instead
	data_holder data;
};

// Coarse motion field between two consecutive *distinct* application frames, sent
// once per application frame while motion smoothing is active. The headset uses it
// to warp the last decoded frame on the refreshes for which the application has not
// produced anything new.
//
// A whole field is a few kilobytes, more than a datagram may carry, so it is cut
// into chunks of whole grid rows of one eye. Every chunk repeats the whole header,
// so chunks are independent and may arrive in any order; the headset only warps
// along a field it has received every chunk of.
//
// Losing a chunk simply means no smoothing until the next application frame: the
// headset ignores a field that does not name the frame it is displaying, and an
// incomplete one just as much.
struct motion_field
{
	// Video frame the field starts from. The field describes where the content of
	// that frame came from, span_ns earlier.
	uint64_t frame_idx;
	// Interval the field spans, in the headset time referential: the display time
	// of frame_idx minus the display time of the previous distinct application
	// frame. Always strictly positive.
	XrTime span_ns;

	// Cells per eye. The grid covers the whole eye image; cell (i, j) is centred at
	// ((i + 0.5) / width, (j + 0.5) / height) in normalized coordinates of the
	// defoveated (full resolution) eye image.
	uint16_t width;
	uint16_t height;

	// Longest displacement in the field, as a fraction of the eye image. A cell
	// value of v means a displacement of (v / 127) * scale, in normalized image
	// coordinates: what is now at p was at p - (v / 127) * scale, span_ns ago.
	float scale;

	// Which eye, and which rows of its grid, this chunk carries. Rows [row_offset,
	// row_offset + row_count) of eye view; a complete field is every row of both
	// eyes.
	uint8_t view;
	uint16_t row_offset;
	uint16_t row_count;

	// Two components (x, y) per cell, row major, for this chunk's rows only:
	// index = ((j - row_offset) * width + i) * 2, so the size is row_count * width
	// * 2. Values are in [-127, 127].
	std::vector<int8_t> vectors;

	// Most vector bytes in one chunk. Small enough that a chunk, header and stream
	// framing included, stays well under any MTU worth worrying about and under the
	// receive buffer the headset reads datagrams into.
	static constexpr size_t max_chunk_bytes = 1024;

	// Rows of one eye that fit in a single chunk, never zero
	static constexpr uint16_t rows_per_chunk(uint16_t width)
	{
		const size_t row_bytes = size_t(width) * 2;
		if (row_bytes == 0 or row_bytes >= max_chunk_bytes)
			return 1;
		return uint16_t(max_chunk_bytes / row_bytes);
	}
};

struct haptics
{
	device_id id;
	std::chrono::nanoseconds duration;
	float frequency;
	float amplitude;
};

struct timesync_query
{
	XrTime query;
};

struct tracking_control
{
	struct sample
	{
		device_id device;
		XrDuration prediction_ns;
	};

	std::vector<sample> pattern;
	XrDuration motions_to_photons;
};

struct feature_control
{
	enum feature
	{
		hid_input,
		microphone,
	};
	feature f;
	bool state;
};

struct refresh_rate_change
{
	float hz;
};

struct stream_tab_change
{
	stream_tab tab;
};

// Everything the headset's Transport page needs that only the server can know, in one
// packet at transport_status_interval while subscribed (from_headset::transport_status_subscribe).
//
// Deliberately small and deliberately incomplete: anything the headset can measure for
// itself — RTT, its own Wi-Fi radio, received bandwidth, FEC repairs, decode timings — is
// measured on the headset and never asked for here, because a number that has crossed the
// link is a number about the past. What is left is the server's own decisions.
struct transport_status
{
	// What the automatic bitrate controller is doing. off means it is not running at all
	// and the bitrate is exactly the one the headset asked for; the rest name the state of
	// whichever control law is in force (mode says which).
	enum class controller_state : uint8_t
	{
		off,
		// aimd: slow additive probing around the current value
		// bbr: cruising at the steady gain on the bandwidth estimate
		steady,
		// aimd only: below a remembered pre-drop bitrate after a deep drop, rebounding
		recovering,
		// bbr only: still growing the bandwidth estimate
		startup,
		// bbr only: a short probe above the estimate
		probe,
	};

	// Which path the server is putting video on, and whether it has a choice.
	enum class path_state : uint8_t
	{
		// No secondary path attached: Wi-Fi, and nothing to fail over to
		wifi_only,
		// USB attached and idle, Wi-Fi still carries video
		wifi_usb_ready,
		// Video flipped to the USB path
		usb,
		// Both paths carry video at once: Wi-Fi takes what fits its pacing window,
		// the tail of every frame goes over USB (docs/multipath.md, stage 3)
		combining,
	};

	// Bitrate the encoders are running at, and the ceiling in force: the one the headset
	// asked for, clamped by the path's own limit when the USB path carries video.
	uint32_t bitrate_bps;
	uint32_t ceiling_bps;

	bitrate_mode mode;
	controller_state state;
	path_state path;

	// Share of the video bytes the server put on the primary (Wi-Fi) path over the last
	// status period, in percent; the rest went over the secondary (USB) one. 100 whenever
	// the two paths are not both carrying video, which is what every other state means.
	uint8_t wifi_share_pct;

	// The radio trend took a preemptive step down and has not released it: upward probing
	// is held off until the signal recovers or the reports go stale.
	bool radio_hold;
	// Shards are being spread over a fraction of the frame period rather than burst.
	bool pacing_active;
	// A parity shard is going out per group of video shards.
	bool fec_active;

	// Motion smoothing is in server mode and armed: the last real frame is being warped
	// forward and encoded on the commits the application produced nothing for. The
	// headset has no motion fields of its own to go by in that mode, so this flag is the
	// only thing that can tell its Transport page whether anything is happening.
	bool server_warping;

	// Bit per video stream index, set when that stream failed over to the software
	// encoder. Sticky for the session, like the failover itself. A clear bit means
	// hardware, which is also what a stream that does not exist reads as — the headset
	// only draws a row for a stream it has a decoder for.
	uint8_t software_encoders;

	// Emergency half-rate mode is engaged: the automatic bitrate ran out of room at the
	// floor and the stream framerate has been halved to keep the link alive. The last
	// automatic rung before a disconnect.
	bool emergency_framerate;
};

struct application_list
{
	std::string language;
	std::string country;
	std::string variant;
	struct application
	{
		std::string id;
		std::string name;
	};
	std::vector<application> applications;
};

struct application_icon
{
	std::string id;
	std::vector<std::byte> image; // In PNG
};

struct running_applications
{
	struct application
	{
		std::string name;
		uint32_t id;
		bool overlay;
		bool active;
	};
	std::vector<application> applications;
};

// One NX Warp transport datagram, verbatim.
//
// The codec's unit is a tile run with its own header, not a slice of an opaque
// byte stream, so it cannot ride video_stream_data_shard: SendData cuts a frame
// into 1400-byte pieces and the shard accumulator reassembles them in order,
// which would destroy the run boundaries the receiver needs and impose an
// ordering the codec explicitly does not want (late tiles are still useful).
// This packet is therefore the whole datagram — nxt header, ciphertext, tag —
// handed to the socket as one write, exactly as nx-warp/docs/TRANSPORT.md
// assumes. The nxt::StreamConfig mtu is set so that this envelope still fits in
// one UDP datagram.
// Reserved path ids on nxwarp_datagram. A real nxt path id is 0 (primary) or 1
// (secondary); the values below are not paths at all, they are how the two ends carry
// the small amount of per-stream control that must not be lost, on the control socket,
// without a packet type of its own and without a byte on the video path.
//
// Both ends read these constants, so they cannot drift.
//
//   0xFF  the codec's raw stream header. Not an nxt datagram: it has no header the
//         receiver could parse, and the client hands it straight to
//         nxvc_decoder_parse_stream_header.
//   0xFE  a resync notice. The payload is a little-endian uint16 frame id, and the
//         claim is "that frame needs no reference". The encoder sends one whenever it
//         feeds the codec an all-zero receipt map, which is the documented way to make
//         every tile of the next frame INTRA. It is the answer to
//         from_headset::nxwarp_frame_not_held, and it is the half only the encoder can
//         state: a headset knows when its reference chain broke, but the per-tile
//         ref_delta on the wire rides the chunk mapping and cannot tell it when the
//         chain is whole again.
inline constexpr uint8_t nxwarp_stream_header_path = 0xFF;
inline constexpr uint8_t nxwarp_resync_path = 0xFE;

struct nxwarp_datagram
{
	// Same stream numbering as video_stream_data_shard
	uint8_t stream_item_idx;
	// nxt path id the sender striped this datagram onto (0 primary, 1 secondary)
	uint8_t path_id;

	// The pose and projection the frame was rendered for, exactly as
	// video_stream_data_shard::view_info_t carries it: display time, per-eye pose
	// and fov, the foveation runs, the alpha flag.
	//
	// Present on the first datagram of a frame and on no other, which is the same
	// rule the shard path follows. It is not derivable from anything else on this
	// wire: the codec's own 26-byte pose header (nxt::PoseHeader) is quantised,
	// opaque to the transport, and carries neither fov nor foveation, so without
	// this field the headset can decode a frame but not reproject it.
	//
	// It rides the *first* datagram rather than the control socket because it must
	// arrive with its frame and no later: a pose that overtakes or trails its
	// picture is worse than no pose. A frame whose first datagram is lost is
	// already undecodable under the chunk mapping (see nxwarp_packetize.h), so this
	// field is never the only casualty of that loss.
	std::optional<video_stream_data_shard::view_info_t> view_info;

	// The server's own timestamps for this frame, in the HEADSET clock, on the
	// LAST datagram of the frame and on no other -- the same rule
	// video_stream_data_shard follows, and the same struct, so the client returns
	// them in from_headset::feedback exactly as it does for H.264 and the
	// dashboard's latency plot works for nxwarp without knowing it is nxwarp.
	//
	// Without it every server-side field of the feedback was zero for this codec,
	// and `encode_begin` is the ORIGIN the dashboard normalises the whole plot
	// against, so nxwarp's latency readout was not merely missing two segments --
	// it was measured from zero.
	std::optional<video_stream_data_shard::timing_info_t> timing_info;

	std::vector<uint8_t> payload;
};

using packets = std::variant<
        crypto_handshake,
        pin_check_2,
        pin_check_4,
        handshake,
        attach_path_response,
        path_pong,
        server_message,
        audio_stream_description,
        video_stream_description,
        audio_data,
        video_stream_data_shard,
        video_stream_parity_shard,
        nxwarp_datagram,
        motion_field,
        haptics,
        timesync_query,
        tracking_control,
        feature_control,
        refresh_rate_change,
        stream_tab_change,
        transport_status,
        application_list,
        application_icon,
        running_applications>;
} // namespace to_headset
} // namespace wivrn

template <>
struct magic_enum::customize::enum_range<wivrn::from_headset::body_part_mask>
{
	static constexpr int min = 0;
	static constexpr int max = static_cast<int>(wivrn::from_headset::body_part_mask::max) - 1;
};
