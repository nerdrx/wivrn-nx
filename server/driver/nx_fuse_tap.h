#pragma once

#include "clock_offset.h"
#include "wivrn_packets.h"
#include "xrt/xrt_defines.h"

#include <array>
#include <cstdint>

namespace wivrn
{

class nx_fuse_tap
{
	static constexpr size_t max_records = 32;
	static constexpr size_t header_size = 40;
	static constexpr size_t record_size = 68;
	static constexpr size_t datagram_size = header_size + max_records * record_size;

	int socket_fd = -1;
	uint64_t sequence = 1;
	uint64_t generation = 1;

	void emit(uint8_t route, uint64_t generation, XrTime sample_time, const std::array<xrt_space_relation, max_records> & poses,
	          const std::array<uint16_t, max_records> & roles, const std::array<uint16_t, max_records> & sources, size_t count,
	          const clock_offset & offset);

public:
	nx_fuse_tap();
	~nx_fuse_tap();

	nx_fuse_tap(const nx_fuse_tap &) = delete;
	nx_fuse_tap & operator=(const nx_fuse_tap &) = delete;

	void emit_tracking(const from_headset::tracking &, const clock_offset &, uint64_t generation);
	void emit_bd(const from_headset::bd_body &, const clock_offset &, uint64_t generation);
	void emit_htc(const from_headset::htc_body &, const clock_offset &, uint64_t generation);
	void recenter();
	uint64_t current_generation() const { return generation; }
};

} // namespace wivrn
