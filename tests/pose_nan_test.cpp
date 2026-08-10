// Pose NaN test harness: the server must never hand the application a pose that
// contains a non-finite float, or a quaternion it cannot normalise.
//
// The bug this was written for: VRChat (through xrizer) died mid loading screen
// with Monado logging
//
//   XR_ERROR_POSE_INVALID: xrEndFrame(...views[0]->pose.orientation == {-nan nan nan nan})
//
// i.e. the head pose the application got out of xrLocateViews() was NaN, and it
// handed it straight back.
//
// Part A: -ffast-math. The whole server is built with it, so std::isnan(),
//         std::isfinite() and the x != x idiom fold to compile time constants and
//         cannot be used to catch anything. Every guard in the fix works on the
//         IEEE-754 bit pattern instead; this part pins that down, because if
//         wivrn::is_finite() ever regresses to <cmath> the rest silently stops
//         testing anything.
// Part B: polynomial_interpolator. The production chain. abs_Δt used to be an int
//         holding a nanosecond difference: a sample more than 2^31 ns from the
//         requested time truncated, and at 2^32 - window ns the weight came out as
//         1 / (1 + (-1)^3) = +inf, which makes the least squares solve return NaN
//         for every coefficient. Reachable as soon as the ring buffer holds samples
//         much older than the query, which is exactly what a tracking freeze (or a
//         multi second uplink stall) leaves behind.
// Part C: identical-timestamp extrapolation, i.e. division by a zero time span.
// Part D: zero and non-finite quaternion handling.
// Part E: the ingest predicate, on the OpenXR types, as pose_list::is_sane() uses it.
// Part F: the sanitize boundary that wivrn_hmd and wivrn_controller sit behind.
// Part G: the standby freeze toggle. The freeze is what leaves the stale samples of
//         Part B behind, so the two belong in the same harness: turning it off must
//         restore upstream behaviour without reopening the NaN, including when it is
//         turned off in the middle of a freeze.
//
// Build (from the repository root, and with -ffast-math: testing these guards
// without the flag that breaks the naive ones would be pointless):
//   g++ -std=c++23 -O2 -ffast-math -w \
//       -I server -I server/driver -I common -I build-server/common \
//       -I build-server/_deps/monado-src/src/xrt/include \
//       -I build-server/_deps/monado-src/src/external/openxr_includes \
//       -isystem $EIGEN_INCLUDE_DIR \
//       -isystem build-server/_deps/boost-src/libs/pfr/include \
//       -isystem external \
//       -o pose_nan_test tests/pose_nan_test.cpp common/smp.cpp -lcrypto
//   ./pose_nan_test
//
// (smp is only there because pose_list.h reaches wivrn_packets.h, which declares the
// pairing constants; nothing under test uses them.)

#include "polynomial_interpolator.h"
#include "pose_list.h"
#include "pose_sanitize.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace wivrn;

namespace
{
int failures = 0;
int checks = 0;

void check(bool ok, const std::string & what)
{
	++checks;
	if (ok)
		return;
	++failures;
	std::printf("  FAIL: %s\n", what.c_str());
}

// 72 Hz, the rate a Pico reports tracking at
constexpr int64_t frame = 13'888'888;
// A plausible monotonic clock reading: large enough that the differences below are
// the only thing that matters
constexpr int64_t t0 = 1'000'000'000'000;

volatile float zero = 0.f;

float make_nan()
{
	return zero / zero;
}

float make_inf()
{
	return 1.f / zero;
}

/*
 *
 * Part A: the guards must survive -ffast-math
 *
 */

void test_fast_math()
{
	std::printf("Part A: finiteness checks under -ffast-math\n");

	const float nan_ = make_nan();
	const float inf_ = make_inf();

	// These are the checks the fix must not use. They are asserted to be *broken*
	// under the build flags, which is the whole reason wivrn::is_finite() exists.
	// If a future -ffast-math removal makes them work again the test still passes,
	// so only report, do not fail, on that half.
	if (std::isnan(nan_) or std::isinf(inf_) or not std::isfinite(nan_))
		std::printf("  note: <cmath> classification works here, -ffast-math must have been dropped\n");
	else
		std::printf("  note: <cmath> classification is folded away, as expected\n");

	check(not is_finite(nan_), "is_finite(NaN) is false");
	check(not is_finite(inf_), "is_finite(+inf) is false");
	check(not is_finite(-inf_), "is_finite(-inf) is false");
	check(is_finite(0.f), "is_finite(0) is true");
	check(is_finite(-1.5f), "is_finite(-1.5) is true");
	check(is_finite(3.4e38f), "is_finite(near FLT_MAX) is true");

	check(not is_finite(xrt_vec3{1, nan_, 3}), "is_finite(vec3 with a NaN) is false");
	check(is_finite(xrt_vec3{1, 2, 3}), "is_finite(finite vec3) is true");
	check(not is_finite(xrt_quat{0, inf_, 0, 1}), "is_finite(quat with an inf) is false");
}

/*
 *
 * Part B: the interpolator
 *
 */

// Fills a position interpolator with `stale` samples at 72 Hz ending at t0, then
// `fresh` samples starting `gap` nanoseconds later, and asks for the position at the
// last fresh sample. Before the fix, a gap that puts a stale sample 2^32 - window ns
// from the query makes this return NaN.
polynomial_interpolator<3>::sample freeze_and_resume(int64_t gap, int stale = 30, int fresh = 3)
{
	polynomial_interpolator<3> positions;

	for (int i = 0; i < stale; ++i)
	{
		polynomial_interpolator<3>::sample s{t0 + i * frame, t0 + i * frame};
		// the head drifts slowly before the freeze
		s.y.emplace(0.001f * i, 1.6f, 0.f);
		positions.add_sample(s);
	}

	const int64_t resume = t0 + (stale - 1) * frame + gap;
	for (int i = 0; i < fresh; ++i)
	{
		polynomial_interpolator<3>::sample s{resume + i * frame, resume + i * frame};
		// and is somewhere else when tracking comes back
		s.y.emplace(0.5f, 1.6f, 0.f);
		positions.add_sample(s);
	}

	return positions.get_at(resume + (fresh - 1) * frame);
}

bool sample_is_finite(const polynomial_interpolator<3>::sample & s)
{
	if (s.y and not(is_finite((*s.y)[0]) and is_finite((*s.y)[1]) and is_finite((*s.y)[2])))
		return false;
	if (s.dy and not(is_finite((*s.dy)[0]) and is_finite((*s.dy)[1]) and is_finite((*s.dy)[2])))
		return false;
	return true;
}

void test_interpolator_stale_samples()
{
	std::printf("Part B: polynomial_interpolator with stale samples in the buffer\n");

	// The exact point where the old int truncation produced an infinite weight:
	// |Δt| = 2^32 - window, so std::abs(Δt) truncated to int is exactly -window and
	// 1 + (Δt/window)^3 is exactly zero.
	constexpr int64_t two_pow_32 = 4'294'967'296;
	constexpr int64_t window = 30'000'000;

	for (int fresh: {1, 2, 3, 5, 8})
	{
		const int64_t inf_point = two_pow_32 - window - (fresh - 1) * frame;

		for (int64_t off: {int64_t(0), int64_t(-1'000), int64_t(1'000), int64_t(-20'000), int64_t(20'000)})
		{
			auto s = freeze_and_resume(inf_point + off, 30, fresh);
			check(sample_is_finite(s),
			      "no NaN at the infinite weight point, fresh=" + std::to_string(fresh) +
			              " off=" + std::to_string(off) + " ns");
		}
	}

	// Sweep a wide range of freeze durations: nothing anywhere may come back
	// non-finite, and once the freeze is long enough that every stale sample is
	// past the usable age, the fit must be the fresh data alone.
	int nonfinite = 0;
	float worst_err = 0;
	int64_t worst_gap = 0;
	for (int64_t gap = 2'000'000'000; gap <= 9'000'000'000; gap += 997'361)
	{
		auto s = freeze_and_resume(gap);
		if (not sample_is_finite(s))
		{
			++nonfinite;
			continue;
		}
		if (not s.y)
			continue;
		const float err = std::fabs((*s.y)[0] - 0.5f);
		if (err > worst_err)
		{
			worst_err = err;
			worst_gap = gap;
		}
	}
	check(nonfinite == 0, "no non-finite result over a 2..9 s freeze sweep (" + std::to_string(nonfinite) + " hits)");
	check(worst_err < 0.01f,
	      "a freeze never drags the resumed pose off by more than 1 cm (worst " +
	              std::to_string(worst_err) + " m at a " + std::to_string(worst_gap / 1000000) + " ms freeze)");

	// A short gap is normal operation and must still interpolate.
	auto normal = freeze_and_resume(3 * frame);
	check(normal.y.has_value(), "a three frame gap still produces a pose");

	// A quaternion interpolator over the same scenario: the value that used to come
	// out here was fed to Eigen's normalize(), and pose_list flagged the result
	// ORIENTATION_VALID whenever its squared norm compared greater than 0.1 - which
	// an infinity does.
	for (int64_t off: {int64_t(0), int64_t(-1'000), int64_t(1'000)})
	{
		polynomial_interpolator<4, true> orientations;
		for (int i = 0; i < 30; ++i)
		{
			polynomial_interpolator<4, true>::sample s{t0 + i * frame, t0 + i * frame};
			const float a = 0.01f * i;
			s.y.emplace(std::cos(a), 0.f, std::sin(a), 0.f);
			orientations.add_sample(s);
		}
		const int64_t resume = t0 + 29 * frame + two_pow_32 - window + off;
		polynomial_interpolator<4, true>::sample s{resume, resume};
		s.y.emplace(std::cos(1.f), 0.f, std::sin(1.f), 0.f);
		orientations.add_sample(s);

		auto o = orientations.get_at(resume);
		bool ok = true;
		if (o.y)
		{
			for (int i = 0; i < 4; ++i)
				ok = ok and is_finite((*o.y)[i]);
			ok = ok and is_finite(o.y->squaredNorm());
		}
		check(ok, "orientation fit stays finite at the infinite weight point, off=" + std::to_string(off));

		// And whatever it is, it must survive pose_list's acceptance test: either
		// rejected, or normalisable without producing a NaN.
		if (o.y and is_finite(o.y->squaredNorm()) and o.y->squaredNorm() > 0.1f)
		{
			auto q = *o.y;
			q.normalize();
			bool finite = true;
			for (int i = 0; i < 4; ++i)
				finite = finite and is_finite(q[i]);
			check(finite, "an accepted orientation normalises to a finite quaternion, off=" + std::to_string(off));
		}
	}
}

void test_interpolator_degenerate()
{
	std::printf("Part B2: degenerate interpolator inputs\n");

	// Every sample at (almost) the same timestamp: the dedup in add_sample keeps
	// one, so this must not reach the solver with a rank one system.
	{
		polynomial_interpolator<3> positions;
		for (int i = 0; i < 10; ++i)
		{
			polynomial_interpolator<3>::sample s{t0 + i, t0};
			s.y.emplace(1.f, 2.f, 3.f);
			positions.add_sample(s);
		}
		auto r = positions.get_at(t0);
		check(sample_is_finite(r), "identical timestamps do not produce a NaN fit");
		check(r.y and std::fabs((*r.y)[0] - 1.f) < 1e-3f, "identical timestamps return the sample itself");
	}

	// An empty interpolator.
	{
		polynomial_interpolator<3> positions;
		auto r = positions.get_at(t0);
		check(not r.y.has_value(), "an empty interpolator has no value");
		check(sample_is_finite(r), "an empty interpolator is finite");
	}

	// A single sample: not enough to fit, must be returned as is.
	{
		polynomial_interpolator<3> positions;
		polynomial_interpolator<3>::sample s{t0, t0};
		s.y.emplace(7.f, 8.f, 9.f);
		positions.add_sample(s);
		auto r = positions.get_at(t0 + frame);
		check(r.y and (*r.y)[0] == 7.f, "a lone sample is returned unchanged");
	}

	// Samples whose values are enormous: the fit may be useless but must not be
	// infinite or NaN.
	{
		polynomial_interpolator<3> positions;
		for (int i = 0; i < 8; ++i)
		{
			polynomial_interpolator<3>::sample s{t0 + i * frame, t0 + i * frame};
			s.y.emplace(1e18f * (i + 1), 0.f, 0.f);
			positions.add_sample(s);
		}
		auto r = positions.get_at(t0 + 8 * frame);
		check(sample_is_finite(r), "huge but finite inputs give a finite fit");
	}
}

/*
 *
 * Part C: extrapolation over a zero time span
 *
 */

// pose_list::extrapolate and tracker_pose_list::extrapolate are the same arithmetic;
// this is that arithmetic, so the harness does not need to link the whole driver.
// The guard under test is "tb == ta returns b unchanged".
xrt_space_relation extrapolate_reference(const xrt_space_relation & a,
                                         const xrt_space_relation & b,
                                         int64_t ta,
                                         int64_t tb,
                                         int64_t t)
{
	xrt_space_relation res = t < ta ? a : b;

	if (tb == ta)
		return b;

	const float h = (tb - ta) / 1.e9;

	xrt_vec3 lin_vel = res.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT
	                           ? res.linear_velocity
	                           : xrt_vec3{
	                                     (b.pose.position.x - a.pose.position.x) / h,
	                                     (b.pose.position.y - a.pose.position.y) / h,
	                                     (b.pose.position.z - a.pose.position.z) / h,
	                             };

	const float dt = (t - tb) / 1.e9;

	if (not is_finite(lin_vel) or not is_finite(dt))
		return b;

	res.pose.position.x += lin_vel.x * dt;
	res.pose.position.y += lin_vel.y * dt;
	res.pose.position.z += lin_vel.z * dt;

	if (not is_usable(res))
		return b;

	return res;
}

void test_extrapolate_zero_span()
{
	std::printf("Part C: extrapolation over a zero time span\n");

	xrt_space_relation a{
	        .relation_flags = xrt_space_relation_flags(XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                                   XRT_SPACE_RELATION_ORIENTATION_VALID_BIT),
	        .pose = {.orientation = {0, 0, 0, 1}, .position = {0, 1.6f, 0}},
	};
	xrt_space_relation b = a;
	b.pose.position.x = 0.25f;

	// Two samples at the same timestamp: h is zero and the finite difference used
	// to divide by it.
	auto r = extrapolate_reference(a, b, t0, t0, t0 + frame);
	check(is_finite(r), "extrapolating over a zero span stays finite");
	check(r.pose.position.x == 0.25f, "extrapolating over a zero span returns the newer sample");

	// One nanosecond apart is not a zero span, but the implied velocity is 2.5e8
	// m/s; still finite, which is all this layer promises.
	auto r2 = extrapolate_reference(a, b, t0, t0 + 1, t0 + 1);
	check(is_finite(r2), "a one nanosecond span stays finite");

	// The normal case still extrapolates.
	auto r3 = extrapolate_reference(a, b, t0, t0 + frame, t0 + 2 * frame);
	check(is_finite(r3), "a normal extrapolation is finite");
	check(r3.pose.position.x > 0.25f, "a normal extrapolation moves the pose forward");
}

/*
 *
 * Part D: quaternions
 *
 */

void test_quaternions()
{
	std::printf("Part D: quaternion validity\n");

	const float nan_ = make_nan();
	const float inf_ = make_inf();

	check(is_valid_orientation(xrt_quat{0, 0, 0, 1}), "identity is a valid orientation");
	check(is_valid_orientation(xrt_quat{0.5f, 0.5f, 0.5f, 0.5f}), "a unit quaternion is valid");

	// The one that bit us: a runtime that never filled the view pose in leaves a
	// zero quaternion, whose norm is zero, and normalising it is 0/0.
	check(not is_valid_orientation(xrt_quat{0, 0, 0, 0}), "a zero quaternion is rejected");
	check(not is_valid_orientation(xrt_quat{nan_, nan_, nan_, nan_}), "an all NaN quaternion is rejected");
	check(not is_valid_orientation(xrt_quat{0, 0, 0, nan_}), "a partly NaN quaternion is rejected");
	check(not is_valid_orientation(xrt_quat{inf_, 0, 0, 1}), "an infinite quaternion is rejected");

	// squaredNorm() of a huge but finite quaternion overflows to infinity, and
	// "norm2 > 0.1" is *true* for infinity: that is how a NaN used to get flagged
	// ORIENTATION_VALID. The bound must catch it before the normalise.
	check(not is_valid_orientation(xrt_quat{1e30f, 1e30f, 1e30f, 1e30f}),
	      "a quaternion whose squared norm overflows is rejected");
	check(not is_valid_orientation(xrt_quat{1e-30f, 0, 0, 1e-30f}),
	      "a quaternion too close to zero to normalise is rejected");

	// And what the old check let through, spelled out: infinity compares greater
	// than 0.1, so an infinite squared norm reached normalize(), which divides by
	// it. (-ffast-math rewrites a literal x/x to 1, hence the volatile: the
	// hardware division Eigen actually performs is the one that gives NaN.)
	{
		const float norm2 = inf_;
		check(norm2 > 0.1f, "the old norm2 > 0.1 test passes for infinity (this is the bug)");
		volatile float num = inf_;
		volatile float den = inf_;
		check(not is_finite(num / den), "and inf/inf, i.e. normalising it, is NaN");
	}

	// Normalising something that passed is safe and actually normalises.
	{
		xrt_quat q{0, 0, 0, 2};
		check(is_valid_orientation(q), "a quaternion of norm 2 is accepted");
		normalize_orientation(q);
		check(is_finite(q), "normalising it stays finite");
		check(std::fabs(q.w - 1.f) < 1e-6f, "and gives a unit quaternion");
	}
}

/*
 *
 * Part E: ingest
 *
 */

void test_ingest()
{
	std::printf("Part E: rejecting non-finite samples on ingest\n");

	const float nan_ = make_nan();
	const float inf_ = make_inf();

	// pose_list::is_sane() applies exactly these predicates to the fields the
	// packet flags claim are valid.
	check(is_valid_orientation(XrQuaternionf{0, 0, 0, 1}), "a good XrQuaternionf is accepted");
	check(not is_valid_orientation(XrQuaternionf{0, 0, 0, 0}), "a zero XrQuaternionf is rejected");
	check(not is_valid_orientation(XrQuaternionf{nan_, 0, 0, 1}), "a NaN XrQuaternionf is rejected");

	check(is_finite(XrVector3f{0, 1.6f, 0}), "a good XrVector3f is accepted");
	check(not is_finite(XrVector3f{0, nan_, 0}), "a NaN XrVector3f is rejected");
	check(not is_finite(XrVector3f{inf_, 0, 0}), "an infinite XrVector3f is rejected");

	check(is_finite(XrPosef{{0, 0, 0, 1}, {0, 1.6f, 0}}), "a good XrPosef is accepted");
	check(not is_finite(XrPosef{{0, 0, 0, 1}, {0, nan_, 0}}), "an XrPosef with a NaN position is rejected");

	check(is_finite(XrFovf{-0.9f, 0.9f, 0.9f, -0.9f}), "a good XrFovf is accepted");
	check(not is_finite(XrFovf{-0.9f, nan_, 0.9f, -0.9f}), "a NaN XrFovf is rejected");
}

/*
 *
 * Part F: the sanitize boundary
 *
 */

void test_sanitize_boundary()
{
	std::printf("Part F: the sanitize boundary\n");

	const float nan_ = make_nan();

	const auto all_valid = xrt_space_relation_flags(
	        XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	        XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	        XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);

	relation_sanitizer s;

	// A good relation passes through untouched and is remembered.
	xrt_space_relation good{
	        .relation_flags = all_valid,
	        .pose = {.orientation = {0, 0, 0, 1}, .position = {0.1f, 1.6f, -0.2f}},
	        .linear_velocity = {1, 0, 0},
	        .angular_velocity = {0, 1, 0},
	};
	xrt_space_relation r = good;
	check(s.sanitize(r), "a good relation is accepted");
	check(r.pose.position.x == 0.1f, "and is not modified");
	check(r.relation_flags == all_valid, "and keeps its flags");
	check(s.dropped() == 0, "and is not counted as dropped");

	// A NaN orientation is replaced by the last good pose, with the tracked and
	// velocity bits cleared: the pose is stale and we must not claim otherwise.
	xrt_space_relation bad = good;
	bad.pose.orientation = {nan_, nan_, nan_, nan_};
	r = bad;
	check(not s.sanitize(r), "a NaN orientation is rejected");
	check(is_finite(r), "the replacement is finite");
	check(is_usable(r), "the replacement is usable");
	check(r.pose.position.x == 0.1f, "the replacement is the last known good pose");
	check((r.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) == 0, "TRACKED orientation is cleared");
	check((r.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0, "TRACKED position is cleared");
	check((r.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0, "VALID orientation is kept");
	check((r.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) == 0, "stale velocities are not claimed valid");
	check(s.dropped() == 1, "the drop is counted");

	// A zero quaternion flagged valid is just as fatal and must go the same way.
	r = good;
	r.pose.orientation = {0, 0, 0, 0};
	check(not s.sanitize(r), "a zero quaternion flagged valid is rejected");
	check(is_usable(r), "and is replaced by something usable");

	// A NaN position, and a NaN hiding in a velocity.
	r = good;
	r.pose.position = {0, nan_, 0};
	check(not s.sanitize(r), "a NaN position is rejected");
	check(is_finite(r), "and replaced by a finite relation");

	r = good;
	r.angular_velocity = {nan_, 0, 0};
	check(not s.sanitize(r), "a NaN angular velocity is rejected");
	check(is_finite(r), "and replaced by a finite relation");

	// A sanitizer that has never seen a good relation must still produce something
	// the application can use, and must not claim it is tracked.
	{
		relation_sanitizer fresh;
		xrt_space_relation first{
		        .relation_flags = all_valid,
		        .pose = {.orientation = {nan_, nan_, nan_, nan_}, .position = {0, nan_, 0}},
		};
		check(not fresh.sanitize(first), "the first ever relation being NaN is rejected");
		check(is_finite(first), "the fallback is finite");
		check(first.relation_flags == XRT_SPACE_RELATION_BITMASK_NONE, "the fallback claims nothing");
		check(first.pose.orientation.w == 1.f, "the fallback is identity");
	}

	// The per eye view pose sanitizer.
	{
		pose_sanitizer p;
		xrt_pose ok{.orientation = {0, 0, 0, 1}, .position = {0.03f, 0, 0}};
		check(p.sanitize(ok), "a good view pose is accepted");

		xrt_pose zero{.orientation = {0, 0, 0, 0}, .position = {0, 0, 0}};
		check(not p.sanitize(zero), "a zero quaternion view pose is rejected");
		check(zero.position.x == 0.03f, "and is replaced by the last good one");
		check(is_valid_orientation(zero.orientation), "whose orientation is usable");

		xrt_pose nan_pose{.orientation = {0, 0, 0, nan_}, .position = {0, 0, 0}};
		check(not p.sanitize(nan_pose), "a NaN view pose is rejected");
		check(is_finite(nan_pose), "and is replaced by a finite one");
		check(p.dropped() == 2, "both drops are counted");

		// Before anything good has been seen at all.
		pose_sanitizer fresh;
		xrt_pose bad{.orientation = {0, 0, 0, 0}, .position = {0, 0, 0}};
		check(not fresh.sanitize(bad), "the first ever view pose being unusable is rejected");
		check(is_valid_orientation(bad.orientation), "and the fallback is identity");
	}

	// The rate limiter behind the warnings.
	{
		rate_limiter limit{1'000'000'000};
		check(limit(t0), "the first event is reported");
		check(not limit(t0 + 1), "an immediate repeat is not");
		check(not limit(t0 + 999'999'999), "nor is one just inside the interval");
		check(limit(t0 + 1'000'000'000), "one past the interval is");
	}
}

/*
 *
 * Part G: the standby freeze toggle
 *
 */

// One component (position or orientation) of one device, driven exactly as
// pose_list::add_sample and pose_list::get_at drive it: the same policy predicates
// decide what reaches the interpolator and at which timestamp it is queried.
struct component_sim
{
	pose_list::tracked_state state;
	polynomial_interpolator<3> positions;

	// Returns whether the sample was given to the interpolator
	bool feed(XrTime timestamp, bool tracked, float x, bool freeze)
	{
		if (not pose_list::update_tracked_state(state, timestamp, timestamp, true, tracked, freeze))
			return false;

		polynomial_interpolator<3>::sample s{timestamp, timestamp};
		s.y.emplace(x, 1.6f, 0.f);
		positions.add_sample(s);
		return true;
	}

	polynomial_interpolator<3>::sample query(XrTime at, bool freeze) const
	{
		if (pose_list::component_tracked(state, freeze))
			return positions.get_at(at);
		return positions.get_at(std::min(at, state.last_tracked_timestamp));
	}
};

// Feeds `n` tracked samples at 72 Hz starting at `from`, the device drifting slowly.
// Returns the timestamp of the last one.
XrTime feed_tracked(component_sim & c, XrTime from, int n, bool freeze)
{
	XrTime last = from;
	for (int i = 0; i < n; ++i)
	{
		last = from + i * frame;
		c.feed(last, true, 0.001f * i, freeze);
	}
	return last;
}

void test_standby_freeze_toggle()
{
	std::printf("Part G: the standby freeze toggle\n");

	// The process-wide switch itself.
	check(pose_list::standby_freeze(), "the freeze is on before anything sets it");
	pose_list::set_standby_freeze(false);
	check(not pose_list::standby_freeze(), "and follows what a settings packet asks for");
	pose_list::set_standby_freeze(true);
	check(pose_list::standby_freeze(), "in both directions");

	// The ingest policy.
	{
		pose_list::tracked_state s;

		// A runtime that never sets the tracked bit keeps upstream behaviour whatever
		// the toggle says: this is the estimated body joint case.
		pose_list::tracked_state never;
		check(pose_list::update_tracked_state(never, t0, t0, true, false, true),
		      "a component that was never tracked keeps its samples with the freeze on");
		check(pose_list::component_tracked(never, true),
		      "and is still reported as tracked");

		check(pose_list::update_tracked_state(s, t0, t0, true, true, true), "a tracked sample is kept");
		check(pose_list::component_tracked(s, true), "and the component reads as tracked");

		// Same state, same sample, the two policies.
		pose_list::tracked_state frozen = s;
		pose_list::tracked_state upstream = s;
		check(not pose_list::update_tracked_state(frozen, t0 + frame, t0 + frame, true, false, true),
		      "a valid but untracked sample is dropped with the freeze on");
		check(pose_list::update_tracked_state(upstream, t0 + frame, t0 + frame, true, false, false),
		      "and kept with the freeze off");

		check(not pose_list::component_tracked(frozen, true),
		      "the frozen component stops reading as tracked");
		check(pose_list::component_tracked(upstream, false),
		      "the upstream one always reads as tracked");

		// The state machine runs whatever the toggle says, so the two agree on
		// everything but the answer.
		check(frozen.currently_tracked == upstream.currently_tracked and
		              frozen.ever_tracked == upstream.ever_tracked and
		              frozen.last_tracked_timestamp == upstream.last_tracked_timestamp,
		      "the tracking state is maintained identically either way");

		// A sample the headset marks invalid is forwarded either way, it is what
		// invalidates the pose.
		check(pose_list::update_tracked_state(frozen, t0 + 2 * frame, t0 + 2 * frame, false, false, true),
		      "an invalid sample is forwarded with the freeze on");
		check(pose_list::update_tracked_state(upstream, t0 + 2 * frame, t0 + 2 * frame, false, false, false),
		      "and with the freeze off");
	}

	// Freeze on: the sleeping controller holds the pose it was last tracked at,
	// instead of the one the runtime keeps reporting for it.
	{
		component_sim c;
		const XrTime last_tracked = feed_tracked(c, t0, 30, true);
		const float held = 0.029f;

		XrTime t = last_tracked;
		int kept = 0;
		for (int i = 1; i <= 300; ++i)
		{
			t = last_tracked + i * frame;
			kept += c.feed(t, false, 0.5f, true);
		}
		check(kept == 0, "every standby sample is dropped (" + std::to_string(kept) + " kept)");

		auto s = c.query(t, true);
		check(s.y.has_value(), "the frozen component still has a pose");
		check(s.y and std::fabs((*s.y)[0] - held) < 0.01f,
		      "which is the last tracked one, not the standby garbage");
		check(not pose_list::component_tracked(c.state, true), "reported not tracked");
	}

	// Freeze off: upstream behaviour, the standby pose is served and flagged tracked.
	{
		component_sim c;
		const XrTime last_tracked = feed_tracked(c, t0, 30, false);

		XrTime t = last_tracked;
		int kept = 0;
		for (int i = 1; i <= 30; ++i)
		{
			t = last_tracked + i * frame;
			kept += c.feed(t, false, 0.5f, false);
		}
		check(kept == 30, "every standby sample is kept with the freeze off (" + std::to_string(kept) + "/30)");

		auto s = c.query(t, false);
		check(s.y.has_value(), "the component has a pose");
		check(s.y and std::fabs((*s.y)[0] - 0.5f) < 0.01f,
		      "which is whatever the runtime reported, teleport included");
		check(pose_list::component_tracked(c.state, false), "reported tracked, as upstream did");
	}

	// The one that matters: the toggle goes off in the middle of a freeze. For the
	// whole freeze the interpolator's ring was not refreshed, so it holds samples as
	// old as the freeze lasted; the moment the toggle flips, the runtime's untracked
	// samples start being kept again and the query moves back to the requested time.
	// That is Part B's state exactly, reached through the toggle rather than through
	// tracking resuming. What keeps it safe is not the toggle but max_sample_age_ns:
	// a sample further than a second from the query is left out of the fit, so the
	// stale block cannot reach the weight math at all.
	const auto toggle_off_mid_freeze = [](int64_t freeze_ns, int fresh) {
		component_sim c;
		const XrTime last_tracked = feed_tracked(c, t0, 30, true);

		// Asleep, freeze on: valid but untracked, nothing reaches the interpolator.
		for (int i = 1; i * frame < freeze_ns; ++i)
			c.feed(last_tracked + i * frame, false, 0.5f, true);

		// The user flips the toggle off while it is still asleep. The runtime is
		// still reporting the same poses, and they are kept from here on.
		const XrTime resume = last_tracked + freeze_ns;
		XrTime t = resume;
		for (int i = 0; i < fresh; ++i)
		{
			t = resume + i * frame;
			c.feed(t, false, 0.5f, false);
		}

		return std::pair{c.query(t, false), c.state};
	};

	{
		// The exact freeze durations that used to make the weight infinite, as in
		// Part B: a stale sample 2^32 - window ns from the query.
		constexpr int64_t two_pow_32 = 4'294'967'296;
		constexpr int64_t window = 30'000'000;

		for (int fresh: {1, 2, 3, 5, 8})
		{
			const int64_t inf_point = two_pow_32 - window - (fresh - 1) * frame;

			for (int64_t off: {int64_t(0), int64_t(-1'000), int64_t(1'000), int64_t(-20'000), int64_t(20'000)})
			{
				auto [s, state] = toggle_off_mid_freeze(inf_point + off, fresh);
				check(sample_is_finite(s),
				      "no NaN when the freeze is lifted at the infinite weight point, fresh=" +
				              std::to_string(fresh) + " off=" + std::to_string(off) + " ns");
				check(pose_list::component_tracked(state, false),
				      "and the component is reported tracked again");
			}
		}

		// And over a wide sweep of freeze durations. Once the stale block is past the
		// usable age the fit is the fresh samples alone, i.e. the pose the runtime is
		// reporting now: upstream behaviour, not a blend with the frozen one.
		int nonfinite = 0;
		int no_pose = 0;
		float worst_err = 0;
		int64_t worst_freeze = 0;

		for (int64_t freeze_ns = 2'000'000'000; freeze_ns <= 9'000'000'000; freeze_ns += 997'361)
		{
			auto [s, state] = toggle_off_mid_freeze(freeze_ns, 3);
			if (not sample_is_finite(s))
			{
				++nonfinite;
				continue;
			}
			if (not s.y)
			{
				++no_pose;
				continue;
			}
			const float err = std::fabs((*s.y)[0] - 0.5f);
			if (err > worst_err)
			{
				worst_err = err;
				worst_freeze = freeze_ns;
			}
		}

		check(nonfinite == 0,
		      "turning the freeze off mid freeze never produces a non-finite pose (" +
		              std::to_string(nonfinite) + " hits)");
		check(no_pose == 0, "and always produces a pose (" + std::to_string(no_pose) + " misses)");
		check(worst_err < 0.01f,
		      "which is the pose the runtime reports now, not a blend with the frozen one (worst " +
		              std::to_string(worst_err) + " m after a " + std::to_string(worst_freeze / 1000000) + " ms freeze)");
	}

	// And back on again: the state kept running while the freeze was off, so the
	// component freezes at the sample it was really last tracked at.
	{
		component_sim c;
		const XrTime last_tracked = feed_tracked(c, t0, 30, false);

		XrTime t = last_tracked;
		for (int i = 1; i <= 10; ++i)
		{
			t = last_tracked + i * frame;
			c.feed(t, false, 0.5f, false);
		}

		check(c.state.last_tracked_timestamp == last_tracked,
		      "the last tracked timestamp survived the freeze being off");
		check(not pose_list::component_tracked(c.state, true),
		      "turning it back on freezes the component immediately");
		check(not c.feed(t + frame, false, 0.5f, true),
		      "and standby samples stop reaching the interpolator again");
	}
}

} // namespace

int main()
{
	std::printf("pose NaN test\n\n");

	test_fast_math();
	test_interpolator_stale_samples();
	test_interpolator_degenerate();
	test_extrapolate_zero_span();
	test_quaternions();
	test_ingest();
	test_sanitize_boundary();
	test_standby_freeze_toggle();

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
