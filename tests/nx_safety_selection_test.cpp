#include "../client/scenes/nx_safety_selection.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using wivrn::nx_safety::choice;
using wivrn::nx_safety::select;
constexpr uint64_t period = 11111111; // 90 Hz display period, rounded to ns.

int main()
{
    // Startup: complete safety can seed presentation when primary is absent.
    assert(select({}, 100, 0, 0, period) == choice::safety);
    assert(select({}, {}, 0, 0, period) == choice::hold);

    // Fresh primary wins normally, including when safety is newer.
    assert(select(101, 200, 100, 0, period) == choice::primary);
    assert(select(100, 200, 100, 2 * period, period) == choice::safety);

    // Primary stall: safety waits until two display periods, inclusive.
    assert(select(100, 200, 100, 2 * period - 1, period) == choice::hold);
    assert(select(100, 200, 100, 2 * period, period) == choice::safety);
    assert(select(100, 200, 100, 3 * period, period) == choice::safety);

    // Same boundary scales with refresh period.
    constexpr uint64_t hz60 = 16666667, hz120 = 8333333;
    assert(select(10, 20, 10, 2 * hz60 - 1, hz60) == choice::hold);
    assert(select(10, 20, 10, 2 * hz60, hz60) == choice::safety);
    assert(select(10, 20, 10, 2 * hz120 - 1, hz120) == choice::hold);
    assert(select(10, 20, 10, 2 * hz120, hz120) == choice::safety);

    // Older, equal, and missing safety never rewind or invent a frame.
    assert(select(100, 99, 100, 4 * period, period) == choice::hold);
    assert(select(100, 100, 100, 4 * period, period) == choice::hold);
    assert(select(100, {}, 100, 4 * period, period) == choice::hold);

    // Returning primary must be newer than last presented; stale primary cannot rewind safety.
    assert(select(150, 200, 200, 0, period) == choice::hold);
    assert(select(201, 200, 200, 0, period) == choice::primary);

    // Sequence never rewinds presented source time.
    assert(select({}, 300, 250, 2 * period, period) == choice::safety);
    assert(select(240, 300, 300, 0, period) == choice::hold);
    assert(select(301, 300, 300, 0, period) == choice::primary);
    assert(select(300, 300, 300, 0, period, true) == choice::primary);

    // Invalid period disables switching.
    assert(select(101, 200, 100, UINT64_MAX, 0) == choice::hold);
    std::puts("NX safety selection: ok");
}
