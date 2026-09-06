// test_base_shadow -- the encoder's shadow decode reproduces the headset's
// pixels, and reproduces them IN ORDER.
//
// Feeds the same access units the device harness fed the Pico, and compares
// against ffmpeg's own decode of the same stream (which docs/NXWARP-HYBRID.md
// 3.3 measured byte-identical to the Pico's ASIC on 180/180 frames).
//
//   test_base_shadow S.hevc S.idx S.ref.yuv [frames]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "base_shadow.h"

static std::vector<uint8_t> slurp(const char * p)
{
	FILE * f = fopen(p, "rb");
	if (not f) { fprintf(stderr, "open %s\n", p); exit(2); }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> v(static_cast<size_t>(n));
	if (fread(v.data(), 1, size_t(n), f) != size_t(n)) exit(2);
	fclose(f);
	return v;
}

int main(int argc, char ** argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: test_base_shadow S.hevc S.idx S.ref.yuv [frames]\n");
		return 1;
	}
	const int want = argc > 4 ? atoi(argv[4]) : 24;

	auto stream = slurp(argv[1]);
	std::vector<std::pair<size_t, size_t>> aus;
	{
		FILE * f = fopen(argv[2], "r");
		if (not f) { fprintf(stderr, "open %s\n", argv[2]); return 2; }
		unsigned long off, sz; int key;
		while (fscanf(f, "%lu %lu %d", &off, &sz, &key) == 3)
			aus.push_back({off, sz});
		fclose(f);
	}
	FILE * ref = fopen(argv[3], "rb");
	if (not ref) { fprintf(stderr, "open %s\n", argv[3]); return 2; }

	wivrn::base_shadow_decoder dec;

	int n = 0, mismatched = 0, out_of_order = 0;
	std::vector<uint8_t> rbuf;
	for (size_t i = 0; i < aus.size() and n < want; i++)
	{
		auto f = dec.decode(stream.data() + aus[i].first, aus[i].second, int64_t(i));
		if (not f.valid)
		{
			// LOW_DELAY + P-only should never buffer; if it does, that is the
			// reordering this component exists to avoid, so say so loudly.
			fprintf(stderr, "  AU %zu produced no picture (decoder is buffering)\n", i);
			continue;
		}
		if (f.pts != int64_t(i))
		{
			out_of_order++;
			fprintf(stderr, "  AU %zu came back as pts %lld\n", i, (long long)f.pts);
		}
		const size_t fs = size_t(f.width) * f.height +
		                  2 * size_t(f.cw) * f.ch;
		rbuf.resize(fs);
		if (fread(rbuf.data(), 1, fs, ref) != fs) break;
		const uint8_t * ry = rbuf.data();
		const uint8_t * rcb = ry + size_t(f.width) * f.height;
		const uint8_t * rcr = rcb + size_t(f.cw) * f.ch;
		const bool same = memcmp(f.y.data(), ry, f.y.size()) == 0 and
		                  memcmp(f.cb.data(), rcb, f.cb.size()) == 0 and
		                  memcmp(f.cr.data(), rcr, f.cr.size()) == 0;
		if (not same) { mismatched++; fprintf(stderr, "  frame %d differs\n", n); }
		n++;
	}
	fclose(ref);

	const std::string colour = dec.check_colour();
	printf("shadow decode: %d frames, %d mismatched, %d out of order\n",
	       n, mismatched, out_of_order);
	printf("colour check : %s\n", colour.empty() ? "ok (full range, BT.709, yuv420p)"
	                                             : colour.c_str());
	const bool pass = n == want and mismatched == 0 and out_of_order == 0;
	printf("%s\n", pass ? "PASS" : "FAILED");
	return pass ? 0 : 1;
}
