#pragma once

#include <cstdint>
#include <optional>

namespace wivrn::nx_safety
{
enum class choice { hold, primary, safety };

// Select the source timestamp to present. Timestamps are monotonic source time.
inline choice select(std::optional<uint64_t> primary_timestamp,
                     std::optional<uint64_t> safety_timestamp,
                     uint64_t last_presented_timestamp,
                     uint64_t primary_hold_ns,
                     uint64_t display_period_ns,
                     bool allow_equal_primary = false)
{
    if (!display_period_ns)
        return choice::hold;
    if (primary_timestamp && (*primary_timestamp > last_presented_timestamp ||
                              (allow_equal_primary && *primary_timestamp == last_presented_timestamp)))
        return choice::primary;
    const bool newer_safety = safety_timestamp && *safety_timestamp > last_presented_timestamp;
    const bool held_long_enough = primary_hold_ns >= display_period_ns &&
                                  primary_hold_ns - display_period_ns >= display_period_ns;
    const bool startup = !primary_timestamp && last_presented_timestamp == 0;
    if (newer_safety && (startup || held_long_enough))
        return choice::safety;
    return choice::hold;
}
} // namespace wivrn::nx_safety
