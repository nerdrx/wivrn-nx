#pragma once
#include "xrt/xrt_device.h"
#include <mutex>
#include <string>
#include <cstdint>

namespace wivrn {
class nx_fuse_tracker final : public xrt_device {
public:
    using base_t = xrt_device;
private:
    mutable std::mutex mutex;
    xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
    uint64_t sample_ns = 0, generation = 0;
    bool valid = false;
    xrt_input pose_input{};
public:
    nx_fuse_tracker(uint16_t role, const char *name, xrt_device *hmd);
    xrt_result_t update_inputs();
    xrt_result_t get_tracked_pose(xrt_input_name name, int64_t at_timestamp_ns, xrt_space_relation *out);
    void publish(uint64_t gen, uint64_t sample, xrt_vec3 p, xrt_quat q);
    void invalidate(uint64_t gen);
};
}
