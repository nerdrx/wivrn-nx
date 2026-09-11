// Analytic pose checks for the metadata accompanying an optical-flow warp.
// This checks rotation/translation and invalid input rejection, not optical-flow quality.
#include "../client/utils/motion_pose.h"
#include <cmath>
#include <cstdio>
#include <limits>

int main()
{
	int failures = 0;
	auto check = [&](bool ok, const char * name) {
		if (!ok) { std::printf("FAIL: %s\n", name); ++failures; }
	};
	auto near = [&](float a, float b) { return std::abs(a - b) < 1e-4f; };
	constexpr float pi = 3.14159265358979323846f;
	auto yaw = [&](float deg) { float a = deg * pi / 360; return XrQuaternionf{0, std::sin(a), 0, std::cos(a)}; };
	const XrPosef previous{yaw(0), {0, 0, 0}};
	const XrPosef current{yaw(10), {.1f, 0, 0}};
	const auto advanced = wivrn::motion_pose_extrapolate(previous, current, 1.5f);
	const float angle = 2 * std::atan2(advanced.orientation.y, advanced.orientation.w) * 180 / pi;
	check(near(angle, 15), "half interval advances 10 degree source pose to 15 degrees");
	check(near(advanced.position.x, .15f), "position follows same half interval");
	// If the real headset is at 20 degrees, the remaining rotation is five.
	// Adding the five degrees already in the flow recovers the required ten,
	// whereas retaining the original source pose would apply fifteen in total.
	check(near((angle - 10) + (20 - angle), 10), "flow plus remaining head rotation composes once");
	const auto unchanged = wivrn::motion_pose_extrapolate(previous, current, 1);
	check(near(unchanged.orientation.y, current.orientation.y), "zero warp preserves source orientation");
	auto opposite = current;
	opposite.orientation = {0, -current.orientation.y, 0, -current.orientation.w};
	const auto same_rotation = wivrn::motion_pose_extrapolate(previous, opposite, 1.5f);
	check(near(same_rotation.orientation.y, advanced.orientation.y), "quaternion sign does not choose long arc");
	const auto still = wivrn::motion_pose_extrapolate(previous, previous, 4);
	check(wivrn::motion_pose_valid(still) && near(still.orientation.w, 1), "stationary pose stays valid at cap");
	auto bad = current; bad.orientation = {0, 0, 0, 0};
	check(!wivrn::motion_pose_valid(bad), "zero quaternion rejected");
	bad.orientation = {0, 0, 0, 1.5f};
	check(!wivrn::motion_pose_valid(bad), "non-unit quaternion rejected");
	bad = current; bad.position.x = std::numeric_limits<float>::infinity();
	check(!wivrn::motion_pose_valid(bad), "infinite position rejected");
	bad = current; bad.orientation.x = std::numeric_limits<float>::quiet_NaN();
	check(!wivrn::motion_pose_valid(bad), "NaN quaternion rejected");
	check(wivrn::motion_fov_valid({-.7f, .7f, .7f, -.7f}), "normal projection accepted");
	check(!wivrn::motion_fov_valid({.7f, -.7f, .7f, -.7f}), "inverted projection rejected");
	check(!wivrn::motion_fov_valid({-1.6f, .7f, .7f, -.7f}), "singular projection rejected");
	std::printf("motion pose: %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
