#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace wivrn::nxwarp_direct {

struct compression_cache_policy
{
	bool hc = false;
	bool zstd = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t eyes = 0;
	bool predictor = false;
	friend bool operator==(const compression_cache_policy &, const compression_cache_policy &) = default;
};

class exact_compression_cache
{
public:
	// Copies output into `out`; cache owns its raw and wire bytes.
	bool lookup(std::span<const uint8_t> raw, compression_cache_policy policy,
	            std::vector<uint8_t> & out) const
	{
		if (!valid || policy_ != policy || raw.size() != raw_.size() ||
		    !std::equal(raw.begin(), raw.end(), raw_.begin()))
			return false;
		out = wire_;
		return true;
	}

	void store(std::span<const uint8_t> raw, compression_cache_policy policy,
	           std::span<const uint8_t> wire)
	{
		valid = false; // A failed allocation must not expose mixed old/new bytes.
		raw_.assign(raw.begin(), raw.end());
		wire_.assign(wire.begin(), wire.end());
		policy_ = policy;
		valid = true;
	}

	void clear() { valid = false; raw_.clear(); wire_.clear(); }

private:
	bool valid = false;
	compression_cache_policy policy_{};
	std::vector<uint8_t> raw_, wire_;
};

} // namespace wivrn::nxwarp_direct
