#include "nx_fuse_tap.h"

#include "xrt_cast.h"
#include "os/os_time.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <string_view>

namespace wivrn
{
namespace
{
constexpr uint16_t role_head = 1;
constexpr uint16_t role_left_grip = 2;
constexpr uint16_t role_right_grip = 3;
constexpr uint16_t role_left_aim = 4;
constexpr uint16_t role_right_aim = 5;
constexpr uint16_t role_left_palm = 6;
constexpr uint16_t role_right_palm = 7;
constexpr uint16_t role_hip = 10;
constexpr uint16_t role_chest = 11;
constexpr uint16_t role_left_elbow = 12;
constexpr uint16_t role_right_elbow = 13;
constexpr uint16_t role_left_knee = 14;
constexpr uint16_t role_right_knee = 15;
constexpr uint16_t role_left_foot = 16;
constexpr uint16_t role_right_foot = 17;
constexpr uint16_t role_htc_generic_base = 0x8000;

uint32_t normalized_flags(xrt_space_relation_flags flags)
{
	uint32_t out = 0;
	if (flags & XRT_SPACE_RELATION_POSITION_VALID_BIT)
		out |= 1u << 0;
	if (flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)
		out |= 1u << 1;
	if (flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT)
		out |= 1u << 2;
	if (flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT)
		out |= 1u << 3;
	if (flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT)
		out |= 1u << 4;
	if (flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT)
		out |= 1u << 5;
	return out;
}

void put_u16(std::byte * out, uint16_t value)
{
	out[0] = std::byte(value & 0xff);
	out[1] = std::byte(value >> 8);
}
void put_u32(std::byte * out, uint32_t value)
{
	for (int i = 0; i < 4; ++i)
		out[i] = std::byte(value >> (i * 8));
}
void put_u64(std::byte * out, uint64_t value)
{
	for (int i = 0; i < 8; ++i)
		out[i] = std::byte(value >> (i * 8));
}
void put_f32(std::byte * out, float value)
{
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	put_u32(out, bits);
}
} // namespace

nx_fuse_tap::nx_fuse_tap()
{
	generation = os_monotonic_get_ns();
	if (generation == 0)
		generation = 1;
	const char * path = std::getenv("NX_FUSE_TAP");
	if (path == nullptr || path[0] == '\0' || std::strlen(path) >= sizeof(sockaddr_un::sun_path))
		return;

	struct stat st{};
	if (lstat(path, &st) != 0 || not S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) != 0)
		return;
	std::array<char, sizeof(sockaddr_un::sun_path)> parent{};
	std::strncpy(parent.data(), path, parent.size() - 1);
	if (char * slash = std::strrchr(parent.data(), '/'); slash != nullptr)
	{
		if (slash == parent.data())
			slash[1] = '\0';
		else
			slash[0] = '\0';
	}
	else
		std::strcpy(parent.data(), ".");
	struct stat parent_st{};
	if (lstat(parent.data(), &parent_st) != 0 || not S_ISDIR(parent_st.st_mode) || parent_st.st_uid != geteuid() || (parent_st.st_mode & 0077) != 0)
		return;

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
	socket_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (socket_fd < 0 || connect(socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
	{
		if (socket_fd >= 0)
			close(socket_fd);
		socket_fd = -1;
	}
}

nx_fuse_tap::~nx_fuse_tap()
{
	if (socket_fd >= 0)
		close(socket_fd);
}

void nx_fuse_tap::recenter()
{
	++generation;
	if (generation == 0)
		generation = 1;
}

void nx_fuse_tap::emit(uint8_t route, uint64_t generation, XrTime sample_time,
                       const std::array<xrt_space_relation, max_records> & poses,
                       const std::array<uint16_t, max_records> & roles,
                       const std::array<uint16_t, max_records> & sources, size_t count,
                       const clock_offset & offset)
{
	if (socket_fd < 0 || not offset || count == 0 || count > max_records)
		return;
	std::array<std::byte, datagram_size> packet{};
	std::memcpy(packet.data(), "NXTP", 4);
	put_u16(packet.data() + 4, 1);
	put_u16(packet.data() + 6, header_size);
	put_u64(packet.data() + 8, sequence++);
	put_u64(packet.data() + 16, os_monotonic_get_ns());
	put_u64(packet.data() + 24, generation);
	packet[32] = std::byte(route);
	packet[33] = std::byte(count);
	put_u16(packet.data() + 34, offset ? 1 : 0);

	for (size_t i = 0; i < count; ++i)
	{
		auto * out = packet.data() + header_size + i * record_size;
		put_u16(out, roles[i]);
		put_u16(out + 2, sources[i]);
		put_u32(out + 4, normalized_flags(poses[i].relation_flags));
		put_f32(out + 8, poses[i].pose.position.x);
		put_f32(out + 12, poses[i].pose.position.y);
		put_f32(out + 16, poses[i].pose.position.z);
		put_f32(out + 20, poses[i].pose.orientation.x);
		put_f32(out + 24, poses[i].pose.orientation.y);
		put_f32(out + 28, poses[i].pose.orientation.z);
		put_f32(out + 32, poses[i].pose.orientation.w);
		put_f32(out + 36, poses[i].linear_velocity.x);
		put_f32(out + 40, poses[i].linear_velocity.y);
		put_f32(out + 44, poses[i].linear_velocity.z);
		put_f32(out + 48, poses[i].angular_velocity.x);
		put_f32(out + 52, poses[i].angular_velocity.y);
		put_f32(out + 56, poses[i].angular_velocity.z);
		put_u64(out + 60, offset ? offset.from_headset(sample_time) : 0);
	}
	(void)send(socket_fd, packet.data(), header_size + count * record_size, MSG_DONTWAIT | MSG_NOSIGNAL);
}

void nx_fuse_tap::emit_tracking(const from_headset::tracking & tracking, const clock_offset & offset, uint64_t generation)
{
	if (socket_fd < 0 || not offset)
		return;
	std::array<xrt_space_relation, max_records> poses{};
	std::array<uint16_t, max_records> roles{};
	std::array<uint16_t, max_records> sources{};
	size_t count = 0;
	for (const auto & pose : tracking.device_poses)
	{
		uint16_t role;
		switch (pose.device)
		{
		case device_id::HEAD: role = role_head; break;
		case device_id::LEFT_GRIP: role = role_left_grip; break;
		case device_id::LEFT_AIM: role = role_left_aim; break;
		case device_id::LEFT_PALM: role = role_left_palm; break;
		case device_id::RIGHT_GRIP: role = role_right_grip; break;
		case device_id::RIGHT_AIM: role = role_right_aim; break;
		case device_id::RIGHT_PALM: role = role_right_palm; break;
		default: continue;
		}
		if (count == max_records)
			break;
		poses[count] = {.relation_flags = from_pose_flags(pose.flags), .pose = xrt_cast(pose.pose),
		                .linear_velocity = xrt_cast(pose.linear_velocity), .angular_velocity = xrt_cast(pose.angular_velocity)};
		roles[count] = role;
		sources[count++] = static_cast<uint16_t>(pose.device);
	}
	emit(0, generation, tracking.timestamp, poses, roles, sources, count, offset);
}

void nx_fuse_tap::emit_bd(const from_headset::bd_body & tracking, const clock_offset & offset, uint64_t generation)
{
	if (socket_fd < 0 || not offset)
		return;
	std::array<xrt_space_relation, max_records> poses{};
	std::array<uint16_t, max_records> roles{};
	std::array<uint16_t, max_records> sources{};
	for (size_t i = 0; i < tracking.joints.size() && i < max_records; ++i)
	{
		const auto & pose = tracking.joints[i];
		poses[i] = {.relation_flags = from_pose_flags(pose.flags), .pose = xrt_cast(XrPosef{.orientation = pose.orientation, .position = pose.position})};
		switch (i)
		{
		case XRT_BODY_JOINT_PELVIS_BD: roles[i] = role_hip; break;
		case XRT_BODY_JOINT_SPINE3_BD: roles[i] = role_chest; break;
		case XRT_BODY_JOINT_LEFT_ELBOW_BD: roles[i] = role_left_elbow; break;
		case XRT_BODY_JOINT_RIGHT_ELBOW_BD: roles[i] = role_right_elbow; break;
		case XRT_BODY_JOINT_LEFT_KNEE_BD: roles[i] = role_left_knee; break;
		case XRT_BODY_JOINT_RIGHT_KNEE_BD: roles[i] = role_right_knee; break;
		case XRT_BODY_JOINT_LEFT_FOOT_BD: roles[i] = role_left_foot; break;
		case XRT_BODY_JOINT_RIGHT_FOOT_BD: roles[i] = role_right_foot; break;
		default: roles[i] = uint16_t(0x0100 | i); break;
		}
		sources[i] = uint16_t(i);
	}
	emit(1, generation, tracking.timestamp, poses, roles, sources, tracking.joints.size(), offset);
}

void nx_fuse_tap::emit_htc(const from_headset::htc_body & tracking, const clock_offset & offset, uint64_t generation)
{
	if (socket_fd < 0 || not offset)
		return;
	std::array<xrt_space_relation, max_records> poses{};
	std::array<uint16_t, max_records> roles{};
	std::array<uint16_t, max_records> sources{};
	for (size_t i = 0; i < tracking.poses.size() && i < max_records; ++i)
	{
		const auto & pose = tracking.poses[i];
		poses[i] = {.relation_flags = from_pose_flags(pose.flags), .pose = xrt_cast(pose.pose),
		            .linear_velocity = xrt_cast(pose.linear_velocity), .angular_velocity = xrt_cast(pose.angular_velocity)};
		roles[i] = uint16_t(role_htc_generic_base | i);
		sources[i] = uint16_t(i);
	}
	emit(2, generation, tracking.timestamp, poses, roles, sources, tracking.poses.size(), offset);
}

} // namespace wivrn
