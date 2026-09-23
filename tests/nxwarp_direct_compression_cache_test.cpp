#include "../server/encoder/nxwarp_direct_compression_cache.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace wivrn::nxwarp_direct;

int main()
{
	auto require = [](bool ok, const char * what) {
		if (!ok)
		{
			std::fprintf(stderr, "FAIL: %s\n", what);
			std::exit(1);
		}
	};
	exact_compression_cache c;
	compression_cache_policy fast{false, true, 2160, 2160, 2};
	compression_cache_policy hc{true, true, 2160, 2160, 2};
	std::vector<uint8_t> raw{1, 2, 3, 4}, wire{9, 8};
	std::vector<uint8_t> out;
	require(!c.lookup(raw, fast, out), "initial miss");
	c.store(raw, fast, wire);
	require(c.lookup(raw, fast, out), "stored hit");
	require(out == wire, "stored wire");
	// Hit must overwrite caller storage, including after a larger prior result.
	out.assign(100, 0);
	require(c.lookup(raw, fast, out), "overwrite hit");
	require(out == wire, "overwrite wire");
	// One-byte changes, layout changes, and HC/Zstd policy changes miss.
	const std::vector<uint8_t> changed{1, 2, 3, 5};
	require(!c.lookup(changed, fast, out), "changed raw miss");
	require(!c.lookup(std::vector<uint8_t>{1, 2, 3}, fast, out), "short raw miss");
	require(!c.lookup(raw, compression_cache_policy{false, false, 2160, 2160, 2}, out), "lz4 policy miss");
	require(!c.lookup(raw, compression_cache_policy{false, true, 2160, 2160, 2, true}, out), "predictor policy miss");
	// Stored bytes must survive mutation/reallocation of both caller vectors.
	raw.assign(100, 0);
	wire.assign(100, 0);
	require(c.lookup(std::vector<uint8_t>{1, 2, 3, 4}, fast, out), "owned bytes hit");
	require(out == std::vector<uint8_t>{9, 8}, "owned wire bytes");
	raw = {1, 2, 3, 4};
	wire = {9, 8};
	require(!c.lookup(raw, compression_cache_policy{false, true, 1088, 1088, 2}, out), "geometry miss");
	require(!c.lookup(raw, hc, out), "HC policy miss");
	// Separate codec instances do not share cached bytes.
	exact_compression_cache child;
	require(!child.lookup(raw, hc, out), "child initial miss");
	child.store(raw, hc, wire);
	require(child.lookup(raw, hc, out), "child hit");
	require(!c.lookup(raw, hc, out), "parent isolation");
	// Raw fallback is still a complete cached wire result.
	c.store(raw, hc, raw);
	require(c.lookup(raw, hc, out), "raw fallback hit");
	require(out == raw, "raw fallback bytes");
	c.clear();
	require(!c.lookup(raw, hc, out), "clear miss");
	std::puts("compression cache ownership/policy: PASS");
}
