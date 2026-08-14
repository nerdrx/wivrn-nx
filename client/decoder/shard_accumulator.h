/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "decoder.h"
#include "frame_window.h"
#include "shard_set.h"
#include "wivrn_packets.h"

#include <atomic>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace xr
{
class instance;
}

namespace scenes
{
class stream;
}

namespace wivrn
{

class shard_accumulator
{
	std::shared_ptr<decoder> decoder_;

public:
	using data_shard = wivrn::to_headset::video_stream_data_shard;
	using parity_shard = wivrn::to_headset::video_stream_parity_shard;
	using shard_set = wivrn::shard_set;

	// How many frames are reassembled at once, and how much older than the newest
	// complete frame the oldest one may get before it is given up on. See
	// frame_window: six frames is 67 ms at 90 Hz of buffered shards, and three frame
	// periods is far more inter-path skew than a Wi-Fi/USB pair ever shows.
	using window_t = wivrn::frame_window<shard_set, 6, 3>;

private:
	window_t window;
	std::weak_ptr<scenes::stream> weak_scene;
	xr::instance & instance;

	// Shards rebuilt from parity since the last report, and when that report was
	// made. Logging every reconstruction would itself become the problem on a link
	// that is losing packets steadily.
	uint64_t fec_reconstructed = 0;
	int64_t fec_last_report = 0;
	// The same reconstructions, never reset: the Transport page differences it to get a
	// rate, which a counter that empties itself every report period cannot give.
	std::atomic<uint64_t> fec_reconstructed_total = 0;

	// Retransmission requests (from_headset::nack). Scratch for the shard indices one
	// request names, the rate-limited log's counters, and the same count kept for good
	// so that a status page can difference it.
	std::vector<uint16_t> nack_scratch;
	uint64_t nack_requests = 0;
	uint64_t nack_shards = 0;
	int64_t nack_last_report = 0;
	std::atomic<uint64_t> nack_shards_total = 0;

public:
	// Video shards rebuilt from parity on this stream since it was created. Monotonic,
	// readable from any thread.
	uint64_t reconstructed_shards() const
	{
		return fec_reconstructed_total;
	}

	// Video shards this stream has asked to have sent again. Monotonic, readable from
	// any thread.
	uint64_t nacked_shards() const
	{
		return nack_shards_total;
	}

	explicit shard_accumulator(
	        vk::raii::Device & device,
	        vk::raii::PhysicalDevice & physical_device,
	        xr::instance & instance,
	        uint32_t vk_queue_family_index,
	        const wivrn::to_headset::video_stream_description & description,
	        std::weak_ptr<scenes::stream> scene,
	        uint8_t stream_index) :
	        decoder_(decoder::make(device, physical_device, vk_queue_family_index, description, stream_index, scene, this)),
	        window(shard_set(stream_index)),
	        weak_scene(scene),
	        instance(instance)
	{
	}

	void push_shard(wivrn::to_headset::video_stream_data_shard &&);
	void push_parity(wivrn::to_headset::video_stream_parity_shard &&);

	vk::Sampler sampler()
	{
		return decoder_->sampler();
	}

	using blit_handle = decoder::blit_handle;

private:
	// Feed the decoder whatever the oldest frame has gained, and finish it if it is
	// whole. Only ever the oldest: a decoder cannot be fed out of order, so a newer
	// frame that completed first waits its turn. What it returns is what tells the
	// window whether the frame is done with — see frame_window::drain.
	window_t::step try_submit_front(shard_set &);
	// Drain the window: submit and retire from the oldest end for as long as
	// anything can be decided there.
	void pump();
	void send_feedback(wivrn::from_headset::feedback & feedback);

	// Try every parity shard `set` is holding and drop the ones that are spent.
	// Returns the lowest shard index rebuilt, which is where try_submit_front has
	// to start looking again.
	std::optional<uint16_t> drain_parity(shard_set & set);
	void report_reconstructions();

	// Ask the server for the shards the window is short of, at most one request per
	// frame per round. Called after every arrival on this stream: the shards of the
	// next frame are what tells us the last one has stopped arriving, and at 90 Hz
	// that is never more than a frame period away.
	void try_nack(XrTime now);
	void report_nacks(XrTime now);

	// Parity shards one frame may hold on to. Only groups with a hole in them are
	// kept, so this is only ever reached by a frame that is losing packets wholesale
	// — at which point the frame is gone anyway and there is no point pinning more
	// receive buffers for it.
	static constexpr size_t max_parity_per_frame = 64;

	// How long a frame has to have been quiet before its holes are treated as loss
	// rather than as shards still on their way.
	//
	// The two paths can hand over the same frame a good deal out of order, and the
	// pacer spreads one frame over a fraction of a frame period, so this cannot be
	// tight. It cannot be loose either: a request is only worth sending if the answer
	// beats the frame's display deadline, and on a LAN that is 2-5 ms of round trip
	// against an 11 ms frame at 90 Hz. 2.5 ms leaves room for two rounds inside the
	// budget and still sits well past the inter-shard spacing of a paced frame.
	static constexpr int64_t nack_delay_ns = 2'500'000;
	// Requests one frame may cost, ever. Two rounds is what the frame budget has room
	// for; past that the frame is not going to be finished by asking again, and the
	// incomplete-frame path — feedback with no sent_to_decoder, and the keyframe the
	// server answers it with — is the way out that always worked.
	static constexpr uint8_t max_nack_rounds = 2;
	// One line at most every this many nanoseconds, whatever the loss rate
	static constexpr int64_t nack_report_period = 10'000'000'000;
};
} // namespace wivrn
