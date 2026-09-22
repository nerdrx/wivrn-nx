#include "nxwarp_direct.h"
#include <cassert>
#include <iostream>
using namespace wivrn::nxwarp_direct;
int main()
{
	layout l{32, 32, 1};
	auto h = stream_header(l);
	assert(parse_stream(h));
	assert(parse_stream(stream_header(l, true)));
	assert(read32(stream_header(l, true), 4) == 2);
	for (size_t n = 0; n < h.size(); ++n)
		assert(!parse_stream(std::span(h).first(n)));
	auto bad = h;
	bad[4] = 99;
	assert(!parse_stream(bad));
	assert(stream_header({4097, 32, 1}).empty());
	assert(stream_header({32, 32, 3}).empty());
	for (uint32_t mode = 0; mode < 4; mode++)
	{
		uint32_t count = mode == 3 ? 0 : 80u >> (2 * mode);
		auto f = frame_header(1, count);
		append32(f, mode == 3 ? 0xc0123456u : mode << 30);
		f.resize(f.size() + count * 4);
		auto v = parse_frame(l, f);
		assert(v);
		assert(v->blocks.size() == count * 4);
		for (size_t n = 0; n < f.size(); ++n)
			assert(!parse_frame(l, std::span(f).first(n)));
		auto trailing = f;
		trailing.push_back(0);
		assert(!parse_frame(l, trailing));
		auto corrupt = f;
		corrupt[16] |= mode == 3 ? 0 : 1;
		if (mode != 3)
			assert(!parse_frame(l, corrupt));
		if (mode == 3)
		{
			corrupt = f;
			corrupt[19] |= 1;
			assert(!parse_frame(l, corrupt));
		}
	}
	auto f = frame_header(1, 0);
	append32(f, 0);
	assert(!parse_frame(l, f));
	f = frame_header(2, 0);
	append32(f, 0xc0000000);
	append32(f, 0xc0000000);
	assert(!parse_frame(l, f));
	// Native centre negotiation is explicit: v7/v8 streams carry the larger
	// bound, while their safety layout remains the legacy v1 shape.
	layout native{32, 32, 1, true};
	layout native_stream{256, 32, 2, true};
	assert(stream_header(native).empty());
	assert(stream_header(native_stream, false, false, true).empty());
	assert(read32(stream_header(native_stream, false, true, true), 4) == 7);
	assert(read32(stream_header(native_stream, true, true, true), 4) == 8);
	assert(parse_stream(stream_header(native_stream, false, true, true))->native_center);
	assert(!parse_frame(native, frame_header(1, 0, native_version)));
	auto nf = frame_header(1, native_rgb_words, native_version);
	append32(nf, 0x20000000u);
	nf.resize(nf.size() + native_rgb_words * 4);
	assert(parse_frame(native, nf));
	// A native descriptor cannot be smuggled into a legacy stream or use a
	// non-zero mode; offsets remain five-word aligned and bounded.
	auto legacy_native = frame_header(1, native_rgb_words, version);
	append32(legacy_native, 0x20000000u);
	legacy_native.resize(legacy_native.size() + native_rgb_words * 4);
	assert(!parse_frame(native, legacy_native));
	auto bad_native = nf;
	bad_native[16] = 0x00;
	bad_native[17] = 0x00;
	bad_native[18] = 0x00;
	bad_native[19] = 0x60; // mode 1 plus native bit
	assert(!parse_frame(native, bad_native));
	auto old_solid = frame_header(1, 0);
	append32(old_solid, 0xc0123456u);
	assert(parse_frame(native, old_solid)); // old v1 payloads remain accepted
	auto truncated_native = nf; truncated_native.pop_back();
	assert(!parse_frame(native, truncated_native));
	auto unaligned_native = nf; unaligned_native[16] = 1;
	assert(!parse_frame(native, unaligned_native));
	std::cout << "NXDB header/frame bounds, dynamic modes, truncation and reserved bits: PASS\n";
}
