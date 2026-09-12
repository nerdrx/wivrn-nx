#include "driver/nx_fuse_tap.h"
#include "os/os_time.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace wivrn;

// The fixture links the production serializer directly. These tiny conversions
// avoid pulling the full server's unrelated crypto/socket/logging graph into a
// standalone wire test; their layouts match the production xrt_cast helpers.
xrt_pose xrt_cast(const XrPosef & pose)
{
	return {.orientation = {.x = pose.orientation.x, .y = pose.orientation.y, .z = pose.orientation.z, .w = pose.orientation.w},
	        .position = {.x = pose.position.x, .y = pose.position.y, .z = pose.position.z}};
}
xrt_vec3 xrt_cast(const XrVector3f & vector)
{
	return {.x = vector.x, .y = vector.y, .z = vector.z};
}
xrt_quat xrt_cast(const XrQuaternionf & quaternion)
{
	return {.x = quaternion.x, .y = quaternion.y, .z = quaternion.z, .w = quaternion.w};
}
xrt_space_relation_flags from_pose_flags(uint8_t flags)
{
	return flags ? xrt_space_relation_flags(XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                         XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT)
	             : xrt_space_relation_flags(0);
}
XrTime wivrn::clock_offset::from_headset(XrTime timestamp) const
{
	return timestamp - b;
}

static int bind_receiver(const char * path)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
	if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
		return -1;
	chmod(path, 0600);
	timeval timeout{.tv_sec = 1, .tv_usec = 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	return fd;
}

static void print_packet(int fd)
{
	std::array<unsigned char, 4096> bytes{};
	const auto count = recv(fd, bytes.data(), bytes.size(), 0);
	if (count <= 0)
	{
		std::fprintf(stderr, "tap packet timeout\n");
		std::exit(2);
	}
	for (int i = 0; i < count; ++i)
		std::printf("%02x", bytes[size_t(i)]);
	std::putchar('\n');
}

static from_headset::tracking anchors()
{
	from_headset::tracking tracking{};
	tracking.timestamp = os_monotonic_get_ns();
	from_headset::tracking::pose head{};
	head.device = device_id::HEAD;
	head.pose.orientation.w = 1;
	head.pose.position.x = 1;
	head.flags = 0xff;
	tracking.device_poses.push_back(head);
	from_headset::tracking::pose left{};
	left.device = device_id::LEFT_GRIP;
	left.pose.orientation.w = 1;
	left.pose.position.x = 2;
	left.flags = 0xff;
	tracking.device_poses.push_back(left);
	return tracking;
}

int main(int argc, char ** argv)
{
	if (argc != 2)
		return 2;
	unsetenv("NX_FUSE_TAP");
	nx_fuse_tap disabled;
	clock_offset offset{.b = 0, .stable = true};
	auto tracking = anchors();
	disabled.emit_tracking(tracking, offset, 1);

	setenv("NX_FUSE_TAP", argv[1], 1);
	const int receiver = bind_receiver(argv[1]);
	if (receiver < 0)
		return 3;
	nx_fuse_tap tap;
	tap.emit_tracking(tracking, offset, tap.current_generation());
	print_packet(receiver);

	from_headset::bd_body bd{};
	bd.timestamp = os_monotonic_get_ns();
	for (auto & joint : bd.joints)
		joint.orientation = packed_quaternion::from_quaternion({.w = 1});
	bd.joints[XRT_BODY_JOINT_PELVIS_BD].position.x = 3;
	bd.joints[XRT_BODY_JOINT_PELVIS_BD].orientation = packed_quaternion::from_quaternion({.w = 1});
	bd.joints[XRT_BODY_JOINT_PELVIS_BD].flags = 0xff;
	tap.emit_bd(bd, offset, tap.current_generation());
	print_packet(receiver);

	from_headset::htc_body htc{};
	htc.timestamp = os_monotonic_get_ns();
	htc.poses[0].pose.orientation.w = 1;
	htc.poses[0].pose.position.x = 4;
	htc.poses[0].flags = 0xff;
	tap.emit_htc(htc, offset, tap.current_generation());
	print_packet(receiver);

	const auto old_generation = tap.current_generation();
	tap.recenter();
	if (tap.current_generation() == old_generation)
		return 4;
	tracking.timestamp = os_monotonic_get_ns();
	tap.emit_tracking(tracking, offset, tap.current_generation());
	print_packet(receiver);
	close(receiver);
	return 0;
}
