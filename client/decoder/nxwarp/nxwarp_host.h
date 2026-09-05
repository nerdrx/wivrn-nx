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

#include "decoder/decoder.h"
#include "wivrn_packets.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

class shard_accumulator;

// Everything nxwarp_decoder needs from the process it runs in, and nothing else.
//
// The decoder used to reach for `application` and `scenes::stream` directly, which
// made it un-runnable anywhere but inside a client that had already brought up an
// OpenXR session — so the one thing nobody could do was decode a frame from the real
// encoder and check the picture. Four things is all it actually wanted: the Vulkan
// instance behind the device it was handed, the process-wide queue lock, a clock, and
// somewhere to put feedback and finished frames.
//
// Two implementations exist: nxwarp_application_host, which is the client and forwards
// to exactly those two singletons, and the one inside wivrn-nxwarp-e2e, which is a
// headless Vulkan device and a pair of queues. The decoder cannot tell them apart, which
// is the point: what the test exercises is the shipping class.
class nxwarp_host
{
public:
	virtual ~nxwarp_host() = default;

	// The instance the decoder's VkDevice came from. nxvc adopts instance, physical
	// device, device, queue and queue family together or allocates its own — never a
	// mixture — so this has to be the real one.
	virtual vk::Instance instance() = 0;

	// The queue nxvc submits on, and the lock every other submitter in the process
	// holds while it does. `fn` runs with the lock held; it must not block on anything
	// that needs the same lock. Used both to adopt the queue at decoder-create time and
	// to wrap each decode submit.
	virtual void with_queue(const std::function<void(vk::Queue)> & fn) = 0;

	// The clock the feedback timestamps are in. In the client this is the OpenXR
	// instance's; the numbers only ever get compared with each other.
	virtual XrTime now() = 0;

	// One transport feedback packet, already formed by nxt::Receiver::band_deadline,
	// bound for from_headset::nxwarp_feedback on the control socket. `decode_us` is
	// this stream's current decode cost per frame in microseconds, or 0 before it has
	// decoded anything; it rides the same packet because it goes out at the same
	// cadence and is what the server's send pacing reads (see the field's comment on
	// from_headset::nxwarp_feedback).
	virtual void send_feedback(uint8_t stream_index, uint8_t path_id, std::vector<uint8_t> payload,
	                           uint16_t decode_us) = 0;

	// One reassembled .nxv frame unit, exactly as it is about to be handed to the codec.
	// The client ignores it; wivrn-nxwarp-e2e uses it to rebuild the byte stream the
	// decoder was actually fed, so that nx-warp's own nxv-dec can be run over the same
	// bytes and the two decoders' output compared. Called on the network thread, before
	// the decode job is queued. `frame_id` is the stream's own 16-bit frame id, which
	// is what lets a caller line the units up with anything else it knows about the
	// stream without guessing at the order they close in.
	virtual void on_frame_unit(uint16_t frame_id, std::span<const uint8_t>) {}

	// A frame that will never be decoded: it closed with a hole. The feedback carries
	// the frame's index, its stream and the arrival of its first datagram, and no
	// sent_to_decoder -- which is precisely how every other decoder in this client
	// reports a frame that never arrived, and what the server's automatic bitrate
	// counts as a lost frame. Called on the network thread.
	//
	// Deliberately separate from publish(): there is no picture and no blit handle to
	// hang one on, and the two callers want different things from it (the client puts
	// it on the control socket, wivrn-nxwarp-e2e counts it).
	virtual void report_frame_lost(const wivrn::from_headset::feedback &) {}

	// A frame this decoder will NOT reconstruct, named by the stream's own 16-bit
	// frame id. See from_headset::nxwarp_frame_not_held for why it exists and why it
	// is not the same thing as report_frame_lost() above.
	//
	// The short version: report_frame_lost() is about the LINK and feeds the automatic
	// bitrate, so it fires only for a frame whose bytes did not arrive. This one is
	// about this DEVICE and feeds the encoder's receipt map, so it fires for every
	// frame that is not reconstructed whatever the reason -- including the ones the
	// link delivered perfectly and this decoder then skipped because it could not keep
	// up. Conflating them would either have the bitrate controller punish the link for
	// a slow decoder, or leave the encoder predicting from a picture that was never
	// built. Called on the network thread and, for a codec refusal, on the worker.
	virtual void report_frame_not_held(uint8_t stream_index, uint16_t frame_id,
	                                   wivrn::from_headset::nxwarp_frame_not_held::reason why)
	{}

	// One frame this decoder actually put through the codec, named by wire frame id,
	// in the order the codec consumed them. That order and that SET are what the
	// codec's reference ring was built from -- which is not the same thing as the
	// units the reassembler produced, because a frame can be reassembled and then
	// dropped before the codec sees it. A harness comparing this decoder against a
	// reference one has to feed the reference the same list or it is comparing two
	// different reference chains. The client ignores it.
	virtual void on_frame_decoded(uint16_t frame_id) {}

	// A decoded frame, with the view_info it was rendered for.
	virtual void publish(shard_accumulator * accumulator, std::shared_ptr<decoder::blit_handle> handle) = 0;
};

} // namespace wivrn
