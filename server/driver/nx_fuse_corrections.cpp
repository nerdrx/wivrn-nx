#include "nx_fuse_corrections.h"
#include "os/os_time.h"
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace wivrn
{
namespace
{
constexpr size_t header_size = 40, record_size = 16, camera_record_size = 32, max_packet = header_size + 8 * record_size;
uint16_t get16(const std::byte * p)
{
	return uint16_t(std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8));
}
uint64_t get64(const std::byte * p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i)
		v |= uint64_t(std::to_integer<unsigned>(p[i])) << (8 * i);
	return v;
}
float getf(const std::byte * p)
{
	uint32_t bits = uint32_t(get16(p)) | (uint32_t(get16(p + 2)) << 16);
	float v;
	std::memcpy(&v, &bits, 4);
	return v;
}
bool role(uint16_t r)
{
	return r == 10 || r == 14 || r == 15 || r == 16 || r == 17;
}
} // namespace

bool nx_fuse_corrections::bind_socket()
{
	const char * env = std::getenv("NX_FUSE_CORRECTIONS");
	if (!env || !*env || std::strlen(env) >= sizeof(sockaddr_un::sun_path))
		return false;
	path = env;
	struct stat parent{};
	auto slash = path.find_last_of('/');
	std::string dir = slash == std::string::npos ? "." : (slash ? path.substr(0, slash) : "/");
	if (lstat(dir.c_str(), &parent) || !S_ISDIR(parent.st_mode) || parent.st_uid != geteuid() || (parent.st_mode & 0077))
		return false;
	struct stat existing{};
	if (!lstat(path.c_str(), &existing) && (not S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid() || (existing.st_mode & 0077)))
		return false;
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return false;
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
	if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
	{
		close_socket();
		return false;
	}
	struct stat owned{};
	if (lstat(path.c_str(), &owned))
	{
		close_socket();
		return false;
	}
	inode = owned.st_ino;
	device = owned.st_dev;
	if (chmod(path.c_str(), 0600))
	{
		close_socket();
		return false;
	}
	return true;
}

void nx_fuse_corrections::close_socket()
{
	if (fd >= 0)
		close(fd);
	fd = -1;
	if (inode)
	{
		struct stat st{};
		if (!lstat(path.c_str(), &st) && st.st_ino == inode && st.st_dev == device && S_ISSOCK(st.st_mode))
			unlink(path.c_str());
	}
	inode = 0;
}

nx_fuse_corrections::nx_fuse_corrections()
{
	bind_socket();
}
nx_fuse_corrections::~nx_fuse_corrections()
{
	close_socket();
}

void nx_fuse_corrections::invalidate(uint64_t current_generation)
{
	generation = current_generation;
	sequence = sample_time = 0;
	count = camera_count = 0;
	enabled = camera_enabled = false;
}

void nx_fuse_corrections::poll(uint64_t current_generation)
{
	if (generation != current_generation)
		invalidate(current_generation);
	if (fd < 0)
		return;
	std::array<std::byte, header_size + 8 * camera_record_size + 1> packet{};
	for (int n = 0; n < 8; ++n)
	{
		const ssize_t size = recv(fd, packet.data(), packet.size(), MSG_DONTWAIT);
		if (size < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				break;
			continue;
		}
		if (size < (ssize_t)header_size || size > (ssize_t)(header_size + 8 * camera_record_size))
			continue;
		if (std::memcmp(packet.data(), "NXFC", 4) || get16(packet.data() + 4) != 1 || get16(packet.data() + 6) != header_size || get64(packet.data() + 24) != current_generation || std::to_integer<uint8_t>(packet[32]) > 1 || packet[34] != std::byte(1) || packet[35] != std::byte(0) || packet[36] != std::byte(0) || packet[37] != std::byte(0) || packet[38] != std::byte(0) || packet[39] != std::byte(0))
			continue;
		const uint8_t number = std::to_integer<uint8_t>(packet[33]);
		const uint8_t route = std::to_integer<uint8_t>(packet[32]);
		const size_t rec_size = route == 1 ? camera_record_size : record_size;
		if ((route > 1) || number > (route == 1 ? 5 : 8) || size != (ssize_t)(header_size + number * rec_size))
			continue;
		const uint64_t host = get64(packet.data() + 16), now = os_monotonic_get_ns();
		const uint64_t next_sequence = get64(packet.data() + 8);
		if (!current_generation || next_sequence <= sequence || host > now || now - host >= 150'000'000)
			continue;
		std::array<item, 8> next{};
		std::array<camera_pose, 5> next_camera{};
		bool valid = true;
		for (uint8_t i = 0; i < number; ++i)
		{
			const auto * p = packet.data() + header_size + i * rec_size;
			const uint16_t r = get16(p);
			const uint16_t reserved = get16(p + 2);
			const float x = getf(p + 4), y = getf(p + 8), z = getf(p + 12);
			if (route == 1)
			{
				const float qx = getf(p + 16), qy = getf(p + 20), qz = getf(p + 24), qw = getf(p + 28);
				auto bad = [](const float v) { uint32_t b; std::memcpy(&b,&v,4); return (b & 0x7f800000u) == 0x7f800000u; };
				const float norm = qx * qx + qy * qy + qz * qz + qw * qw;
				if (!role(r) || reserved || bad(x) || bad(y) || bad(z) || bad(qx) || bad(qy) || bad(qz) || bad(qw) || std::abs(x) > 20.f || std::abs(y) > 20.f || std::abs(z) > 20.f || std::abs(norm - 1.f) > .05f)
				{
					valid = false;
					break;
				}
				for (uint8_t j = 0; j < i; ++j)
					if (next_camera[j].role == r)
						valid = false;
				if (!valid)
					break;
				next_camera[i] = {r, {x, y, z}, {qx, qy, qz, qw}};
				continue;
			}
			// Validate IEEE exponent bits: server builds use fast-math, which can remove isfinite checks.
			if (!role(r) || reserved || (get16(p + 6) & 0x7f80) == 0x7f80 || (get16(p + 10) & 0x7f80) == 0x7f80 || (get16(p + 14) & 0x7f80) == 0x7f80 || std::abs(x) > .25f || std::abs(y) > .25f || std::abs(z) > .25f || x * x + y * y + z * z > .0625f)
			{
				valid = false;
				break;
			}
			for (uint8_t j = 0; j < i; ++j)
				if (next[j].role == r)
					valid = false;
			if (!valid)
				break;
			next[i] = {r, x, y, z};
		}
		if (valid)
		{
			generation = current_generation;
			sequence = next_sequence;
			sample_time = host;
			if (route == 1)
			{
				camera_items = next_camera;
				camera_count = number;
				camera_enabled = number != 0;
				count = 0;
				enabled = false;
			}
			else
			{
				items = next;
				count = number;
				enabled = number != 0;
				camera_count = 0;
				camera_enabled = false;
			}
		}
	}
}

void nx_fuse_corrections::apply(from_headset::bd_body & body, uint64_t current_generation) const
{
	const uint64_t now = os_monotonic_get_ns();
	if (!enabled || generation != current_generation || sample_time > now || now - sample_time >= 150'000'000)
		return;
	for (size_t i = 0; i < count; ++i)
		for (size_t j = 0; j < body.joints.size(); ++j)
		{
			uint16_t r = j == XR_BODY_JOINT_PELVIS_BD ? 10 : j == XR_BODY_JOINT_LEFT_KNEE_BD ? 14
			                                         : j == XR_BODY_JOINT_RIGHT_KNEE_BD      ? 15
			                                         : j == XR_BODY_JOINT_LEFT_FOOT_BD       ? 16
			                                         : j == XR_BODY_JOINT_RIGHT_FOOT_BD      ? 17
			                                                                                 : 0;
			if (r == items[i].role && (body.joints[j].flags & 0x22) == 0x22)
			{
				body.joints[j].position.x += items[i].x;
				body.joints[j].position.y += items[i].y;
				body.joints[j].position.z += items[i].z;
			}
		}
}

size_t nx_fuse_corrections::camera_snapshot(uint64_t current_generation, std::array<camera_pose, 5> & out, uint64_t * sample_out) const
{
	const uint64_t now = os_monotonic_get_ns();
	if (!camera_enabled || generation != current_generation || sample_time > now || now - sample_time >= 150'000'000)
		return 0;
	out = camera_items;
	if (sample_out)
		*sample_out = sample_time;
	return camera_count;
}
} // namespace wivrn
