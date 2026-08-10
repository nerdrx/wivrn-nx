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

public:
	// Video shards rebuilt from parity on this stream since it was created. Monotonic,
	// readable from any thread.
	uint64_t reconstructed_shards() const
	{
		return fec_reconstructed_total;
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

	// Parity shards one frame may hold on to. Only groups with a hole in them are
	// kept, so this is only ever reached by a frame that is losing packets wholesale
	// — at which point the frame is gone anyway and there is no point pinning more
	// receive buffers for it.
	static constexpr size_t max_parity_per_frame = 64;
};
} // namespace wivrn
