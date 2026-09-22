#include "nxwarp_compression_credit.h"

#include <cassert>
#include <cmath>

using wivrn::nxwarp_direct::compression_credit;

static void good(compression_credit & credit, size_t raw = 200, size_t wire = 100)
{
	(void)credit.observe(raw, wire, 100, 200);
}

int main()
{
	compression_credit credit;
	for (int i = 0; i < 7; ++i)
		good(credit);
	assert(credit.value() == 1.0);
	good(credit);
	assert(credit.value() == 1.1);
	for (int window = 0; window < 4; ++window)
	{
		for (int i = 0; i < 7; ++i)
			good(credit);
		good(credit);
		assert(std::abs(credit.value() - (1.2 + 0.1 * window)) < 1e-9);
	}

	assert(credit.observe(200, 100, 111, 100) && credit.value() == 1.0);
	for (int i = 0; i < 8; ++i)
		assert(!credit.observe(200, 200, 100, 200));
	assert(credit.value() == 1.0);

	for (int i = 0; i < 7; ++i)
		good(credit);
	credit.reset();
	good(credit);
	assert(credit.value() == 1.0);
	for (int i = 0; i < 7; ++i)
		good(credit);
	good(credit);
	assert(credit.value() == 1.1);

	credit.budget_changed();
	assert(credit.value() == 1.1);
	for (int i = 0; i < 8; ++i)
		assert(!credit.observe(200, 100, 95, 100));
	assert(credit.value() == 1.1); // Near the new limit: hold, never grow.
	assert(credit.observe(200, 100, 120, 100));
	assert(credit.value() == 1.0); // Compression no longer fits: retreat.

	compression_credit worst;
	for (int i = 0; i < 7; ++i)
		assert(!worst.observe(200, 100, 100, 200));
	assert(!worst.observe(200, 170, 100, 200));
	assert(worst.value() == 1.0);
	compression_credit pending;
	for (int i = 0; i < 3; ++i)
		good(pending);
	assert(!pending.observe(200, 200, 100, 200));
	assert(pending.value() == 1.0);
	return 0;
}
