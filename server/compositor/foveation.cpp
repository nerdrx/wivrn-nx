/*
 * WiVRn VR streaming
 * Copyright (C) 2024  galister <galister@librevr.org>
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "foveation.h"

#include "driver/xrt_cast.h"
#include "is_finite.h"
#include "utils/wivrn_vk_bundle.h"
#include "vk/specialization_constants.h"
#include "wivrn_packets.h"

#include "xrt/xrt_defines.h"
#include "xrt/xrt_limits.h"

#include <array>
#include <cmath>
#include <ranges>
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_structs.hpp>
#include <openxr/openxr.h>

#define RENDER_FOVEATION_BUFFER_DIMENSIONS (4096 + 1)

namespace
{
struct ubo_data
{
	uint32_t x[XRT_MAX_VIEWS * RENDER_FOVEATION_BUFFER_DIMENSIONS];
	uint32_t y[XRT_MAX_VIEWS * RENDER_FOVEATION_BUFFER_DIMENSIONS];
};

vk::raii::Sampler make_sampler(wivrn::vk_bundle & vk)
{
	vk::raii::Sampler res(
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eLinear,
	                .minFilter = vk::Filter::eLinear,
	                .mipmapMode = vk::SamplerMipmapMode::eLinear,
	                .addressModeU = vk::SamplerAddressMode::eClampToBorder,
	                .addressModeV = vk::SamplerAddressMode::eClampToBorder,
	                .addressModeW = vk::SamplerAddressMode::eClampToBorder,
	                .borderColor = vk::BorderColor::eFloatOpaqueBlack,
	        });
	vk.name(res, "foveation sampler");
	return res;
}

vk::raii::DescriptorSetLayout make_ds_layout(wivrn::vk_bundle & vk)
{
	std::array bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 2,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eStorageBuffer,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 3,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	vk::raii::DescriptorSetLayout res{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = bindings.size(),
	                .pBindings = bindings.data(),
	        },
	};
	vk.name(*res, "foveation descriptor set layout");
	return res;
}

vk::raii::PipelineLayout make_layout(wivrn::vk_bundle & vk, vk::DescriptorSetLayout ds_layout)
{
	vk::raii::PipelineLayout res(vk.device,
	                             vk::PipelineLayoutCreateInfo{
	                                     .setLayoutCount = 1,
	                                     .pSetLayouts = &ds_layout,
	                             });
	vk.name(*res, "foveation pipeline layout");
	return res;
}

std::array<vk::raii::Pipeline, 2> make_pipelines(wivrn::vk_bundle & vk, vk::PipelineLayout layout, int32_t alpha_width)
{
	auto shader = vk.load_shader("foveation");
	auto spc = make_specialization_constants(alpha_width);
	std::array res{
	        vk::raii::Pipeline{
	                vk.device,
	                nullptr,
	                vk::ComputePipelineCreateInfo{
	                        .stage = {
	                                .stage = vk::ShaderStageFlagBits::eCompute,
	                                .module = *shader,
	                                .pName = "main",
	                        },
	                        .layout = layout,
	                },
	        },
	        vk::raii::Pipeline{
	                vk.device,
	                nullptr,
	                vk::ComputePipelineCreateInfo{
	                        .stage = {
	                                .stage = vk::ShaderStageFlagBits::eCompute,
	                                .module = *shader,
	                                .pName = "main",
	                                .pSpecializationInfo = spc,
	                        },
	                        .layout = layout,
	                },
	        },
	};
	vk.name(*res[0], "foveation pipeline");
	vk.name(*res[1], "foveation+alpha pipeline");
	return res;
}

vk::raii::DescriptorPool make_ds_pool(wivrn::vk_bundle & vk)
{
	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 2,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 2,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageBuffer,
	                .descriptorCount = 1,
	        },
	};
	vk::raii::DescriptorPool res{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 5,
	                .poolSizeCount = pool_sizes.size(),
	                .pPoolSizes = pool_sizes.data(),
	        },
	};
	vk.name(*res, "foveation descriptor pool");
	return res;
}
uint32_t divide_and_round_up(uint32_t a, uint32_t b)
{
	return (a + (b - 1)) / b;
}

} // namespace

// The foveation curve maps foveated (encoded, destination) coordinates back to source
// (full size) coordinates, both in the -1,1 range. Only a portion of the image is encoded at
// full resolution; the rest is compressed by the tan periphery below.
//
// Foveation v2 generalises the single-tan curve to a central 1:1 plateau surrounded by two
// independent tan periphery branches:
//
//   defoveate(x) = c + λ·(x - xc)                        for |x - xc| <= p   (1:1 plateau)
//                = (c + λp) + λ/aR·tan(aR·(x - xc - p))   for  x > xc + p     (right periphery)
//                = (c - λp) + λ/aL·tan(aL·(x - xc + p))   for  x < xc - p     (left periphery)
//
// λ: pixel ratio between full size and foveated image (in ]0,1[), i.e. foveated_dim/source_dim
// c: source coordinate that sits at 1:1 (the fovea centre)
// xc: destination coordinate of the fovea centre; the neutral single-tan curve's 1:1 point
// p: half-width of the 1:1 plateau in destination coordinates, 0 for the neutral curve
// aL, aR: periphery steepness of each branch, solved so that:
//   - the edges of the image are not moved: defoveate(-1) = -1, defoveate(1) = 1
//   - the pixel ratio is exactly 1:1 across the whole [xc-p, xc+p] plateau (slope λ, so one
//     source pixel per destination pixel), and the periphery joins it C¹ (slope λ at xc±p)
//
// At p = 0 the two branches collapse to the original single tan (aL = aR = a from
// solve_foveation, and defoveate(x) = λ/a·tan(a·x + b) + c with b = -a·xc), so a strength of
// 0 reproduces the pre-v2 curve exactly.
//
// The plateau widens (and the periphery steepens) with foveation_strength at a FIXED encode
// size: more source pixels are kept 1:1 in the centre, paid for by a steeper periphery, for
// the same number of encoded pixels. We then round to integer pixel ratios (1:1, 1:2 …) and
// sort the spans so ratios only increase going out from the centre — the wire format and the
// client defoveator are unchanged, they just receive a different span distribution.
struct fov_curve
{
	double λ;
	double c;
	double xc;
	double p;
	double aL;
	double aR;
};

static double defoveate(const fov_curve & cu, double x)
{
	const double d = x - cu.xc;
	if (d >= -cu.p and d <= cu.p)
		return cu.c + cu.λ * d; // 1:1 plateau
	if (d > cu.p)
		return (cu.c + cu.λ * cu.p) + cu.λ / cu.aR * tan(cu.aR * (d - cu.p));
	return (cu.c - cu.λ * cu.p) + cu.λ / cu.aL * tan(cu.aL * (d + cu.p));
}

static std::tuple<float, float> solve_foveation(float λ, float c)
{
	// Compute a and b for the foveation function such that:
	//   foveate(a, b, scale, c, -1) = -1   (eq. 1)
	//   foveate(a, b, scale, c,  1) =  1   (eq. 2)
	//
	// Use eq. 2 to express a as function of b, then replace in eq. 1
	// equation that needs to be null is:
	auto b = [λ, c](double a) { return atan(a * (1 - c) / λ) - a; };
	auto eq = [λ, c](double a) { return atan(a * (1 - c) / λ) + atan(a * (1 + c) / λ) - 2 * a; }; // (eq. 3)

	// function starts positive, reaches a maximum then decreases to -∞
	double a0 = 0;
	// Find a negative value by computing eq(2^n)
	double a1 = 1;
	while (eq(a1) > 0)
		a1 *= 2;

	// last computed values for f(a0) and f(a1)
	std::optional<double> f_a0;
	double f_a1 = eq(a1);

	int n = 0;
	double a = 0;
	while (std::abs(a1 - a0) > 0.0000001 && n++ < 100)
	{
		if (not f_a0)
		{
			// use binary search
			a = 0.5 * (a0 + a1);
			double val = eq(a);
			if (val > 0)
			{
				a0 = a;
				f_a0 = val;
			}
			else
			{
				a1 = a;
				f_a1 = val;
			}
		}
		else
		{
			// f(a1) is always defined
			// when f(a0) is defined, use secant method
			a = a1 - f_a1 * (a1 - a0) / (f_a1 - *f_a0);
			a0 = a1;
			a1 = a;
			f_a0 = f_a1;
			f_a1 = eq(a);
		}
	}

	return {a, b(a)};
}

// Solve one periphery branch: find a > 0 such that tan(a·d) = a·Δ/λ, where d is the branch's
// destination length (junction to edge) and Δ its source length. This is the same equation
// solve_foveation solves for the coupled two-sided curve, split into an independent branch so
// that the plateau can consume different destination and source amounts on each side while the
// edge still lands exactly (defoveate(±1) = ±1) and the junction slope stays λ.
//
// g(a) = tan(a·d) - a·Δ/λ is negative near 0 (Δ/λ > d whenever the plateau leaves any
// periphery to compress) and +∞ as a·d → π/2, so it has a single root; bisection is robust and
// converges in double precision well within the iteration budget.
static double solve_branch(double λ, double d, double Δ)
{
	if (d <= 0)
		return 0;
	double lo = 1e-12;
	double hi = (M_PI / 2) / d - 1e-12;
	auto g = [&](double a) { return tan(a * d) - a * Δ / λ; };
	for (int i = 0; i < 100; ++i)
	{
		double m = 0.5 * (lo + hi);
		if (g(m) > 0)
			hi = m;
		else
			lo = m;
	}
	return 0.5 * (lo + hi);
}

// Steepest periphery pixel ratio (source pixels per destination pixel) of a curve, evaluated
// at both edges. slope_norm/λ converts the normalised-coordinate slope to a pixel ratio.
static double edge_ratio(const fov_curve & cu)
{
	const double h = 1e-6;
	double sR = (1.0 - defoveate(cu, 1.0 - h)) / h / cu.λ;
	double sL = (defoveate(cu, -1.0 + h) - (-1.0)) / h / cu.λ;
	return std::max(sR, sL);
}

// Build the foveation curve for a given fovea centre (source coord c), strength and the active
// render_scale. strength widens the 1:1 plateau; render_scale caps how far it may go so the
// combined peripheral factor (render_scale × 1/periphery_ratio) does not collapse into a blocky
// FSR upscale — see the render_scale/FSR guardrail note. strength 0 returns the neutral curve.
static fov_curve build_curve(double λ, double c, double strength, double render_scale)
{
	auto [a, b] = solve_foveation(λ, c);
	const double xc = -b / a;

	fov_curve neutral{λ, c, xc, 0.0, a, a};
	if (strength <= 0)
		return neutral;

	// How far the plateau may reach toward the nearer edge, in destination coordinates.
	const double room = std::min(1.0 - xc, 1.0 + xc);

	auto build_p = [&](double p) -> fov_curve {
		double dR = 1.0 - xc - p, ΔR = 1.0 - c - λ * p;
		double dL = 1.0 + xc - p, ΔL = 1.0 + c - λ * p;
		return {λ, c, xc, p, solve_branch(λ, dL, ΔL), solve_branch(λ, dR, ΔR)};
	};

	// Guardrail: bound the added periphery steepness relative to the neutral curve, and tighten
	// that bound as render_scale falls (a smaller encode already upscales the periphery by
	// 1/render_scale before foveation acts). At render_scale 1 the periphery may reach 3× the
	// neutral edge ratio, at 0.5 only 2×. render_scale owns central sharpness, foveation owns
	// the peripheral taper, FSR reconstructs — this keeps the two from compounding into mush.
	const double cap = edge_ratio(neutral) * (1.0 + 2.0 * std::clamp(render_scale, 0.0, 1.0));

	double p_target = std::clamp(strength, 0.0, 1.0) * 0.6 * room;
	fov_curve cand = build_p(p_target);
	if (edge_ratio(cand) <= cap)
		return cand;

	// edge_ratio is monotone in p; bisect for the widest plateau that stays within the cap.
	double lo = 0, hi = p_target;
	for (int i = 0; i < 40; ++i)
	{
		double m = 0.5 * (lo + hi);
		if (edge_ratio(build_p(m)) <= cap)
			lo = m;
		else
			hi = m;
	}
	return build_p(lo);
}

static bool is_zero_quat(xrt_quat q)
{
	return q.x == 0 and q.y == 0 and q.z == 0 and q.w == 0;
}

static xrt_vec2 yaw_pitch(xrt_quat q)
{
	if (is_zero_quat(q))
		return xrt_vec2{};

	float sine_theta = std::clamp(-2.0f * (q.y * q.z - q.w * q.x), -1.0f, 1.0f);

	float pitch = std::asin(sine_theta);

	if (std::abs(sine_theta) > 0.99999f)
	{
		float scale = std::copysign(2.0, sine_theta);
		return {scale * std::atan2(-q.z, q.w), pitch};
	}

	return {
	        std::atan2(2.0f * (q.x * q.z + q.w * q.y),
	                   q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z),
	        pitch};
}

static float angles_to_center(float e, float l, float r)
{
	e = tan(e);
	l = tan(l);
	r = tan(r);
	float res = std::clamp((e - l) / (r - l) * 2 - 1, -1.f, 1.f);
	// If the center isn't in the FoV, fallback to middle of image
	if (not wivrn::is_finite(res))
		return 0;
	return res;
}

static float convergence_angle(float distance, float eye_x, float gaze_yaw)
{
	float target_x = distance * std::sin(gaze_yaw);
	float target_z = distance * std::cos(gaze_yaw);

	float dx = target_x - eye_x;
	float dz = target_z;

	return std::atan2(dx, dz);
}

static void fill_param_2d(
        float c,
        size_t foveated_dim,
        size_t source_dim,
        float strength,
        float render_scale,
        std::vector<uint16_t> & out)
{
	double scale = double(foveated_dim) / source_dim;
	fov_curve curve = build_curve(scale, c, strength, render_scale);

	uint16_t last = 0;
	std::vector<uint16_t> left; // index 0: 1:1 ratio, then 2:1 etc.
	std::vector<uint16_t> right;
	for (size_t i = 1; i < foveated_dim; ++i)
	{
		double u = (i * 2.) / foveated_dim - 1;
		auto f = defoveate(curve, u);
		uint16_t n = std::clamp<uint16_t>((f * 0.5 + 0.5) * source_dim + 0.5, 0, source_dim);
		assert(n > last);
		size_t count = n - last;
		auto & vec = u < c ? left : right;
		if (count > vec.size())
			vec.resize(count);
		vec[count - 1]++;
		last = n;
	}
	assert(last < source_dim);
	size_t count = source_dim - last;
	if (count > right.size())
		right.resize(count);
	right[count - 1]++;

	count = std::max(left.size(), right.size());
	out.clear();
	out.resize(count - left.size());
	out.insert(out.end(), left.rbegin(), left.rend());
	if (not right.empty())
		out.back() += right.front();
	if (right.size() > 1)
		out.insert(out.end(), right.begin() + 1, right.end());
	out.resize(count * 2 - 1);
}

namespace wivrn
{

void foveation::compute_params()
{
	auto e = yaw_pitch(gaze);

	if (manual_foveation.enabled)
		e.y = manual_foveation.pitch;

	for (size_t i = 0; i < 2; ++i)
	{
		const auto & fov = last.fovs[i];

		size_t extent_w = std::abs(last.src[i].extent.w);
		if (foveated_size.width < extent_w)
		{
			auto distance = manual_foveation.enabled ? manual_foveation.distance : convergence_distance;
			auto angle_x = convergence_angle(distance, eye_x[i], -e.x);
			auto center = angles_to_center(angle_x, fov.angle_left, fov.angle_right);
			fill_param_2d(center, foveated_size.width, extent_w, shape.strength, shape.render_scale, params[i].x);
		}
		else
			params[i].x = {uint16_t(extent_w)};

		size_t extent_h = std::abs(last.src[i].extent.h);
		if (foveated_size.height < extent_h)
		{
			auto angle_y = -e.y;
			if (is_zero_quat(gaze) and not manual_foveation.enabled)
			{
				// Natural gaze is not straight forward, adjust the angle
				angle_y += angle_offset;
			}
			auto center = angles_to_center(-angle_y, fov.angle_up, fov.angle_down);
			fill_param_2d(center, foveated_size.height, extent_h, shape.strength, shape.render_scale, params[i].y);
		}
		else
			params[i].y = {uint16_t(extent_h)};
	}
}

foveation::foveation(wivrn::vk_bundle & bundle, vk::Extent3D foveated_size) :
        foveated_size(foveated_size),
        // normal sight line is between 10° and 15° below horizontal
        // https://apps.dtic.mil/sti/tr/pdf/AD0758339.pdf pages 393-394
        // testing shows 10° looks better
        angle_offset(10 * M_PI / 180),
        convergence_distance(1 /* meter*/),
        gpu_buffer(
                bundle.device,
                {
                        .size = sizeof(ubo_data),
                        .usage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
                },
                VmaAllocationCreateInfo{
                        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                },
                "foveation storage buffer"),
        sampler(make_sampler(bundle)),
        ds_layout(make_ds_layout(bundle)),
        layout(make_layout(bundle, ds_layout)),
        pipeline(make_pipelines(bundle, layout, foveated_size.width / 2)),
        descriptor_pool(make_ds_pool(bundle)),
        descriptor_set(bundle.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
                .descriptorPool = descriptor_pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &*ds_layout,
        })[0]
                               .release())
{
	bundle.name(descriptor_set, "foveation descriptor set");
}

void foveation::update_tracking(const from_headset::tracking & tracking)
{
	std::lock_guard lock(mutex);

	const uint8_t orientation_ok = from_headset::pose_flags::orientation_valid | from_headset::pose_flags::orientation_tracked;

	if (tracking.view_flags & XR_VIEW_STATE_POSITION_VALID_BIT)
	{
		eye_x[0] = tracking.views[0].pose.position.x;
		eye_x[1] = tracking.views[1].pose.position.x;
	}

	for (const auto & pose: tracking.device_poses)
	{
		if (pose.device != device_id::EYE_GAZE)
			continue;

		if ((pose.flags & orientation_ok) != orientation_ok)
			return;

		gaze = xrt_cast(pose.pose.orientation);
		return;
	}
}

void foveation::update_foveation_center_override(const from_headset::override_foveation_center & center)
{
	std::lock_guard lock(mutex);
	manual_foveation = center;
}

void foveation::set_shape(float strength, float render_scale)
{
	std::lock_guard lock(mutex);
	shape.strength = std::clamp(strength, 0.f, 1.f);
	shape.render_scale = std::clamp(render_scale, 0.f, 1.f);
}

static void fill_ubo(
        std::span<uint32_t> ubo,
        const std::vector<uint16_t> & params,
        bool flip,
        size_t offset,
        size_t size,
        [[maybe_unused]] int count)
{
	assert(params.size() % 2 == 1);
	const int n_ratio = (params.size() - 1) / 2;
	ubo[0] = offset;
	if (flip)
		ubo[0] += size;
	for (auto [i, n]: std::ranges::enumerate_view(params))
	{
		const int n_source = std::abs(n_ratio - int(i)) + 1;
		for (size_t j = 0; j < n; ++j)
		{
			assert(count > 0);
			if (flip)
				ubo[1] = ubo[0] - n_source;
			else
				ubo[1] = ubo[0] + n_source;
			ubo = ubo.subspan(1);
			--count;
		}
	}
	if (not ubo.empty())
		std::ranges::fill(ubo, ubo[0]);
}

template <typename T>
static bool operator==(const T & a, const T & b)
{
	static_assert(std::has_unique_object_representations_v<T>);
	return std::memcmp(&a, &b, sizeof(T)) == 0;
}
static bool operator==(const xrt_quat & a, const xrt_quat & b)
{
	return a.x == b.x and a.y == b.y and a.z == b.z and a.w == b.w;
}
static bool operator==(const xrt_fov & a, const xrt_fov & b)
{
	return a.angle_left == b.angle_left and a.angle_right == b.angle_right and a.angle_up == b.angle_up and a.angle_down == b.angle_down;
}
void foveation::update_ubo(
        vk::raii::CommandBuffer & cmd,
        bool flip_y,
        std::array<xrt_rect, 2> src_rect,
        std::array<xrt_fov, 2> src_fov)
{
	// Check if the last value is still valid
	std::lock_guard lock(mutex);
	if (last.flip_y == flip_y and
	    last.src[0] == src_rect[0] and
	    last.src[1] == src_rect[1] and
	    last.fovs[0] == src_fov[0] and
	    last.fovs[1] == src_fov[1] and
	    (last.gaze == gaze or manual_foveation.enabled) and // Ignore the gaze if foveation center is overridden
	    std::abs(last.eye_x[0] - eye_x[0]) < 0.0005 and
	    std::abs(last.eye_x[1] - eye_x[1]) < 0.0005 and
	    last.manual_foveation.enabled == manual_foveation.enabled and
	    std::abs(last.manual_foveation.pitch - manual_foveation.pitch) < 0.0005 and
	    std::abs(last.manual_foveation.distance - manual_foveation.distance) < 0.0005 and
	    // Foveation v2: a changed curve shape re-quantises the spans without touching the
	    // encode size, so it is picked up here and applied live on the next frame.
	    std::abs(last.shape.strength - shape.strength) < 0.0005 and
	    std::abs(last.shape.render_scale - shape.render_scale) < 0.0005)
		return;

	last = {
	        .gaze = gaze,
	        .flip_y = flip_y,
	        .src = {src_rect[0], src_rect[1]},
	        .fovs = {src_fov[0], src_fov[1]},
	        .eye_x = {eye_x[0], eye_x[1]},
	        .manual_foveation = manual_foveation,
	        .shape = shape,
	};

	compute_params();

	ubo_data ubo;
	for (size_t view = 0; view < 2; ++view)
	{
		bool flip = false;
		size_t offset, extent;

		if (src_rect[view].extent.w < 0)
		{
			flip = true;
			offset = src_rect[view].offset.w + src_rect[view].extent.w;
			extent = -src_rect[view].extent.w;
		}
		else
		{
			offset = src_rect[view].offset.w;
			extent = src_rect[view].extent.w;
		}
		fill_ubo(std::span(ubo.x + view * RENDER_FOVEATION_BUFFER_DIMENSIONS, RENDER_FOVEATION_BUFFER_DIMENSIONS),
		         params[view].x,
		         flip,
		         offset,
		         extent,
		         foveated_size.width);

		if (src_rect[view].extent.h < 0)
		{
			flip = not flip_y;
			offset = src_rect[view].offset.h + src_rect[view].extent.h;
			extent = -src_rect[view].extent.h;
		}
		else
		{
			flip = flip_y;
			offset = src_rect[view].offset.h;
			extent = src_rect[view].extent.h;
		}
		fill_ubo(std::span(ubo.y + view * RENDER_FOVEATION_BUFFER_DIMENSIONS, RENDER_FOVEATION_BUFFER_DIMENSIONS),
		         params[view].y,
		         flip,
		         offset,
		         extent,
		         foveated_size.height);
	}
	vmaCopyMemoryToAllocation(vk_allocator::instance(), &ubo, gpu_buffer, 0, sizeof(ubo));
	std::memcpy(gpu_buffer.data<ubo_data>(), &ubo, sizeof(ubo));
}

std::array<to_headset::foveation_parameter, 2> foveation::foveate(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        vk::ImageView y,
        vk::ImageView cbcr,
        bool flip_y,
        std::array<vk::ImageView, 2> src,
        std::array<xrt_rect, 2> src_rect,
        std::array<xrt_fov, 2> src_fov,
        bool alpha)
{
	update_ubo(cmd, flip_y, src_rect, src_fov);
	auto ubo = gpu_buffer.data<ubo_data>();

	std::array src_image_info{
	        vk::DescriptorImageInfo{
	                .sampler = *sampler,
	                .imageView = src[0],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	        vk::DescriptorImageInfo{
	                .sampler = *sampler,
	                .imageView = src[1],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	};

	vk::DescriptorBufferInfo ubo_info{
	        .buffer = gpu_buffer,
	        .range = vk::WholeSize,
	};

	vk::DescriptorImageInfo y_info{
	        .imageView = y,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo cbcr_info{
	        .imageView = cbcr,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};

	std::array writes = {
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_set,
	                .dstBinding = 0,
	                .descriptorCount = src_image_info.size(),
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = src_image_info.data(),
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_set,
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageBuffer,
	                .pBufferInfo = &ubo_info,
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_set,
	                .dstBinding = 2,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &y_info,
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_set,
	                .dstBinding = 3,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &cbcr_info,
	        },
	};

	device.updateDescriptorSets(writes, {});

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline[alpha]);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, descriptor_set, {});
	cmd.dispatch(divide_and_round_up(foveated_size.width, 8),
	             divide_and_round_up(foveated_size.height, 8),
	             2);

	return params;
}
} // namespace wivrn
