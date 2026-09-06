/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#include "compositor.h"

// Monado includes
#include "driver/xrt_cast.h"
#include "main/comp_frame.h"
#include "math/m_api.h"
#include "util/comp_render_helpers.h"
#include "util/comp_vulkan.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_time.h"
#include "vk/vk_helpers.h"

#include "driver/configuration.h"
#include "driver/pose_sanitize.h"
#include "driver/wivrn_session.h"
#include "encoder/video_encoder.h"
#include "inplace_vector.hpp"
#include "utils/method.h"
#include "utils/wivrn_trace.h"

#include <cinttypes>
#include <magic_enum.hpp>

#if WIVRN_USE_PIPEWIRE
#include "pipewire_mirror.h"
#endif

#include "xrt/xrt_config_build.h" // IWYU pragma: keep
#ifdef XRT_FEATURE_RENDERDOC
#include "renderdoc_app.h"

static auto renderdoc()
{
	auto x = []() {
		RENDERDOC_API_1_5_0 * rdoc_api = nullptr;
		const char * env = std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
		if (not env or env != std::string_view("1"))
			return rdoc_api;
		void * mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
		if (mod)
		{
			pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
			XRT_MAYBE_UNUSED int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_5_0, (void **)&rdoc_api);
			assert(ret == 1);
		}
		return rdoc_api;
	};
	static auto res = x();
	return res;
}
#endif

DEBUG_GET_ONCE_LOG_OPTION(log, "XRT_COMPOSITOR_LOG", U_LOGGING_INFO)

namespace details
{
template <auto Method, typename Result, typename... Args>
struct method_trait<Method, Result (wivrn::compositor::*)(Args...)>
{
	static Result magic(xrt_compositor * arg, Args... args)
	{
		return std::invoke(
		        Method,
		        static_cast<wivrn::compositor *>(reinterpret_cast<struct comp_base *>(arg)),
		        args...);
	}
};

} // namespace details

namespace
{
const comp_swapchain_image & get_layer_image(const comp_layer & layer, uint32_t swapchain_index, uint32_t image_index)
{
	return reinterpret_cast<struct comp_swapchain *>(comp_layer_get_swapchain(&layer, swapchain_index))->images[image_index];
}

// Extrapolation helpers for server-side motion smoothing. A warped duplicate carries
// the retained frame advanced along the motion field to a later instant; these move a
// pose/fov to that same instant so the runtime's timewarp complements the head motion
// the warp already baked into the picture rather than double-counting it.
//
// The parameter u runs along the segment prev(u=0) -> cur(u=1): u == 1 + t reaches t
// field-intervals past the retained frame, which is exactly where the warp put the
// content. u > 1 is therefore an extrapolation, not an interpolation.

// Shortest-arc quaternion slerp that also extrapolates past u == 1. -ffast-math build,
// so no std::isnan; the caller validates the result and falls back if it is not usable.
XrQuaternionf slerp_extrapolate(const XrQuaternionf & a, XrQuaternionf b, float u)
{
	float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	// Take the shorter of the two arcs to the same orientation.
	if (dot < 0)
	{
		b = {-b.x, -b.y, -b.z, -b.w};
		dot = -dot;
	}
	dot = std::clamp(dot, -1.f, 1.f);

	XrQuaternionf r;
	if (dot > 0.9995f)
	{
		// Nearly parallel: sin(theta0) -> 0 makes the slerp weights blow up. nlerp
		// extrapolates the same way to first order and cannot divide by ~zero.
		r = {
		        a.x + u * (b.x - a.x),
		        a.y + u * (b.y - a.y),
		        a.z + u * (b.z - a.z),
		        a.w + u * (b.w - a.w),
		};
	}
	else
	{
		const float theta0 = __builtin_acosf(dot);
		const float sin0 = __builtin_sinf(theta0);
		const float sa = __builtin_sinf((1.f - u) * theta0) / sin0;
		const float sb = __builtin_sinf(u * theta0) / sin0;
		r = {
		        sa * a.x + sb * b.x,
		        sa * a.y + sb * b.y,
		        sa * a.z + sb * b.z,
		        sa * a.w + sb * b.w,
		};
	}

	// nlerp is not unit and slerp drifts a hair; renormalise so the result is a rotation.
	const float n2 = r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w;
	if (n2 > 0)
	{
		const float inv = 1.f / __builtin_sqrtf(n2);
		r = {r.x * inv, r.y * inv, r.z * inv, r.w * inv};
	}
	return r;
}

XrVector3f lerp_extrapolate(const XrVector3f & a, const XrVector3f & b, float u)
{
	return {
	        a.x + u * (b.x - a.x),
	        a.y + u * (b.y - a.y),
	        a.z + u * (b.z - a.z),
	};
}

XrPosef pose_extrapolate(const XrPosef & prev, const XrPosef & cur, float u)
{
	return {
	        .orientation = slerp_extrapolate(prev.orientation, cur.orientation, u),
	        .position = lerp_extrapolate(prev.position, cur.position, u),
	};
}

XrFovf fov_extrapolate(const XrFovf & prev, const XrFovf & cur, float u)
{
	return {
	        .angleLeft = prev.angleLeft + u * (cur.angleLeft - prev.angleLeft),
	        .angleRight = prev.angleRight + u * (cur.angleRight - prev.angleRight),
	        .angleUp = prev.angleUp + u * (cur.angleUp - prev.angleUp),
	        .angleDown = prev.angleDown + u * (cur.angleDown - prev.angleDown),
	};
}

// The formats a view over the compositor's YCbCr image may take, and the
// VkImageFormatListCreateInfo the image is created with.
//
// The two _UNORM plane formats are what the compositor itself writes through
// (view_y and view_cbcr below). The two _UINT ones are for NX Warp's GPU
// encoder: its E0 pass reads the stored codes of the planes through UINT
// storage views — a sampled _UNORM read would apply a conversion the codec
// must not have, and YCoCg-R is exactly invertible only over integers — and a
// format list that does not name them makes those views invalid, which is a
// driver's right to refuse. Naming them costs nothing when no encoder asks for
// them, and without them the encoder has to read the whole frame back to the
// host and upload it again.
//
// The planar format stays LAST: callers take it as `formats.back()`.
std::array<vk::Format, 5> image_formats(int bit_depth)
{
	switch (bit_depth)
	{
		case 8:
			return {
			        vk::Format::eR8Unorm,
			        vk::Format::eR8G8Unorm,
			        vk::Format::eR8Uint,
			        vk::Format::eR8G8Uint,
			        vk::Format::eG8B8R82Plane420Unorm,
			};
		case 10:
			return {
			        vk::Format::eR16Unorm,
			        vk::Format::eR16G16Unorm,
			        vk::Format::eR16Uint,
			        vk::Format::eR16G16Uint,
			        vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16,
			};
	}
	throw std::runtime_error(std::format("Unsupported bit depth {}", bit_depth));
}

std::array<wivrn::compositor::image, 2> make_images(wivrn::vk_bundle & vk, vk::CommandPool command_pool, std::span<wivrn::encoder_settings> encoders)
{
	auto formats = image_formats(encoders[0].bit_depth);

	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat,
	                .imageType = vk::ImageType::e2D,
	                .format = formats.back(),
	                .extent = {
	                        .width = encoders[0].width,
	                        .height = encoders[0].height,
	                        .depth = 1,
	                },
	                .mipLevels = 1,
	                .arrayLayers = 3, // left, right then alpha
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = formats.size(),
	                .pViewFormats = formats.data(),
	        },
	};
#if WIVRN_USE_VULKAN_ENCODE
	if (
	        std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(vk.feat).videoMaintenance1 and
	        std::ranges::contains(
	                encoders,
	                wivrn::encoder_vulkan,
	                &wivrn::encoder_settings::encoder_name))
	{
		image_info.get().flags |= vk::ImageCreateFlagBits::eVideoProfileIndependentKHR;
		image_info.get().usage |= vk::ImageUsageFlagBits::eVideoEncodeSrcKHR;
	}
#endif

	auto make_image = [&](int i) {
		vk::ImageViewUsageCreateInfo usage{
		        .usage = vk::ImageUsageFlagBits::eStorage,
		};
		image_allocation image{
		        vk.device,
		        image_info.get(),
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("compositor YCbCr image {}", i),
		};
		vk::Image vk_image{image};
		return wivrn::compositor::image{
		        .image{std::move(image)},
		        .view_y{
		                vk.device,
		                {
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2DArray,
		                        .format = formats[0],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane0,
		                                .levelCount = 1,
		                                .layerCount = image_info.get().arrayLayers,
		                        },
		                },
		        },
		        .view_cbcr{
		                vk.device,
		                {
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2DArray,
		                        .format = formats[1],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane1,
		                                .levelCount = 1,
		                                .layerCount = image_info.get().arrayLayers,
		                        },
		                },
		        }};
	};

	return {make_image(0), make_image(1)};
}

vk::raii::Semaphore make_semaphore(wivrn::vk_bundle & vk)
{
	vk::raii::Semaphore res{
	        vk.device,
	        vk::StructureChain{
	                vk::SemaphoreCreateInfo{},
	                vk::SemaphoreTypeCreateInfo{
	                        .semaphoreType = vk::SemaphoreType::eTimeline,
	                },
	        }
	                .get(),
	};
	vk.name(res, "compositor semaphore");
	return res;
}

vk::Extent3D render_extent(const wivrn::from_headset::headset_info_packet & info)
{
	return {
	        .width = info.render_eye_width,
	        .height = info.render_eye_height,
	        .depth = 1,
	};
}

// Whether a quad layer can be pulled out of the eye images and streamed on its own.
//
// The stream carries colour only, so a layer whose alpha channel the application
// asked to have blended would come out opaque: those are left in the squash unless
// the server configuration says the overlay is known to be a solid rectangle. Same
// for a layer visible in one eye only (it would need a stream per eye) and for a
// colour transform, which is applied by the squasher and is not on the wire.
bool quad_promotable(const xrt_layer_data & data, bool allow_blended)
{
	if (data.type != XRT_LAYER_QUAD)
		return false;
	if (data.quad.visibility != XRT_LAYER_EYE_VISIBILITY_BOTH)
		return false;
	if (data.flags & XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE)
		return false;
	if ((data.flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) and not allow_blended)
		return false;
	if (data.quad.size.x <= 0 or data.quad.size.y <= 0)
		return false;
	return true;
}

// How much of the view the quad takes up, as a solid angle in steradians, near
// enough: its area foreshortened by how obliquely it is seen, over the square of
// the distance to it. Both the quad pose and the head pose are in the same space,
// which for a head locked layer is the view space where the head is the origin.
//
// This is the selection criterion because it is what "the panel the user is
// looking at" means, and because it moves smoothly: a panel does not change size
// or jump across the room between two frames, so the choice does not flicker.
float quad_solid_angle(const xrt_layer_quad_data & q, const xrt_pose & head)
{
	xrt_vec3 to_quad{
	        q.pose.position.x - head.position.x,
	        q.pose.position.y - head.position.y,
	        q.pose.position.z - head.position.z,
	};
	const float d2 = to_quad.x * to_quad.x + to_quad.y * to_quad.y + to_quad.z * to_quad.z;
	if (d2 < 1e-4f)
		return 0;

	xrt_vec3 normal{0, 0, 1};
	math_quat_rotate_vec3(&q.pose.orientation, &normal, &normal);

	const float obliquity = std::abs(normal.x * to_quad.x + normal.y * to_quad.y + normal.z * to_quad.z) /
	                        std::sqrt(d2);

	return q.size.x * q.size.y * obliquity / d2;
}

// Motion smoothing gating. The ratio is filtered over about a second at stream
// rate, and the two thresholds keep an application that hovers around the limit
// from switching the estimator on and off every few frames.
constexpr float motion_ratio_filter = 1.f / 90.f;
constexpr float motion_enter_ratio = 0.8f; // below this the application is behind
constexpr float motion_leave_ratio = 0.9f; // above this it has caught up again
// Longest interval a single field may describe. Beyond it the two frames have
// nothing to do with each other any more and extrapolating from the field would be
// worse than showing the frame as it is.
constexpr XrTime motion_max_span = 500'000'000;

void fingerprint(uint64_t & h, const void * data, size_t size)
{
	// FNV-1a
	auto bytes = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < size; ++i)
	{
		h ^= bytes[i];
		h *= 0x100000001b3;
	}
}

// Identity of the content of a layer stack: everything an application changes when
// it renders a new frame, and nothing the compositor changes on its own. The frame
// id in layer_accum.data is the *native* compositor's, it increases on every commit
// including the replayed ones, so it is deliberately left out.
uint64_t layer_fingerprint(const comp_layer_accum & layers)
{
	uint64_t h = 0xcbf29ce484222325;
	fingerprint(h, &layers.layer_count, sizeof(layers.layer_count));

	for (uint32_t i = 0; i < layers.layer_count; ++i)
	{
		const auto & layer = layers.layers[i];
		fingerprint(h, &layer.data, sizeof(layer.data));
		for (size_t sc = 0; sc < std::size(layer.sc_array); ++sc)
		{
			// The swapchain a layer points at is part of its identity: an
			// application that alternates between two of them reuses image
			// index 0 in both.
			auto ptr = reinterpret_cast<uintptr_t>(layer.sc_array[sc]);
			fingerprint(h, &ptr, sizeof(ptr));
		}
	}
	return h;
}

} // namespace

namespace wivrn
{

void compositor::timings::add(float us)
{
	int index = this->index;
	this->index = (this->index + 1) % values.size();
	values[index] = us;
}

xrt_result_t compositor::predict_frame(int64_t * out_frame_id,
                                       int64_t * out_wake_time_ns,
                                       int64_t * out_predicted_gpu_time_ns,
                                       int64_t * out_predicted_display_time_ns,
                                       int64_t * out_predicted_display_period_ns)
{
	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t present_slop_ns = 0;
	int64_t desired_present_time_ns = 0;
	int64_t predicted_display_time_ns = 0;
	pacer.predict(
	        frame_id,
	        wake_up_time_ns,
	        desired_present_time_ns,
	        present_slop_ns,
	        predicted_display_time_ns);

	frame.waited.id = frame_id;
	frame.waited.desired_present_time_ns = desired_present_time_ns;
	frame.waited.present_slop_ns = present_slop_ns;
	frame.waited.predicted_display_time_ns = predicted_display_time_ns;

	*out_frame_id = frame_id;
	*out_wake_time_ns = wake_up_time_ns;
	*out_predicted_gpu_time_ns = desired_present_time_ns; // Not quite right but close enough.
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_predicted_display_period_ns = pacer.get_frame_duration();
	return XRT_SUCCESS;
}

xrt_result_t compositor::mark_frame(int64_t frame_id,
                                    xrt_compositor_frame_point point,
                                    int64_t when_ns)
{
	switch (point)
	{
		case XRT_COMPOSITOR_FRAME_POINT_WOKE:
			session.dump_time("wake_up", frame_id, when_ns);
			return XRT_SUCCESS;
		default:
			assert(false);
	}
	return XRT_ERROR_VULKAN;
}

std::array<std::shared_ptr<wivrn::video_encoder>, num_streams> compositor::get_encoders() const
{
	std::lock_guard lock(encoders_mutex);
	return encoders;
}

void compositor::check_encoder_health()
{
	if (not failover_enabled)
		return;

	const int64_t now = os_monotonic_get_ns();
	auto snapshot = get_encoders();
	for (auto [i, encoder]: std::ranges::enumerate_view(snapshot))
	{
		if (not encoder)
			continue;

		// Reported exactly once per encoder, so a stream that cannot be saved
		// costs one line in the log rather than one per frame.
		auto verdict = encoder->watchdog.poll(now);
		if (verdict)
			fail_over_encoder(i, verdict->reason);
	}
}

bool compositor::fail_over_encoder(size_t idx, const std::string & reason)
{
	auto conf = settings[idx];

	U_LOG_E("Stream %zu: %s encoder (%s) written off — %s",
	        idx,
	        conf.encoder_name.c_str(),
	        std::string(magic_enum::enum_name(conf.codec)).c_str(),
	        reason.c_str());

	// The headset's decoder was created once, from the codec in the stream
	// description, and there is no way to change it that does not tear down every
	// decoder on the headset — a path that today only ever runs after a reconnect,
	// and that leaves the stream black unless the encoders are reset with it. So
	// the swap is only ever within one codec: a hardware H.264 stream becomes an
	// x264 stream and the decoder never notices, because the new encoder starts on
	// an IDR carrying its own parameter sets and nothing on the wire says which
	// encoder produced a shard.
	if (conf.codec != video_codec::h264)
	{
		U_LOG_E("Stream %zu stays down: the headset decodes %s on this stream and the software encoder "
		        "only produces H.264. Changing codec needs the headset to build new decoders, which only "
		        "happens on a reconnect — reconnect the headset to recover, or configure the H.264 codec "
		        "to have the software fallback available.",
		        idx,
		        std::string(magic_enum::enum_name(conf.codec)).c_str());
		return false;
	}
	// Same story one level down: the compositor's images are 16 bit per sample in a
	// 10-bit session and the software encoder only reads 8.
	if (conf.bit_depth != 8)
	{
		U_LOG_E("Stream %zu stays down: the session is %d-bit and the software encoder only does 8-bit. "
		        "Reconnect the headset to recover.",
		        idx,
		        conf.bit_depth);
		return false;
	}

	conf.encoder_name = encoder_x264;

	std::shared_ptr<video_encoder> replacement;
	const int64_t begin = os_monotonic_get_ns();
	try
	{
		replacement = video_encoder::create(vk, conf, idx);
	}
	catch (std::exception & e)
	{
		U_LOG_E("Stream %zu stays down: the software encoder could not be created either: %s", idx, e.what());
		return false;
	}

	// Carry the live state over. The bitrate is the one the controller has walked
	// to, not the one the session started with, and it is left exactly where it
	// was: x264 at full eye resolution is expensive in CPU, not in bandwidth, and
	// dropping the bitrate on top of losing the hardware encoder would make the
	// picture worse for no reason.
	uint64_t encoder_bitrate = conf.bitrate;
	if (auto & old = encoders[idx]; old)
	{
		if (auto bitrate = old->get_bitrate())
		{
			replacement->set_bitrate(bitrate);
			encoder_bitrate = bitrate * conf.bitrate_multiplier;
		}
	}
	replacement->set_framerate(frame_rate);
	replacement->set_pacing(pacing_enabled, pacing_window);
	replacement->set_fec(fec_enabled);
	replacement->set_fec_adaptive(fec_adaptive_enabled);
	replacement->set_shard_retransmit(retransmit_enabled);
	// x264 has a refresh mechanism of its own, and `conf` carries the same intra_refresh
	// the failed encoder was built from, so the replacement configures one; this hands it
	// whatever the live half of the switch has since become.
	replacement->set_intra_refresh(intra_refresh_enabled);
	// x264 has no reference invalidation call (see video_encoder_x264.cpp), so the
	// replacement never configures one and this only records the live half for it. Said
	// anyway rather than left out: the replacement must end up in the same state as any
	// other encoder, whatever a future backend does with it.
	replacement->set_ref_invalidation(ref_invalidation_enabled);
	replacement->watchdog.set_enabled(failover_enabled);
	// A fresh IDR handler already asks for a keyframe on its first frame; say so
	// rather than leaving it implicit. The headset needs it: everything it holds
	// for this stream was produced by an encoder that no longer exists, and a P
	// frame against those references would be undecodable.
	replacement->reset();

	{
		std::lock_guard lock(encoders_mutex);
		retired_encoders.push_back(std::move(encoders[idx]));
		encoders[idx] = replacement;
	}
	// Only ever read by this thread once the session is up (print_encoders runs in
	// the constructor), and the codec is unchanged, so the stream description the
	// headset already has stays correct.
	settings[idx] = conf;
	software_fallback = true;
	software_fallback_var = true;
	software_fallback_mask |= uint8_t(1u << idx);

	U_LOG_W("Stream %zu is now on the software encoder (x264, %ux%u, %.1f Mbit/s), built in %" PRId64 " ms. "
	        "It starts on a keyframe, so the picture comes back within a frame or two. Expect a heavier CPU "
	        "load; the hardware encoder is not tried again before the headset reconnects.",
	        idx,
	        conf.width,
	        conf.height,
	        encoder_bitrate / 1e6,
	        (os_monotonic_get_ns() - begin) / 1'000'000);

	return true;
}

std::optional<compositor::promoted_quad> compositor::select_quad_layer(int64_t display_time_ns)
{
	// Read without the lock: only this thread ever replaces an encoder.
	// No encoder for it (the headset did not ask when the session was set up), or
	// the headset has since turned the feature off: everything stays in the squash.
	if (not quad or not encoders[quad_stream_idx] or not session.get_settings()->quad_layers)
		return std::nullopt;

	beman::inplace_vector::inplace_vector<int, XRT_MAX_LAYERS> candidates;
	for (uint32_t i = 0; i < layer_accum.layer_count and candidates.size() < candidates.capacity(); ++i)
	{
		if (quad_promotable(layer_accum.layers[i].data, quad_allow_blended))
			candidates.push_back(i);
	}

	if (candidates.empty())
		return std::nullopt;

	// Head pose, to weigh the candidates by how much of the view they take up. A
	// head locked layer is already expressed relative to the head.
	xrt_space_relation head_rel = XRT_SPACE_RELATION_ZERO;
	{
		std::array<xrt_fov, XRT_MAX_VIEWS> fovs;
		std::array<xrt_pose, XRT_MAX_VIEWS> poses;
		session.get_hmd().get_view_poses(
		        nullptr,
		        display_time_ns,
		        XRT_VIEW_TYPE_STEREO,
		        2,
		        &head_rel,
		        fovs.data(),
		        poses.data());
	}
	const xrt_pose identity = XRT_POSE_IDENTITY;

	int best = -1;
	float best_angle = 0;
	for (int i: candidates)
	{
		const auto & data = layer_accum.layers[i].data;
		const auto & head = is_layer_view_space(&data) ? identity : head_rel.pose;
		const float angle = quad_solid_angle(data.quad, head);

		// Hysteresis on the layer that was promoted last time: switching costs a
		// key frame, so a challenger has to be clearly bigger, not just bigger.
		const bool was_promoted = comp_layer_get_swapchain(&layer_accum.layers[i], 0) == quad_last_swapchain;
		const float weighted = was_promoted ? angle * 1.25f : angle;

		if (weighted > best_angle)
		{
			best_angle = weighted;
			best = i;
		}
	}

	if (best < 0 or best_angle <= 0)
		return std::nullopt;

	const auto & layer = layer_accum.layers[best];
	const auto & data = layer.data;
	const auto & q = data.quad;

	auto * swapchain = reinterpret_cast<struct comp_swapchain *>(comp_layer_get_swapchain(&layer, 0));
	const auto & image = swapchain->images[q.sub.image_index];

	xrt_normalized_rect src_rect{};
	// invert_flip is false here, unlike the squasher's quad path: what comes out
	// of this is sampled straight into an image the headset hands to the runtime
	// as a quad layer, so the source rectangle is read in texture order, top row
	// first, and a bottom up (OpenGL) source is what needs turning around.
	set_post_transform_rect(&data, &q.sub.norm_rect, false, &src_rect);

	return promoted_quad{
	        .layer_index = best,
	        .view = get_image_view(&image, data.flags, q.sub.array_index),
	        .src_rect = src_rect,
	        .src_width = swapchain->vkic.info.width,
	        .src_height = swapchain->vkic.info.height,
	        .aspect = q.size.x / q.size.y,
	        .swapchain = swapchain,
	        .info = {
	                .pose = xrt_cast(q.pose),
	                .size = {.width = q.size.x, .height = q.size.y},
	                .head_locked = is_layer_view_space(&data),
	        },
	};
}

xrt_result_t compositor::layer_commit(xrt_graphics_sync_handle_t sync_handle)
{
	u_graphics_sync_unref(&sync_handle);

	// Before anything is handed to an encoder: an encoder that has been written off
	// is replaced here, so that everything below — the ownership transfer barriers,
	// the presents — already sees the new one, and a wedged encoder thread is never
	// the one asked to notice its own problem.
	check_encoder_health();

	// Move waited frame to rendering frame, clear waited.
	comp_frame_move_and_clear_locked(&frame.rendering, &frame.waited);

	U_LOG_IFL_D(log_level, "frame %ld commit %d layers", frame.rendering.id, layer_accum.layer_count);

	if (encode_request >= 0 // encoders have not picked up the previous frame
	    or not session.connected() or not session.get_offset())
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

	int i = acquire_image();
	if (i < 0)
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->StartFrameCapture(NULL, NULL);
#endif

	auto & view_info = images[i].view_info;
	images[i].frame_index = frame.rendering.id;
	view_info = {
	        .display_time = session.get_offset().to_headset(frame.rendering.predicted_display_time_ns),
	        .alpha = layer_accum.data.env_blend_mode == XRT_BLEND_MODE_ALPHA_BLEND,
	};

	// One overlay layer may be pulled out of the stack and streamed on its own. It
	// is then left out of the squash entirely, which also gives back the encoded
	// field of view an off axis quad would have widened, and can even give back the
	// single projection fast path below.
	const auto promoted = select_quad_layer(frame.rendering.predicted_display_time_ns);
	images[i].quad_info = promoted ? std::optional(promoted->info) : std::nullopt;
	quad_last_swapchain = promoted ? promoted->swapchain : nullptr;

	session.dump_time("begin", frame.rendering.id, os_monotonic_get_ns());

	cmd_pool.reset();
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	cmd.resetQueryPool(*query_pool, 0, 5);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *query_pool, 0);

	// Whether this commit carries a new application frame, and what motion smoothing
	// is going to do about it. Decided here because the server-side warp has to be
	// recorded before the foveation pass reads its output, several steps below.
	motion_begin();

	bool flip_y = false;
	std::array<vk::ImageView, 2> src;
	std::array<xrt_rect, 2> src_rect;
	std::array<xrt_fov, 2> src_fov;

	beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 4> image_barriers;

	// Index of the only layer left once the promoted one is taken out, or -1 if
	// there is not exactly one left.
	int lone_layer = -1;
	if (layer_accum.layer_count == 1 + (promoted ? 1u : 0u))
	{
		lone_layer = 0;
		if (promoted and promoted->layer_index == 0)
			lone_layer = 1;
	}

	// Check if we can pass a layer directly to foveation
	if (lone_layer >= 0 and
	    (layer_accum.layers[lone_layer].data.type == XRT_LAYER_PROJECTION or
	     layer_accum.layers[lone_layer].data.type == XRT_LAYER_PROJECTION_DEPTH))
	{
		const auto & layer = layer_accum.layers[lone_layer];
		for (int view = 0; view < 2; ++view)
		{
			const auto & data = (layer.data.type == XRT_LAYER_PROJECTION ? layer.data.proj.v : layer.data.depth.v)[view];
			src[view] = get_image_view(
			        &get_layer_image(layer, view, data.sub.image_index),
			        layer.data.flags,
			        data.sub.array_index);
			src_rect[view] = data.sub.rect;
			src_fov[view] = data.fov;
			flip_y = layer.data.flip_y;
			view_info.pose[view] = xrt_cast(data.pose);
			view_info.fov[view] = xrt_cast(data.fov);
		}
	}
	else
	{
		// no fast-path, squash layers
		std::array<xrt_pose, 2> poses;
		const auto extent = images[0].image.info().extent;
		std::tie(poses, src_fov, src_rect) = squasher.do_layers(
		        vk.device,
		        cmd,
		        session.get_hmd(),
		        pacer.get_frame_duration(),
		        frame.rendering,
		        layer_accum,
		        xrt_rect{.extent{.w = int(extent.width), .h = int(extent.height)}},
		        promoted ? promoted->layer_index : -1);

		src = squasher.get_views();

		for (int view = 0; view < 2; ++view)
		{
			view_info.pose[view] = xrt_cast(poses[view]);
			view_info.fov[view] = xrt_cast(src_fov[view]);
		}

		image_barriers.push_back(
		        vk::ImageMemoryBarrier2{
		                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		                .oldLayout = vk::ImageLayout::eGeneral,
		                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		                .image = squasher.get_image(),
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 2,
		                },
		        });
	}

	image_barriers.push_back(
	        vk::ImageMemoryBarrier2{
	                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	                .oldLayout = vk::ImageLayout::eUndefined,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = images[i].image,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = images[i].image.info().arrayLayers,
	                },
	        });

	if (promoted)
	{
		image_barriers.push_back(
		        vk::ImageMemoryBarrier2{
		                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		                .oldLayout = vk::ImageLayout::eUndefined,
		                .newLayout = vk::ImageLayout::eGeneral,
		                .image = quad->image(i),
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 1,
		                },
		        });
	}

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	image_barriers.clear();

	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 1);

	// Server-side motion smoothing. The estimator below still needs the live
	// composited views, so what it is given is kept aside before the warp rewrites
	// them: on a warped commit src points at the warp output instead, and everything
	// downstream — foveation, the encoders, the desktop mirror — sees the synthesized
	// frame, which is the frame being sent.
	const auto live_src = src;
	const auto live_src_rect = src_rect;
	const bool live_flip_y = flip_y;

	motion_warp_commit(src, src_rect, src_fov, flip_y, view_info, promoted ? promoted->swapchain : nullptr);

	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 2);

	if (session.get_info().eye_gaze)
	{
		auto now = os_monotonic_get_ns();
		session.add_tracking_request(device_id::EYE_GAZE, frame.rendering.desired_present_time_ns, now, now);
	}
	// Foveation v2: refresh the curve shape (base "sharper center" setting plus any adaptive
	// bump) before foveating. Recomputed per frame with no encode-size change, so it is live.
	update_foveation_shape();
	view_info.foveation = foveation.foveate(
	        vk.device,
	        cmd,
	        images[i].view_y,
	        images[i].view_cbcr,
	        flip_y,
	        src,
	        src_rect,
	        src_fov,
	        view_info.alpha);

	// The promoted layer goes to its own image, at its own resolution, with none of
	// the foveation the eye images get.
	if (promoted)
	{
		const auto filled = quad->convert(
		        vk.device,
		        cmd,
		        i,
		        promoted->view,
		        promoted->src_rect,
		        promoted->aspect,
		        promoted->src_width,
		        promoted->src_height);

		images[i].quad_info->source = {
		        .offset = {filled.offset.w, filled.offset.h},
		        .extent = {filled.extent.w, filled.extent.h},
		};
	}

	for (auto & encoder: encoders)
	{
		if (not encoder)
			continue;
		if (encoder->stream_idx == 2 and not view_info.alpha)
			continue;
		else if (encoder->stream_idx == quad_stream_idx and not promoted)
			continue;
		else if (encoder->need_transfer or encoder->target_queue == vk.queue.family_index)
		{
			image_barriers.push_back(
			        vk::ImageMemoryBarrier2{
			                .srcStageMask = vk::PipelineStageFlagBits2KHR::eComputeShader,
			                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
			                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
			                // For a queue-family ownership transfer in EXCLUSIVE
			                // sharing mode, the release barrier's old/new layout
			                // must match the encoder-side acquire. newLayout is
			                // the encoder's target_layout; per the VkImageMemoryBarrier2
			                // spec the layout transition is executed exactly once
			                // between the queues, so this single QFOT barrier covers
			                // both the queue-family transfer and the layout
			                // transition the encoder needs.
			                .oldLayout = vk::ImageLayout::eGeneral,
			                .newLayout = encoder->target_layout,
			                .srcQueueFamilyIndex = vk.queue.family_index,
			                .dstQueueFamilyIndex = encoder->target_queue,
			                .image = encoder->stream_idx == quad_stream_idx ? quad->image(i) : vk::Image(images[i].image),
			                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
			                                     .baseMipLevel = 0,
			                                     .levelCount = 1,
			                                     .baseArrayLayer = encoder->src_layer,
			                                     .layerCount = 1},
			        });
		}
	}

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 3);

	// Motion estimation, always against the live composited views: the field
	// describes what the application drew, never what was synthesized from it. They
	// are still in eShaderReadOnlyOptimal here — the barriers above only concern the
	// encoders' copy of the composited image.
	update_motion_field(view_info.display_time, images[i].frame_index, live_src, live_src_rect, live_flip_y);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 4);

	bool mirrored = false;
#if WIVRN_USE_PIPEWIRE
	// Desktop mirror: resample the left eye, as it is just before foveation, into
	// a readback buffer. src[0] is still in eShaderReadOnlyOptimal and stays valid
	// until this submission completes, which is waited on below.
	if (mirror)
		mirrored = mirror->capture(
		        cmd,
		        {
		                .view = src[0],
		                .x = src_rect[0].offset.w,
		                .y = src_rect[0].offset.h,
		                .width = src_rect[0].extent.w,
		                .height = src_rect[0].extent.h,
		                .flip_y = flip_y,
		        });
#endif

	cmd.end();

	const vk::SemaphoreSubmitInfo sem_info{
	        .semaphore = *sem,
	        .value = ++sem_value,
	        .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
	};

	{
		vk::CommandBufferSubmitInfo cmd_info{
		        .commandBuffer = cmd,
		};
		std::unique_lock lock{vk.queue.mutex};
		vk.queue.queue.submit2(vk::SubmitInfo2{
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmd_info,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &sem_info,
		});
	}

#if WIVRN_USE_PIPEWIRE
	// Hand the capture to the mirror reader thread, it waits on the semaphore
	if (mirrored)
		mirror->submitted(*sem, sem_info.value);
#else
	(void)mirrored;
#endif

	pacer.mark_timing_point(COMP_TARGET_TIMING_POINT_SUBMIT_END, frame.rendering.id, os_monotonic_get_ns());
	auto info = pacer.present_to_info(frame.rendering.desired_present_time_ns);

	for (auto & encoder: encoders)
	{
		if (not encoder)
			continue;
		if (encoder->stream_idx == 2 and not view_info.alpha)
			continue;
		if (encoder->stream_idx == quad_stream_idx)
		{
			if (not promoted)
				continue;
			encoder->present_image(quad->image(i), sem_info, info.frame_id, view_info);
			continue;
		}
		encoder->present_image(
		        images[i].image,
		        sem_info,
		        info.frame_id,
		        view_info);
	}

	auto j = encode_request.exchange(i);
	encode_request.notify_all();
	assert(j == -1);

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->EndFrameCapture(NULL, NULL);
#endif

	comp_frame_clear_locked(&frame.rendering);

	if (vk.device.waitSemaphores(vk::SemaphoreWaitInfo{
	                                     .semaphoreCount = 1,
	                                     .pSemaphores = &*sem,
	                                     .pValues = &sem_info.value,
	                             },
	                             U_TIME_1S_IN_NS) == vk::Result::eTimeout)
	{
		U_LOG_IFL_W(log_level, "compositor timeout");
		// The submission may still be running, and it may hold estimator work.
		// Freeing the estimator, or recording more work into it, would be a use
		// after free; update_motion_field leaves it alone while this is set.
		motion_unsafe = true;
		motion_pending = false;
	}
	else
	{
		motion_unsafe = false;

		auto [res, ts] = query_pool.getResults<uint64_t>(
		        0,
		        5,
		        5 * sizeof(uint64_t),
		        sizeof(uint64_t),
		        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);

		if (res == vk::Result::eSuccess)
		{
			static const auto period = vk.physical_device.getProperties().limits.timestampPeriod;
			squasher_times.add((ts[1] - ts[0]) * period / 1e3);
			motion_warp_times.add((ts[2] - ts[1]) * period / 1e3);
			foveation_times.add((ts[3] - ts[2]) * period / 1e3);
			motion_times.add((ts[4] - ts[3]) * period / 1e3);
		}

		// The estimator wrote to host visible memory, made visible by the wait
		// above. Sending is a few kilobytes on the stream socket.
		send_motion_field();
	}

	// Now is a good point to garbage collect.
	comp_swapchain_shared_garbage_collect(&cscs);

	return XRT_SUCCESS;
}

void compositor::drop_retained_frame()
{
	motion_warp.reset();
	motion_retained = false;
	motion_retained_prev = false;
	motion_retained_field = false;
	motion_retained_display_time = 0;
	motion_retained_quad = nullptr;
}

void compositor::motion_begin()
{
	const uint64_t fingerprint = layer_fingerprint(layer_accum);
	motion_new_frame = fingerprint != last_layer_fingerprint;
	last_layer_fingerprint = fingerprint;

	app_frame_ratio += ((motion_new_frame ? 1.f : 0.f) - app_frame_ratio) * motion_ratio_filter;
	if (app_frame_ratio < motion_enter_ratio)
		app_behind = true;
	else if (app_frame_ratio > motion_leave_ratio)
		app_behind = false;

	motion_mode_now = motion_mode::off;
	motion_server_warping = false;

	// A previous submission timed out and may still be executing the estimator's
	// pipelines against its pyramids, or the warper's against its images. Record
	// nothing into either and, above all, destroy neither, until a wait has succeeded
	// again. The retained frame is left alone too: it is only read by a warp, and no
	// warp is recorded while this is set.
	if (motion_unsafe)
		return;

	const motion_mode wanted = effective_motion_mode(*session.get_settings());

	if (wanted == motion_mode::off or not app_behind or motion_failed)
	{
		if (motion)
		{
			U_LOG_IFL_I(log_level, "Motion smoothing idle");
			motion.reset();
		}
		motion_previous_display_time = 0;
		drop_retained_frame();
		return;
	}

	if (not motion)
	{
		try
		{
			motion = std::make_unique<motion_estimator>(
			        vk,
			        vk::Extent2D{
			                .width = session.get_info().render_eye_width,
			                .height = session.get_info().render_eye_height,
			        });
			U_LOG_IFL_I(log_level,
			            "Motion smoothing active, %ux%u vectors per eye, %zu kiB of device memory",
			            motion->grid_width(),
			            motion->grid_height(),
			            motion->device_memory() / 1024);
		}
		catch (std::exception & e)
		{
			U_LOG_IFL_W(log_level, "Motion estimator creation failed, motion smoothing disabled: %s", e.what());
			// Whatever made the creation fail will not fix itself, and retrying
			// on the next commit would flood the log: hold the feature off for
			// the rest of the session.
			motion_failed = true;
			drop_retained_frame();
			return;
		}
		motion_previous_display_time = 0;
	}

	motion_mode_now = motion_mode::headset;

	// The warper is only worth its fifty odd megabytes in server mode and only while
	// the application is behind, which is exactly when the estimator exists.
	if (wanted != motion_mode::server or motion_warp_failed)
	{
		drop_retained_frame();
	}
	else if (motion_warp)
	{
		motion_mode_now = motion_mode::server;
	}
	else
	{
		try
		{
			motion_warp = std::make_unique<motion_warper>(
			        vk,
			        vk::Extent2D{
			                .width = session.get_info().render_eye_width,
			                .height = session.get_info().render_eye_height,
			        },
			        vk::Extent2D{motion->grid_width(), motion->grid_height()});
			U_LOG_IFL_I(log_level,
			            "Motion smoothing warping on the server, %zu MiB of device memory",
			            motion_warp->device_memory() / (1024 * 1024));
			motion_mode_now = motion_mode::server;
		}
		catch (std::exception & e)
		{
			// Same reasoning as the estimator, with a softer landing: the
			// headset-side warp still works, so fall back to it for the session
			// rather than losing the feature.
			U_LOG_IFL_W(log_level,
			            "Server side motion warping unavailable, falling back to the headset: %s",
			            e.what());
			motion_warp_failed = true;
			drop_retained_frame();
		}
	}

	motion_server_warping = motion_mode_now == motion_mode::server;
}

void compositor::motion_warp_commit(
        std::array<vk::ImageView, 2> & src,
        std::array<xrt_rect, 2> & src_rect,
        std::array<xrt_fov, 2> & src_fov,
        bool & flip_y,
        to_headset::video_stream_data_shard::view_info_t & view_info,
        const void * promoted_swapchain)
{
	if (motion_mode_now != motion_mode::server)
		return;

	if (motion_new_frame)
	{
		// Keep the frame as it was composited. It is encoded live, unwarped: a
		// real frame is never anything but itself.
		motion_warp->retain(vk.device, cmd, src, src_rect, flip_y);
		// The field the estimator produces next spans the previously retained frame
		// to this one, so keep that frame's pose/fov as the far end of the interval
		// before this frame overwrites them; a later duplicate extrapolates along it.
		// On the very first retained frame there is no previous one to keep.
		if (motion_retained)
		{
			motion_retained_prev_pose = motion_retained_pose;
			motion_retained_prev_fov = motion_retained_fov;
			motion_retained_prev = true;
		}
		else
		{
			motion_retained_prev = false;
		}
		motion_retained = true;
		motion_retained_display_time = view_info.display_time;
		motion_retained_pose = view_info.pose;
		motion_retained_fov = view_info.fov;
		motion_retained_src_fov = src_fov;
		motion_retained_quad = promoted_swapchain;
		// Whether a field starting at this frame exists is up to the estimator,
		// which runs further down this same commit.
		motion_retained_field = false;
		return;
	}

	// Nothing to move, nothing to move it along, or a layer stack that no longer
	// matches how the retained frame was composited — the promoted quad is not in it,
	// so promoting a different one would make the retained image the wrong picture.
	if (not motion_retained or not motion_retained_field or motion_retained_quad != promoted_swapchain)
		return;

	const float t = motion_warp_step(
	        view_info.display_time,
	        motion_retained_display_time,
	        motion_retained_span,
	        MOTION_MAX_STEPS);
	if (t <= 0)
		return;

	motion_warp->warp(vk.device, cmd, motion->vectors(), t);

	// From here on this commit carries the synthesized frame, and has to describe it.
	src = motion_warp->output_views();
	src_rect = motion_warp->output_rect();
	src_fov = motion_retained_src_fov;
	flip_y = false;

	// The picture is the retained application frame advanced along the motion field to
	// this commit's instant: the field spans the previous real frame -> the retained
	// one, and the warp read the retained image t of those intervals forward. The
	// field is optical flow between two composited eye views rendered from two
	// different head poses, so it already carries the head-motion component. Freezing
	// the submitted pose to the retained frame would make the runtime's timewarp apply
	// that head motion a second time, over an interval that beats against the warp's t
	// frame to frame -> head-correlated jitter. So advance the pose the same t past the
	// retained frame (u = 1 + t along the prev -> retained segment); the runtime's
	// timewarp then only closes the small residual to actual scanout.
	if (motion_retained_prev)
	{
		const float u = 1.f + t;
		for (size_t view = 0; view < 2; ++view)
		{
			const XrPosef pose = pose_extrapolate(motion_retained_prev_pose[view], motion_retained_pose[view], u);
			const XrFovf fov = fov_extrapolate(motion_retained_prev_fov[view], motion_retained_fov[view], u);
			// -ffast-math: a non-finite or degenerate extrapolation must never reach
			// the encoder or the headset. Fall back to the frozen retained value.
			view_info.pose[view] = (is_finite(pose) and is_valid_orientation(pose.orientation))
			                               ? pose
			                               : motion_retained_pose[view];
			view_info.fov[view] = is_finite(fov) ? fov : motion_retained_fov[view];
		}
	}
	else
	{
		// No previous real frame yet (first frame after a (re)start or reset): keep the
		// frozen retained pose, the behaviour before this fix.
		view_info.pose = motion_retained_pose;
		view_info.fov = motion_retained_fov;
	}
}

void compositor::update_motion_field(
        XrTime display_time,
        uint64_t frame_index,
        std::array<vk::ImageView, 2> src,
        std::array<xrt_rect, 2> src_rect,
        bool flip_y)
{
	motion_pending = false;

	if (motion_mode_now == motion_mode::off or not motion_new_frame)
		return;

	const XrTime span = display_time - motion_previous_display_time;
	const bool usable = motion_previous_display_time != 0 and span > 0 and span < motion_max_span;
	// In server mode nothing goes on the wire and the vectors are consumed on the
	// GPU, so the copy into host visible memory, and the send that follows it, are
	// both skipped.
	const bool send_field = motion_mode_now == motion_mode::headset;

	if (motion->estimate(vk.device, cmd, src, src_rect, flip_y, send_field) and usable)
	{
		if (send_field)
		{
			motion_pending = true;
			motion_frame_index = frame_index;
			motion_span = span;
		}
		else
		{
			motion_retained_field = true;
			motion_retained_span = span;
		}
	}

	motion_previous_display_time = display_time;
}

void compositor::send_motion_field()
{
	if (not motion_pending)
		return;
	motion_pending = false;

	try
	{
		auto field = motion->read_back();
		field.frame_idx = motion_frame_index;
		field.span_ns = motion_span;

		// A whole field is larger than a datagram, so it goes out as several
		// chunks, each carrying the full header.
		for (auto & chunk: split_motion_field(field))
			session.send_stream(std::move(chunk));
	}
	catch (std::exception & e)
	{
		U_LOG_IFL_D(log_level, "Failed to send motion field: %s", e.what());
	}
}

xrt_result_t compositor::get_display_refresh_rate(float * hz)
{
	auto settings = session.get_settings();
	// there should not be rounding errors, hz must be one of the available refresh rates
	*hz = frame_rate * settings->fps_divider;
	assert(std::ranges::contains(session.get_info().available_refresh_rates, *hz));
	return XRT_SUCCESS;
}

xrt_result_t compositor::request_display_refresh_rate(float hz)
{
	requested_refresh_rate = hz;
	U_LOG_I("request refresh rate: %fHz", hz);
	if (hz > 0)
	{
		try
		{
			session.send_control(to_headset::refresh_rate_change{.hz = hz});
		}
		catch (std::exception & e)
		{
			U_LOG_W("refresh rate change failed: %s", e.what());
		}
	}
	return XRT_SUCCESS;
}

xrt_result_t compositor::get_view_config(
        xrt_compositor_native * self,
        xrt_view_type view_type,
        xrt_view_config * out_view_config)
{
	const auto & session = reinterpret_cast<compositor *>(self)->session;
	const auto extent = render_extent(session.get_info());
	switch (view_type)
	{
		case XRT_VIEW_TYPE_MONO:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
		case XRT_VIEW_TYPE_STEREO:
			*out_view_config = {
			        .view_type = XRT_VIEW_TYPE_STEREO,
			        .view_count = 2,
			        .views = {}};
			for (auto & view: std::span(out_view_config->views, out_view_config->view_count))
			{
				view = {
				        .recommended = {
				                .width_pixels = extent.width,
				                .height_pixels = extent.height,
				                .sample_count = 1,
				        },
				        .max = {
				                .width_pixels = extent.width * 2u,
				                .height_pixels = extent.height * 2u,
				                .sample_count = 1,
				        },
				};
			}
			return XRT_SUCCESS;
		case XRT_VIEW_TYPE_QUAD:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
	}
	return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
}

int compositor::acquire_image()
{
	for (auto [i, image]: std::ranges::enumerate_view(images))
	{
		if (not image.busy.exchange(true))
			return i;
	}
	return -1;
}

void compositor::encoder_work(std::stop_token tok)
{
	while (not tok.stop_requested())
	{
		auto req = encode_request.exchange(-1);
		if (req < 0)
		{
			encode_request.wait(req);
			wivrn::trace::cpu_instant(wivrn::trace::cpu_track::compositor, "encoder_work wake", 0, 0);
			continue;
		}

		assert(req < images.size());
		auto & image = images[req];

		wivrn::trace::scope trace_iter(wivrn::trace::cpu_track::compositor, 0, image.frame_index, "encoder_work iter");

		// One copy for the whole frame: an encoder replaced from the present
		// thread half way through this loop must not change which object the
		// rest of it talks to, and the one it replaced stays alive as long as
		// this copy does.
		for (auto & encoder: get_encoders())
		{
			if (not encoder)
				continue;

			// Per encoder rather than around the loop: one stream's driver
			// giving up must not cost the other eyes their frame as well.
			try
			{
				if (encoder->stream_idx == quad_stream_idx)
				{
					if (not image.quad_info)
						continue;
					// Same frame index and same view info as the eyes,
					// with the quad's placement attached and the eye
					// foveation dropped: nothing on this stream is
					// foveated and the headset never reads it there.
					auto view_info = image.view_info;
					view_info.foveation = {};
					view_info.quad = image.quad_info;
					encoder->encode(session, view_info, image.frame_index);
				}
				else if (encoder->stream_idx < 2 or image.view_info.alpha)
					encoder->encode(session, image.view_info, image.frame_index);
			}
			catch (std::exception & e)
			{
				// The watchdog has the same news, and it is the one that
				// decides what to do about it.
				U_LOG_W("encode error on stream %d: %s", encoder->stream_idx, e.what());
			}
		}
		image.busy = false;
	}
}

void compositor::send_video_stream_description()

{
	to_headset::video_stream_description desc{
	        .width = uint16_t(images[0].image.info().extent.width),
	        .height = uint16_t(images[0].image.info().extent.height),
	        .frame_rate = settings[0].fps,
	};
	get_display_refresh_rate(&desc.refresh_rate);
	static_assert(std::tuple_size_v<decltype(settings)> == std::tuple_size_v<decltype(desc.codec)>);
	std::ranges::transform(settings, desc.codec.begin(), &encoder_settings::codec);
	// Both eyes on stream 0, when the NX Warp encoder paired them. Stream 1 keeps
	// its codec entry -- the view-to-stream mapping is unchanged -- but
	// stream_size() then reports it as zero and the headset builds no decoder for
	// a stream that will never receive a datagram.
	desc.paired_eyes = uint8_t(settings[0].eyes ? settings[0].eyes : 1);
	// What each stream IS, so the client routes by role instead of by index. The
	// defaults in the packet are the old positional rule, and every stream except
	// a hybrid base layer still carries exactly that, so this is a no-op for any
	// session that has not enabled one. A disabled stream keeps its default role;
	// its stream_size() is zero and the client builds no decoder for it either way.
	static_assert(std::tuple_size_v<decltype(settings)> == std::tuple_size_v<decltype(desc.role)>);
	for (auto [i, s]: std::ranges::enumerate_view(settings))
	{
		if (not s.enabled)
			continue;
		desc.role[i] = s.role;
		desc.serves_stream[i] = s.serves_stream;
	}
	// Zero unless a quad layer stream exists, which is how the headset knows whether
	// to create a decoder for it at all.
	if (quad)
	{
		desc.quad_width = uint16_t(quad->extent().width);
		desc.quad_height = uint16_t(quad->extent().height);
	}
	session.send_control(std::move(desc));
}

compositor::compositor(wivrn_session & session) :
        comp_base{
                .base = {
                        .base = {
                                .info{},
                                .begin_session = method_pointer<&compositor::begin_session>,
                                .end_session = method_pointer<&compositor::end_session>,
                                .predict_frame = method_pointer<&compositor::predict_frame>,
                                .mark_frame = method_pointer<&compositor::mark_frame>,
                                .begin_frame = method_pointer<&compositor::begin_frame>,
                                .discard_frame = method_pointer<&compositor::discard_frame>,
                                .layer_commit = method_pointer<&compositor::layer_commit>,
                                .get_display_refresh_rate = method_pointer<&compositor::get_display_refresh_rate>,
                                .request_display_refresh_rate = method_pointer<&compositor::request_display_refresh_rate>,
                                .destroy = method_pointer<&compositor::destroy>,
                        },
                },
        },
        log_level(debug_get_log_option_log()),
        session(session),
        cmd_pool(vk.device, vk::CommandPoolCreateInfo{
                                    .flags = vk::CommandPoolCreateFlagBits::eTransient,
                                    .queueFamilyIndex = vk.queue.family_index,
                            }),
        query_pool(vk.device, vk::QueryPoolCreateInfo{
                                      .queryType = vk::QueryType::eTimestamp,
                                      .queryCount = 5,
                              }),
        settings(get_encoder_settings(vk, session)),
        images{make_images(vk, cmd_pool, settings)},
        cmd{std::move(vk.device.allocateCommandBuffers({.commandPool = *cmd_pool, .commandBufferCount = 1})[0])},
        sem{make_semaphore(vk)},
        frame_rate(settings[0].fps),
        pacer(U_TIME_1S_IN_NS / frame_rate),
        squasher(vk, render_extent(session.get_info())),
        foveation(vk, images[0].image.info().extent)
{
	comp_base * c_base = this;
	// Ensure we can safely cast pointers
	assert(intptr_t(&base) == intptr_t(this));
	auto res = vk_init_from_given(
	        &c_base->vk,
	        (PFN_vkGetInstanceProcAddr)vk.instance.getProcAddr("vkGetInstanceProcAddr"),
	        *vk.instance,
	        *vk.physical_device,
	        *vk.device,
	        vk.queue.family_index,
	        0,
	        vk.has_device_ext(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME),
	        vk.has_device_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME),
	        std::get<vk::PhysicalDeviceVulkan12Features>(vk.feat).timelineSemaphore,
	        true, // in Vulkan 1.2
	        vk.has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	        log_level);
	vk::detail::resultCheck(vk::Result(res), "vk_init_from_given");

	c_base->vk.version = vk_bundle::api_version;
	// vk_init_from_given can't enable calibrated timestamps; do it here.
#ifdef VK_EXT_calibrated_timestamps
	c_base->vk.has_EXT_calibrated_timestamps = vk.has_device_ext(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif

	// Share monado's vk_bundle so gpu_timestamp_pool reuses its calibration cache.
	wivrn::trace::set_calibration_source(&c_base->vk);

	// vk_init_from_given assumes a graphics queue was provided
	c_base->vk.graphics_queue = nullptr;

	// Monado submits to the main queue from IPC client threads under
	// main_queue->mutex: our own submissions must take the same lock.
	vk::detail::resultCheck(vk::Result(vk_init_mutex(&c_base->vk)), "vk_init_mutex");
	if (c_base->vk.main_queue)
		vk.queue.mutex.share(c_base->vk.main_queue->mutex.mutex);

	{
		comp_vulkan_formats formats{};
		comp_vulkan_formats_check(&c_base->vk, &formats);
		comp_vulkan_formats_copy_to_info(&formats, &base.base.info);
		comp_vulkan_formats_log(log_level, &formats);
	}

	comp_base_init(this);

	// Tie the lifetimes of swapchains to Vulkan.
	xrt_result_t xret = comp_swapchain_shared_init(&cscs, &c_base->vk);
	if (xret != XRT_SUCCESS)
		throw std::runtime_error("comp_swapchain_shared_init failed");

	print_encoders(settings);
	for (auto [i, settings]: std::ranges::enumerate_view(settings))
	{
		if (not settings.enabled or i == quad_stream_idx)
			continue;
		encoders[i] = video_encoder::create(vk, settings, i);
	}

	// The hybrid base layer's tee. A stream whose role is `base` feeds the
	// stream it serves, so that encoder can put base-sourced tiles in its atlas
	// instead of coding them again. Set once, here, because this is the only
	// place that holds every encoder at the same time.
	//
	// Nothing to do for a session with no base layer, which is every session
	// that has not opted in: encoder_settings leaves the role at its default and
	// serves_stream at 0xff.
	for (auto [i, s]: std::ranges::enumerate_view(settings))
	{
		if (not s.enabled or s.role != stream_role::base)
			continue;
		if (s.serves_stream >= num_streams or not encoders[s.serves_stream])
		{
			U_LOG_W("hybrid: stream %u is a base layer serving stream %u, which "
			        "has no encoder -- base-sourced tiles are off",
			        unsigned(i),
			        unsigned(s.serves_stream));
			continue;
		}
		if (encoders[i])
			encoders[i]->set_base_consumer(encoders[s.serves_stream].get());
	}

	// The quad layer stream is a bonus, not a requirement: a fourth encode session
	// is one more than this build has ever asked for and some hardware refuses it.
	// Losing it must cost the panel's sharpness, not the session.
	if (settings[quad_stream_idx].enabled)
	{
		try
		{
			encoders[quad_stream_idx] = video_encoder::create(vk, settings[quad_stream_idx], quad_stream_idx);
			quad = std::make_unique<wivrn::quad_converter>(vk, settings[quad_stream_idx], images.size());
			quad_allow_blended = configuration().quad_layers.allow_blended;

			U_LOG_I("Quad layer streaming enabled, up to %ux%u",
			        quad->extent().width,
			        quad->extent().height);
		}
		catch (std::exception & e)
		{
			U_LOG_W("Quad layer streaming disabled: %s", e.what());
			encoders[quad_stream_idx].reset();
			quad.reset();
			settings[quad_stream_idx].enabled = false;
		}
	}

	send_video_stream_description();

#if WIVRN_USE_PIPEWIRE
	// Desktop mirror, the configuration is only read here: toggling it requires
	// reconnecting the headset.
	if (auto conf = configuration().mirror; conf.enabled)
	{
		const auto extent = render_extent(session.get_info());
		vk::Extent2D mirror_size{
		        std::max<uint32_t>(2, uint32_t(extent.width * conf.scale) & ~1u),
		        std::max<uint32_t>(2, uint32_t(extent.height * conf.scale) & ~1u),
		};
		mirror = pipewire_mirror::create(vk, mirror_size, conf.fps);
	}
#endif

	u_var_add_root(this, "Compositor", false);
	u_var_add_bool(this, &software_fallback_var, "software encoder fallback");
	u_var_add_f32_timing(this, &squasher_times.var, "layers processing");
	u_var_add_f32_timing(this, &motion_warp_times.var, "motion warp");
	u_var_add_f32_timing(this, &foveation_times.var, "foveation");
	u_var_add_f32_timing(this, &motion_times.var, "motion estimation");

	// Start the thread after everything is initialized
	encoder_thread = std::jthread{[&](std::stop_token t) { encoder_work(t); }};
}

compositor::~compositor()
{
	// A submission that timed out (a GPU hang) may still be running: draining it before
	// anything below frees the motion/warp images and pipelines keeps it from reading or
	// writing them after destruction. Errors here (device already lost) are nothing we can
	// act on at teardown, so they are ignored.
	try
	{
		vk.device.waitIdle();
	}
	catch (...)
	{
	}

	u_var_remove_root(this);
	encoder_thread.request_stop();
	encode_request = -2;
	encode_request.notify_all();
	comp_base * c_base = this;
	comp_swapchain_shared_garbage_collect(&cscs);
	comp_swapchain_shared_destroy(&cscs, &c_base->vk);
	comp_base_fini(this);
}

xrt_system_compositor_info compositor::sys_info() const
{
	const auto & info = session.get_info();
	auto [prop, dev_id] = vk.physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
	xrt_system_compositor_info res{
	        .view_type_count = 1,
	        .view_types = {
	                XRT_VIEW_TYPE_STEREO,
	        },
	        .max_layers = squasher.max_layers(prop.properties),
	        .supported_blend_modes = {
	                XRT_BLEND_MODE_OPAQUE,
	                XRT_BLEND_MODE_ALPHA_BLEND,
	        },
	        .supported_blend_mode_count = uint8_t(1 + info.passthrough),
	        .refresh_rate_count = std::min<uint32_t>(info.available_refresh_rates.size(), std::size(res.refresh_rates_hz)),
	        .supports_fov_mutable = true,
	};

	std::ranges::copy(dev_id.deviceUUID, res.compositor_vk_deviceUUID.data);
	std::ranges::copy(dev_id.deviceUUID, res.client_vk_deviceUUID.data);
	std::ranges::copy(std::span(info.available_refresh_rates).subspan(0, res.refresh_rate_count), res.refresh_rates_hz);
	return res;
}

void compositor::set_bitrate(uint32_t bitrate)
{
	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_bitrate(bitrate);
	}
}

void compositor::set_pacing(bool enabled, float window)
{
	window = std::clamp(window, 0.f, shard_pacer::max_window);

	if (pacing_enabled == enabled and pacing_window == window)
		return;

	pacing_enabled = enabled;
	pacing_window = window;

	if (enabled)
		U_LOG_I("Packet pacing enabled, spreading each frame over %.0f%% of a frame period", window * 100);
	else
		U_LOG_I("Packet pacing disabled");

	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_pacing(enabled, window);
	}
}

void compositor::set_fec(bool enabled)
{
	if (fec_enabled == enabled)
		return;

	fec_enabled = enabled;

	if (enabled)
		U_LOG_I("Forward error correction enabled, one parity shard per %d video shards", int(wivrn::fec::group_size));
	else
		U_LOG_I("Forward error correction disabled");

	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_fec(enabled);
	}
}

void compositor::set_fec_adaptive(bool enabled)
{
	if (fec_adaptive_enabled == enabled)
		return;

	fec_adaptive_enabled = enabled;

	if (enabled)
		U_LOG_I("Adaptive error correction enabled, parity ratio between %d+1 and %d+1 with interleaved groups",
		        int(wivrn::fec::clean_group_size),
		        int(wivrn::fec::heavy_group_size));
	else
		U_LOG_I("Adaptive error correction disabled, parity ratio fixed at %d+1", int(wivrn::fec::group_size));

	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_fec_adaptive(enabled);
	}
}

void compositor::set_shard_retransmit(bool enabled)
{
	if (retransmit_enabled == enabled)
		return;

	retransmit_enabled = enabled;

	if (enabled)
		U_LOG_I("Shard retransmission enabled, keeping %zu kB of recently sent shards per stream",
		        wivrn::shard_history::capacity / 1024);
	else
		U_LOG_I("Shard retransmission disabled");

	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_shard_retransmit(enabled);
	}
}

void compositor::collect_retransmits(const from_headset::nack & n,
                                     std::vector<to_headset::video_stream_data_shard> & out)
{
	if (not retransmit_enabled)
		return;

	auto snapshot = get_encoders();
	if (n.stream_index >= snapshot.size() or not snapshot[n.stream_index])
		return;

	snapshot[n.stream_index]->collect_retransmits(n, out);
}

void compositor::on_nxwarp_feedback(const from_headset::nxwarp_feedback & fb)
{
	auto snapshot = get_encoders();
	if (fb.stream_item_idx >= snapshot.size() or not snapshot[fb.stream_item_idx])
		return;

	snapshot[fb.stream_item_idx]->on_nxwarp_feedback(fb.path_id, fb.payload, fb.decode_us,
	                                                 fb.held_base, fb.held_mask);
}

void compositor::on_nxwarp_frame_not_held(const from_headset::nxwarp_frame_not_held & fb)
{
	auto snapshot = get_encoders();
	if (fb.stream_item_idx >= snapshot.size() or not snapshot[fb.stream_item_idx])
		return;

	snapshot[fb.stream_item_idx]->on_nxwarp_frame_not_held(fb.frame_id, fb.why);
}

uint64_t compositor::retransmitted_shards() const
{
	uint64_t total = 0;
	for (const auto & encoder: get_encoders())
	{
		if (encoder)
			total += encoder->retransmitted_shards();
	}
	return total;
}

void compositor::set_intra_refresh(bool enabled)
{
	if (intra_refresh_enabled == enabled)
		return;

	intra_refresh_enabled = enabled;

	if (enabled)
		U_LOG_I("Intra refresh loss recovery enabled, on the streams whose encoder was built with it");
	else
		U_LOG_I("Intra refresh loss recovery disabled, a lost frame now asks for a keyframe again");

	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_intra_refresh(enabled);
	}
}

void compositor::set_ref_invalidation(bool enabled)
{
	if (ref_invalidation_enabled == enabled)
		return;

	ref_invalidation_enabled = enabled;

	if (enabled)
		U_LOG_I("Reference invalidation loss recovery enabled, on the streams whose encoder was built with it");
	else
		U_LOG_I("Reference invalidation loss recovery disabled, a lost frame now goes straight to the intra refresh or the keyframe");

	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_ref_invalidation(enabled);
	}
}

void compositor::set_encoder_failover(bool enabled)
{
	if (failover_enabled == enabled)
		return;

	failover_enabled = enabled;

	if (enabled)
		U_LOG_I("Hardware encoder failover enabled");
	else
		U_LOG_I("Hardware encoder failover disabled, a failing encoder now freezes its stream");

	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->watchdog.set_enabled(enabled);
	}
}

void compositor::set_framerate(float hz)
{
	if (frame_rate.exchange(hz) == hz)
		return;
	U_LOG_IFL_D(log_level, "Framerate change from %.0f to %.0f", frame_rate.load(), hz);
	// frame_rate keeps the normal (commanded) rate so the panel refresh rate reported by
	// get_display_refresh_rate stays correct; the pacer and encoders get the effective rate,
	// which the emergency divider may have halved.
	const float eff = hz / float(emergency_divider.load());
	pacer.set_frame_duration(U_TIME_1S_IN_NS / eff);
	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_framerate(eff);
	}
}

void compositor::set_emergency_framerate(bool active)
{
	const uint32_t divider = active ? 2 : 1;
	if (emergency_divider.exchange(divider) == divider)
		return;

	if (active)
		U_LOG_I("Emergency half-rate engaged: streaming at half the framerate to halve bandwidth");
	else
		U_LOG_I("Emergency half-rate restored: streaming at full framerate again");

	// Re-apply the effective rate through the new divider. frame_rate (the normal rate) is
	// untouched, so the panel refresh rate the application sees does not change.
	const float eff = frame_rate.load() / float(divider);
	pacer.set_frame_duration(U_TIME_1S_IN_NS / eff);
	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->set_framerate(eff);
	}
}

void compositor::update_tracking(const from_headset::tracking & tracking)
{
	foveation.update_tracking(tracking);
}

void compositor::update_foveation_center_override(const from_headset::override_foveation_center & center)
{
	foveation.update_foveation_center_override(center);
}

void compositor::update_foveation_shape()
{
	float base;
	bool adaptive;
	{
		auto s = session.get_settings();
		base = std::clamp(s->foveation_strength, 0.f, 1.f);
		adaptive = s->foveation_adaptive;
	}
	// The effective encode scale: the headset's render_scale already capped by the server's
	// "stream_scale" (see get_encoder_settings). The guardrail bounds the peripheral factor
	// against how much the encode was shrunk, so it has to read the scale the images were
	// actually created at, which is fixed for the life of the session — not the headset's
	// live setting, which cannot resize an encode session anyway.
	const float render_scale = std::clamp(settings[0].encode_scale, 0.f, 1.f);

	// Lever 2: steepen the curve as the automatic bitrate controller backs off its ceiling, so
	// the periphery compresses under a Wi-Fi dip instead of the whole image losing quality.
	// Gated on the automatic bitrate being active — with a fixed bitrate there is no controller
	// state to read. Only ever raises the strength; the base setting is the floor.
	float target = base;
	if (adaptive and session.bitrate_auto_active())
	{
		auto st = session.bitrate_status();
		if (st.ceiling_bps > 0)
		{
			// 1 while riding the ceiling, falling toward 0 as the controller backs off. The
			// 0.85 dead zone leaves the steady-state headroom (BBR cruises a little under the
			// estimate) alone, so foveation only kicks in on a real degradation.
			float ratio = float(st.bitrate_bps) / float(st.ceiling_bps);
			float degrade = std::clamp((0.85f - ratio) / 0.85f, 0.f, 1.f);
			target = std::clamp(base + degrade * (1.f - base), 0.f, 1.f);
		}
	}

	// Slew-limit the pushed strength so the curve never pops frame to frame: a first-order lag
	// toward the target, rising over ~2 s and falling more slowly (~4 s) so a brief dip does not
	// flap the periphery. The first frame snaps straight to the target.
	auto now = std::chrono::steady_clock::now();
	if (foveation_adaptive_state < 0)
	{
		foveation_adaptive_state = target;
	}
	else
	{
		float dt = std::chrono::duration<float>(now - foveation_adaptive_last).count();
		dt = std::clamp(dt, 0.f, 0.5f);
		float tau = target > foveation_adaptive_state ? 2.0f : 4.0f;
		foveation_adaptive_state += (target - foveation_adaptive_state) * std::clamp(dt / tau, 0.f, 1.f);
	}
	foveation_adaptive_last = now;

	// Snap to coarse steps so a slowly drifting lag does not re-quantise the spans every single
	// frame; a real change of one step still applies live on the next frame.
	float pushed = std::round(foveation_adaptive_state / 0.02f) * 0.02f;
	foveation.set_shape(pushed, render_scale);
}

void compositor::resume()
{
	// reset_stream(), not reset(): the headset that comes back from a reconnect is a
	// new client process with a new decoder, and for a codec whose transport keeps
	// per-client state (NX Warp) a keyframe alone leaves it unable to authenticate a
	// single datagram. See video_encoder::reset_stream.
	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->reset_stream();
	}
	// The headset lost whatever it had; the pyramid of the frame before the pause
	// has nothing to do with the one that comes next, and neither does the frame the
	// warper is holding.
	if (motion)
		motion->reset();
	motion_previous_display_time = 0;
	motion_retained = false;
	motion_retained_prev = false;
	motion_retained_field = false;
	send_video_stream_description();
}

void compositor::request_idr()
{
	for (auto & encoder: get_encoders())
	{
		if (encoder)
			encoder->reset();
	}
}

void compositor::on_feedback(const from_headset::feedback & feedback, const clock_offset & o)
{
	uint8_t stream = feedback.stream_index;
	auto snapshot = get_encoders();
	if (stream >= snapshot.size() or not snapshot[stream])
		return;
	snapshot[stream]->on_feedback(feedback);
	if (not o)
		return;
	pacer.on_feedback(feedback, o);
}

} // namespace wivrn
