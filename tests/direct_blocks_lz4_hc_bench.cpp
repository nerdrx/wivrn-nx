#include "nxwarp_direct_lz4.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

static Bytes read(const char *path)
{
	std::ifstream f(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(f), {});
}

static double median(std::vector<double> values)
{
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}
static double p95(std::vector<double> values)
{
	std::sort(values.begin(), values.end());
	return values[(values.size() * 95) / 100];
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s fixture.nxdf [...]\n", argv[0]);
		return 2;
	}
	for (int arg = 1; arg < argc; ++arg)
	{
		const Bytes raw = read(argv[arg]);
		if (raw.empty())
			return 2;
		for (int level : {LZ4HC_CLEVEL_MIN, 3, 6, 9})
		{
			Bytes fast_out, hc_out, round;
			std::vector<double> fast_ms, hc_ms;
			for (int i = 0; i < 48; ++i)
			{
				auto start = Clock::now();
				auto fast = wivrn::nxwarp_direct::compress_lz4(raw, fast_out);
				fast_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
				start = Clock::now();
				auto hc = wivrn::nxwarp_direct::compress_lz4_hc(raw, hc_out, level);
				hc_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
				if ((wivrn::nxwarp_direct::is_lz4(fast) &&
				    (!wivrn::nxwarp_direct::decompress_lz4({2176, 2176, 2}, fast, round) || round != raw)) ||
				    (wivrn::nxwarp_direct::is_lz4(hc) &&
				    (!wivrn::nxwarp_direct::decompress_lz4({2176, 2176, 2}, hc, round) || round != raw))
				   )
					return 1;
			}
			fast_ms.erase(fast_ms.begin(), fast_ms.begin() + 8);
			hc_ms.erase(hc_ms.begin(), hc_ms.begin() + 8);
			const auto fast = wivrn::nxwarp_direct::compress_lz4(raw, fast_out);
			const auto hc = wivrn::nxwarp_direct::compress_lz4_hc(raw, hc_out, level);
			std::printf("%s,level=%d,raw=%zu,fast=%zu,hc=%zu,delta_bytes=%zd,fast_p50_ms=%.4f,fast_p95_ms=%.4f,hc_p50_ms=%.4f,hc_p95_ms=%.4f\n",
			            argv[arg], level, raw.size(), fast.size(), hc.size(), ssize_t(hc.size()) - ssize_t(fast.size()),
			            median(fast_ms), p95(fast_ms), median(hc_ms), p95(hc_ms));
		}
	}
}
