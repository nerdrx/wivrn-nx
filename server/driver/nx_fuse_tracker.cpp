#include "nx_fuse_tracker.h"
#include "util/u_device_id.h"
#include "utils/method.h"
#include "xrt/xrt_results.h"
#include "os/os_time.h"
#include <cstring>
#include <cmath>

namespace wivrn {
nx_fuse_tracker::nx_fuse_tracker(uint16_t role, const char *name, xrt_device *hmd) :
    xrt_device{.id=u_device_id_generate(), .name=XRT_DEVICE_VIVE_TRACKER,
      .device_type=XRT_DEVICE_TYPE_GENERIC_TRACKER, .hmd=nullptr,
      .tracking_origin=hmd->tracking_origin, .supported={.orientation_tracking=true,.position_tracking=true},
      .update_inputs=method_pointer<&nx_fuse_tracker::update_inputs>,
      .get_tracked_pose=method_pointer<&nx_fuse_tracker::get_tracked_pose>, .destroy=[](xrt_device*){}}
{
    std::string label = std::string("NX Fuse camera ") + name;
    strlcpy(str, label.c_str(), sizeof(str));
    std::string serial = "nx-fuse-camera-" + std::to_string(role);
    strlcpy(this->serial, serial.c_str(), sizeof(this->serial));
    pose_input.name = XRT_INPUT_GENERIC_TRACKER_POSE; pose_input.active = true;
    inputs = &pose_input; input_count = 1;
}
xrt_result_t nx_fuse_tracker::update_inputs() { return XRT_SUCCESS; }
xrt_result_t nx_fuse_tracker::get_tracked_pose(xrt_input_name name, int64_t, xrt_space_relation *out)
{
    if (name != XRT_INPUT_GENERIC_TRACKER_POSE) return XRT_ERROR_INPUT_UNSUPPORTED;
    std::lock_guard lock(mutex);
    if (!valid || os_monotonic_get_ns() - sample_ns >= 150'000'000) { *out=XRT_SPACE_RELATION_ZERO; return XRT_SUCCESS; }
    *out=relation; return XRT_SUCCESS;
}
void nx_fuse_tracker::publish(uint64_t gen, uint64_t sample, xrt_vec3 p, xrt_quat q)
{
    std::lock_guard lock(mutex); generation=gen; sample_ns=sample; relation=XRT_SPACE_RELATION_ZERO;
    relation.relation_flags = xrt_space_relation_flags(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
    relation.pose.position = p; float n = std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    relation.pose.orientation = {q.x/n,q.y/n,q.z/n,q.w/n}; valid=true;
}
void nx_fuse_tracker::invalidate(uint64_t gen) { std::lock_guard lock(mutex); generation=gen; valid=false; relation=XRT_SPACE_RELATION_ZERO; }
}
