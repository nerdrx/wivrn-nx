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
	std::cout << "NXDB header/frame bounds, dynamic modes, truncation and reserved bits: PASS\n";
}
