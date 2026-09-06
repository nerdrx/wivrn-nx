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
#include "constants.h"
#include "xr/body_tracker.h"
#include "xr/face_tracker.h"
#include "xr/fb_face_tracker2.h"
#include "xr/space.h"
#include "xr/system.h"
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/quaternion.hpp>
#include <magic_enum.hpp>
#include <openxr/openxr.h>
#define GLM_FORCE_RADIANS

#include "stream.h"

#include "application.h"
#include "audio/audio.h"
#include "boost/pfr/core.hpp"
#include "decoder/decoder.h"
#include "decoder/shard_accumulator.h"
#include "inplace_vector.hpp"
#include "is_finite.h"
#include "spdlog/spdlog.h"
#include "utils/named_thread.h"
#include "utils/ranges.h"
#include "wivrn_packets.h"
#include <algorithm>
#include <mutex>
#include <ranges>
#include <thread>
#include <vulkan/vulkan_raii.hpp>

#include "wivrn_config.h"
#if WIVRN_FEATURE_RENDERDOC
#include "vk/renderdoc.h"
#endif

using namespace wivrn;
using namespace beman::inplace_vector;

// clang-format off
static const std::unordered_map<std::string, device_id> device_ids = {
	{"/user/hand/left/input/x/click",             device_id::X_CLICK},
	{"/user/hand/left/input/x/touch",             device_id::X_TOUCH},
	{"/user/hand/left/input/y/click",             device_id::Y_CLICK},
	{"/user/hand/left/input/y/touch",             device_id::Y_TOUCH},
	{"/user/hand/left/input/menu/click",          device_id::MENU_CLICK},
	{"/user/hand/left/input/squeeze/click",       device_id::LEFT_SQUEEZE_CLICK},
	{"/user/hand/left/input/squeeze/force",       device_id::LEFT_SQUEEZE_FORCE},
	{"/user/hand/left/input/squeeze/value",       device_id::LEFT_SQUEEZE_VALUE},
	{"/user/hand/left/input/trigger/value",       device_id::LEFT_TRIGGER_VALUE},
	{"/user/hand/left/input/trigger/click",       device_id::LEFT_TRIGGER_CLICK},
	{"/user/hand/left/input/trigger/touch",       device_id::LEFT_TRIGGER_TOUCH},
	{"/user/hand/left/input/trigger/proximity",   device_id::LEFT_TRIGGER_PROXIMITY},
	{"/user/hand/left/input/trigger/proximity_fb",device_id::LEFT_TRIGGER_PROXIMITY},
	{"/user/hand/left/input/trigger/proximity_meta",device_id::LEFT_TRIGGER_PROXIMITY},
	{"/user/hand/left/input/trigger/curl_fb",     device_id::LEFT_TRIGGER_CURL},
	{"/user/hand/left/input/trigger/curl_meta",   device_id::LEFT_TRIGGER_CURL},
	{"/user/hand/left/input/trigger_curl/value",  device_id::LEFT_TRIGGER_CURL},
	{"/user/hand/left/input/trigger/slide_fb",    device_id::LEFT_TRIGGER_SLIDE},
	{"/user/hand/left/input/trigger/slide_meta",  device_id::LEFT_TRIGGER_SLIDE},
	{"/user/hand/left/input/trigger_slide/value", device_id::LEFT_TRIGGER_SLIDE},
	{"/user/hand/left/input/trigger/force",       device_id::LEFT_TRIGGER_FORCE},
	{"/user/hand/left/input/thumbstick",          device_id::LEFT_THUMBSTICK_X},
	{"/user/hand/left/input/thumbstick/click",    device_id::LEFT_THUMBSTICK_CLICK},
	{"/user/hand/left/input/thumbstick/touch",    device_id::LEFT_THUMBSTICK_TOUCH},
	{"/user/hand/left/input/thumbrest/touch",     device_id::LEFT_THUMBREST_TOUCH},
	{"/user/hand/left/input/thumbrest/force",     device_id::LEFT_THUMBREST_FORCE},
	{"/user/hand/left/input/thumb_resting_surfaces/proximity",device_id::LEFT_THUMB_PROXIMITY},
	{"/user/hand/left/input/thumb_meta/proximity_meta",device_id::LEFT_THUMB_PROXIMITY},
	{"/user/hand/left/input/trackpad",            device_id::LEFT_TRACKPAD_X},
	{"/user/hand/left/input/trackpad/click",      device_id::LEFT_TRACKPAD_CLICK},
	{"/user/hand/left/input/trackpad/touch",      device_id::LEFT_TRACKPAD_TOUCH},
	{"/user/hand/left/input/trackpad/force",      device_id::LEFT_TRACKPAD_FORCE},
	{"/user/hand/left/input/stylus/force",        device_id::LEFT_STYLUS_FORCE},
	{"/user/hand/left/input/stylus_fb/force",     device_id::LEFT_STYLUS_FORCE},

	{"/user/hand/right/input/a/click",             device_id::A_CLICK},
	{"/user/hand/right/input/a/touch",             device_id::A_TOUCH},
	{"/user/hand/right/input/b/click",             device_id::B_CLICK},
	{"/user/hand/right/input/b/touch",             device_id::B_TOUCH},
	{"/user/hand/right/input/system/click",        device_id::SYSTEM_CLICK},
	{"/user/hand/right/input/squeeze/click",       device_id::RIGHT_SQUEEZE_CLICK},
	{"/user/hand/right/input/squeeze/force",       device_id::RIGHT_SQUEEZE_FORCE},
	{"/user/hand/right/input/squeeze/value",       device_id::RIGHT_SQUEEZE_VALUE},
	{"/user/hand/right/input/trigger/value",       device_id::RIGHT_TRIGGER_VALUE},
	{"/user/hand/right/input/trigger/click",       device_id::RIGHT_TRIGGER_CLICK},
	{"/user/hand/right/input/trigger/touch",       device_id::RIGHT_TRIGGER_TOUCH},
	{"/user/hand/right/input/trigger/proximity",   device_id::RIGHT_TRIGGER_PROXIMITY},
	{"/user/hand/right/input/trigger/proximity_fb",device_id::RIGHT_TRIGGER_PROXIMITY},
	{"/user/hand/right/input/trigger/proximity_meta",device_id::RIGHT_TRIGGER_PROXIMITY},
	{"/user/hand/right/input/trigger/curl_fb",     device_id::RIGHT_TRIGGER_CURL},
	{"/user/hand/right/input/trigger/curl_meta",   device_id::RIGHT_TRIGGER_CURL},
	{"/user/hand/right/input/trigger_curl/value",  device_id::RIGHT_TRIGGER_CURL},
	{"/user/hand/right/input/trigger/slide_fb",    device_id::RIGHT_TRIGGER_SLIDE},
	{"/user/hand/right/input/trigger/slide_meta",  device_id::RIGHT_TRIGGER_SLIDE},
	{"/user/hand/right/input/trigger_slide/value", device_id::RIGHT_TRIGGER_SLIDE},
	{"/user/hand/right/input/trigger/force",       device_id::RIGHT_TRIGGER_FORCE},
	{"/user/hand/right/input/thumbstick",          device_id::RIGHT_THUMBSTICK_X},
	{"/user/hand/right/input/thumbstick/click",    device_id::RIGHT_THUMBSTICK_CLICK},
	{"/user/hand/right/input/thumbstick/touch",    device_id::RIGHT_THUMBSTICK_TOUCH},
	{"/user/hand/right/input/thumbrest/touch",     device_id::RIGHT_THUMBREST_TOUCH},
	{"/user/hand/right/input/thumbrest/force",     device_id::RIGHT_THUMBREST_FORCE},
	{"/user/hand/right/input/thumb_resting_surfaces/proximity",device_id::RIGHT_THUMB_PROXIMITY},
	{"/user/hand/right/input/thumb_meta/proximity_meta",device_id::RIGHT_THUMB_PROXIMITY},
	{"/user/hand/right/input/trackpad",            device_id::RIGHT_TRACKPAD_X},
	{"/user/hand/right/input/trackpad/click",      device_id::RIGHT_TRACKPAD_CLICK},
	{"/user/hand/right/input/trackpad/touch",      device_id::RIGHT_TRACKPAD_TOUCH},
	{"/user/hand/right/input/trackpad/force",      device_id::RIGHT_TRACKPAD_FORCE},
	{"/user/hand/right/input/stylus/force",        device_id::RIGHT_STYLUS_FORCE},
	{"/user/hand/right/input/stylus_fb/force",     device_id::RIGHT_STYLUS_FORCE},

	// XR_EXT_hand_interaction
	{"/user/hand/left/input/pinch_ext/value",      device_id::LEFT_PINCH_VALUE},
	{"/user/hand/left/input/pinch_ext/ready_ext",  device_id::LEFT_PINCH_READY},
	{"/user/hand/left/input/aim_activate_ext/value",device_id::LEFT_AIM_ACTIVATE_VALUE},
	{"/user/hand/left/input/aim_activate_ext/ready_ext",device_id::LEFT_AIM_ACTIVATE_READY},
	{"/user/hand/left/input/grasp_ext/value",      device_id::LEFT_GRASP_VALUE},
	{"/user/hand/left/input/grasp_ext/ready_ext",  device_id::LEFT_GRASP_READY},

	{"/user/hand/right/input/pinch_ext/value",      device_id::RIGHT_PINCH_VALUE},
	{"/user/hand/right/input/pinch_ext/ready_ext",  device_id::RIGHT_PINCH_READY},
	{"/user/hand/right/input/aim_activate_ext/value",device_id::RIGHT_AIM_ACTIVATE_VALUE},
	{"/user/hand/right/input/aim_activate_ext/ready_ext",device_id::RIGHT_AIM_ACTIVATE_READY},
	{"/user/hand/right/input/grasp_ext/value",      device_id::RIGHT_GRASP_VALUE},
	{"/user/hand/right/input/grasp_ext/ready_ext",  device_id::RIGHT_GRASP_READY},

	{"/user/gamepad/input/menu/click",             device_id::GAMEPAD_MENU_CLICK},
	{"/user/gamepad/input/view/click",             device_id::GAMEPAD_VIEW_CLICK},
	{"/user/gamepad/input/a/click",                device_id::GAMEPAD_A_CLICK},
	{"/user/gamepad/input/b/click",                device_id::GAMEPAD_B_CLICK},
	{"/user/gamepad/input/x/click",                device_id::GAMEPAD_X_CLICK},
	{"/user/gamepad/input/y/click",                device_id::GAMEPAD_Y_CLICK},
	{"/user/gamepad/input/dpad_down/click",        device_id::GAMEPAD_DPAD_DOWN_CLICK},
	{"/user/gamepad/input/dpad_right/click",       device_id::GAMEPAD_DPAD_RIGHT_CLICK},
	{"/user/gamepad/input/dpad_up/click",          device_id::GAMEPAD_DPAD_UP_CLICK},
	{"/user/gamepad/input/dpad_left/click",        device_id::GAMEPAD_DPAD_LEFT_CLICK},
	{"/user/gamepad/input/shoulder_left/click",    device_id::GAMEPAD_SHOULDER_LEFT_CLICK},
	{"/user/gamepad/input/shoulder_right/click",   device_id::GAMEPAD_SHOULDER_RIGHT_CLICK},
	{"/user/gamepad/input/thumbstick_left/click",  device_id::GAMEPAD_THUMBSTICK_LEFT_CLICK},
	{"/user/gamepad/input/thumbstick_right/click", device_id::GAMEPAD_THUMBSTICK_RIGHT_CLICK},
	{"/user/gamepad/input/trigger_left/value",     device_id::GAMEPAD_TRIGGER_LEFT_VALUE},
	{"/user/gamepad/input/trigger_right/value",    device_id::GAMEPAD_TRIGGER_RIGHT_VALUE},
	{"/user/gamepad/input/thumbstick_left/x",      device_id::GAMEPAD_THUMBSTICK_LEFT_X},
	{"/user/gamepad/input/thumbstick_left/y",      device_id::GAMEPAD_THUMBSTICK_LEFT_Y},
	{"/user/gamepad/input/thumbstick_right/x",     device_id::GAMEPAD_THUMBSTICK_RIGHT_X},
	{"/user/gamepad/input/thumbstick_right/y",     device_id::GAMEPAD_THUMBSTICK_RIGHT_Y},
};
// clang-format on

static const std::array supported_color_formats = {
        vk::Format::eR8G8B8A8Srgb,
        vk::Format::eB8G8R8A8Srgb,
};

static const std::array supported_depth_formats{
        vk::Format::eD32Sfloat,
        vk::Format::eX8D24UnormPack32,
};

scenes::stream::stream(std::string server_name, scene & parent_scene) :
        scene_impl<stream>(supported_color_formats, supported_depth_formats, parent_scene),
        apps{*this, std::move(server_name)}
{
	auto views = system.view_configuration_views(viewconfig);
	width = views[0].recommendedImageRectWidth;
	height = views[0].recommendedImageRectHeight;
}

static from_headset::visibility_mask_changed::masks get_visibility_mask(xr::instance & inst, xr::session & session, int view)
{
	assert(inst.has_extension(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME));
	static auto xrGetVisibilityMaskKHR = inst.get_proc<PFN_xrGetVisibilityMaskKHR>("xrGetVisibilityMaskKHR");

	from_headset::visibility_mask_changed::masks res{};
	for (auto [type, mask]: utils::enumerate(res))
	{
		XrVisibilityMaskKHR xr_mask{
		        .type = XR_TYPE_VISIBILITY_MASK_KHR,
		};
		CHECK_XR(xrGetVisibilityMaskKHR(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view, XrVisibilityMaskTypeKHR(type + 1), &xr_mask));
		mask.vertices.resize(xr_mask.vertexCountOutput);
		mask.indices.resize(xr_mask.indexCountOutput);
		xr_mask = {
		        .type = XR_TYPE_VISIBILITY_MASK_KHR,
		        .vertexCapacityInput = uint32_t(mask.vertices.size()),
		        .vertices = mask.vertices.data(),
		        .indexCapacityInput = uint32_t(mask.indices.size()),
		        .indices = mask.indices.data(),
		};
		CHECK_XR(xrGetVisibilityMaskKHR(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view, XrVisibilityMaskTypeKHR(type + 1), &xr_mask));
		mask.vertices.resize(xr_mask.vertexCountOutput);
		mask.indices.resize(xr_mask.indexCountOutput);
	}
	return res;
}

std::shared_ptr<scenes::stream> scenes::stream::create(std::unique_ptr<wivrn_session> network_session, float guessed_fps, std::string server_name, scene & parent_scene, std::optional<reconnect_info> reconnect_target)
{
	std::shared_ptr<stream> self{new stream{std::move(server_name), parent_scene}};
	self->network_session = std::move(network_session);
	self->guessed_fps = guessed_fps;
	self->reconnect_target = std::move(reconnect_target);

	self->send_initial_control_packets(*self->network_session, guessed_fps);

	self->network_thread = utils::named_thread("network_thread", &stream::process_packets, self.get());

	// Secondary path over the USB tunnel, if the headset allows it
	{
		stream * raw = self.get();
		self->path_manager.emplace(*self->network_session, [raw]() {
			return application::get_config().multipath_usb and raw->current_state() == state::streaming;
		});
	}

	self->command_buffer = std::move(self->device.allocateCommandBuffers({
	        .commandPool = *self->commandpool,
	        .level = vk::CommandBufferLevel::ePrimary,
	        .commandBufferCount = 1,
	})[0]);

	self->fence = self->device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled});

	// Look up the XrActions for haptics
	for (auto [id, path, output]: {
	             std::tuple(device_id::LEFT_CONTROLLER_HAPTIC, "/user/hand/left", "/output/haptic"),
	             std::tuple(device_id::RIGHT_CONTROLLER_HAPTIC, "/user/hand/right", "/output/haptic"),

	             std::tuple(device_id::LEFT_TRIGGER_HAPTIC, "/user/hand/left", "/output/haptic_trigger"),
	             std::tuple(device_id::RIGHT_TRIGGER_HAPTIC, "/user/hand/right", "/output/haptic_trigger"),
	             std::tuple(device_id::LEFT_TRIGGER_HAPTIC, "/user/hand/left", "/output/haptic_trigger_fb"),
	             std::tuple(device_id::RIGHT_TRIGGER_HAPTIC, "/user/hand/right", "/output/haptic_trigger_fb"),

	             std::tuple(device_id::LEFT_THUMB_HAPTIC, "/user/hand/left", "/output/haptic_thumb"),
	             std::tuple(device_id::RIGHT_THUMB_HAPTIC, "/user/hand/right", "/output/haptic_thumb"),
	             std::tuple(device_id::LEFT_THUMB_HAPTIC, "/user/hand/left", "/output/haptic_thumb_fb"),
	             std::tuple(device_id::RIGHT_THUMB_HAPTIC, "/user/hand/right", "/output/haptic_thumb_fb"),

	             std::tuple(device_id::GAMEPAD_HAPTIC_LEFT, "/user/gamepad", "/output/haptic_left"),
	             std::tuple(device_id::GAMEPAD_HAPTIC_RIGHT, "/user/gamepad", "/output/haptic_right"),
	             std::tuple(device_id::GAMEPAD_HAPTIC_LEFT_TRIGGER, "/user/gamepad", "/output/haptic_left_trigger"),
	             std::tuple(device_id::GAMEPAD_HAPTIC_RIGHT_TRIGGER, "/user/gamepad", "/output/haptic_right_trigger")})
	{
		if (auto action = application::get_action(std::string(path) + output); action.first)
		{
			self->haptics_actions.emplace(id, haptics_action{
			                                          .action = action.first,
			                                          .path = self->instance.string_to_path(path),
			                                  });
		}
	}

	// Look up the XrActions for input
	for (const auto & [action, action_type, name]: application::inputs())
	{
		auto it = device_ids.find(name);

		if (it == device_ids.end())
			continue;

		self->input_actions.emplace_back(it->second, action, action_type);
	}

	spdlog::info("Using format {}", vk::to_string(self->swapchain_format));

	self->query_pool = vk::raii::QueryPool(
	        self->device,
	        vk::QueryPoolCreateInfo{
	                .queryType = vk::QueryType::eTimestamp,
	                .queryCount = size_gpu_timestamps,
	        });

	self->wifi = application::get_wifi_lock().get_wifi_lock();

	return self;
}

void scenes::stream::send_initial_control_packets(wivrn_session & net, float guessed_fps)
{
	net.send_control([&]() {
		from_headset::headset_info_packet info{
		        .language = application::get_messages_info().language,
		        .country = application::get_messages_info().country,
		        .variant = application::get_messages_info().variant,
		};

		{
			auto [flags, views] = session.locate_views(
			        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			        instance.now(),
			        application::space(xr::spaces::view));

			assert(views.size() == info.fov.size());

			for (auto [i, j]: std::views::zip(views, info.fov))
				j = i.fov;
		}

		const auto & config = application::get_config();

		{
			auto view = system.view_configuration_views(viewconfig)[0];
			view = application::get_hmd_traits().override_view(view);

			info.render_eye_width = view.recommendedImageRectWidth * config.resolution_scale;
			info.render_eye_height = view.recommendedImageRectHeight * config.resolution_scale;
		}

		{
			auto scale = config.get_stream_scale();
			info.stream_eye_width = info.render_eye_width * scale;
			info.stream_eye_height = info.render_eye_height * scale;
		}

		if (instance.has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
		{
			info.available_refresh_rates = session.get_refresh_rates();
			info.settings.preferred_refresh_rate = config.preferred_refresh_rate;
			info.settings.minimum_refresh_rate = config.minimum_refresh_rate.value_or(0);
		}
		info.settings.fps_divider = config.fps_divider;

		if (info.available_refresh_rates.empty())
		{
			spdlog::warn("Unable to detect refresh rates");
			info.available_refresh_rates = {guessed_fps};
			info.settings.preferred_refresh_rate = guessed_fps;
		}

		info.settings.bitrate_bps = config.bitrate_bps;
		info.settings.bitrate_auto = config.bitrate_auto;
		info.settings.bitrate_control = config.bitrate_control();
		info.settings.radio_aware = config.radio_aware;
		info.settings.smooth_pacing = config.smooth_pacing;
		info.settings.fec = config.fec;
		info.settings.fec_adaptive = config.fec_adaptive;
		info.settings.shard_retransmit = config.shard_retransmit;
		info.settings.wifi_qos = config.wifi_qos;
		info.settings.sharp_text = config.sharp_text;
		info.settings.encoder_failover = config.encoder_failover;
		info.settings.intra_refresh = config.intra_refresh;
		info.settings.ref_invalidation = config.ref_invalidation;
		info.settings.emergency_framerate = config.emergency_framerate;
		info.settings.motion_smoothing = config.motion_smoothing;
		info.settings.motion_smoothing_mode = config.motion_mode();
		info.settings.multipath = config.multipath_mode();
		info.settings.quad_layers = config.quad_layers;
		info.settings.low_latency_audio = config.low_latency_audio;
		info.settings.standby_freeze = config.standby_freeze;
		info.settings.mirror_gamepad = config.forward_gamepad;
		info.settings.enabled_body_parts = config.body_part_mask;
		// Reduced resolution streaming: the server reads this when it builds the encoders,
		// so it only takes effect on connection (this is that connection).
		info.settings.render_scale = config.effective_render_scale();
		// Fixed-foveation "sharper center": reshapes the foveation curve live, no reconnect.
		info.settings.foveation_strength = config.effective_foveation_strength();
		info.settings.foveation_adaptive = config.foveation_adaptive;
		info.settings.foveation_foveal_qp = config.foveation_foveal_qp;

		info.hand_tracking = config.check_feature(feature::hand_tracking);
		info.eye_gaze = config.check_feature(feature::eye_gaze);

		if (instance.has_extension(XR_EXT_USER_PRESENCE_EXTENSION_NAME))
		{
			info.user_presence = system.user_presence_properties().supportsUserPresence;
		}

		if (config.check_feature(feature::face_tracking))
		{
			switch (system.face_tracker_supported())
			{
				case xr::face_tracker_type::none:
					info.face_tracking = from_headset::face_type::none;
					break;
				case xr::face_tracker_type::android:
					info.face_tracking = from_headset::face_type::android;
					break;
				case xr::face_tracker_type::fb:
				case xr::face_tracker_type::pico:
					info.face_tracking = from_headset::face_type::fb2;
					break;
				case xr::face_tracker_type::htc:
					info.face_tracking = from_headset::face_type::htc;
					break;
			}
		}

		if (config.check_feature(feature::body_tracking))
		{
			switch (system.body_tracker_supported())
			{
				case xr::body_tracker_type::none:
					info.body_tracking = from_headset::body_type::none;
					break;
				case xr::body_tracker_type::fb:
					info.body_tracking = from_headset::body_type::fb;
					break;
				case xr::body_tracker_type::meta:
					info.body_tracking = from_headset::body_type::meta;
					break;
				case xr::body_tracker_type::pico:
					info.body_tracking = from_headset::body_type::bd;
					break;
				case xr::body_tracker_type::htc:
					info.body_tracking = from_headset::body_type::htc;
					info.num_generic_trackers = application::get_generic_trackers().size();
					break;
			}
		}

		info.palm_pose = application::space(xr::spaces::palm_left) or application::space(xr::spaces::palm_right);
		info.passthrough = system.passthrough_supported() != xr::passthrough_type::none;
		info.system_name = std::string(system.properties().systemName);

		audio::get_audio_description(info);
		if (not(config.check_feature(feature::microphone)))
			info.microphone = {};

		// The nxvc tool mask this headset's decoder will accept, for the NX Warp
		// negotiation. Sent unconditionally: it costs eight bytes and it is what lets
		// the server pick ENTROPY_LITE, which trades bytes for Pass A time and which
		// only the decoder can judge. Zero from a build without an NX Warp decoder,
		// which the server reads as "no information".
		info.nxvc_tools = decoder::nxvc_tools(application::get_physical_device_properties());

		if (config.codec)
		{
			info.supported_codecs = {*config.codec};
			switch (*config.codec)
			{
				case h264:
				case raw:
				case nxwarp:
					break;
				case h265:
				case av1:
					info.bit_depth = config.bit_depth;
			}
		}
		else
			info.supported_codecs = decoder::supported_codecs();

		// The NX Warp switch is a re-ordering of this list and nothing else, which is why
		// it needs no protocol field: with it on, nxwarp goes to the front and the server
		// picks it if it has an encoder for it, falling back to the hardware codecs behind
		// it if it does not; with it off, nxwarp is removed and the negotiation is exactly
		// what it was. See configuration::nxwarp.
		//
		// A manual codec choice still wins: the combo and this switch would otherwise
		// disagree about what was asked for, and one of them would be a lie.
		if (not config.codec)
		{
			auto & cs = info.supported_codecs;
			std::erase(cs, video_codec::nxwarp);
			if (config.nxwarp)
				cs.insert(cs.begin(), video_codec::nxwarp);
		}

		return info;
	}());

	net.send_control(from_headset::session_state_changed{
	        .state = application::get_session_state(),
	});
	net.send_control(from_headset::stream_tab_changed{
	        .tab = gui_status,
	});

	if (instance.has_extension(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME))
	{
		for (uint8_t view = 0; view < view_count; ++view)
		{
			try
			{
				net.send_control(from_headset::visibility_mask_changed{
				        .data = get_visibility_mask(instance, session, view),
				        .view_index = view});
			}
			catch (std::exception & e)
			{
				spdlog::warn("Failed to get visibility mask: ", e.what());
			}
		}
	}

	{
		const auto & config = application::get_config();
		override_foveation_enable = config.override_foveation_enable;
		override_foveation_pitch = config.override_foveation_pitch;
		override_foveation_distance = config.override_foveation_distance;

		if (override_foveation_enable)
			net.send_control(from_headset::override_foveation_center{
			        .enabled = override_foveation_enable,
			        .pitch = override_foveation_pitch,
			        .distance = override_foveation_distance,
			});
	}
}

void scenes::stream::on_focused()
{
	gui_status_last_change = instance.now();

	const auto & profile = application::get_hmd_traits().controller_profile;
	input.emplace(
	        *this,
	        "assets://controllers/" + profile + "/profile.json",
	        layer_controllers,
	        layer_rays,
	        get_action("left_trigger").first,
	        get_action("right_trigger").first);

	spdlog::info("Loaded input profile {}", input->id);

	for (auto i: {xr::spaces::aim_left, xr::spaces::aim_right, xr::spaces::grip_left, xr::spaces::grip_right})
	{
		auto [p, q] = input->offset[i] = application::get_hmd_traits().controller_offset(i);

		auto rot = glm::degrees(glm::eulerAngles(q));
		spdlog::info("Initializing offset of space {} to ({}, {}, {}) mm, ({}, {}, {})°",
		             magic_enum::enum_name(i),
		             1000 * p.x,
		             1000 * p.y,
		             1000 * p.z,
		             rot.x,
		             rot.y,
		             rot.z);
	}

	std::array imgui_inputs{
	        imgui_context::controller{
	                .aim = get_action_space("left_aim"),
	                .offset = input->offset[xr::spaces::aim_left],
	                .trigger = get_action("left_trigger").first,
	                .squeeze = get_action("left_squeeze").first,
	                .scroll = get_action("left_scroll").first,
	                .haptic_output = get_action("left_haptic").first,
	        },
	        imgui_context::controller{
	                .aim = get_action_space("right_aim"),
	                .offset = input->offset[xr::spaces::aim_right],
	                .trigger = get_action("right_trigger").first,
	                .squeeze = get_action("right_squeeze").first,
	                .scroll = get_action("right_scroll").first,
	                .haptic_output = get_action("right_haptic").first,
	        },
	};

	xr::swapchain swapchain_imgui(
	        instance,
	        session,
	        device,
	        swapchain_format,
	        3600,
	        1200);

	std::vector<imgui_context::viewport> vps{
	        {
	                // Main window
	                .space = xr::spaces::world,
	                // Position and orientation are set at each frame
	                .size = {1.2, 0.6666},
	                .vp_origin = {0, 0},
	                .vp_size = {1800, 1000},
	        },
	        {
	                .space = xr::spaces::world,
	                .size = {1.2, 0.1333},
	                .vp_origin = {0, 1000},
	                .vp_size = {1800, 200},
	                .tooltip_viewport = true,
	        },
	        {
	                // popup window for combos and modals, tracks the main panel each frame at the same pixel density
	                .space = xr::spaces::world,
	                .size = {1.2, 0.6666},
	                .vp_origin = {1800, 0},
	                .vp_size = {1800, 1000},
	        },
	};

	imgui_ctx.emplace(physical_device,
	                  device,
	                  queue_family_index,
	                  queue,
	                  imgui_inputs,
	                  std::move(swapchain_imgui),
	                  std::move(vps),
	                  image_cache);

	// match the lobby's seasonal wordmark logo
	{
		auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		auto tm = std::localtime(&t);
		switch (tm->tm_mon)
		{
			case 5:
				wivrn_logo = imgui_ctx->load_texture("assets://wivrn-pride.ktx2");
				break;
			case 11:
				wivrn_logo = imgui_ctx->load_texture("assets://wivrn-christmas.ktx2");
				break;
			default:
				wivrn_logo = imgui_ctx->load_texture("assets://wivrn.ktx2");
				break;
		}
	}

	plots_toggle_1 = get_action("plots_toggle_1").first;
	plots_toggle_2 = get_action("plots_toggle_2").first;
	recenter_left = get_action("recenter_left").first;
	recenter_right = get_action("recenter_right").first;
	gui_distance_left = get_action("gui_distance_left").first;
	gui_distance_right = get_action("gui_distance_right").first;
	settings_adjust = get_action("settings_adjust").first;
	foveation_distance = get_action("foveation_distance").first;
	foveation_ok = get_action("foveation_ok").first;
	foveation_cancel = get_action("foveation_cancel").first;

	if (application::get_config().high_power_mode)
	{
		session.set_performance_level(XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
		session.set_performance_level(XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
	}
	else
	{
		session.set_performance_level(XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT);
		session.set_performance_level(XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT);
	}
}

void scenes::stream::on_unfocused()
{
	renderer->wait_idle(); // Must be before the scene data because the renderer uses its descriptor sets;
	world.clear();
	input.reset();
	left_hand.reset();
	right_hand.reset();
	apps.reset();

	imgui_ctx.reset();
}

scenes::stream::~stream()
{
	exit();

	// Stops the probe/keepalive thread and closes the secondary path
	path_manager.reset();

	if (tracking_thread && tracking_thread->joinable())
		tracking_thread->join();

	if (network_thread.joinable())
		network_thread.join();
}

// Whether the two eyes arrive as one stream item or as two.
//
// The SERVER's answer, from the stream description, not the codec's from its own stream
// header. Both carry the eye count and they cannot disagree -- the encoder that sets one
// sets the other -- but only this one is known in time. The description arrives before any
// decoder is built, which is what lets setup() skip stream 1 entirely (its stream_size is
// zero, the same signal the quad stream uses); the .nxv header does not turn up until the
// first frame, so a gate that waited for it would have a window, on every connection, in
// which the scene answered "two streams" and blocked on a decoder that was never created.
bool scenes::stream::eyes_in_one_stream() const
{
	return video_stream_description and video_stream_description->paired_eyes > 1;
}

void scenes::stream::push_blit_handle(shard_accumulator * decoder, std::shared_ptr<shard_accumulator::blit_handle> handle)
{
	assert(handle);
	if (!application::is_visible())
		return;

	{
		std::shared_lock lock(decoder_mutex);
		std::unique_lock frame_lock(frames_mutex);
		auto stream = handle->feedback.stream_index;
		if (stream < decoders.size())
		{
			if (decoder != decoders[stream].decoder.get())
				return;
			// Only when the decoder did not already say. A decoder that knows when
			// its own work finished is a better answer than the moment the handle
			// reached this scene: nxwarp stamps the fence its GPU work signalled,
			// and everything between that and here -- the ring swap, this lock --
			// is display bookkeeping rather than decode. The codecs that leave it
			// zero (the shard path hands the frame over exactly here) are unchanged.
			if (not handle->feedback.received_from_decoder)
				handle->feedback.received_from_decoder = instance.now();

			// One de-jitter sample per frame, not per stream: the eyes are encoded from
			// the same composited image and stamped with the same display time, so
			// sampling all of them would just weight the same arrival three times.
			// Taken before the swap below, while `handle` is still the frame that
			// arrived rather than the one it displaces.
			if (stream == 0)
				dejitter.sample(handle->feedback.received_from_decoder, handle->view_info.display_time);

			// One decoded frame on this stream, for the frame rate readout. Monotonic
			// and never reset: the render thread differences it over a rolling window.
			decoded_frames[stream].fetch_add(1, std::memory_order_relaxed);

			std::swap(handle, decoders[stream].latest_frames[handle->feedback.frame_index % decoders[stream].latest_frames.size()]);
		}

		// The scene is ready when every stream that will ever produce a frame has produced
		// one. On a stereo stream item 1 never will -- the server sends it no datagrams at
		// all -- so waiting on it here would leave the scene stuck short of state::streaming
		// for the whole session, which among other things never arms the stall watchdog and
		// never stops the connecting overlay. Stream 0's own frame covers both eyes.
		if (state_ != state::streaming and
		    not(decoders[0].empty() or (not eyes_in_one_stream() and decoders[1].empty())))
		{
			set_state(state::streaming);
			spdlog::info("Stream scene ready at t={}", instance.now());
		}
	}

	if (handle and not handle->feedback.blitted)
	{
		send_feedback(handle->feedback);
	}
}

bool scenes::stream::accumulator_images::empty() const
{
	for (const auto & frame: latest_frames)
	{
		if (frame)
			return false;
	}
	return true;
}

std::array<std::shared_ptr<shard_accumulator::blit_handle>, scenes::stream::decoder_count> scenes::stream::common_frame(XrTime display_time)
{
	if (decoders.empty())
		return {};
	std::unique_lock lock(frames_mutex);

	// The whole of the de-jitter buffer, on the reading side. Choosing the frame nearest
	// `display_time - D` rather than nearest `display_time` is what holding frames for D
	// means here: the display times the server stamps are one refresh period apart, so
	// walking the target back by D walks the choice back by D worth of frames, and
	// successive refreshes still pick successive frames — which is exactly the even pacing
	// the buffer exists for. A clump of three that all landed at once is then released one
	// per refresh instead of collapsing to its newest member.
	//
	// With the setting off, dejitter.delay_ns() is zero unconditionally, `target` is
	// `display_time`, and every comparison below is the one that was there before.
	const XrTime target = display_time - dejitter.delay_ns();

	inplace_vector<shard_accumulator::blit_handle *, decoder_count> common_frames;
	const bool alpha = decoders[0].latest_frames[0] and decoders[0].latest_frames[0]->view_info.alpha;
	// The join below intersects the frame indices every eye stream holds, so that the eyes
	// are never shown one frame apart. A stereo stream has nothing to intersect with: both
	// eyes came out of one decode of one stream, so they cannot disagree, and stream 1's
	// permanently empty buffer would intersect the candidate list down to nothing on every
	// refresh -- the "Failed to find a common frame" branch below, once per frame, forever.
	const bool eyes_joined = eyes_in_one_stream();
	for (size_t i = 0; i < view_count + alpha; ++i)
	{
		if (eyes_joined and i == 1)
			continue;
		if (i == 0)
		{
			for (const auto & h: decoders[i].latest_frames)
				if (h)
					common_frames.push_back(h.get());
		}
		else
		{
			// clang-format off
			erase_if(common_frames,
				[this, i](auto & left)
				{
					return std::ranges::none_of(
						decoders[i].latest_frames,
						[&left](auto & right)
						{
							return right and left->feedback.frame_index == right->feedback.frame_index;
						});
				});
			// clang-format on
		}
	}
	std::array<std::shared_ptr<shard_accumulator::blit_handle>, decoder_count> result;
	if (not common_frames.empty())
	{
		auto min = std::ranges::min_element(common_frames,
		                                    std::ranges::less{},
		                                    [target](auto frame) {
			                                    if (not frame)
				                                    return std::numeric_limits<XrTime>::max();
			                                    return std::abs(frame->view_info.display_time - target);
		                                    });

		assert(*min);
		auto frame_index = (*min)->feedback.frame_index;
		for (auto [i, decoder]: utils::enumerate(decoders))
		{
			if (alpha or i < view_count)
				result[i] = decoder.frame(frame_index);
		}

		// The quad layer stream runs in lockstep with the eyes when it runs at
		// all, but it is silent on every frame that promotes no layer, so it is
		// looked up by exact frame index and simply left out when it is not
		// there. It never takes part in the join above: the eyes must never wait
		// on it.
		if (decoders[quad_stream_idx].decoder)
			result[quad_stream_idx] = decoders[quad_stream_idx].frame(frame_index);
	}
	else
	{
		spdlog::warn("Failed to find a common frame for all decoders, dumping available frames per decoder");
		for (const auto & decoder: decoders)
		{
			if (not decoder.decoder)
				continue;
			std::string frames;
			for (const auto & frame: decoder.latest_frames)
			{
				if (frame)
					frames += " " + std::to_string(frame->feedback.frame_index);
				else
					frames += " -";
			}
			spdlog::warn(frames);
		}

		for (auto [i, decoder]: utils::enumerate(decoders))
		{
			if (alpha or i < view_count)
			{
				auto min = std::ranges::min_element(decoder.latest_frames,
				                                    std::ranges::less{},
				                                    [target](auto frame) {
					                                    if (not frame)
						                                    return std::numeric_limits<XrTime>::max();
					                                    return std::abs(frame->view_info.display_time - target);
				                                    });
				result[i] = *min;
			}
		}
	}
	return result;
}

std::shared_ptr<shard_accumulator::blit_handle> scenes::stream::accumulator_images::frame(uint64_t id) const
{
	auto & frame = latest_frames[id % latest_frames.size()];
	if (frame and frame->feedback.frame_index != id)
		return nullptr;
	return frame;
}

std::shared_ptr<shard_accumulator::blit_handle> scenes::stream::accumulator_images::previous_frame(uint64_t before) const
{
	// Scanned rather than looked up by `before - 1`: NX Warp decodes one frame in every
	// stride, so consecutive decoded frames are not consecutive frame indices, and a
	// stride that divides the buffer size would put every one of them in the same slot.
	// The newest thing older than `before` is the frame that was on screen, whatever the
	// stride is doing; how old it is allowed to be is the caller's business.
	std::shared_ptr<shard_accumulator::blit_handle> best;
	for (const auto & h: latest_frames)
	{
		if (not h or h->feedback.frame_index >= before)
			continue;
		if (not best or h->feedback.frame_index > best->feedback.frame_index)
			best = h;
	}
	return best;
}

void scenes::stream::update_gui_position(xr::spaces controller, float predicted_display_period)
{
	std::optional<std::pair<glm::vec3, glm::quat>> aim = application::locate_controller(
	        application::space(controller),
	        application::space(xr::spaces::world),
	        predicted_display_time);

	if (not aim)
		return;

	auto [offset_position, offset_orientation] = input->offset[controller];

	auto world_controller_position = aim->first + glm::mat3_cast(aim->second * offset_orientation) * offset_position;
	auto world_controller_orientation = aim->second * offset_orientation;
	auto world_controller_direction = -glm::column(glm::mat3_cast(world_controller_orientation), 2);

	if (not recentering_context)
	{
		// First frame of recentering: get the GUI position relative to the controller

		// Compute the intersection of the ray with the GUI
		auto gui_controller_direction = glm::conjugate(world_gui_orientation) * world_controller_direction;
		auto gui_controller_position = glm::conjugate(world_gui_orientation) * (world_controller_position - world_gui_position);

		float lambda = -gui_controller_position.z / gui_controller_direction.z;
		auto gui_intersection = gui_controller_position + lambda * gui_controller_direction;

		auto viewport_size = imgui_ctx->layers()[0].size;
		if (not wivrn::is_finite(lambda) or lambda < 0 or
		    std::abs(gui_intersection.x) > viewport_size.x / 2 or
		    std::abs(gui_intersection.y) > viewport_size.y / 2)
		{
			// Reset the relative GUI position if the ray does not intersect
			recentering_context.emplace(controller, glm::vec3{0, 0, -1}, glm::quat{1, 0, 0, 0});
		}
		else
		{
			glm::vec3 controller_gui_position = glm::conjugate(world_controller_orientation) * (world_gui_position - world_controller_position);
			glm::quat controller_gui_orientation = glm::conjugate(world_controller_orientation) * world_gui_orientation;

			recentering_context.emplace(controller, controller_gui_position, controller_gui_orientation);
		}
	}
	else
	{
		// Subsequent frames of recentering: keep the GUI locked to the controller
		auto & [_, controller_gui_position, controller_gui_orientation] = *recentering_context;

		if (auto gui_distance = application::read_action_float(controller == xr::spaces::aim_left ? gui_distance_left : gui_distance_right))
		{
			controller_gui_position.z *= std::pow(constants::stream::gui_max_layer_speed, gui_distance->second * predicted_display_period);
			controller_gui_position.z = -std::clamp<float>(-controller_gui_position.z, constants::stream::gui_min_layer_distance, constants::stream::gui_max_layer_distance);
		}

		world_gui_position = world_controller_position + world_controller_orientation * controller_gui_position;
		world_gui_orientation = world_controller_orientation * controller_gui_orientation;
	}
}

bool scenes::stream::is_interactable(stream_tab tab)
{
	switch (tab)
	{
		case stream_tab::stats:
		case stream_tab::transport:
		case stream_tab::settings:
		case stream_tab::foveation_settings:
		case stream_tab::applications:
		case stream_tab::application_launcher:
			return true;

		case stream_tab::hidden:
		case stream_tab::overlay_only:
		case stream_tab::compact:
			return false;
	}
	spdlog::warn("Invalid tab value {}", int(tab));
	return false;
}

namespace
{
// Angle between two orientations, in radians. The quaternions are unit length, so the
// half angle between them is acos of their dot product, sign folded away.
float pose_angle(const XrQuaternionf & a, const XrQuaternionf & b)
{
	const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	return 2 * std::acos(std::clamp(d, 0.f, 1.f));
}

float pose_shift(const XrVector3f & a, const XrVector3f & b)
{
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 1 at or below `full`, 0 at or above `zero`, smoothly in between: a hard cut off would
// make the blend appear and disappear as the head drifted across the threshold, which is
// the very flicker the setting exists to remove.
float fade_out(float v, float full, float zero)
{
	if (v <= full)
		return 1;
	if (v >= zero)
		return 0;
	const float t = 1 - (v - full) / (zero - full);
	return t * t * (3 - 2 * t);
}
} // namespace

bool scenes::stream::is_gui_interactable() const
{
	return is_interactable(gui_status);
}

// --- how fast does the render loop actually turn, and what holds it up?
//
// The compositor reported 33 of 90 while streaming and the decode rate was also 33, which
// is two facts and not yet a cause: a loop that iterates 90 times and submits 33 of them
// is a different bug from a loop that only iterates 33 times. These counters separate
// them, and time every place in render() that can block, so "it waits on the decoder" is
// something the numbers say rather than something the shape of the code suggests.
//
// Statics rather than members: render() is called from one thread and this is a probe.
namespace
{
struct render_probe
{
	std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
	uint64_t iters = 0, gated_out = 0, no_render = 0, cache_hits = 0;
	double period_ms = 0, fence_ms = 0, query_ms = 0, submit_ms = 0, blit_ms = 0;
	double app_gpu_ms = 0;
	int32_t out_w = 0, out_h = 0;
	float sharpness = 0, glow = 0, vignette = 0, deband = 0;
	bool fsr = false, use_alpha = false, motion_on = false, blend_on = false, reduce_gpu_load = false;
	double worst_fence_ms = 0;
} g_rp;
} // namespace

// Clamped once here rather than at each of the three places that need it: the swapchain,
// the layer rect and the pass's own viewport must agree exactly or the picture is cropped
// instead of scaled.
float scenes::stream::defoveate_scale() const
{
	return std::clamp(application::get_config().defoveate_scale, 0.4f, 1.0f);
}

void scenes::stream::render(const XrFrameState & frame_state)
{
	const auto rp_t0 = std::chrono::steady_clock::now();
	const auto rp_ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
	g_rp.iters++;

	if (state_ == state::shutdown)
		application::pop_scene();

	display_time_phase = frame_state.predictedDisplayPeriod ? frame_state.predictedDisplayTime % frame_state.predictedDisplayPeriod : 0;
	display_time_period = frame_state.predictedDisplayPeriod;
	real_display_period = last_display_time ? frame_state.predictedDisplayTime - last_display_time : frame_state.predictedDisplayPeriod;
	last_display_time = frame_state.predictedDisplayTime;

	// The playout delay's switch and the refresh period it is bounded in terms of. Done here,
	// before common_frame is reached below, so that a toggle takes effect on the refresh the
	// user flipped it on rather than the next one.
	dejitter.configure(application::get_config().dejitter, frame_state.predictedDisplayPeriod);

	// While a seamless reconnect is in progress the "Reconnecting…" toast is kept from
	// fading by holding its timestamp at the current frame.
	if (state_ == state::reconnecting)
		gui_status_last_change = frame_state.predictedDisplayTime;

	std::shared_lock lock(decoder_mutex);
	// One stream carrying both eyes, or two carrying one each. Taken once here and passed
	// down the rest of this refresh so that every gate below answers the same question the
	// same way even if the stream header lands mid-render.
	const bool eyes_joined = eyes_in_one_stream();
	// Nothing decoded yet on a stream that will decode something: there is no picture, and
	// there is no picture to hold either. Stream 1 is only asked about when it is a stream
	// of its own; on a stereo stream it is silent by design and requiring a frame from it
	// would black the headset out for the whole session.
	if (not frame_state.shouldRender or decoders[0].empty() or (not eyes_joined and decoders[1].empty()) or state_ == state::shutdown)
	{
		// Nothing is on screen, so frame smoothing has nothing to carry forward: forget
		// the frame index, so a frame from before a gap is never blended into the first
		// frame after it, and hand the pinned images back as soon as the last submission
		// has retired. This path returns before the fence wait below, so the fence is
		// tested rather than waited on: releasing a handle gives its image back to the
		// decoder, which must not happen while the GPU is still sampling it.
		g_rp.no_render++;
		smoothing_frame_index = uint64_t(-1);
		if (*fence and fence.getStatus() == vk::Result::eSuccess)
			smoothing_blend_handles = {};

		// TODO: stop/restart video stream
		session.begin_frame();
		session.end_frame(frame_state.predictedDisplayTime, {});
		return;
	}

	if (state_ == state::stalled)
	{
		network_session->send_control(from_headset::get_application_list{
		        .language = application::get_messages_info().language,
		        .country = application::get_messages_info().country,
		        .variant = application::get_messages_info().variant,
		});

		next_gui_status = stream_tab::hidden;
		application::pop_scene();
	}

	const auto rp_fence0 = std::chrono::steady_clock::now();
	if (device.waitForFences(*fence, VK_TRUE, UINT64_MAX) == vk::Result::eTimeout)
		throw std::runtime_error("Vulkan fence timeout");
	{
		const double f = rp_ms(std::chrono::steady_clock::now() - rp_fence0);
		g_rp.fence_ms += f;
		g_rp.worst_fence_ms = std::max(g_rp.worst_fence_ms, f);
	}

	// We don't need those after vkWaitForFences
	current_blit_handles.fill(nullptr);
	// Frame smoothing: the pass that sampled these has now retired, so the decoder's pool
	// can have them back. Nothing of the decoders' is held between here and the next
	// blend, which is what keeps the pool as free as it was without the feature.
	smoothing_blend_handles = {};

	gpu_timestamps timestamps;
	const auto rp_query0 = std::chrono::steady_clock::now();
	if (query_pool_filled)
	{
		auto [res, timestamps2] = query_pool.getResults<uint64_t>(
		        0,
		        size_gpu_timestamps,
		        size_gpu_timestamps * sizeof(uint64_t),
		        sizeof(uint64_t),
		        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);

		if (res == vk::Result::eSuccess)
		{
			boost::pfr::for_each_field(timestamps, [n = 1, &timestamps2](float & t) mutable {
				t = (timestamps2[n++] - timestamps2[0]) * application::get_physical_device_properties().limits.timestampPeriod / 1e9;
			});
		}
	}

	g_rp.query_ms += rp_ms(std::chrono::steady_clock::now() - rp_query0);
	// The client's OWN pass, on the device: what the defoveate/reprojection submission
	// costs the GPU. Against the two decodes measured next to it this is the whole
	// arithmetic of whether the GPU is saturated.
	g_rp.app_gpu_ms += double(timestamps.gpu_time) * 1000.0;

	session.begin_frame();

	std::array<int, view_count> image_indices;

#if WIVRN_FEATURE_RENDERDOC
	renderdoc_begin(*vk_instance);
#endif
	command_buffer.reset();
	command_buffer.begin(vk::CommandBufferBeginInfo{
	        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
	});

	// Keep a reference to the resources needed to blit the images until vkWaitForFences

	command_buffer.resetQueryPool(*query_pool, 0, size_gpu_timestamps);
	command_buffer.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *query_pool, 0);

	// Search for frame with desired display time on all decoders
	// If no such frame exists, use the latest frame for each decoder
	current_blit_handles = common_frame(frame_state.predictedDisplayTime);
	std::array<XrPosef, view_count> pose;
	std::array<XrFovf, view_count> fov;
	std::array<wivrn::to_headset::foveation_parameter, view_count> foveation;
	bool use_alpha = false;

	std::array<stream_defoveator::input, view_count> images;
	for (size_t i = 0; i < view_count + use_alpha; ++i)
	{
		auto & blit_handle = current_blit_handles[i];
		// On a stereo stream this is null at i == 1 and stays null: common_frame looked
		// stream 1 up and found the empty buffer it will always have. The iteration is
		// skipped here and view 1 is filled in below, out of view 0's picture, which also
		// keeps the once-per-frame bookkeeping just above once per frame -- counting a
		// display twice for one handle would make times_displayed lie to the server and to
		// the re-present gate.
		if (not blit_handle)
		{
			if (i == view_count)
				use_alpha = false;
			continue;
		}

		blit_handle->feedback.blitted = instance.now();
		// The 1 s "no decoded output" watchdog drops the scene to the lobby. It must not
		// fire while a seamless reconnect is holding the last frame (state::reconnecting),
		// nor in the brief window after a successful adopt before the first fresh frame
		// arrives: only run it in the steady streaming state. refresh_reconnect_watchdog()
		// also bumps the held frames' timestamps forward on resume as a second guard.
		if (state_ == state::streaming and blit_handle->feedback.blitted - blit_handle->feedback.received_from_decoder > 1'000'000'000)
			set_state(state::stalled);
		++blit_handle->feedback.times_displayed;
		blit_handle->feedback.displayed = frame_state.predictedDisplayTime;

		use_alpha = blit_handle->view_info.alpha;

		if (blit_handle->current_layout == vk::ImageLayout::eUndefined)
		{
			vk::ImageMemoryBarrier barrier{
			        .srcAccessMask = vk::AccessFlagBits::eNone,
			        .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
			        .oldLayout = vk::ImageLayout::eUndefined,
			        .newLayout = vk::ImageLayout::eGeneral,
			        .image = blit_handle->image,
			        .subresourceRange = {
			                .aspectMask = vk::ImageAspectFlagBits::eColor,
			                .levelCount = 1,
			                .layerCount = 1,
			        },
			};

			command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, barrier);
			blit_handle->current_layout = vk::ImageLayout::eGeneral;
		}

		if (i < view_count)
		{
			// Which views this one decoded picture feeds. One, as it always has, unless
			// this is stream 0 of a stereo stream: then the picture is the eye pair side
			// by side and it feeds both, told apart only by the source rectangle below.
			// view_info already carries a pose, a fov and a foveation run per eye, so
			// there is nothing view 1 needs that this handle does not have.
			const size_t last_view = (eyes_joined and i == 0) ? view_count : i + 1;
			for (size_t v = i; v < last_view; ++v)
			{
				foveation[v] = blit_handle->view_info.foveation[v];
				pose[v] = blit_handle->view_info.pose[v];
				fov[v] = blit_handle->view_info.fov[v];
				// colour image
				//
				// rect_rgb is the sub-rectangle of the decoded image this view is to
				// be sampled from, in the units the alpha plane below already uses:
				// the offset is where the view starts and the extent is the WHOLE
				// image, because reprojection.glsl divides by the extent to normalise
				// ((uv + rgb_rect.xy) / rgb_rect.zw). At one eye per stream that is
				// offset zero over the eye's own image, which is what it has always
				// been. At two, the decoder made the pool image eyes*width wide, so
				// eye v begins at v * the per-eye width the stream description gives.
				images[v] = {
				        .rgb = blit_handle->image_view,
				        .sampler_rgb = decoders[i].decoder->sampler(),
				        .rect_rgb = {
				                .offset = {
				                        .x = eyes_joined ? int32_t(v * video_stream_description->width) : 0,
				                },
				                .extent = blit_handle->extent,
				        },
				        .layout_rgb = blit_handle->current_layout,
				};
			}
		}
		else
		{
			// alpha image, must set for each view
			for (int j = 0; j < view_count; ++j)
			{
				images[j].a = blit_handle->image_view;
				images[j].sampler_a = decoders[i].decoder->sampler();
				images[j].rect_a = vk::Rect2D{
				        // in full size pixels
				        .offset = {
				                .x = j * video_stream_description->width,
				        },
				        .extent = {
				                .width = blit_handle->extent.width * 2,
				                .height = blit_handle->extent.height * 2,
				        },
				};
				images[j].layout_a = blit_handle->current_layout;
			}
		}
	}

	// Promoted quad layer, if one came in for this frame. It is deliberately outside
	// the loop above: it takes no part in the eye join, and the frames it is missing
	// from are the normal case, not a fault.
	std::optional<wivrn::to_headset::video_stream_data_shard::view_info_t::quad_info_t> quad_info;
	if (const auto & handle = current_blit_handles[quad_stream_idx];
	    handle and handle->view_info.quad and decoders[quad_stream_idx].decoder)
	{
		handle->feedback.blitted = instance.now();
		++handle->feedback.times_displayed;
		handle->feedback.displayed = frame_state.predictedDisplayTime;

		if (handle->current_layout == vk::ImageLayout::eUndefined)
		{
			vk::ImageMemoryBarrier barrier{
			        .srcAccessMask = vk::AccessFlagBits::eNone,
			        .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
			        .oldLayout = vk::ImageLayout::eUndefined,
			        .newLayout = vk::ImageLayout::eGeneral,
			        .image = handle->image,
			        .subresourceRange = {
			                .aspectMask = vk::ImageAspectFlagBits::eColor,
			                .levelCount = 1,
			                .layerCount = 1,
			        },
			};

			command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, barrier);
			handle->current_layout = vk::ImageLayout::eGeneral;
		}

		quad_info = handle->view_info.quad;
	}

	{
		// Estimate how often the application produces a frame: a blit handle that is
		// displayed for the first time means a new frame made it all the way through.
		// With space warp the stream runs at a fraction of the display rate by design,
		// so the ratio is scaled back up before it is compared to the thresholds.
		const auto & config = application::get_config();
		const float dt = std::min<float>(frame_state.predictedDisplayPeriod / 1e9, 0.1);
		const bool new_frame = current_blit_handles[0] and current_blit_handles[0]->feedback.times_displayed == 1;

		app_frame_ratio = std::lerp(app_frame_ratio,
		                            new_frame ? 1.f : 0.f,
		                            std::min<float>(1, dt / constants::stream::vignette_average_time));

		const float ratio = app_frame_ratio * std::max<uint32_t>(1, config.fps_divider);
		if (not config.comfort_vignette)
			comfort_vignette_active = false;
		else if (ratio < constants::stream::vignette_enter_ratio)
			comfort_vignette_active = true;
		else if (ratio > constants::stream::vignette_leave_ratio)
			comfort_vignette_active = false;

		comfort_vignette_fade = std::clamp<float>(
		        comfort_vignette_fade + (comfort_vignette_active ? dt : -dt) / constants::stream::vignette_fade_duration,
		        0,
		        1);
	}

	// Motion smoothing only does anything on the refreshes that redisplay a frame, so
	// on a headset that normally discards those the pass has to be re-enabled for
	// them. That submission is the cost the toggle buys; without a field for the
	// frame on screen nothing changes. Server mode never gets here: the PC sends a
	// genuinely new frame for every refresh, so there is nothing to redisplay.
	bool motion_available = false;
	if (application::get_config().motion_mode() == wivrn::motion_mode::headset and current_blit_handles[0])
	{
		auto lock = motion_field.lock();
		motion_available = lock->complete() and
		                   lock->field().frame_idx == current_blit_handles[0]->feedback.frame_index and
		                   lock->field().span_ns > 0;
	}

	// Allow the headset to time warp if we are redisplaying a frame
	if ((not application::get_hmd_traits().discard_frame) or
	    std::ranges::any_of(current_blit_handles, [](const auto & h) { return h and h->feedback.times_displayed < 2; }) or
	    motion_available or
	    is_gui_interactable() or
	    // While reconnecting, keep re-submitting the held frame every refresh (so the
	    // compositor keeps a stable image the runtime can time warp) and keep drawing the
	    // overlay, even on headsets that would otherwise discard a repeated frame.
	    state_ == state::reconnecting)
	{
		g_rp.gated_out++; // counted as "the gate LET IT THROUGH"; see the report line
		XrExtent2Di extents[view_count];
		{
			int32_t max_width = 0;
			int32_t max_height = 0;
			for (size_t i = 0; i < view_count; ++i)
			{
				extents[i] = stream_defoveator::defoveated_size(foveation[i], defoveate_scale());
				max_width = std::max(max_width, extents[i].width);
				max_height = std::max(max_height, extents[i].height);
			}
			if (not swapchain)
				setup_reprojection_swapchain(max_width, max_height);
			else if (swapchain.width() < max_width or swapchain.height() < max_height)
			{
				// If the defoveated image is larger than the swapchain, try to reallocate one
				try
				{
					spdlog::info("Recreating swapchain, from {}x{} to {}x{}",
					             swapchain.width(),
					             swapchain.height(),
					             max_width,
					             max_height);
					setup_reprojection_swapchain(max_width, max_height);
				}
				catch (std::exception & e)
				{
					spdlog::warn("failed to increase swapchain size");
					for (size_t i = 0; i < view_count; ++i)
					{
						extents[i].width = std::min(extents[i].width, swapchain.width());
						extents[i].height = std::min(extents[i].height, swapchain.height());
					}
				}
			}
		}
		assert(swapchain);

		// Packed defoveated size per view, for the vsync cache signature below.
		const std::array<int32_t, 2 * view_count> extents_packed{
		        extents[0].width,
		        extents[0].height,
		        extents[1].width,
		        extents[1].height,
		};

		switch (gui_status)
		{
			case stream_tab::hidden:
			case stream_tab::foveation_settings:
			case stream_tab::compact:
			case stream_tab::overlay_only:
				dimming = dimming - frame_state.predictedDisplayPeriod / (1e9 * constants::stream::fade_duration);
				break;
			case stream_tab::stats:
			case stream_tab::transport:
			case stream_tab::settings:
			case stream_tab::applications:
			case stream_tab::application_launcher:
				dimming = dimming + frame_state.predictedDisplayPeriod / (1e9 * constants::stream::fade_duration);
				break;
		}

		dimming = std::clamp<float>(dimming, 0, 1);
		float x = dimming * dimming * (3 - 2 * dimming); // Easing function

		const float scale = std::lerp(1, constants::stream::dimming_scale, x);
		const float bias = std::lerp(0, constants::stream::dimming_bias, x);

		const auto & config = application::get_config();
		const float v = comfort_vignette_fade * comfort_vignette_fade * (3 - 2 * comfort_vignette_fade); // Easing function

		// FSR takes over the sharpen when it is on: sharpness then feeds the RCAS lobe, and
		// the CAS controls are ignored. Off, it is the CAS strength exactly as before.
		const bool fsr_on = config.fsr;
		stream_defoveator::post_processing post{
		        .sharpness = fsr_on
		                             ? std::clamp<float>(config.fsr_sharpness, 0, 1)
		                             : (config.cas_sharpening ? std::clamp<float>(config.cas_sharpness, 0, 1) : 0.f),
		        .vignette = v * constants::stream::vignette_strength,
		        .vignette_inner = constants::stream::vignette_inner_radius,
		        .vignette_outer = constants::stream::vignette_outer_radius,
		        // Ambient bias lighting samples whatever frame is on screen, so it needs
		        // no special handling when motion smoothing is warping or repeating a
		        // frame: it fills the periphery with the current image's edge colours
		        // either way, which is exactly when the softened cutoff helps most.
		        .glow = config.ambient_glow
		                        ? std::clamp<float>(config.ambient_glow_intensity, 0, 1) * constants::stream::ambient_glow_strength
		                        : 0.f,
		        .glow_margin = constants::stream::ambient_glow_margin,
		        // Debanding dither, in units of one 8-bit step (1/255). Static, so it is
		        // part of the cached image: a cache hit re-presents the same dithered
		        // frame, with no temporal shimmer.
		        .deband = config.deband ? std::clamp<float>(config.deband_strength, 0, 4) : 0.f,
		};

		// Whether this refresh can re-present the image already in the swapchain
		// instead of drawing a new one. Its signature and the decision are taken under
		// the motion lock, so the motion step folded into the signature is exactly the
		// one a draw would use.
		bool cache_hit = false;
		defoveate_state state;
		{
			// Motion smoothing. The field describes one application frame interval
			// ending at the frame being displayed, so how far to move along it is
			// how far past that frame this refresh lands, in units of that
			// interval: near zero for the refresh that first shows a frame, growing
			// with every repeat until a new frame arrives or the cap is reached.
			//
			// The lock is held across the pass because it is where the field data is
			// read from; it is only ever contended by the network thread replacing
			// it, and the pass records commands rather than waiting on anything.
			auto motion_lock = motion_field.lock();
			stream_defoveator::motion_warp motion;

			if (config.motion_mode() == wivrn::motion_mode::headset and motion_lock->complete() and current_blit_handles[0])
			{
				const auto & field = motion_lock->field();
				const auto & handle = *current_blit_handles[0];
				// A field that does not name the frame on screen is stale: a lost
				// chunk, a dropped frame or an IDR. Nothing to warp along then.
				if (field.frame_idx == handle.feedback.frame_index and field.span_ns > 0)
				{
					motion.field = &field;
					motion.step = motion_warp_step(
					        frame_state.predictedDisplayTime,
					        handle.view_info.display_time,
					        field.span_ns,
					        constants::stream::motion_max_steps);
				}
			}

			// Frame smoothing. The decoded frame rate can sit far below the panel's
			// (NX Warp on a Pico 4 runs at a fraction of 90 Hz), so one decoded frame is
			// held for several refreshes and the picture steps rather than moves. On the
			// first refresh that shows a new decoded frame, and on no other, mix half of
			// the frame it replaces into it: the step is split in two and reads as a
			// short blur. Nothing is synthesized — every refresh still shows a real
			// decoded frame, or a blend of two consecutive ones.
			//
			// Off, the weight is zero, nothing is bound differently and the pass output
			// is bit identical to not having the feature.
			//
			// Two things make it safe, both learnt the hard way on a Pico 4:
			// the blend is faded out over the head motion between the two frames,
			// because they were rendered for different poses and are drawn here under
			// one; and it never holds a reference to a frame the decoders' rolling
			// buffer has released, because pinning one starved the NX Warp image pool
			// until the picture went black.
			stream_defoveator::frame_blend blend;
			{
				const uint64_t idx = current_blit_handles[0]
				                             ? current_blit_handles[0]->feedback.frame_index
				                             : uint64_t(-1);

				if (not config.frame_smoothing or idx == uint64_t(-1))
				{
					// Off, or nothing on screen. Remember nothing: no state is
					// carried across a gap in the stream, so the first frame after
					// a black patch has no predecessor to blend with rather than
					// one from before the gap.
					smoothing_frame_index = uint64_t(-1);
				}
				else if (idx != smoothing_frame_index)
				{
					smoothing_frame_index = idx;

					// The frame this one replaced, out of the rolling buffer the
					// scene is already holding. Nothing is pinned on its behalf
					// between refreshes: a frame the buffer has let go of is a
					// frame this does not blend with, which is what keeps the
					// decoder's image pool exactly as free as it was without the
					// feature.
					//
					// Where view v's pictures come from. On a stereo stream both
					// come from stream 0's rolling buffer -- stream 1 has none,
					// and asking it would leave prev[1] null, which is not a
					// stereo mismatch but simply the feature switched off for the
					// whole session on the streams that need it most.
					const auto eye_stream = [eyes_joined](size_t v) -> size_t {
						return eyes_joined ? 0 : v;
					};

					std::array<std::shared_ptr<shard_accumulator::blit_handle>, view_count> prev;
					{
						std::unique_lock frame_lock(frames_mutex);
						for (size_t v = 0; v < view_count; ++v)
							prev[v] = decoders[eye_stream(v)].previous_frame(idx);
					}

					// Both eyes, same decoded geometry (the pass reuses this
					// frame's texture coordinates for the previous image), and
					// already transitioned for sampling — a frame that was never
					// put on screen is still in the undefined layout.
					bool usable = true;
					for (size_t v = 0; v < view_count; ++v)
						usable = usable and prev[v] and current_blit_handles[eye_stream(v)] and
						         prev[v]->extent == current_blit_handles[eye_stream(v)]->extent and
						         prev[v]->current_layout != vk::ImageLayout::eUndefined;

					// And the same frame in both eyes. The two streams are kept in
					// lockstep, but if one has let go of a frame the other still
					// has, blending eye to eye across different frames would put a
					// stereo mismatch on the headset, which is far worse than not
					// smoothing at all.
					for (size_t v = 1; usable and v < view_count; ++v)
						usable = prev[v]->feedback.frame_index == prev[0]->feedback.frame_index;

					float weight = 0;
					if (usable)
					{
						const XrDuration age = current_blit_handles[0]->view_info.display_time -
						                       prev[0]->view_info.display_time;
						if (age > 0 and age <= constants::stream::frame_smoothing_max_age)
						{
							// The two frames were rendered for two different
							// head poses and are drawn here at the same texture
							// coordinates: the difference between those poses is
							// exactly how far the older one ghosts. Fade the
							// blend out over it, worst eye and worst of rotation
							// and translation, so it only acts where the ghost
							// is below seeing.
							float f = 1;
							for (size_t v = 0; v < view_count; ++v)
							{
								const XrPosef & a = prev[v]->view_info.pose[v];
								const XrPosef & b = current_blit_handles[eye_stream(v)]->view_info.pose[v];
								f = std::min(f, fade_out(pose_angle(a.orientation, b.orientation),
								                         constants::stream::frame_smoothing_full_angle,
								                         constants::stream::frame_smoothing_zero_angle));
								f = std::min(f, fade_out(pose_shift(a.position, b.position),
								                         constants::stream::frame_smoothing_full_shift,
								                         constants::stream::frame_smoothing_zero_shift));
							}
							weight = constants::stream::frame_smoothing_weight * f;
						}
					}

					// Below a five hundredth the blend is not worth a second
					// sampler, and rounding it to zero keeps the "off" path exact.
					if (weight > 0.002f)
					{
						// Pinned only from here to the fence wait at the top of
						// the next render(), by which time the pass that reads
						// them has retired.
						smoothing_blend_handles = prev;
						for (size_t v = 0; v < view_count; ++v)
						{
							images[v].prev_rgb = prev[v]->image_view;
							images[v].layout_prev_rgb = prev[v]->current_layout;
						}
						blend.weight = weight;
					}
				}
			}

			// Everything the defoveation pass output depends on this refresh. Any
			// difference from the image in the swapchain forces a real render.
			state.extents = extents_packed;
			state.use_alpha = use_alpha;
			state.scale = scale;
			state.bias = bias;
			state.sharpness = post.sharpness;
			state.cas_full = config.cas_full_kernel;
			state.fsr = fsr_on;
			state.vignette = post.vignette;
			state.glow = post.glow;
			state.deband = post.deband;
			state.motion_on = motion.field != nullptr and motion.step > 0;
			state.motion_step = motion.step;
			state.motion_frame = motion.field ? motion.field->frame_idx : uint64_t(-1);
			state.frame_blend = blend.weight;
			state.gui_interactable = is_gui_interactable();
			state.gui_status = int(gui_status);
			for (size_t i = 0; i < decoder_count; ++i)
				state.frame_index[i] = current_blit_handles[i]
				                               ? current_blit_handles[i]->feedback.frame_index
				                               : uint64_t(-1);

			// Re-present only when the toggle is on, we have produced an image, its
			// signature is unchanged, and no promoted quad is on screen (the quad has
			// its own swapchain and blit path). When off, cache_hit is always false, so
			// the render path below is byte identical to not having this feature.
			cache_hit = config.reduce_gpu_load and defoveate_cache_valid and
			            not quad_info and state == defoveate_cache;

			g_rp.cache_hits += cache_hit ? 1 : 0;
			g_rp.out_w = extents[0].width;
			g_rp.out_h = extents[0].height;
			g_rp.sharpness = post.sharpness;
			g_rp.fsr = fsr_on;
			g_rp.use_alpha = use_alpha;
			g_rp.motion_on = motion.field != nullptr and motion.step > 0;
			g_rp.blend_on = blend.weight > 0;
			g_rp.glow = post.glow;
			g_rp.vignette = post.vignette;
			g_rp.deband = post.deband;
			g_rp.reduce_gpu_load = config.reduce_gpu_load;
			if (not cache_hit)
			{
				// defoveate the image, apply scale/bias
				int image_index = swapchain.acquire();
				swapchain.wait();

				defoveator->defoveate(command_buffer,
				                      foveation,
				                      images,
				                      {scale, scale, scale, 1.},
				                      {bias, bias, bias, 0.},
				                      post,
				                      motion,
				                      blend,
				                      image_index,
				                      config.cas_full_kernel,
				                      fsr_on,
				                      config.atlas_prototype);
			}
		}

		// Promoted quad layer: straight into the swapchain the runtime is handed as
		// a quad layer, at the resolution the server encoded it at.
		int quad_image_index = -1;
		if (quad_info)
		{
			const auto & handle = current_blit_handles[quad_stream_idx];

			// source is a signed rectangle straight off the wire: a negative offset
			// or an extent reaching past the decoded image is an out-of-bounds read in
			// the blit below, and an overrun of the quad swapchain in the layer further
			// down (both are sized to the decoded quad). Clamp it to that image, in
			// place, so both consumers see the same corrected rectangle.
			auto & src = quad_info->source;
			const int32_t decoded_w = int32_t(handle->extent.width);
			const int32_t decoded_h = int32_t(handle->extent.height);
			src.offset.x = std::clamp(src.offset.x, 0, decoded_w);
			src.offset.y = std::clamp(src.offset.y, 0, decoded_h);
			src.extent.width = std::clamp(src.extent.width, 0, decoded_w - src.offset.x);
			src.extent.height = std::clamp(src.extent.height, 0, decoded_h - src.offset.y);
			const auto & rect = src;
			try
			{
				setup_quad_swapchain(decoders[quad_stream_idx].decoder->sampler());

				quad_image_index = quad_swapchain.acquire();
				quad_swapchain.wait();

				quad_blitter->blit(
				        command_buffer,
				        handle->image_view,
				        handle->current_layout,
				        vk::Rect2D{
				                .offset = {rect.offset.x, rect.offset.y},
				                .extent = {uint32_t(rect.extent.width), uint32_t(rect.extent.height)},
				        },
				        handle->extent,
				        vk::Extent2D{uint32_t(rect.extent.width), uint32_t(rect.extent.height)},
				        quad_image_index);
			}
			catch (std::exception & e)
			{
				spdlog::warn("Failed to compose the streamed quad layer: {}", e.what());
				if (quad_image_index >= 0)
					quad_swapchain.release();
				quad_image_index = -1;
				quad_info.reset();
				quad_blitter.reset();
				quad_swapchain = xr::swapchain{};
			}
		}

		command_buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 1);

		command_buffer.end();
		vk::SubmitInfo submit_info;
		submit_info.setCommandBuffers(*command_buffer);

		inplace_vector<vk::Semaphore, decoder_count> semaphores;
		inplace_vector<uint64_t, decoder_count> semaphore_vals;
		inplace_vector<vk::PipelineStageFlags, decoder_count> wait_stages;
		for (auto b: current_blit_handles)
		{
			if (b and b->semaphore)
			{
				assert(b->semaphore_val);
				semaphores.push_back(b->semaphore);
				semaphore_vals.push_back(*b->semaphore_val);
				wait_stages.push_back(vk::PipelineStageFlagBits::eFragmentShader);
			}
		}
		submit_info.setWaitDstStageMask(wait_stages);
		submit_info.setWaitSemaphores(semaphores);
		vk::TimelineSemaphoreSubmitInfo sem_info{
		        .waitSemaphoreValueCount = uint32_t(semaphore_vals.size()),
		        .pWaitSemaphoreValues = semaphore_vals.data(),
		};
		submit_info.pNext = &sem_info;

		device.resetFences(*fence);
		const auto rp_sub0 = std::chrono::steady_clock::now();
		queue.lock()->submit(submit_info, *fence);
		g_rp.submit_ms += rp_ms(std::chrono::steady_clock::now() - rp_sub0);
#if WIVRN_FEATURE_RENDERDOC
		renderdoc_end(*vk_instance);
#endif
		// On a cache hit no swapchain image was acquired; the projection layer below
		// re-references the one released by the last real render, which the runtime
		// keeps as the layer's source until a new image is released.
		if (not cache_hit)
			swapchain.release();
		if (quad_image_index >= 0)
			quad_swapchain.release();

		if (use_alpha)
			session.enable_passthrough(system);
		else
			session.disable_passthrough();

		render_start(use_alpha, frame_state.predictedDisplayTime);

		// Add the layer with the streamed content
		std::array<XrCompositionLayerProjectionView, view_count> layer_view;
		for (uint32_t view = 0; view < view_count; view++)
		{
			layer_view[view] =
			        {
			                .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
			                .pose = pose[view],
			                .fov = fov[view],

			                .subImage = {
			                        .swapchain = swapchain,
			                        .imageRect = {
			                                .offset = {0, 0},
			                                .extent = extents[view],
			                        },
			                        .imageArrayIndex = view,
			                },
			        };
		}
		add_projection_layer(
		        use_alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0,
		        application::space(xr::spaces::world),
		        layer_view);

		if (const configuration::openxr_post_processing_settings openxr_post_processing = application::get_config().openxr_post_processing;
		    (openxr_post_processing.sharpening | openxr_post_processing.super_sampling) > 0)
			set_layer_settings(openxr_post_processing.sharpening | openxr_post_processing.super_sampling);

		// The promoted overlay panel, on top of the world and below the GUI. The
		// pose is the one the server took the layer out of the stack with, in the
		// same space the eye poses above are in, or in the view space for a layer
		// the application asked to be head locked: from here on it is the runtime
		// that holds the panel still, at display rate, which is the whole point.
		if (quad_image_index >= 0)
		{
			add_quad_layer(
			        0,
			        application::space(quad_info->head_locked ? xr::spaces::view : xr::spaces::world),
			        XR_EYE_VISIBILITY_BOTH,
			        XrSwapchainSubImage{
			                .swapchain = quad_swapchain,
			                // Only the corner of the image the panel was rendered
			                // into: the swapchain is the size of the encode image,
			                // the panel is whatever fraction of it keeps its aspect
			                // ratio.
			                .imageRect = {
			                        .offset = {0, 0},
			                        .extent = {quad_info->source.extent.width, quad_info->source.extent.height},
			                },
			                .imageArrayIndex = 0,
			        },
			        quad_info->pose,
			        quad_info->size);

			// The eye images are dimmed by the defoveation pass while the GUI
			// is up; the panel is no longer in them, so it is dimmed here.
			if (composition_layer_color_scale_bias_supported and dimming > 0)
				set_color_scale_bias({scale, scale, scale, 1}, {bias, bias, bias, 0});
		}

		// One frame the render thread actually presented, for the frame rate readout.
		++displayed_frames;

		accumulate_metrics(frame_state.predictedDisplayTime, current_blit_handles, timestamps);

		draw_gui(frame_state.predictedDisplayTime, frame_state.predictedDisplayPeriod);

		try
		{
			render_end();
		}
		catch (std::system_error & e)
		{
			if (e.code().category() == xr::error_category() and e.code().value() == XR_ERROR_POSE_INVALID)
				spdlog::info("Invalid pose submitted");
			else
				throw;
		}

		// Record what the swapchain now holds. On a full render that is the image just
		// drawn; on a cache hit the signature is unchanged, so this is a no-op. Either
		// way a valid image is now available for a later refresh to re-present.
		defoveate_cache = state;
		defoveate_cache_valid = true;
	}

	// Network operations may be blocking, do them once everything was submitted
	{
		// Keep a copy of the feedback packets as they can be modified if they're encrypted
		inplace_vector<from_headset::feedback, decoder_count> feedbacks;
		inplace_vector<serialization_packet, decoder_count> packets;

		feedbacks.reserve(current_blit_handles.size());
		packets.reserve(current_blit_handles.size());

		for (const auto & handle: current_blit_handles)
		{
			if (handle)
			{
				auto & packet = packets.emplace_back();
				wivrn_session::control_socket_t::serialize(packet, feedbacks.emplace_back(handle->feedback));
			}
		}
		if (not packets.empty())
		{
			try
			{
				network_session->send_control(std::span(packets));
			}
			catch (std::exception & e)
			{
				spdlog::warn("Exception while sending feedback packet: {}", e.what());
			}
		}
	}

	read_actions();

	if (application::get_config().enable_stream_gui)
	{
		XrActionStateGetInfo get_info{
		        .type = XR_TYPE_ACTION_STATE_GET_INFO,
		        .action = plots_toggle_1,
		};

		XrActionStateBoolean state_1{XR_TYPE_ACTION_STATE_BOOLEAN};
		CHECK_XR(xrGetActionStateBoolean(session, &get_info, &state_1));
		get_info.action = plots_toggle_2;
		XrActionStateBoolean state_2{XR_TYPE_ACTION_STATE_BOOLEAN};
		CHECK_XR(xrGetActionStateBoolean(session, &get_info, &state_2));

		if (state_1.currentState and state_2.currentState and (state_1.changedSinceLastSync or state_2.changedSinceLastSync))
		{
			// Arbitrary transitions can happen from network commands
			// Ensure we can't have a set of 2 non interactable states
			if (is_gui_interactable())
				next_gui_status = stream_tab::hidden;
			else if (is_interactable(stored_gui_status))
				next_gui_status = stored_gui_status;
			else
				next_gui_status = stream_tab::applications;
		}
	}

	g_rp.blit_ms += rp_ms(std::chrono::steady_clock::now() - rp_t0);
	g_rp.period_ms += double(real_display_period) / 1e6;
	if (rp_t0 - g_rp.since > std::chrono::seconds(2))
	{
		const double n = double(g_rp.iters);
		const double secs = rp_ms(rp_t0 - g_rp.since) / 1000.0;
		spdlog::info("render: {} iterations in {:.1f} s ({:.1f}/s), {} submitted a layer, {} skipped by the repeat gate, {} with nothing to show; display period {:.1f} ms",
		             g_rp.iters, secs, n / secs, g_rp.gated_out,
		             g_rp.iters - g_rp.gated_out - g_rp.no_render, g_rp.no_render,
		             g_rp.period_ms / n);
		spdlog::info("render: this app's own GPU pass {:.1f} ms per iteration", g_rp.app_gpu_ms / n);
		spdlog::info("render: defoveate {}x{} per eye x2 = {:.2f} Mpx/frame at scale {:.2f} atlas-mode {}; {} re-presented from the cache (reduce_gpu_load {})",
		             g_rp.out_w, g_rp.out_h,
		             2.0 * double(g_rp.out_w) * double(g_rp.out_h) / 1e6, defoveate_scale(),
		             application::get_config().atlas_prototype,
		             g_rp.cache_hits, g_rp.reduce_gpu_load ? "on" : "off");
		spdlog::info("render: shader path sharpness {:.2f} fsr {} alpha {} motion {} blend {} glow {:.2f} vignette {:.2f} deband {:.2f}",
		             g_rp.sharpness, g_rp.fsr, g_rp.use_alpha, g_rp.motion_on, g_rp.blend_on,
		             g_rp.glow, g_rp.vignette, g_rp.deband);
		spdlog::info("render: per iteration fence {:.1f} (worst {:.1f}) | queries {:.1f} | submit {:.1f} | whole render() {:.1f} ms",
		             g_rp.fence_ms / n, g_rp.worst_fence_ms, g_rp.query_ms / n,
		             g_rp.submit_ms / n, g_rp.blit_ms / n);
		g_rp = {};
		g_rp.since = rp_t0;
	}

	query_pool_filled = true;
}

void scenes::stream::exit()
{
	state_ = state::shutdown;
}

void scenes::stream::setup(const to_headset::video_stream_description & description)
{
	spdlog::info("setup, refresh rate {}", description.refresh_rate);
	session.set_refresh_rate(description.refresh_rate);

	std::unique_lock lock(decoder_mutex);
	if (video_stream_description == description)
		return;
	spdlog::info("Creating decoders, size {}x{}", description.width, description.height);
	video_stream_description = description;

	// New decoders mean new frame timings; whatever the window holds describes a stream that
	// no longer exists (a different refresh rate, most of the time).
	{
		std::unique_lock frame_lock(frames_mutex);
		dejitter.reset();
	}

	for (const auto & [stream_index, item]: utils::enumerate(decoders))
	{
		// The quad layer stream only exists when the server is actually promoting
		// layers; it announces that by giving it a size. Nothing else in the
		// client may assume the decoder is there.
		auto [width, height] = description.stream_size(stream_index);
		if (width == 0 or height == 0)
		{
			item = accumulator_images{};
			continue;
		}

		item = accumulator_images{
		        .decoder = std::make_unique<shard_accumulator>(device, physical_device, instance, queue_family_index, description, shared_from_this(), stream_index),
		};
	}

	if (defoveator)
	{
		// reset_pipelines resets the descriptor pool; the last submitted frame
		// may still be using its sets, so wait for it (setup runs on the network
		// thread; decoder_mutex only excludes concurrent recording).
		if (device.waitForFences(*fence, VK_TRUE, UINT64_MAX) == vk::Result::eTimeout)
			throw std::runtime_error("Vulkan fence timeout");
		defoveator->reset_pipelines();
	}

	// Belongs to the decoder that has just been replaced
	quad_blitter.reset();
	quad_swapchain = xr::swapchain{};
}

// Swapchain and pass for the promoted quad layer, built the first time a quad comes
// in. The swapchain is the size of the encode image, which is fixed for the session,
// so a panel that changes size or aspect ratio just uses a different corner of it and
// nothing is reallocated mid-stream. Only a new decoder, whose sampler the pass is
// compiled against, rebuilds anything.
void scenes::stream::setup_quad_swapchain(vk::Sampler sampler)
{
	if (quad_blitter and quad_blitter->sampler() == sampler)
		return;

	const uint32_t width = video_stream_description->quad_width;
	const uint32_t height = video_stream_description->quad_height;
	if (width == 0 or height == 0)
		throw std::runtime_error("no quad layer stream");

	device.waitIdle();
	quad_blitter.reset();
	quad_swapchain = xr::swapchain{};

	spdlog::info("Creating quad layer swapchain: {}x{}", width, height);
	quad_swapchain = xr::swapchain(instance, session, device, swapchain_format, width, height, 1, 1);

	quad_blitter.emplace(
	        device,
	        quad_swapchain.images(),
	        vk::Extent2D{uint32_t(quad_swapchain.width()), uint32_t(quad_swapchain.height())},
	        quad_swapchain.format(),
	        sampler);
}

void scenes::stream::setup_reprojection_swapchain(uint32_t swapchain_width, uint32_t swapchain_height)
{
	assert(swapchain_width);
	assert(swapchain_height);
	// The cached defoveated image lives in the swapchain about to be replaced.
	defoveate_cache_valid = false;
	device.waitIdle();
	spdlog::info("swapchain setup, refresh rate {}", video_stream_description->refresh_rate);
	session.set_refresh_rate(video_stream_description->refresh_rate);

	auto views = system.view_configuration_views(viewconfig);

	swapchain = xr::swapchain(instance, session, device, swapchain_format, swapchain_width, swapchain_height, 1, views.size());
	spdlog::info("Created stream swapchain: {}x{}", swapchain.width(), swapchain.height());
	for (auto view: views)
	{
		if (swapchain.width() > view.maxImageRectWidth or swapchain.height() > view.maxImageRectHeight)
			spdlog::warn("Swapchain size larger than maximum {}x{}", view.maxImageRectWidth, view.maxImageRectHeight);
	}

	spdlog::info("Initializing reprojector");
	vk::Extent2D extent = {(uint32_t)swapchain.width(), (uint32_t)swapchain.height()};

	defoveator.emplace(
	        device,
	        physical_device,
	        swapchain.images(),
	        extent,
	        swapchain.format());
	// The pass must lay its viewport out with the same scale the swapchain and the
	// layer rect were sized with, or the picture is cropped rather than scaled.
	defoveator->set_output_scale(defoveate_scale());
}

scene::meta & scenes::stream::get_meta_scene()
{
	static meta m{
	        .name = "Stream",
	        .actions = {
	                {"left_aim", XR_ACTION_TYPE_POSE_INPUT},
	                {"left_trigger", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"left_squeeze", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"left_scroll", XR_ACTION_TYPE_VECTOR2F_INPUT},
	                {"left_haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
	                {"right_aim", XR_ACTION_TYPE_POSE_INPUT},
	                {"right_trigger", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"right_squeeze", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"right_scroll", XR_ACTION_TYPE_VECTOR2F_INPUT},
	                {"right_haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},

	                {"plots_toggle_1", XR_ACTION_TYPE_BOOLEAN_INPUT},
	                {"plots_toggle_2", XR_ACTION_TYPE_BOOLEAN_INPUT},

	                {"recenter_left", XR_ACTION_TYPE_BOOLEAN_INPUT},
	                {"recenter_right", XR_ACTION_TYPE_BOOLEAN_INPUT},
	                {"gui_distance_left", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"gui_distance_right", XR_ACTION_TYPE_FLOAT_INPUT},

	                {"settings_adjust", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"foveation_distance", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"foveation_ok", XR_ACTION_TYPE_BOOLEAN_INPUT},
	                {"foveation_cancel", XR_ACTION_TYPE_BOOLEAN_INPUT},
	        },
	        .bindings = {
	                suggested_binding{
	                        {
	                                "/interaction_profiles/oculus/touch_controller",
	                                "/interaction_profiles/facebook/touch_controller_pro",
	                                "/interaction_profiles/meta/touch_pro_controller",
	                                "/interaction_profiles/meta/touch_controller_plus",
	                                "/interaction_profiles/meta/touch_plus_controller",
	                                "/interaction_profiles/bytedance/pico_neo3_controller",
	                                "/interaction_profiles/bytedance/pico4_controller",
	                                "/interaction_profiles/bytedance/pico4s_controller",
	                                "/interaction_profiles/yvr/touch_controller_yvr",
	                                "/interaction_profiles/htc/vive_focus3_controller",
	                        },
	                        {
	                                {"left_aim", "/user/hand/left/input/aim/pose"},
	                                {"left_trigger", "/user/hand/left/input/trigger/value"},
	                                {"left_squeeze", "/user/hand/left/input/squeeze/value"},
	                                {"left_scroll", "/user/hand/left/input/thumbstick"},
	                                {"left_haptic", "/user/hand/left/output/haptic"},
	                                {"right_aim", "/user/hand/right/input/aim/pose"},
	                                {"right_trigger", "/user/hand/right/input/trigger/value"},
	                                {"right_squeeze", "/user/hand/right/input/squeeze/value"},
	                                {"right_scroll", "/user/hand/right/input/thumbstick"},
	                                {"right_haptic", "/user/hand/right/output/haptic"},

	                                {"recenter_left", "/user/hand/left/input/squeeze/value"},
	                                {"recenter_right", "/user/hand/right/input/squeeze/value"},
	                                {"gui_distance_left", "/user/hand/left/input/thumbstick/y"},
	                                {"gui_distance_right", "/user/hand/right/input/thumbstick/y"},
	                                {"settings_adjust", "/user/hand/right/input/thumbstick/y"},
	                                {"foveation_distance", "/user/hand/left/input/thumbstick/y"},
	                                {"foveation_ok", "/user/hand/right/input/a/click"},
	                                {"foveation_cancel", "/user/hand/right/input/b/click"},

	                                {"plots_toggle_1", "/user/hand/left/input/thumbstick/click"},
	                                {"plots_toggle_2", "/user/hand/right/input/thumbstick/click"},
	                        },
	                },
	                suggested_binding{
	                        {
	                                "/interaction_profiles/khr/simple_controller",
	                        },
	                        {},
	                },
	        },
	};

	return m;
}

std::optional<std::string> scenes::stream::pop_stream_error()
{
	auto queue = stream_error_queue.lock();
	if (queue->empty())
		return std::nullopt;

	auto err = std::make_optional(queue->front());
	queue->pop();

	return err;
}

void scenes::stream::on_xr_event(const xr::event & event)
{
	switch (event.header.type)
	{
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			if (event.space_changed_pending.referenceSpaceType == XrReferenceSpaceType::XR_REFERENCE_SPACE_TYPE_LOCAL)
				recenter_requested = true;
			break;
		case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
			network_session->send_control(from_headset::refresh_rate_changed{
			        .from = event.refresh_rate_changed.fromDisplayRefreshRate,
			        .to = event.refresh_rate_changed.toDisplayRefreshRate,
			});
			break;
		case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
			network_session->send_control(from_headset::visibility_mask_changed{
			        .data = get_visibility_mask(instance, session, event.visibility_mask_changed.viewIndex),
			        .view_index = uint8_t(event.visibility_mask_changed.viewIndex),
			});
			break;
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			// Override session state if the GUI is interactable
			if (event.state_changed.state == XR_SESSION_STATE_FOCUSED and is_gui_interactable())
				network_session->send_control(from_headset::session_state_changed{
				        .state = XR_SESSION_STATE_VISIBLE,
				});
			else
				network_session->send_control(from_headset::session_state_changed{
				        .state = event.state_changed.state,
				});
			break;
		case XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT:
			network_session->send_control(from_headset::user_presence_changed{
			        .present = (bool)event.user_presence_changed.isUserPresent,
			        .change_time = instance.now(),
			});
			break;
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
			on_interaction_profile_changed(event.interaction_profile_changed);
			break;
		default:
			break;
	}
}

bool scenes::stream::forward_hid_input(from_headset::hid::input_t packet, bool device_enabled)
{
	// hid_forwarding is whether the server permits it; device_enabled is the headset toggle.
	if (not hid_forwarding or not device_enabled)
		return false;
	network_session->send_control(from_headset::hid::input{packet});
	return true;
}

bool scenes::stream::on_input_key_down(uint8_t key_code)
{
	return forward_hid_input(from_headset::hid::key_down{key_code}, application::get_config().forward_keyboard);
}
bool scenes::stream::on_input_key_up(uint8_t key_code)
{
	return forward_hid_input(from_headset::hid::key_up{key_code}, application::get_config().forward_keyboard);
}
bool scenes::stream::on_input_mouse_move(float x, float y)
{
	return forward_hid_input(from_headset::hid::mouse_move{x, y}, application::get_config().forward_mouse);
}
bool scenes::stream::on_input_button_down(uint8_t button)
{
	return forward_hid_input(from_headset::hid::button_down{button}, application::get_config().forward_mouse);
}
bool scenes::stream::on_input_button_up(uint8_t button)
{
	return forward_hid_input(from_headset::hid::button_up{button}, application::get_config().forward_mouse);
}
bool scenes::stream::on_input_scroll(float h, float v)
{
	return forward_hid_input(from_headset::hid::mouse_scroll{h, v}, application::get_config().forward_mouse);
}
