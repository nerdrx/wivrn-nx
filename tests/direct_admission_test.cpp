#include "nxwarp_direct_admission.h"
#include <cassert>
using wivrn::nxwarp_direct::frame_admission;
int main()
{
	frame_admission a;
	// Raw estimate would skip every other refresh; actual compressed bytes fit.
	for (int64_t now = 0; now < 1000; now += 10)
	{
		assert(a.admit(now, 20));
		a.settle(8);
	}
	// A large scene-cut frame is charged fully; subsequent work must wait.
	a.reset();
	assert(a.admit(0, 10));
	a.settle(25);
	assert(!a.admit(10, 10));
	assert(!a.admit(20, 10));
	assert(a.admit(30, 10));
	a.settle(10);
	assert(!a.admit(30, 10));
	assert(a.admit(40, 10));
	a.settle(10);
	// No admission means no charge. Reset also cancels any stale reservation.
	a.reset();
	a.settle(1000);
	assert(a.admit(0, 10));
	// Legacy raw mode retains its original reservation if not settled.
	assert(!a.admit(5, 10));
	assert(a.admit(10, 10));
	// A long idle interval cannot build an unbounded burst allowance.
	a.reset();
	assert(a.admit(1000, 20));
	a.settle(10);
	assert(a.admit(1000, 20));
	a.settle(10);
	assert(!a.admit(1000, 20));
}
