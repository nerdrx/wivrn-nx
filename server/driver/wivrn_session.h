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

#include "app_pacer.h"
#include "bitrate_controller.h"
#include "clock_offset.h"
#include "compositor/compositor.h"
#include "configuration.h"
#include "inplace_vector.hpp"
#include "tracking_control.h"
#include "utils/thread_safe.h"
#include "wivrn_android_face_tracker.h"
#include "wivrn_body_tracker.h"
#include "wivrn_connection.h"
#include "wivrn_controller.h"
#include "wivrn_eye_tracker.h"
#include "wivrn_fb_face2_tracker.h"
#include "wivrn_gamepad.h"
#include "wivrn_generic_tracker.h"
#include "wivrn_hmd.h"
#include "wivrn_htc_face_tracker.h"
#include "wivrn_ipc.h"
#include "wivrn_packets.h"
#include "wivrn_uinput.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_system.h"
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

struct b_system;
struct ipc_server;
struct xrt_space_overseer;
struct xrt_system_compositor;
union xrt_session_event;

namespace wivrn
{
class wivrn_eye_tracker;
class wivrn_android_face_tracker;
class wivrn_fb_face2_tracker;
class wivrn_htc_face_tracker;
class wivrn_body_tracker;
class wivrn_generic_tracker;
struct audio_device;

class wivrn_session : public xrt_system_devices
{
	std::unique_ptr<wivrn_connection> connection;
	from_headset::headset_info_packet headset_info;
	// run-time editable settings
	thread_safe<from_headset::settings_changed> settings;

	wivrn::compositor compositor;
	// Adjusts the bitrate from the per-frame delivery timings reported by the client.
	// Has its own mutex, may be used from any thread.
	wivrn::bitrate_controller bitrate_ctl;
	// Ceiling applied while the secondary (USB) path carries video
	uint32_t multipath_usb_max_bitrate = 0;
	// Primary path capacity latched when the combine posture was entered, 0 otherwise.
	// Read by the encoder sender threads on every frame.
	std::atomic<uint32_t> combine_wifi_share_bps = 0;
	// Video bytes put on each path since the session started, for the Transport page's
	// per-path share. Written by the encoder sender threads, differenced by the worker one.
	std::atomic<uint64_t> path_bytes_primary = 0;
	std::atomic<uint64_t> path_bytes_secondary = 0;
	uint64_t reported_path_bytes_primary = 0;
	uint64_t reported_path_bytes_secondary = 0;
	// Server side half of the packet pacing switch, read from the configuration once
	configuration::pacing_config pacing_conf;
	// Same story for the hardware encoder failover
	bool encoder_failover_conf = true;
	// And for intra refresh loss recovery
	bool intra_refresh_conf = true;
	// And for the reference invalidation rung below it
	bool ref_invalidation_conf = true;
	// And for the emergency half-rate mode (auto framerate drop below the bitrate floor)
	bool emergency_framerate_conf = true;
	pacing_app_factory app_pacers;

	b_system & xrt_system;
	ipc_server * mnd_ipc_server;
	xrt_space_overseer * space_overseer;
	xrt_system_compositor * system_compositor;

	std::mutex roles_mutex;
	xrt_system_roles roles{
	        .generation_id = 1,
	        .left = -1,
	        .right = -1,
	        .gamepad = -1,
	};

	tracking_control control;

	wivrn_hmd hmd;
	wivrn_controller left_controller;
	int32_t left_controller_index;
	wivrn_controller right_controller;
	int32_t right_controller_index;
	wivrn_controller left_hand_interaction;
	int32_t left_hand_interaction_index;
	wivrn_controller right_hand_interaction;
	int32_t right_hand_interaction_index;
	std::optional<wivrn_eye_tracker> eye_tracker;
	std::optional<wivrn_gamepad> gamepad_device;
	std::optional<wivrn_android_face_tracker> android_face_tracker;
	std::optional<wivrn_fb_face2_tracker> fb_face2_tracker;
	std::optional<wivrn_htc_face_tracker> htc_face_tracker;
	std::optional<wivrn_body_tracker> body_tracker;
	beman::inplace_vector::inplace_vector<wivrn_generic_tracker, from_headset::htc_body::max_tracked_poses> generic_trackers;
	std::optional<wivrn_uinput> uinput_handler;
	bool gamepad_connected = false; // network thread only

	clock_offset_estimator offset_est;
	std::atomic<XrDuration> tracking_latency; // production to reception time

	// Transport status feed, for the headset's Transport page. The deadline is a lease
	// rather than a flag: the page renews it while it is open and the feed stops on its
	// own once it lapses, so a headset that goes away mid-session cannot leave it running.
	// Written from the network thread by the subscribe handler, read from the worker
	// thread; steady_clock nanoseconds since the epoch, 0 while nobody is subscribed.
	std::atomic<int64_t> transport_status_until = 0;
	// Worker thread only
	std::chrono::steady_clock::time_point transport_status_next{};

	std::mutex csv_mutex;
	std::ofstream feedback_csv;

	std::unique_ptr<audio_device> audio_handle;

	// when sessions shall be destroyed, key is client id, value is timestamp
	thread_safe<std::map<uint32_t, int64_t>> session_loss;

	std::jthread net_thread;
	std::jthread worker_thread;

	wivrn_session(std::unique_ptr<wivrn_connection> connection, b_system &);

public:
	using base_t = xrt_system_devices;
	~wivrn_session();

	static xrt_result_t create_session(std::unique_ptr<wivrn_connection> connection,
	                                   b_system & system,
	                                   xrt_system_devices ** out_xsysd,
	                                   xrt_space_overseer ** out_xspovrs,
	                                   xrt_system_compositor ** out_xsysc);

	void start(ipc_server *);
	void stop();

	bool request_stop();
	void quit_if_no_client();

	clock_offset get_offset();
	bool connected();
	const from_headset::headset_info_packet & get_info() const
	{
		return headset_info;
	};

	float default_fps();

	locked<from_headset::settings_changed> get_settings()
	{
		return settings.lock();
	}

	// For adaptive foveation: the automatic bitrate controller's current state, read per frame
	// by the compositor to steepen the foveation curve as the link backs off the ceiling. Its
	// own mutex, safe from the present thread.
	wivrn::bitrate_controller::status bitrate_status() const
	{
		return bitrate_ctl.snapshot();
	}
	bool bitrate_auto_active() const
	{
		return bitrate_ctl.enabled();
	}

	wivrn_hmd & get_hmd()
	{
		return hmd;
	}

	void add_tracking_request(device_id, int64_t at_ns, int64_t produced_ns, int64_t now);
	void add_tracking_request(device_id, int64_t at_ns, int64_t produced_ns);

	void operator()(from_headset::crypto_handshake &&) {}
	void operator()(from_headset::pin_check_1 &&) {}
	void operator()(from_headset::pin_check_3 &&) {}
	// Handled by the main loop process, never reaches the session
	void operator()(from_headset::attach_path &&) {}
	void operator()(from_headset::path_ping &&);
	void operator()(from_headset::headset_info_packet &&);
	void operator()(const from_headset::settings_changed &);
	void operator()(from_headset::handshake &&) {}
	void operator()(const from_headset::tracking &);
	void operator()(from_headset::derived_pose &&);
	void operator()(from_headset::hand_tracking &&);
	void operator()(from_headset::meta_body &&);
	void operator()(from_headset::meta_body_skeleton &&);
	void operator()(from_headset::bd_body &&);
	void operator()(from_headset::htc_body &&);
	void operator()(from_headset::inputs &&);
	void operator()(from_headset::hid::input && e);
	void operator()(from_headset::timesync_response &&);
	void operator()(from_headset::feedback &&);
	void operator()(from_headset::nack &&);
	void operator()(from_headset::nxwarp_feedback &&);
	void operator()(from_headset::battery &&);
	void operator()(from_headset::wifi_state &&);
	void operator()(from_headset::visibility_mask_changed &&);
	void operator()(from_headset::session_state_changed &&);
	void operator()(from_headset::user_presence_changed &&);
	void operator()(from_headset::refresh_rate_changed &&);
	void operator()(from_headset::stream_tab_changed &&);
	void operator()(from_headset::transport_status_subscribe &&);
	void operator()(from_headset::override_foveation_center &&);
	void operator()(from_headset::get_application_list &&);
	void operator()(const from_headset::start_app &);
	void operator()(const from_headset::get_running_applications &);
	void operator()(const from_headset::set_active_application &);
	void operator()(const from_headset::stop_application &);
	void operator()(audio_data &&);

	void operator()(to_monado::stop &&);
	void operator()(to_monado::disconnect &&);
	void operator()(to_monado::set_bitrate &&);
	void operator()(to_headset::stream_tab_change &&);

	bool has_stream()
	{
		return connection->has_stream();
	}

	// True while video rides the secondary (TCP) path, in which case there is
	// nothing for the shard pacer to do
	bool video_on_secondary() const
	{
		return connection->video_on_secondary();
	}

	// True while video is striped over both paths (multipath stage 3): the primary
	// keeps everything its pacing window has room for, the tail of every frame
	// goes to the secondary.
	bool video_combining() const
	{
		return connection->combining();
	}

	// Capacity of the primary path, in bits per second, as it was measured while
	// the whole of every frame still rode it. Latched when the combine posture is
	// entered; 0 whenever the posture is not combine. See spill_scheduler.
	uint32_t wifi_share_bps() const
	{
		return combine_wifi_share_bps;
	}

	template <typename T>
	void send_stream(T && packet)
	{
		connection->send_stream(std::forward<T>(packet));
	}

	template <typename T>
	void send_control(T && packet)
	{
		connection->send_control(std::forward<T>(packet));
	}

	// One video shard onto the secondary path while combining. False when it did
	// not go out, in which case the path has just been dropped and the caller
	// must put this shard, and the rest of the frame, on the primary. Never
	// retried on the secondary: see wivrn_connection::send_spill.
	template <typename T>
	bool send_spill(T && packet)
	{
		return connection->send_spill(std::forward<T>(packet));
	}

	xrt_result_t push_event(const xrt_session_event &);

	void set_foveated_size(uint32_t width, uint32_t height);

	void dump_time(const std::string & event, uint64_t frame, int64_t time, uint8_t stream = -1, const char * extra = "");

	// One video frame of one stream finished going out, with the number of bytes it put on
	// the wire (parity shards included). Called from the encoder's send path; only the
	// bandwidth estimating bitrate control law uses it, and it takes no lock of its own.
	void on_frame_sent(uint64_t frame_index, uint8_t stream_index, uint32_t bytes);

	// How one frame's bytes were split between the two paths, for the Transport page.
	// Called from the encoder's send path right after on_frame_sent; two atomic adds.
	void on_frame_paths(uint32_t primary_bytes, uint32_t secondary_bytes);

private:
	void run_net(std::stop_token stop);
	void run_worker(std::stop_token stop);
	void reconnect(std::stop_token stop);

	void pause_session();
	void resume_session();

	// Forwards a bitrate decided by the automatic controller to the encoders
	void apply_auto_bitrate(std::optional<uint32_t>);
	// Push the packet pacing switch down to the encoders and the resulting window to the
	// bitrate controller, which reads it as the shortest a frame can possibly take
	void set_pacing(bool client_enabled);

	// The posture of the path selector changed (multipath failover, or entering or
	// leaving the striping posture). Called from the network thread.
	void on_path_switch(path_selector::posture, std::string_view reason);

	// Assemble and send one to_headset::transport_status, if the subscription lease is
	// still valid. Worker thread.
	void send_transport_status();

	void update_client_states(bool visible, bool focused);
	void poll_session_loss();

	// checks if a headset is usable with this session
	std::pair<bool, std::optional<std::string>> validate_headset_info(const from_headset::headset_info_packet & info);

	// xrt_system implementation
	xrt_result_t get_roles(xrt_system_roles * out_roles);
	xrt_result_t feature_inc(xrt_device_feature_type type);
	xrt_result_t feature_dec(xrt_device_feature_type type);
	void destroy();
};

} // namespace wivrn
