#include "nxwarp_direct_recovery.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace wivrn::nxwarp_direct;
using bytes = std::vector<uint8_t>;

static void put(bytes & b, uint32_t v)
{
	append32(b, v);
}

static bytes frame(uint32_t a, uint32_t b, uint32_t words = 80)
{
	bytes out = frame_header(2, words);
	put(out, a); put(out, b);
	for (uint32_t i = 0; i < words; ++i) put(out, 0x10000000u + i);
	return out;
}

static std::vector<bytes> chunks(const bytes & unit, size_t chunk, size_t missing = SIZE_MAX)
{
	bytes wire;
	put(wire, uint32_t(unit.size()));
	wire.insert(wire.end(), unit.begin(), unit.end());
	std::vector<bytes> out((wire.size() + chunk - 1) / chunk);
	for (size_t i = 0; i < out.size(); ++i)
		if (i != missing)
			out[i] = bytes(wire.begin() + long(i * chunk), wire.begin() + long(std::min(wire.size(), (i + 1) * chunk)));
	return out;
}

int main()
{
	const layout l{64, 32, 1};
	const bytes previous = frame(0, (3u << 30) | 0x00123456u);
	assert(parse_frame(l, previous));

	// Current tile 0 uses a new mode and is missing one complete transport chunk.
	const bytes current = frame(1u << 30, (3u << 30) | 0x00abcdefu, 20);
	auto slots = chunks(current, 16, 2);
	auto recovered = recover_partial(l, slots, 16, previous);
	assert(recovered);
	assert(recovered->fresh_tiles == 1 && recovered->retained_tiles == 1);
	assert(parse_frame(l, recovered->unit));
	assert((read32(recovered->unit, 16) >> 30) == 0);
	assert(read32(recovered->unit, 20) == (3u << 30 | 0x00abcdefu));

	// Complete current frame: changed modes and offsets pack safely.
	slots = chunks(current, 16);
	recovered = recover_partial(l, slots, 16, previous);
	assert(recovered && recovered->fresh_tiles == 2 && recovered->retained_tiles == 0);
	assert(parse_frame(l, recovered->unit));

	// Header/table loss, malformed current offset, absent history, and malformed final chunk reject.
	auto no_header = chunks(current, 16, 0);
	assert(!recover_partial(l, no_header, 16, previous));
	bytes malformed = current;
	malformed[20] = 0xff; malformed[21] = 0xff; malformed[22] = 0xff; malformed[23] = 0x3f;
	assert(!recover_partial(l, chunks(malformed, 16), 16, previous));
	assert(!recover_partial(l, slots, 16, {}));
	slots.back().push_back(0);
	assert(!recover_partial(l, slots, 16, previous));

	// A separate missing descriptor chunk cannot be inferred from history.
	assert(!recover_partial(l, chunks(current, 16, 1), 16, previous));
	assert(!recover_partial(l, chunks(current, 16, 2), 16, previous, 0));

	// Both tiles have blocks; offsets and modes differ between frames. Verify
	// actual bytes after repacking, not only that the output passes validation.
	const bytes old_swapped = frame(20, 1u << 30, 100);
	const bytes new_swapped = frame((2u << 30) | 20, 1u << 30, 25);
	auto changed = chunks(new_swapped, 16, 7);
	auto mix = recover_partial(l, changed, 16, old_swapped, 1);
	assert(mix && mix->fresh_tiles == 1 && mix->retained_tiles == 1);
	auto mv = parse_frame(l, mix->unit);
	auto ov = parse_frame(l, old_swapped);
	auto nv = parse_frame(l, new_swapped);
	assert(mv && ov && nv);
	assert(read32(mv->descriptors, 0) == 0);
	assert(read32(mv->descriptors, 4) == ((1u << 30) | 80));
	for (size_t i = 0; i < 80 * 4; ++i) assert(mv->blocks[i] == ov->blocks[20 * 4 + i]);
	for (size_t i = 0; i < 20 * 4; ++i) assert(mv->blocks[80 * 4 + i] == nv->blocks[i]);
	for (size_t i = 2; i < changed.size(); ++i) changed[i].clear();
	assert(!recover_partial(l, changed, 16, old_swapped));

	std::puts("direct block partial recovery: ok");
}
