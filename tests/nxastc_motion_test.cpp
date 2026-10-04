#include "../common/nxastc_packet_decode.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <lz4.h>
#include <span>
#include <stdexcept>
#include <vector>
#include <zstd.h>

using namespace wivrn::nxastc_packet;

static std::vector<uint8_t> zpack(std::span<const uint8_t> input, bool content_size = true)
{
	std::vector<uint8_t> out(ZSTD_compressBound(input.size()));
	if (!content_size)
	{
		ZSTD_CCtx * ctx = ZSTD_createCCtx();
		assert(ctx);
		assert(!ZSTD_isError(ZSTD_CCtx_setParameter(ctx, ZSTD_c_contentSizeFlag, 0)));
		const size_t n = ZSTD_compress2(ctx, out.data(), out.size(), input.data(), input.size());
		ZSTD_freeCCtx(ctx);
		assert(!ZSTD_isError(n));
		out.resize(n);
	}
	else
	{
		const size_t n = ZSTD_compress(out.data(), out.size(), input.data(), input.size(), 3);
		assert(!ZSTD_isError(n));
		out.resize(n);
	}
	return out;
}

static std::vector<uint8_t> make_packet(std::span<const uint8_t> header, std::span<const uint8_t> payload)
{
	std::vector<uint8_t> packet(header.begin(), header.end());
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

int main()
{
	constexpr uint32_t bw = 3, bh = 2, pixel_w = bw * 8, pixel_h = bh * 8;
	const size_t raw_size = size_t(bw) * bh * 16, packed_size = size_t(bw) * bh * 17;
	assert(motion_reference_capacity == 16 && motion_max_reference_age == 8);
	motion_decode_ack ack;
	assert(ack.frame() == independent_frame);
	ack.observe(99, false); // Receipt alone is not permission to predict from it.
	assert(ack.frame() == independent_frame);
	ack.observe(0, true);
	assert(ack.frame() == 0);
	ack.observe(5, true);
	ack.observe(3, true); // Reordered feedback cannot rewind the reference.
	ack.observe(100, false);
	assert(ack.frame() == 5);
	// Lost decode reports leave the ACK behind until an anchor recovers the stream.
	ack.observe(6, false);
	assert(ack.frame() == 5 && motion_reference_usable(13, ack.frame()));
	assert(!motion_reference_usable(14, ack.frame())); // Encoder must send an anchor now.
	ack.observe(13, true);
	assert(ack.frame() == 13 && motion_reference_usable(14, ack.frame()));
	ack.reset();
	assert(ack.frame() == independent_frame);
	ack.observe(independent_frame, true);
	assert(ack.frame() == independent_frame);
	assert(motion_reference_usable(1, 0));
	assert(motion_reference_usable(9, 1));
	assert(!motion_reference_usable(10, 1));
	assert(!motion_reference_usable(7, 7));
	assert(!motion_reference_usable(7, 8));
	assert(!motion_reference_usable(UINT64_MAX - 1, UINT64_MAX - 10));
	assert(motion_reference_usable(UINT64_MAX - 1, UINT64_MAX - 9));
	assert(!motion_reference_usable(0, independent_frame));
	std::vector<uint8_t> ref(raw_size), current(raw_size), scratch(packed_size), output(raw_size, 0xa5);
	for (size_t block = 0; block < size_t(bw) * bh; ++block)
		std::fill_n(ref.data() + block * 16, 16, uint8_t(block * 23 + 7));
	// Left edge selects the in-bounds right neighbor. Right edge cannot wrap to column zero.
	std::copy(ref.begin(), ref.end(), current.begin());
	std::copy_n(ref.data() + 16, 16, current.data());
	std::copy_n(ref.data(), 16, current.data() + (bw - 1) * 16);
	assert(encode_motion_blocks(bw, bh, ref, current, scratch));
	assert(scratch[0] == 2); // dy=-1 clamps to row zero, dx=+1 selects block one.
	const uint8_t right_selector = scratch[bw - 1];
	const uint32_t right_neighbor_x = right_selector % 3 == 0 ? bw - 2 : bw - 1;
	assert(right_neighbor_x != 0); // At right edge, every valid candidate stays in columns one or two.
	assert(reconstruct_motion_blocks(bw, bh, ref, scratch, output) && output == current);
	std::vector<uint8_t> tied_ref(raw_size, 0x5a), tied_current(raw_size, 0x5a), tied_scratch(packed_size);
	assert(encode_motion_blocks(bw, bh, tied_ref, tied_current, tied_scratch));
	assert(std::all_of(tied_scratch.begin(), tied_scratch.begin() + bw * bh, [](uint8_t s) { return s == 0; }));

	// Length failure and invalid selectors leave output untouched.
	auto untouched = std::vector<uint8_t>(raw_size, 0xa5);
	assert(!encode_motion_blocks(bw, bh, ref, current, std::span<uint8_t>(scratch).first(packed_size - 1)));
	assert(!reconstruct_motion_blocks(bw, bh, std::span<const uint8_t>(ref).first(raw_size - 1), scratch, output));
	assert(output == current);
	auto bad_selectors = scratch;
	bad_selectors[bw] = 9;
	assert(!reconstruct_motion_blocks(bw, bh, ref, bad_selectors, untouched));
	assert(untouched == std::vector<uint8_t>(raw_size, 0xa5));

	// Legacy v1 none/LZ4 and v2 Zstd wire headers remain unchanged.
	auto v1 = make_header(pixel_w, pixel_h, uint32_t(raw_size), compression::none);
	assert(v1[4] == 1 && v1[5] == 0 && v1.size() == 24);
	std::vector<uint8_t> lz4(LZ4_compressBound(int(raw_size)));
	int lz4n = LZ4_compress_default(reinterpret_cast<const char *>(current.data()), reinterpret_cast<char *>(lz4.data()), int(raw_size), int(lz4.size()));
	assert(lz4n > 0);
	lz4.resize(size_t(lz4n));
	auto v1lz4 = make_header(pixel_w, pixel_h, uint32_t(lz4.size()), compression::lz4);
	assert(v1lz4[4] == 1 && v1lz4[5] == 1);
	auto v2zstd = zpack(current);
	auto v2 = make_header(pixel_w, pixel_h, uint32_t(v2zstd.size()), compression::zstd);
	assert(v2[4] == 2 && v2[5] == 2);
	bool legacy_rejected_motion = false;
	try { (void)make_header(pixel_w, pixel_h, 1, compression::motion_zstd); }
	catch (const std::invalid_argument &) { legacy_rejected_motion = true; }
	assert(legacy_rejected_motion);
	assert(parse_packet(make_packet(v1, current)));
	assert(parse_packet(make_packet(v1lz4, lz4)));
	assert(parse_packet(make_packet(v2, v2zstd)));

	// v3 independent raw and Zstd anchors.
	auto raw_anchor = make_motion_header(pixel_w, pixel_h, uint32_t(raw_size), compression::motion_raw, independent_frame);
	auto raw_anchor_packet = make_packet(raw_anchor, current);
	auto parsed = parse_packet(raw_anchor_packet);
	assert(parsed && parsed->header_bytes == motion_header_size && parsed->reference_frame == independent_frame);
	assert(decode_motion_payload(*parsed, current, {}, output, {}) == decode_status::ok && output == current);
	auto z_anchor = zpack(current);
	auto z_anchor_header = make_motion_header(pixel_w, pixel_h, uint32_t(z_anchor.size()), compression::motion_zstd, independent_frame);
	auto z_anchor_packet = make_packet(z_anchor_header, z_anchor);
	parsed = parse_packet(z_anchor_packet);
	assert(parsed && decode_motion_payload(*parsed, z_anchor, {}, output, {}) == decode_status::ok && output == current);

	// v3 temporal Zstd stores selector prefix then 16-byte XOR residuals.
	auto z_delta = zpack(scratch);
	auto delta_header = make_motion_header(pixel_w, pixel_h, uint32_t(z_delta.size()), compression::motion_zstd, 42);
	auto delta_packet = make_packet(delta_header, z_delta);
	parsed = parse_packet(delta_packet);
	assert(parsed && parsed->header_bytes == 32 && parsed->reference_frame == 42);
	assert(decode_motion_payload(*parsed, z_delta, ref, output, scratch) == decode_status::ok && output == current);
	assert(decode_motion_payload(*parsed, z_delta, {}, output, scratch) == decode_status::motion_reference_mismatch);
	assert(decode_motion_payload(*parsed, z_delta, ref, output, std::span<uint8_t>(scratch).first(packed_size - 1)) == decode_status::motion_reference_mismatch);
	auto wrong_grid = *parsed;
	wrong_grid.width += 8;
	assert(decode_motion_payload(wrong_grid, z_delta, ref, output, scratch) == decode_status::length_mismatch);

	// Wrong reference length, malformed selectors, corrupt/truncated/trailing/unknown-size Zstd reject.
	assert(decode_motion_payload(*parsed, z_delta, std::span<const uint8_t>(ref).first(raw_size - 1), output, scratch) == decode_status::motion_reference_mismatch);
	auto malformed_frame = scratch;
	malformed_frame[0] = 9;
	auto bad_selector_zstd = zpack(malformed_frame);
	auto bad_selector_header = make_motion_header(pixel_w, pixel_h, uint32_t(bad_selector_zstd.size()), compression::motion_zstd, 42);
	parsed = parse_packet(make_packet(bad_selector_header, bad_selector_zstd));
	assert(parsed && decode_motion_payload(*parsed, bad_selector_zstd, ref, untouched, scratch) == decode_status::motion_bad_selector);
	assert(untouched == std::vector<uint8_t>(raw_size, 0xa5));
	auto truncated = z_delta;
	truncated.pop_back();
	auto truncated_header = make_motion_header(pixel_w, pixel_h, uint32_t(truncated.size()), compression::motion_zstd, 42);
	parsed = parse_packet(make_packet(truncated_header, truncated));
	assert(parsed && decode_motion_payload(*parsed, truncated, ref, output, scratch) == decode_status::zstd_bad_frame);
	parsed = parse_packet(delta_packet);
	assert(parsed);
	assert(decode_motion_payload(*parsed, z_delta, ref, output, std::span<uint8_t>(scratch).first(packed_size - 1)) == decode_status::motion_reference_mismatch);
	auto trailing = z_delta;
	trailing.push_back(0);
	auto trailing_header = make_motion_header(pixel_w, pixel_h, uint32_t(trailing.size()), compression::motion_zstd, 42);
	parsed = parse_packet(make_packet(trailing_header, trailing));
	assert(parsed && decode_motion_payload(*parsed, trailing, ref, output, scratch) == decode_status::zstd_trailing_data);
	auto unknown_size = zpack(scratch, false);
	auto unknown_header = make_motion_header(pixel_w, pixel_h, uint32_t(unknown_size.size()), compression::motion_zstd, 42);
	parsed = parse_packet(make_packet(unknown_header, unknown_size));
	assert(parsed && decode_motion_payload(*parsed, unknown_size, ref, output, scratch) == decode_status::zstd_unknown_content_size);
	auto corrupt = z_delta;
	corrupt.back() ^= 0xff;
	assert(decode_motion_payload(*parse_packet(delta_packet), corrupt, ref, output, scratch) != decode_status::ok);

	// Factory/parser forbid non-anchor motion_raw and reject bad v3 fields or packet lengths.
	bool threw = false;
	try { (void)make_motion_header(pixel_w, pixel_h, uint32_t(raw_size), compression::motion_raw, 7); }
	catch (const std::invalid_argument &) { threw = true; }
	assert(threw);
	auto bad_raw_anchor = std::vector<uint8_t>(raw_anchor_packet);
	bad_raw_anchor[24] = 1;
	assert(!parse_packet(bad_raw_anchor));
	auto short_packet = raw_anchor_packet;
	short_packet.pop_back();
	assert(!parse_packet(short_packet));
	auto extra_packet = raw_anchor_packet;
	extra_packet.push_back(0);
	assert(!parse_packet(extra_packet));
	auto short_header = std::vector<uint8_t>(raw_anchor.begin(), raw_anchor.begin() + 31);
	assert(!parse_packet(short_header));
	assert(!parse_packet({}));

	// Losing a delta reference resumes cleanly on an independent anchor.
	std::fill(output.begin(), output.end(), 0);
	parsed = parse_packet(z_anchor_packet);
	assert(parsed && decode_motion_payload(*parsed, z_anchor, {}, output, {}) == decode_status::ok && output == current);
	std::puts("NX ASTC v1/v2 compatibility, v3 anchor/delta, predictor bounds, loss recovery, and rejects: PASS");
}
