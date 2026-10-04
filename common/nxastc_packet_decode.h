#pragma once

#include "nxastc_packet.h"

#include <climits>
#include <cstring>
#include <lz4.h>
#include <span>
#include <zstd.h>

namespace wivrn::nxastc_packet
{
enum class decode_status
{
	ok,
	length_mismatch,
	lz4_mismatch,
	zstd_bad_frame,
	zstd_trailing_data,
	zstd_unknown_content_size,
	zstd_wrong_content_size,
	zstd_decode_error,
};

inline const char * decode_status_message(decode_status status)
{
	switch (status)
	{
	case decode_status::ok: return "ok";
	case decode_status::length_mismatch: return "payload/output length mismatch";
	case decode_status::lz4_mismatch: return "LZ4 payload did not decode to exact block length";
	case decode_status::zstd_bad_frame: return "invalid Zstd frame";
	case decode_status::zstd_trailing_data: return "Zstd payload has trailing data";
	case decode_status::zstd_unknown_content_size: return "Zstd frame has no declared content size";
	case decode_status::zstd_wrong_content_size: return "Zstd frame content size mismatch";
	case decode_status::zstd_decode_error: return "Zstd payload did not decode to exact block length";
	}
	return "unknown ASTC payload error";
}

// Decode into caller-owned storage; no allocation and no state retained.
inline decode_status decode_payload(const packet_header & header,
                                   std::span<const uint8_t> payload,
                                   std::span<uint8_t> output)
{
	if (payload.size() != header.payload_bytes || output.size() != header.raw_bytes)
		return decode_status::length_mismatch;
	if (header.encoding == compression::none)
	{
		if (payload.size() != output.size())
			return decode_status::length_mismatch;
		std::memcpy(output.data(), payload.data(), payload.size());
		return decode_status::ok;
	}
	if (header.encoding == compression::lz4)
	{
		if (payload.size() > INT_MAX || output.size() > INT_MAX)
			return decode_status::length_mismatch;
		const int decoded = LZ4_decompress_safe(reinterpret_cast<const char *>(payload.data()),
		                                        reinterpret_cast<char *>(output.data()),
		                                        int(payload.size()),
		                                        int(output.size()));
		return decoded == int(output.size()) ? decode_status::ok : decode_status::lz4_mismatch;
	}
	if (header.encoding != compression::zstd)
		return decode_status::length_mismatch;

	const size_t frame_size = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
	if (ZSTD_isError(frame_size))
		return decode_status::zstd_bad_frame;
	if (frame_size != payload.size())
		return decode_status::zstd_trailing_data;
	const unsigned long long content_size = ZSTD_getFrameContentSize(payload.data(), payload.size());
	if (content_size == ZSTD_CONTENTSIZE_ERROR)
		return decode_status::zstd_bad_frame;
	if (content_size == ZSTD_CONTENTSIZE_UNKNOWN)
		return decode_status::zstd_unknown_content_size;
	if (content_size != output.size())
		return decode_status::zstd_wrong_content_size;
	const size_t decoded = ZSTD_decompress(output.data(), output.size(), payload.data(), payload.size());
	if (ZSTD_isError(decoded) || decoded != output.size())
		return decode_status::zstd_decode_error;
	return decode_status::ok;
}
} // namespace wivrn::nxastc_packet
