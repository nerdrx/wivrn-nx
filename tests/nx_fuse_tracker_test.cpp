#include "driver/nx_fuse_tracker.h"
#include "os/os_time.h"
#include <cassert>
#include <cmath>
#include <unistd.h>

int main()
{
    xrt_device hmd{};
    wivrn::nx_fuse_tracker tracker(10, "hip", &hmd);
    xrt_space_relation out{};
    assert(tracker.get_tracked_pose(XRT_INPUT_GENERIC_TRACKER_POSE, os_monotonic_get_ns(), &out) == XRT_SUCCESS);
    assert(out.relation_flags == 0);
    assert(tracker.get_tracked_pose(XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns(), &out) == XRT_ERROR_INPUT_UNSUPPORTED);
    const uint64_t sample = os_monotonic_get_ns();
    tracker.publish(7, sample, {1.f, 2.f, 3.f}, {0.f, 0.f, 0.f, 1.02f});
    assert(tracker.get_tracked_pose(XRT_INPUT_GENERIC_TRACKER_POSE, sample, &out) == XRT_SUCCESS);
    assert((out.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
    assert(std::abs(out.pose.position.x - 1.f) < .001f);
    const auto q = out.pose.orientation;
    if (std::abs(std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w) - 1.f) >= .001f) return 4;
    usleep(160000);
    assert(tracker.get_tracked_pose(XRT_INPUT_GENERIC_TRACKER_POSE, os_monotonic_get_ns(), &out) == XRT_SUCCESS);
    assert(out.relation_flags == 0);
    tracker.publish(8, os_monotonic_get_ns(), {4.f, 5.f, 6.f}, {0.f, 0.f, 0.f, 1.f});
    tracker.invalidate(9);
    assert(tracker.get_tracked_pose(XRT_INPUT_GENERIC_TRACKER_POSE, os_monotonic_get_ns(), &out) == XRT_SUCCESS);
    assert(out.relation_flags == 0);
    return 0;
}
