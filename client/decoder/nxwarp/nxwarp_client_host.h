/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "wivrn_config.h"

#if WIVRN_USE_NXWARP

#include "nxwarp_host.h"

#include <memory>

namespace scenes
{
class stream;
}

namespace wivrn
{

// nxwarp_host as the shipping client provides it: the application singleton for the
// Vulkan instance, the queue lock and the clock, and one scenes::stream for feedback and
// finished frames. Held by weak_ptr for the same reason the decoder used to hold it that
// way — the scene can go away underneath a decode that is already in flight.
class nxwarp_application_host : public nxwarp_host
{
	std::weak_ptr<scenes::stream> weak_scene;

public:
	explicit nxwarp_application_host(std::weak_ptr<scenes::stream> scene) :
	        weak_scene(std::move(scene)) {}

	vk::Instance instance() override;
	void with_queue(const std::function<void(vk::Queue)> & fn) override;
	XrTime now() override;
	void send_feedback(uint8_t stream_index, uint8_t path_id, std::vector<uint8_t> payload,
	                   uint16_t decode_us, uint16_t held_base, uint32_t held_mask) override;
	void report_frame_lost(const wivrn::from_headset::feedback &) override;
	void report_frame_not_held(uint8_t stream_index, uint16_t frame_id,
	                           wivrn::from_headset::nxwarp_frame_not_held::reason why) override;
	void publish(shard_accumulator * accumulator, std::shared_ptr<decoder::blit_handle> handle) override;
};

} // namespace wivrn

#endif // WIVRN_USE_NXWARP
