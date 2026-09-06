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

#include "app_launcher.h"
#include "audio/audio.h"
#include "constants.h"
#include "decoder/dejitter.h"
#include "decoder/shard_accumulator.h"
#include "render/imgui_impl.h"
#include "scene.h"
#include "scenes/input_profile.h"
#include "secondary_path.h"
#include "stream_defoveator.h"
#include "stream_jit.h"
#include "stream_quad_blitter.h"
#include "utils/frame_ring.h"
#include "utils/thread_safe.h"
#include "wifi_lock.h"
#include "wivrn_client.h"
#include "wivrn_packets.h"
#include "xr/space.h"
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <variant>
#include <vulkan/vulkan_core.h>

namespace scenes
{
class stream : public scene_impl<stream>, public std::enable_shared_from_this<stream>
{
public:
	enum class state
	{
		initializing,
		streaming,
		// Seamless reconnect (config.seamless_reconnect): a network error tore the
		// primary path down, the stream scene is held alive with the last frame frozen
		// and the network thread is re-handshaking in the background. The 1 s output
		// watchdog is suppressed in this state.
		reconnecting,
		stalled,
		shutdown,
	};
	static const size_t image_buffer_size = 3;

	// Enough to rebuild a session to the same server from the network thread, without
	// going back to the lobby: the address that actually connected, the port, and the
	// transport flags. The headset keypair is reloaded from disk at reconnect time, so
	// nothing sensitive is copied into the scene. Built by the lobby at connection time.
	struct reconnect_info
	{
		std::variant<in_addr, in6_addr> address;
		int port = 0;
		bool tcp_only = false;
		std::string pin;
	};

	app_launcher apps;

private:
	static const size_t view_count = 2;
	// left, right, alpha, and the promoted quad layer
	static const size_t decoder_count = view_count + 2;

	// --- What each stream item IS (stream_role) ----------------------------
	// The client used to read a stream's compositing role out of its index: 0 and 1 are
	// the eyes, 2 is the alpha plane, 3 is the promoted quad layer. The server now says
	// the role on the wire, because the hybrid base layer breaks that rule -- it is an
	// atlas patch source, it is never composited on its own, and it lives in the stream-1
	// slot the NX Warp eye pairing vacates.
	//
	// Cached here, once per description, rather than asked of the description at each of
	// the dozen sites that need it: those sites are on the render thread's per-frame path,
	// video_stream_description is an optional every one of them would have to check first,
	// and one array lookup reads better than a call through it. Written by setup() under
	// decoder_mutex, exactly where the description itself is written.
	//
	// The initial value is the historical positional rule, which is also the wire default.
	// So every path before the first description behaves as it always has, and so does
	// every session against a server that never sets a role.
	std::array<wivrn::stream_role, decoder_count> stream_roles = {
	        wivrn::stream_role::view,
	        wivrn::stream_role::view,
	        wivrn::stream_role::alpha,
	        wivrn::stream_role::quad,
	};
	wivrn::stream_role stream_role_of(size_t stream_index) const
	{
		return stream_index < stream_roles.size()
		               ? stream_roles[stream_index]
		               : wivrn::stream_role::view;
	}
	// A picture that is composited into an eye. The only streams the reprojection pass,
	// the projection layer and the "views ready" gate may ever look at.
	//
	// Stream 0 is a view in every session there is, and a handful of places still say so
	// by writing 0: the de-jitter sample, the "is this a new frame" test, the motion field
	// and the frame smoothing age all mean "the stream the frame cadence comes from". Only
	// the slot stream 1 occupies is negotiable -- the eye pairing empties it and the base
	// layer moves in -- so there is no reading of the description that puts something
	// other than a view at 0.
	bool is_view(size_t stream_index) const
	{
		return stream_role_of(stream_index) == wivrn::stream_role::view;
	}
	bool is_alpha(size_t stream_index) const
	{
		return stream_role_of(stream_index) == wivrn::stream_role::alpha;
	}
	// The hybrid base layer: decoded like any other stream, then handed to the atlas and
	// never presented. See handle_base_frame(), which is the only consumer it has.
	bool is_base(size_t stream_index) const
	{
		return stream_role_of(stream_index) == wivrn::stream_role::base;
	}

	// Index of the promoted quad layer stream, or decoder_count when this session has
	// none. Found by role in setup(): 3 is its default, no longer its definition.
	size_t quad_stream_idx = 3;
	bool has_quad_stream() const
	{
		return quad_stream_idx < decoder_count;
	}

	struct accumulator_images
	{
		std::unique_ptr<wivrn::shard_accumulator> decoder;
		// latest frames, rolling buffer
		std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, image_buffer_size> latest_frames;

		std::shared_ptr<wivrn::shard_accumulator::blit_handle> frame(uint64_t id) const;
		// The newest frame the rolling buffer still holds that is older than `before`,
		// or null. Frame smoothing reads it and nothing else does: it deliberately has
		// no reference of its own to fall back on, so it can only ever blend with a
		// frame the scene was keeping anyway.
		std::shared_ptr<wivrn::shard_accumulator::blit_handle> previous_frame(uint64_t before) const;
		bool empty() const;
	};

	wifi_lock::wifi wifi;

	// True when stream item 0 carries BOTH eyes in one picture and stream item 1 will
	// therefore never produce a frame. Only an NX Warp (nxvc) stream can be like that,
	// and it says so in its own .nxv stream header rather than anywhere on the WiVRn
	// wire -- see nxwarp_decoder::eye_count().
	//
	// The header arrives on the network thread some time after the decoders are built,
	// so this answers false first and true later, and every gate that consults it must
	// be correct in that order: false means "the two eye streams are independent", which
	// is what the client has always assumed and is exactly right for the window before
	// the header, because stream 0 has produced nothing yet either.
	//
	// Callers must hold decoder_mutex.
	bool eyes_in_one_stream() const;

	// True when every stream that carries a VIEW and will ever produce a frame has
	// produced one -- the gate that lets the scene call itself streaming and lets
	// render() draw. The alpha plane, the quad layer and the base layer are deliberately
	// not part of it: none of them is a picture the headset is waiting on to show
	// anything, and a base stream in particular would hold the scene in
	// state::initializing for the whole session if it were.
	//
	// Callers must hold decoder_mutex.
	bool views_ready() const;

	// The one place a decoded base layer frame arrives. See stream.cpp.
	void handle_base_frame(uint8_t stream_index, const std::shared_ptr<wivrn::shard_accumulator::blit_handle> & handle);

	// The newest frame whose decode has FINISHED, as a few plain numbers, published by
	// the decode threads and read by the render thread without a lock.
	//
	// The render thread needs to know what is ready before it decides how long to sleep
	// -- that is the just-in-time schedule's whole input -- and it needs to know it
	// without waiting on anything the decoders hold. frames_mutex is a short critical
	// section, but "short" is a property of today's code and the render loop should not
	// depend on it; this carries no lock and no fence at all.
	//
	// Deliberately NOT the frames themselves. A sequence lock copies a slot
	// speculatively and discards it if it was being written, which is only safe for
	// plain data: a shared_ptr copied mid-write would touch a reference count before
	// the check could say the copy was bad. The handles keep travelling under
	// frames_mutex; see latest_complete_ring's own note.
	struct latest_complete
	{
		uint64_t frame_id = 0;
		// The display time the server stamped on it, which is what the pose age and
		// the just-in-time schedule are both measured against.
		XrTime display_time = 0;
		// When this client finished decoding it, on the same clock as the render
		// loop's own timestamps.
		XrTime completed_at = 0;
	};
	wivrn::latest_complete_ring<latest_complete, 4> complete_ring;
	// The last thing the ring gave the render thread. Render-thread only, so no
	// synchronisation of their own: they exist so the statistics and the just-in-time
	// schedule can talk about the frame that was actually ready, rather than about the
	// one the choice below happened to settle on.
	uint64_t latest_complete_frame_id = 0;
	XrTime latest_complete_display_time = 0;

	// for frames inside accumulator images
	std::mutex frames_mutex;
	std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, decoder_count> common_frame(XrTime display_time);

	// Adaptive playout delay (config.dejitter). Fed one sample per arriving eye-zero frame
	// from the decoder thread, under frames_mutex; configured once per refresh from the
	// render thread; read by both and by the Transport page. Its delay is zero whenever the
	// setting is off, which is what makes common_frame's choice of frame identical to what
	// it was before the buffer existed.
	wivrn::dejitter_buffer dejitter;

	std::unique_ptr<wivrn_session> network_session;
	std::thread network_thread;

	// --- Seamless reconnect (config.seamless_reconnect) -------------------------------
	std::optional<reconnect_info> reconnect_target;
	// The display refresh guess passed at creation, reused when re-sending the headset
	// info on reconnect (only matters on headsets that cannot enumerate refresh rates).
	float guessed_fps = 60;
	// Set by the user (Reconnecting overlay) to abandon a reconnect in progress.
	std::atomic<bool> reconnect_cancelled = false;

	// Declared after network_session so that it is stopped before the session is
	// destroyed
	std::optional<secondary_path_manager> path_manager;
	std::atomic<int64_t> secondary_rtt_ns = 0;
	std::chrono::steady_clock::time_point secondary_rtt_next_log{};
	std::atomic<int64_t> primary_rtt_ns = 0;
	std::chrono::steady_clock::time_point primary_rtt_next_log{};

	thread_safe<to_headset::tracking_control> tracking_control{};
	std::array<std::atomic<interaction_profile>, 3> interaction_profiles; // left hand, right hand, gamepad
	std::atomic<bool> interaction_profile_changed = false;
	std::atomic<XrTime> scheduled_derived_pose = 0; // Tracking thread will compute derived pose when time is reached
	std::atomic<bool> recenter_requested = false;
	std::atomic<bool> hid_forwarding = false;
	std::atomic<XrDuration> display_time_phase = 0;
	std::atomic<XrDuration> display_time_period = 0;
	XrTime last_display_time = 0;
	std::atomic<XrDuration> real_display_period = 0;

	// Just-in-time display scheduling. Touched only from render(), which is one
	// thread; the two atomics below are the parts the GUI thread reads.
	wivrn::jit_scheduler jit;
	// How stale the picture on the panel is, in nanoseconds: the predicted display
	// time of the refresh this pass is drawing for, minus the display time the server
	// stamped on the decoded frame the pass chose. This is the client's own
	// motion-to-photon estimate for the part of the path it can see, and it is the
	// number just-in-time scheduling exists to move.
	std::atomic<XrDuration> displayed_pose_age = 0;
	// Nanoseconds the last iteration slept before starting the pass.
	std::atomic<XrDuration> jit_sleep = 0;
	std::optional<std::thread> tracking_thread;

	std::shared_mutex decoder_mutex;
	std::optional<to_headset::video_stream_description> video_stream_description;
	std::array<accumulator_images, decoder_count> decoders; // Locked by decoder_mutex

	std::optional<stream_defoveator> defoveator;

	// Promoted quad layer: its own swapchain, sized to whatever the server is
	// encoding, and the pass that copies the decoded picture into it. Both stay
	// null while no quad layer is being streamed.
	xr::swapchain quad_swapchain;
	std::optional<stream_quad_blitter> quad_blitter;
	void setup_quad_swapchain(vk::Sampler);

	vk::raii::Fence fence = nullptr;
	vk::raii::CommandBuffer command_buffer = nullptr;

	struct haptics_action
	{
		XrAction action;
		XrPath path;
		float amplitude;
	};
	std::unordered_multimap<device_id, haptics_action> haptics_actions;
	std::vector<std::tuple<device_id, XrAction, XrActionType>> input_actions;

	std::atomic<state> state_ = state::initializing;

	void set_state(state new_state)
	{
		state prev = state_;
		if (prev == state::shutdown)
			return;

		state_.compare_exchange_strong(prev, new_state);
	}

	xr::swapchain swapchain;

	// Vsync caching (config.reduce_gpu_load). Everything the defoveation pass reads that
	// can change between refreshes, captured from the last real render; a refresh whose
	// state matches this re-presents the swapchain image already in it instead of drawing
	// again. A missed field here would cache a stale image, so the set is deliberately
	// broad and quad frames are never cached.
	struct defoveate_state
	{
		// frame_index of every decoder handle in play, sentinel for an absent one
		std::array<uint64_t, decoder_count> frame_index{};
		// Defoveated size per view, derived from foveation (redundant with frame_index,
		// kept as a cheap direct guard)
		std::array<int32_t, 2 * view_count> extents{};
		bool use_alpha = false;
		// Dimming scale/bias and the post-processing amounts folded into the pass
		float scale = 1;
		float bias = 0;
		float sharpness = 0;
		bool cas_full = false;
		// Whether FSR (EASU + RCAS) is the sampling path this refresh; sharpness above then
		// carries the FSR/RCAS strength instead of the CAS strength.
		bool fsr = false;
		float vignette = 0;
		float glow = 0;
		float deband = 0;
		// Low poly region filter: its strength and posterise levels. Part of the
		// signature so the reduce_gpu_load cache cannot re-present an image filtered
		// with the settings the user just changed away from.
		float low_poly = 0;
		float low_poly_levels = 0;
		bool low_poly_full = false;
		// Motion smoothing: whether it warps this refresh, how far, along which field
		bool motion_on = false;
		float motion_step = 0;
		uint64_t motion_frame = uint64_t(-1);
		// Frame smoothing: how much of the previous decoded frame is mixed in. Only ever
		// non-zero on the refresh that first shows a new frame, which already differs by
		// frame_index, but it belongs in the signature all the same.
		float frame_blend = 0;
		// GUI state that changes what the pass draws (through dimming) or whether the
		// gate forces a render
		bool gui_interactable = false;
		int gui_status = -1;

		bool operator==(const defoveate_state &) const = default;
	};
	// Signature of the image currently held in the reprojection swapchain, and whether
	// one has ever been produced. Invalidated whenever the swapchain is (re)created.
	defoveate_state defoveate_cache;
	bool defoveate_cache_valid = false;

	std::optional<audio> audio_handle;

	std::optional<xr::hand_tracker> left_hand;
	std::optional<xr::hand_tracker> right_hand;
	std::optional<input_profile> input;
	static inline const uint32_t layer_controllers = 1 << 0;
	static inline const uint32_t layer_rays = 1 << 1;

	// Size of the composition layer used for the controllers
	uint32_t width;
	uint32_t height;

	std::optional<imgui_context> imgui_ctx;
	ImTextureID wivrn_logo = 0; // wordmark logo shown in the top bar, like the lobby
	struct gui_toast
	{
		std::string content;
		bool is_urgent = false;
	};

	static bool is_interactable(stream_tab);
	bool is_gui_interactable() const;
	// configuration::defoveate_scale resolved for this frame: the configured value
	// clamped, or, at the default of 0, the scale that makes the pass's output equal
	// the stream's own per-eye size. See that field.
	//
	// Computed once per frame and remembered, because the swapchain, the layer rect
	// and the pass's own viewport must all be sized with the SAME number or the
	// picture is cropped instead of scaled -- and at auto the number depends on the
	// decoded frames in hand, which change under it.
	float defoveate_scale_ = 1.0f;
	float resolve_defoveate_scale(const std::array<wivrn::to_headset::foveation_parameter, view_count> & foveation) const;

	// settings sub-page, client-only: the wire stream_tab stays settings
	enum class settings_page
	{
		video,
		audio,
		streaming,
		post_processing,
		devices,
		tracking,
		theme,
		system,
	};
	settings_page current_settings_page = settings_page::video;

	// Application frame cadence, used by the comfort vignette: low pass filtered ratio of
	// newly decoded frames to displayed frames, 1 when the application keeps up
	float app_frame_ratio = 1;
	bool comfort_vignette_active = false;
	float comfort_vignette_fade = 0;

	// Motion smoothing: the last field the server sent. A field is larger than a
	// datagram, so it arrives as several chunks that are gathered here; only a field
	// every chunk of which has arrived is usable. It is only used for the frame it
	// names, so a lost or late chunk just means no smoothing until the next
	// application frame.
	thread_safe<wivrn::motion_field_assembler> motion_field;
	// When a complete field last arrived, and how many have. Only the Transport page reads
	// them; the warp itself works off the assembler.
	std::atomic<XrTime> motion_field_last = 0;
	std::atomic<uint64_t> motion_field_count = 0;

	// --- Transport page ---------------------------------------------------------------
	// Server side transport state, refreshed at wivrn::transport_status_interval while the
	// page holds a subscription and left alone afterwards; transport_status_received dates
	// it so the page can say "no answer" rather than show numbers that stopped moving.
	thread_safe<std::optional<to_headset::transport_status>> transport_status;
	std::atomic<XrTime> transport_status_received = 0;
	// When the subscription is next renewed. Render thread only, 0 while not subscribed.
	XrTime transport_status_next_req = 0;

	// Frames that never reached the decoder, which is exactly what makes the server force
	// an IDR: the headset never asks for one, it reports the hole and the server decides.
	std::atomic<uint64_t> incomplete_frames = 0;

	// Last reading of the headset's own Wi-Fi radio. The server keeps a trend of its own to
	// drive the bitrate; this one is local because a trend that has crossed the link is a
	// trend about the past, and because the page must still work with the radio-aware
	// bitrate switched off.
	std::atomic<bool> radio_valid = false;
	std::atomic<int32_t> radio_rssi_dbm = 0;
	std::atomic<int32_t> radio_link_speed_mbps = 0;
	// Fast and slow exponential averages of the RSSI; their difference is the direction the
	// signal is moving, which is all an arrow needs.
	std::atomic<float> radio_rssi_fast = 0;
	std::atomic<float> radio_rssi_slow = 0;
	// Fold one radio reading in. Tracking thread.
	void on_wifi_sample(bool valid, int rssi_dbm, int link_speed_mbps);

	// Tab currently being displayed
	stream_tab gui_status = stream_tab::hidden;
	// Tab that we will switch to if button is pressed
	stream_tab stored_gui_status = stream_tab::applications;
	// Tab that will be displayed on next render()
	std::atomic<stream_tab> next_gui_status = stream_tab::hidden;
	float dimming = 0;

	thread_safe<std::optional<gui_toast>> gui_toast;
	std::atomic<XrTime> gui_status_last_change;

	thread_safe<std::queue<std::string>> stream_error_queue;

	XrAction plots_toggle_1 = XR_NULL_HANDLE;
	XrAction plots_toggle_2 = XR_NULL_HANDLE;
	XrAction recenter_left = XR_NULL_HANDLE;
	XrAction recenter_right = XR_NULL_HANDLE;
	XrAction gui_distance_left = XR_NULL_HANDLE;
	XrAction gui_distance_right = XR_NULL_HANDLE;
	XrAction settings_adjust = XR_NULL_HANDLE;
	XrAction foveation_distance = XR_NULL_HANDLE;
	XrAction foveation_ok = XR_NULL_HANDLE;
	XrAction foveation_cancel = XR_NULL_HANDLE;

	// Position of the GUI relative to the view space, in view space axes, used when the GUI is not interactable
	glm::vec3 head_gui_position{-0.1, -0.3, -1.2}; // Shift 10cm left by default so that the stats are centered accounting for the tab list
	glm::quat head_gui_orientation{1, 0, 0, 0};

	// Position of the GUI relative to the world space, in world space axes, used when the GUI is interactable
	glm::vec3 world_gui_position;
	glm::quat world_gui_orientation;

	bool override_foveation_enable;
	float override_foveation_pitch; // The pitch is the opposite as the height displayed in the GUI
	float override_foveation_distance;

	// Which controller is used for recentering and position of the GUI relative to the controller, in controller axes, during recentering
	std::optional<std::tuple<xr::spaces, glm::vec3, glm::quat>> recentering_context;
	void update_gui_position(xr::spaces controller, float predicted_display_period);

	// Keep a reference to the resources needed to blit the images until vkWaitForFences
	std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, decoder_count> current_blit_handles;

	// --- Frame smoothing (config.frame_smoothing) --------------------------------------
	// The eye images the pass is sampling as the previous frame, pinned for exactly one
	// render: from the moment the pass is recorded to the fence wait at the top of the
	// next render(), after which the GPU is done with them.
	//
	// This is the ONLY reference frame smoothing ever takes, and it is always a frame the
	// decoders' rolling buffer is holding anyway. An earlier version kept a reference of
	// its own to the frame on screen, which pinned one more image than the NX Warp
	// decoder's five-image pool could spare: get_free() started returning null, the
	// decoder discarded frames, the picture went black, and the one-second no-output
	// watchdog then dropped the whole scene to the lobby.
	std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, view_count> smoothing_blend_handles;
	// frame_index of the decoded frame currently on screen, so the render thread can tell
	// the refresh that first shows a new one — the only refresh that blends — from the
	// repeats after it. Sentinel until the first frame, and reset to it whenever the
	// stream has no frame at all, so nothing is ever carried across a gap.
	uint64_t smoothing_frame_index = uint64_t(-1);

	// --- Frame rate readout (Statistics page, compact view) ----------------------------
	// Monotonic counters, differenced every constants::stream::fps_sample_period into the
	// rolling rates the GUI shows. displayed_frames is bumped by the render thread once
	// per presented frame; decoded_frames by push_blit_handle, from the decoder threads.
	uint64_t displayed_frames = 0;
	std::array<std::atomic<uint64_t>, decoder_count> decoded_frames{};
	// Bumped at the TOP of render(), before every gate: the count of times the loop
	// entered this scene's frame, and the predicted display periods summed over them.
	// displayed_frames counts the subset that reached a submission, so the two together
	// separate "the loop is slow" from "the loop runs and submits nothing" -- which is a
	// distinction the shown/decoded pair alone cannot make.
	uint64_t render_iterations = 0;
	uint64_t render_period_ns = 0;
	// Displayed pose age, summed over the iterations that had a frame to show, and
	// the count of those. Same arithmetic as the period above: the mean over a window
	// is the difference of the sums over the difference of the counts.
	uint64_t pose_age_ns = 0;
	uint64_t pose_age_frames = 0;

	// One second of rates, recomputed four times a second so the figures are readable
	// rather than flickering. All render-thread state.
	struct fps_readout
	{
		float displayed = 0;
		std::array<float, view_count> decoded{};
		// NX Warp only: what its own counters say, per second, plus the last
		// two-second decode profile. nxwarp is false for every other codec and the
		// second line is then not drawn at all.
		bool nxwarp = false;
		float nxwarp_closed = 0;
		float nxwarp_decoded = 0;
		float nxwarp_late = 0;
		float nxwarp_holes = 0;
		float nxwarp_ms = 0;
		// --- the rest of the NX Warp block ---------------------------------------
		// Rates, like the four above.
		float nxwarp_withheld = 0;
		// Straight from the decoder's last two-second window, taken as they are: the
		// GPU decode and its two halves (pass B is the one that scales with the pixel
		// count), the frame size, and the interval frames are arriving at.
		float nxwarp_gpu_ms = 0;
		float nxwarp_pass_a_ms = 0;
		// The ENVELOPE. It contains Pass W and all three reconstruction segments, so
		// the parts below are what it is made of and not extra work beside it.
		float nxwarp_pass_b_ms = 0;
		float nxwarp_pass_w_ms = 0;
		float nxwarp_pass_b_skip_ms = 0;
		float nxwarp_pass_b_coded_ms = 0;
		float nxwarp_pass_b_dir_ms = 0;
		float nxwarp_tiles_skip = 0;
		float nxwarp_tiles_coded = 0;
		float nxwarp_tiles_dir = 0;
		bool nxwarp_pass_segments = false;
		float nxwarp_bytes = 0;
		float nxwarp_arrival_ms = 0;
		// Fixed for the stream: how many arriving frames one decode takes, the size
		// being encoded, and which entropy coder the server settled on.
		uint32_t nxwarp_stride = 1;
		uint32_t nxwarp_width = 0;
		uint32_t nxwarp_height = 0;
		bool nxwarp_entropy_lite = false;
		// Both eyes coded as one stereo frame on stream 0, from the stream description.
		// Stream 1 then has no decoder and its counters never move, so the readout must
		// not list it: a permanent "/0" reads as a dead eye, not an absent stream.
		bool nxwarp_paired = false;
		// Transport tiles the last closed frame carried, and the grid's total. The HUD
		// turns the pair into "spans" or "chunks": see stream_gui.cpp.
		uint32_t nxwarp_frame_tiles = 0;
		uint32_t nxwarp_grid_tiles = 0;
		// Not a codec figure at all: how often render() is entered, and the mean
		// interval the runtime is predicting between displayed frames. These are the
		// two numbers the "render: N iterations in ... display period ..." log line
		// carries, and on this device they are the only source left for them -- the
		// compositor's own PxrMetric output has gone silent.
		//
		// The pair is what makes a low frame rate readable: 33 shown out of a loop
		// running 90 times a second is a submission problem, 33 shown out of a loop
		// running 33 times is the loop itself being late, and those have nothing in
		// common. loop_rate counts EVERY entry into render(), including the ones that
		// return early with nothing decoded and the ones the repeat gate skips.
		float loop_rate = 0;
		float display_period_ms = 0;
		// How old the pose on the panel is: the refresh a pass was drawn for, minus
		// the display time the server stamped on the image it chose. This is the
		// client's own half of motion-to-photon, and the only source for it on this
		// device now that the compositor's PxrMetric output is silent. Zero means no
		// frame in the window carried one.
		float pose_age_ms = 0;
	};
	fps_readout fps;
	// One snapshot of every counter the readout differences, with the time it was taken.
	struct fps_counters
	{
		XrTime t = 0;
		uint64_t displayed = 0;
		std::array<uint64_t, view_count> decoded{};
		uint64_t nxwarp_closed = 0, nxwarp_decoded = 0, nxwarp_late = 0, nxwarp_holes = 0;
		uint64_t nxwarp_withheld = 0;
		// render() entries and the predicted display periods summed over them. The mean
		// period is the difference of the sums over the difference of the counts, which
		// is the same arithmetic the log line does over its own two-second window.
		uint64_t iterations = 0;
		uint64_t period_ns = 0;
		// See render_iterations/pose_age_ns above.
		uint64_t pose_age_ns = 0;
		uint64_t pose_age_frames = 0;
	};
	// Snapshots taken every fps_sample_period; the rate is the difference between the
	// newest and the one fps_window old, which is what makes the average roll rather
	// than restart every period. One more slot than the window holds samples.
	static constexpr size_t fps_ring_size = 1 + size_t(constants::stream::fps_window / constants::stream::fps_sample_period);
	std::array<fps_counters, fps_ring_size> fps_ring{};
	size_t fps_ring_head = 0;
	size_t fps_ring_count = 0;
	XrTime fps_last_sample = 0;
	// Fold one window into `fps`. Called from accumulate_metrics, render thread.
	void accumulate_fps(XrTime now);
	// The short lines that go under the latency figure. Empty strings are not drawn.
	//
	// Built once per sample window by accumulate_fps and cached here, NOT formatted per
	// frame: both call sites draw them every frame, and the numbers in them only move when
	// the decoder's two-second profile window turns over.
	static constexpr size_t fps_line_count = 7;
	std::array<std::string, fps_line_count> fps_line_cache{};
	// The decoder window sequence last reported to the server, per stream. Zero is the
	// value a decoder that has published nothing reports, so nothing is sent until a
	// real window exists.
	std::array<uint64_t, decoder_count> nxwarp_profile_seq{};
	void rebuild_fps_lines();
	const std::array<std::string, fps_line_count> & fps_lines() const
	{
		return fps_line_cache;
	}
	// Draws the cached block. Both the full and the compact view call it.
	void draw_fps_lines();
	// Colour for one rate against the panel: muted at or above the refresh rate, warning
	// below half of it, the plain text colour in between.
	ImVec4 fps_colour(float rate) const;
	// Panel refresh rate, from the runtime's own frame cadence. 0 until it is known.
	float panel_refresh_rate() const;

	XrTime running_application_req = 0;
	thread_safe<to_headset::running_applications> running_applications;

	stream(std::string server_name, scene & parent_scene);

	bool forward_hid_input(from_headset::hid::input_t, bool device_enabled);

public:
	~stream();

	static std::shared_ptr<stream> create(
	        std::unique_ptr<wivrn_session> session,
	        float guessed_fps,
	        std::string server_name,
	        scene & parent_scene,
	        std::optional<reconnect_info> reconnect_target = std::nullopt);

	void render(const XrFrameState &) override;
	// Whether just-in-time display scheduling runs; see the definition in stream.cpp.
	static bool jit_enabled();
	// Whether the re-present cache is on; see the definition in stream.cpp.
	static bool reduce_gpu_load_enabled();
	void on_focused() override;
	void on_unfocused() override;
	void on_xr_event(const xr::event &) override;

	bool on_input_key_down(uint8_t key_code) override;
	bool on_input_key_up(uint8_t key_code) override;
	bool on_input_mouse_move(float x, float y) override;
	bool on_input_button_down(uint8_t button) override;
	bool on_input_button_up(uint8_t button) override;
	bool on_input_scroll(float h, float v) override;

	void operator()(to_headset::crypto_handshake &&) {};
	void operator()(to_headset::pin_check_2 &&) {};
	void operator()(to_headset::pin_check_4 &&) {};
	void operator()(to_headset::handshake &&) {};
	// Handled by the path manager during the attach handshake
	void operator()(to_headset::attach_path_response &&) {};
	void operator()(to_headset::path_pong &&);
	void operator()(to_headset::server_message &&);
	void operator()(to_headset::video_stream_data_shard &&);
	void operator()(to_headset::video_stream_parity_shard &&);
	void operator()(to_headset::nxwarp_datagram &&);
	void operator()(to_headset::motion_field &&);
	void operator()(to_headset::haptics &&);
	void operator()(to_headset::timesync_query &&);
	void operator()(to_headset::tracking_control &&);
	void operator()(to_headset::feature_control &&);
	void operator()(to_headset::audio_stream_description &&);
	void operator()(to_headset::video_stream_description &&);
	void operator()(to_headset::refresh_rate_change &&);
	void operator()(to_headset::stream_tab_change &&);
	void operator()(to_headset::transport_status &&);
	void operator()(to_headset::application_list &&);
	void operator()(to_headset::application_icon &&);
	void operator()(to_headset::running_applications &&);
	void operator()(audio_data &&);

	void push_blit_handle(wivrn::shard_accumulator * decoder, std::shared_ptr<wivrn::shard_accumulator::blit_handle> handle);

	void send_feedback(const wivrn::from_headset::feedback & feedback);
	// Hands one already-encoded NX Warp feedback packet to the lossy socket. Called from
	// the decoder's network-thread path once per band deadline.
	void send_nxwarp_feedback(uint8_t stream_index, uint8_t path_id, std::vector<uint8_t> payload,
	                          uint16_t decode_us, uint16_t held_base, uint32_t held_mask);
	// Where one eye's GPU decode time went, about twice a second. Reported for the
	// dashboard only -- nothing on the server acts on it -- so a lost one costs a stale
	// card and is not resent.
	void send_nxwarp_decode_profile(wivrn::from_headset::nxwarp_decode_profile p);
	// One frame this headset received and will not reconstruct. Control socket: losing
	// it is the corruption it exists to prevent.
	void send_nxwarp_frame_not_held(uint8_t stream_index, uint16_t frame_id,
	                                wivrn::from_headset::nxwarp_frame_not_held::reason why);

	// Ask the server for video shards this headset never received. On the stream
	// socket, not the control one: a request that misses the frame's display deadline
	// is worth nothing, and head-of-line blocking is what would make it miss.
	void send_nack(const wivrn::from_headset::nack & nack);

	state current_state() const
	{
		return state_;
	}

	// Whether the server mirrors forwarded input devices to uinput. The gamepad is also exposed
	// through OpenXR regardless, so this only affects forwarded keyboard and mouse.
	bool hid_forwarding_enabled() const
	{
		return hid_forwarding;
	}

	void exit();
	void start_application(std::string appid);

	static meta & get_meta_scene();
	std::optional<std::string> pop_stream_error();

private:
	void process_packets();
	// Send the initial batch of control packets the server reads to bring a stream up:
	// the headset info, the session state, the current tab, the visibility masks and the
	// foveation override. The headset info MUST be the first control packet on the socket
	// (the server does std::get<headset_info_packet> on it during the handshake), so on a
	// reconnect this is sent on the freshly handshaken session BEFORE it is adopted, while
	// no other thread can touch it. Called once at creation and again after each seamless
	// reconnect (the server re-reads the headset info when it re-accepts).
	void send_initial_control_packets(wivrn_session & net, float guessed_fps);
	// Called from the network thread when poll() throws. When seamless reconnect is on and
	// this is not a shutdown, holds the scene alive, freezes the last frame, and retries the
	// handshake with backoff for a bounded window. Returns true if the stream was resumed;
	// false means fall back to the old behaviour (drop to the lobby).
	bool try_seamless_reconnect();
	// Build a fresh, fully handshaken session to the same server. Throws or returns nullptr
	// (cancelled) on failure. Network thread only.
	std::unique_ptr<wivrn_session> build_reconnect_session();
	// Bump the held frames' decoder-receipt timestamps to now so the 1 s output watchdog
	// does not fire on the stale held frame before the first fresh frame of the resumed
	// stream is decoded. Taken under frames_mutex.
	void refresh_reconnect_watchdog();
	void tracking();
	void read_actions();

	void on_interaction_profile_changed(const XrEventDataInteractionProfileChanged &);
	void send_derived_pose();

	void setup(const to_headset::video_stream_description &);
	void setup_reprojection_swapchain(uint32_t width, uint32_t height);

	vk::raii::QueryPool query_pool = nullptr;
	bool query_pool_filled = false;

	// Used for plots
	uint64_t bytes_received = 0;
	uint64_t bytes_sent = 0;
	float bandwidth_rx = 0;
	float bandwidth_tx = 0;

	struct gpu_timestamps
	{
		float gpu_time = 0;
	};

	struct global_metric
	{
		float gpu_time;
		float cpu_time = 0;
		float bandwidth_rx = 0;
		float bandwidth_tx = 0;
	};

	struct plot
	{
		std::string title;
		struct subplot
		{
			std::string title;
			float scenes::stream::global_metric::* data;
		};
		std::vector<subplot> subplots;
		const char * unit;
	};

	static const inline int size_gpu_timestamps = 1 + sizeof(gpu_timestamps) / sizeof(float);

	struct decoder_metric
	{
		// All times are in seconds relative to encode_begin
		float encode_begin;
		float encode_end;
		float send_begin;
		float send_end;
		float received_first_packet;
		float received_last_packet;
		float sent_to_decoder;
		float received_from_decoder;
		float blitted;
		float displayed;
		float predicted_display;
	};

	// One sample per constants::stream::transport_sample_period, for the Transport page's
	// plots. Kept apart from global_metrics: those are per displayed frame, these are on a
	// wall clock so that 60 s of history is 60 s whatever the framerate does.
	struct transport_metric
	{
		float video_bps = 0;    // measured, from the bytes the sockets actually took in
		float setpoint_bps = 0; // what the server last said it was encoding at
		float primary_rtt_s = 0;
		float secondary_rtt_s = 0;
		float fec_per_s = 0;       // shards rebuilt from parity
		float conceal_per_min = 0; // audio gaps papered over
	};

	std::vector<global_metric> global_metrics{300};
	std::vector<std::vector<decoder_metric>> decoder_metrics;
	std::vector<float> axis_scale;
	XrTime last_metric_time = 0;
	int metrics_offset = 0;

	std::vector<transport_metric> transport_metrics{constants::stream::transport_history};
	int transport_offset = 0;
	XrTime transport_last_sample = 0;
	uint64_t transport_bytes_received = 0;
	uint64_t transport_reconstructed = 0;
	uint64_t transport_concealments = 0;
	std::vector<float> transport_axis_scale;

	// Used for compact view
	float compact_bandwidth_rx = 0;
	float compact_bandwidth_tx = 0;
	float compact_cpu_time = 0;
	float compact_gpu_time = 0;

	void accumulate_metrics(XrTime predicted_display_time, const std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, decoder_count> & blit_handles, const gpu_timestamps & timestamps);
	// Wall clock sampling behind the Transport page's plots, driven from accumulate_metrics
	void accumulate_transport_metrics(XrTime predicted_display_time);
	void gui_performance_metrics();
	void gui_transport();
	void gui_compact_view();
	void gui_settings(float predicted_display_period);
	void gui_bitrate_settings(float predicted_display_period);
	void gui_foveation_settings(float predicted_display_period);
	void gui_applications();
	void gui_toasts();
	void draw_gui(XrTime predicted_display_time, XrDuration predicted_display_period);
};
} // namespace scenes
