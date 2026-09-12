#pragma once

#include "wivrn_packets.h"
#include <array>
#include <cstdint>
#include <string>
#include <sys/types.h>

namespace wivrn
{
class nx_fuse_corrections
{
public:
    struct camera_pose { uint16_t role; XrVector3f position; XrQuaternionf orientation; };
private:
    struct item { uint16_t role; float x, y, z; };
    int fd = -1;
    std::string path;
    uint64_t generation = 0, sequence = 0, sample_time = 0;
    ino_t inode = 0;
    dev_t device = 0;
    std::array<item, 8> items{};
    size_t count = 0;
    bool enabled = false;
    std::array<camera_pose, 5> camera_items{};
    size_t camera_count = 0;
    bool camera_enabled = false;
    void close_socket();
    bool bind_socket();
public:
    nx_fuse_corrections();
    ~nx_fuse_corrections();
    nx_fuse_corrections(const nx_fuse_corrections &) = delete;
    nx_fuse_corrections & operator=(const nx_fuse_corrections &) = delete;
    void poll(uint64_t current_generation);
    void invalidate(uint64_t current_generation);
    void apply(from_headset::bd_body &, uint64_t current_generation) const;
    size_t camera_snapshot(uint64_t current_generation, std::array<camera_pose, 5> & out, uint64_t *sample_out = nullptr) const;
};
}
