#include "nxwarp_direct_compression_cache.h"
#include "nxwarp_direct_lz4.h"
#include "nxwarp_direct_zstd.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace wivrn::nxwarp_direct;
using clock_type = std::chrono::steady_clock;

static std::vector<uint8_t> read_file(const char *path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), {}};
}

static std::span<const uint8_t> pack(std::span<const uint8_t> raw,
	                                    compression_cache_policy policy,
	                                    std::vector<uint8_t> & out,
	                                    std::vector<uint8_t> & zstd)
{
	auto fast = policy.hc ? compress_lz4_hc(raw, out) : compress_lz4(raw, out);
	if (policy.zstd)
	{
		auto dense = compress_zstd(raw, zstd);
		if (dense.size() * 100 <= fast.size() * 90)
			return dense;
	}
	return fast;
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: " << argv[0] << " frame.nxdf\n";
		return 2;
	}
	const auto original = read_file(argv[1]);
	if (original.empty())
		return 2;
	const compression_cache_policy base{false, true, 2176, 2176, 2};
	std::vector<uint8_t> raw = original, out, zstd;
	exact_compression_cache cache;
	const auto wire = pack(raw, base, out, zstd);
	cache.store(raw, base, wire);

	constexpr int samples = 32;
	auto measure = [&](auto && lookup) {
		auto start = clock_type::now();
		for (int i = 0; i < samples; ++i)
			lookup(i);
		return std::chrono::duration<double, std::milli>(clock_type::now() - start).count() / samples;
	};
	const double hit_ms = measure([&](int) { cache.lookup(raw, base, out); });
	const double miss_ms = measure([&](int i) {
		raw = original;
		raw[size_t(i) % raw.size()] ^= 1;
		if (!cache.lookup(raw, base, out))
		{
			auto selected = pack(raw, base, out, zstd);
			cache.store(raw, base, selected);
		}
	});
	const double baseline_ms = measure([&](int i) {
		raw = original;
		raw[size_t(i) % raw.size()] ^= 1;
		pack(raw, base, out, zstd);
	});
	const double alternate_ms = measure([&](int i) {
		raw = original;
		const auto policy = i & 1 ? compression_cache_policy{true, true, 2176, 2176, 2}
		                          : compression_cache_policy{false, true, 2160, 2160, 2};
		if (!cache.lookup(raw, policy, out))
		{
			auto selected = pack(raw, policy, out, zstd);
			cache.store(raw, policy, selected);
		}
	});
	std::cout << "raw=" << original.size() << " hit_ms=" << hit_ms << " miss_ms=" << miss_ms
	          << " baseline_miss_ms=" << baseline_ms
	          << " alternate_policy_layout_ms=" << alternate_ms << '\n';
}
