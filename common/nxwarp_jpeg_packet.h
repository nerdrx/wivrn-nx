/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace wivrn
{

inline constexpr size_t nxwarp_jpeg_header_size = 25;
inline constexpr size_t nxwarp_jpeg_payload_limit = 1100;
inline constexpr size_t nxwarp_jpeg_chunk_size = nxwarp_jpeg_payload_limit - nxwarp_jpeg_header_size;

// Build one bounded NXJ2 UDP payload. Empty result means invalid metadata/chunk.
inline std::vector<uint8_t> nxwarp_jpeg_chunk_payload(uint32_t frame,
                                                       uint8_t eye,
                                                       uint16_t width,
                                                       uint16_t height,
                                                       uint16_t index,
                                                       uint16_t count,
                                                       uint32_t total_bytes,
                                                       std::span<const uint8_t> chunk)
{
	if (eye > 1 || width != 544 || height != 544 || !count || count > 2048 || index >= count ||
	    !total_bytes || total_bytes > 1024 * 1024)
		return {};
	const size_t expected_count = (total_bytes + nxwarp_jpeg_chunk_size - 1) / nxwarp_jpeg_chunk_size;
	const size_t expected_bytes = index + 1 == count
	                                      ? total_bytes - size_t(index) * nxwarp_jpeg_chunk_size
	                                      : nxwarp_jpeg_chunk_size;
	if (count != expected_count || chunk.size() != expected_bytes)
		return {};
	std::vector<uint8_t> payload(nxwarp_jpeg_header_size + chunk.size());
	std::memcpy(payload.data(), "NXJ2", 4);
	auto be16 = [&](size_t i, uint16_t v) { payload[i] = uint8_t(v >> 8); payload[i + 1] = uint8_t(v); };
	auto be32 = [&](size_t i, uint32_t v) {
		payload[i] = uint8_t(v >> 24); payload[i + 1] = uint8_t(v >> 16);
		payload[i + 2] = uint8_t(v >> 8); payload[i + 3] = uint8_t(v);
	};
	be32(4, frame);
	payload[8] = eye;
	be16(9, 0);
	be16(11, 0);
	be16(13, width);
	be16(15, height);
	be16(17, index);
	be16(19, count);
	be32(21, total_bytes);
	std::memcpy(payload.data() + nxwarp_jpeg_header_size, chunk.data(), chunk.size());
	return payload;
}

} // namespace wivrn
