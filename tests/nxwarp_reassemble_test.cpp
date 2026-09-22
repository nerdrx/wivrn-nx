#include "decoder/nxwarp/nxwarp_reassemble.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace
{
using bytes = std::vector<uint8_t>;

// Fixed-chunk frames put the four-byte length prefix in slot zero.  The direct
// path must keep that invariant; otherwise a lost slot zero can turn body bytes
// into a plausible-looking length.
bytes frame()
{
	return {12, 0, 0, 0, 0x10, 0x11, 0x12, 0x13, 0x55, 0x55, 0x55, 0x55,
	        0x20, 0x21, 0x22, 0x23};
}

void test_fixed_chunks()
{
	nxt::StreamConfig cfg;
	cfg.cols = 4;
	cfg.rows = 1;
	cfg.band_rows = 1;
	cfg.mtu = 1400;
	cfg.caps = 0;
	constexpr size_t chunk = 8;

	const bytes all = frame();
	const bytes expected(all.begin() + 4, all.end());
	std::vector<bytes> slots{{12, 0, 0, 0, 0x10, 0x11, 0x12, 0x13},
	                         {0x55, 0x55, 0x55, 0x55, 0x20, 0x21, 0x22, 0x23}};

	// Reordered arrival is represented by final tile-index placement.
	assert(wivrn::nxwarp_wire::is_complete(cfg, slots, chunk, true));
	assert(wivrn::nxwarp_wire::reassemble(cfg, slots, chunk, true) == expected);

	// Losing slot zero must never parse body bytes as the length prefix.
	std::vector<bytes> no_head{{}, slots[1]};
	assert(!wivrn::nxwarp_wire::is_complete(cfg, no_head, chunk, true));
	assert(wivrn::nxwarp_wire::reassemble(cfg, no_head, chunk, true).empty());

	// A middle or terminal chunk loss is also incomplete.
	std::vector<bytes> no_middle{slots[0], {}, slots[1]};
	assert(!wivrn::nxwarp_wire::is_complete(cfg, no_middle, chunk, true));
	assert(wivrn::nxwarp_wire::reassemble(cfg, no_middle, chunk, true).empty());
	std::vector<bytes> no_tail{slots[0]};
	assert(!wivrn::nxwarp_wire::is_complete(cfg, no_tail, chunk, true));
	assert(wivrn::nxwarp_wire::reassemble(cfg, no_tail, chunk, true).empty());

	// Non-terminal short chunks cannot be accepted as complete fixed chunks.
	std::vector<bytes> short_middle{slots[0], {0x55, 0x55}, slots[1]};
	assert(!wivrn::nxwarp_wire::is_complete(cfg, short_middle, chunk, true));
	assert(wivrn::nxwarp_wire::reassemble(cfg, short_middle, chunk, true).empty());

	// Span mapping keeps its old lowest-nonempty-slot prefix rule.
	std::vector<bytes> spans{{}, {4, 0, 0, 0, 0xaa, 0xab, 0xac, 0xad}};
	const bytes span_expected{0xaa, 0xab, 0xac, 0xad};
	assert(wivrn::nxwarp_wire::is_complete(cfg, spans, chunk));
	assert(wivrn::nxwarp_wire::reassemble(cfg, spans, chunk) == span_expected);
	// The same plausible body prefix fooled the old direct path after slot-zero loss.
	assert(!wivrn::nxwarp_wire::is_complete(cfg, spans, chunk, true));
	assert(wivrn::nxwarp_wire::reassemble(cfg, spans, chunk, true).empty());
	// Valid final partial chunk and surplus bytes.
	std::vector<bytes> partial{{6, 0, 0, 0, 1, 2, 3, 4}, {5, 6}};
	assert(wivrn::nxwarp_wire::is_complete(cfg, partial, chunk, true));
	partial[1].push_back(7);
	assert(!wivrn::nxwarp_wire::is_complete(cfg, partial, chunk, true));
}
} // namespace

int main()
{
	test_fixed_chunks();
}
