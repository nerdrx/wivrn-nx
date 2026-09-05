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

#include "nxwarp_client_host.h"

#include "wivrn_config.h"

#if WIVRN_USE_NXWARP

#include "nxwarp_decoder.h"

#include "application.h"
#include "scenes/stream.h"

namespace wivrn
{

// This file is the only part of the NX Warp decoder that knows the client exists. It is
// deliberately tiny and deliberately separate: keeping application.h and scenes/stream.h
// out of nxwarp_decoder.cpp is what lets wivrn-nxwarp-e2e link the decoder without
// linking an OpenXR session, an ImGui context and a renderer behind it.

vk::Instance nxwarp_application_host::instance()
{
	return *application::get_vulkan_instance();
}

void nxwarp_application_host::with_queue(const std::function<void(vk::Queue)> & fn)
{
	// The decode queue, which is a SECOND queue of the renderer's family where
	// the driver offers one and the renderer's own queue where it does not.
	// Everything nxvc and this decoder submit goes through here -- the decode,
	// the copy into the pool image and the semaphore drain -- so on a device
	// with two queues none of it is scheduled behind the compositor's frame.
	// The pool item's semaphore then crosses queues, which is what a binary
	// semaphore is for; both queues are in one family, so no image passes an
	// ownership transfer.
	auto queue = application::get_decode_queue().lock();
	fn(**queue);
}

XrTime nxwarp_application_host::now()
{
	return application::get_xr_instance().now();
}

void nxwarp_application_host::send_feedback(uint8_t stream_index, uint8_t path_id, std::vector<uint8_t> payload)
{
	if (auto scene = weak_scene.lock())
		scene->send_nxwarp_feedback(stream_index, path_id, std::move(payload));
}

void nxwarp_application_host::report_frame_lost(const wivrn::from_headset::feedback & fb)
{
	// The same socket and the same packet the conventional decoders' incomplete-frame
	// report goes out on; scenes::stream::send_feedback is what counts it.
	if (auto scene = weak_scene.lock())
		scene->send_feedback(fb);
}

void nxwarp_application_host::report_frame_not_held(
        uint8_t stream_index, uint16_t frame_id,
        wivrn::from_headset::nxwarp_frame_not_held::reason why)
{
	if (auto scene = weak_scene.lock())
		scene->send_nxwarp_frame_not_held(stream_index, frame_id, why);
}

void nxwarp_application_host::publish(shard_accumulator * accumulator, std::shared_ptr<decoder::blit_handle> handle)
{
	if (auto scene = weak_scene.lock())
		scene->push_blit_handle(accumulator, std::move(handle));
}

// The constructor the client's decoder factory calls. It lives here rather than in
// nxwarp_decoder.cpp so that nothing in the decoder's own translation unit refers to
// nxwarp_application_host — which is what lets wivrn-nxwarp-e2e link the decoder while
// leaving this file, and the whole client behind it, out of the binary.
nxwarp_decoder::nxwarp_decoder(vk::raii::Device & device,
                               vk::raii::PhysicalDevice & physical_device,
                               uint32_t vk_queue_family_index,
                               const wivrn::to_headset::video_stream_description & description,
                               uint8_t stream_index,
                               std::weak_ptr<scenes::stream> scene,
                               shard_accumulator * accumulator) :
        nxwarp_decoder(device, physical_device, vk_queue_family_index, description, stream_index,
                       *new nxwarp_application_host(std::move(scene)), accumulator)
{
	// Adopt the host the delegated constructor was handed, so it dies with the decoder.
	owned_host.reset(&host);
}

} // namespace wivrn

#endif // WIVRN_USE_NXWARP
