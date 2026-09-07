#include "nxwarp_held_ack.h"

#include <cstdint>
#include <cstdio>

static int failures;

static void check(bool ok, const char * name)
{
	if (!ok)
	{
		std::printf("FAIL %s\n", name);
		++failures;
	}
}

static void merge(uint16_t & base, uint32_t & mask, bool & valid,
                  uint16_t held_base, uint32_t held_mask)
{
	wivrn::nxwarp_merge_held_ack(base, mask, valid, held_base, held_mask);
}

int main()
{
	uint16_t base = 0;
	uint32_t mask = 0;
	bool valid = false;

	merge(base, mask, valid, 10, 0);
	check(!valid && mask == 0, "zero report is ignored");
	merge(base, mask, valid, 10, 1u << 31);
	check(valid && base == 10 && mask == (1u << 31), "first report and bit 31");

	merge(base, mask, valid, 12, 1);
	check(base == 12 && mask == ((1u << 31) << 2 | 1), "newer base shifts old window left");
	merge(base, mask, valid, 10, 1);
	check(base == 12 && (mask & (1u << 2)), "older base shifts incoming bit left");

	base = 100; mask = 1; valid = true;
	merge(base, mask, valid, 98, 1);
	check(base == 100 && mask == 5, "98 bit 0 maps to 100 bit 2");

	base = 1; mask = 1; valid = true;
	merge(base, mask, valid, 0xffff, 1);
	check(base == 1 && (mask & (1u << 2)), "16-bit wrap older distance 2");
	merge(base, mask, valid, uint16_t(base + 32), 1);
	check(base == 33 && mask == 1, "forward distance 32 drops old window");

	base = 100; mask = 1; valid = true;
	merge(base, mask, valid, 68, 1);
	check(base == 100 && mask == 1, "older distance 32 is outside window");
	merge(base, mask, valid, uint16_t(base + 0x8000), 1);
	check(base == 100 && mask == 1, "half-range ambiguity is ignored");

	if (failures)
		return 1;
	std::puts("nxwarp held ACK merge: all checks passed");
	return 0;
}
