#pragma once

#include <cstdint>

namespace wivrn {

// Merge a headset's positive reconstruction window into the newest known window.
// Bit k names base-k; frame ids are uint16_t and the mask is 32 bits wide.
inline void nxwarp_merge_held_ack(uint16_t & ack_base, uint32_t & ack_mask,
                                  bool & ack_valid, uint16_t held_base,
                                  uint32_t held_mask)
{
	if (!held_mask)
		return;
	if (!ack_valid)
	{
		ack_base = held_base;
		ack_mask = held_mask;
		ack_valid = true;
		return;
	}
	const uint16_t forward = uint16_t(held_base - ack_base);
	if (int16_t(forward) > 0)
	{
		ack_mask = (forward >= 32 ? 0u : (ack_mask << forward)) | held_mask;
		ack_base = held_base;
		return;
	}
	const uint16_t backward = uint16_t(ack_base - held_base);
	if (backward < 32)
		ack_mask |= held_mask << backward;
}

} // namespace wivrn
