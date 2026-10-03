#pragma once

#include "video_encoder.h"
#include "vk/allocation.h"

#include <array>
#include <unordered_map>
#include <vector>

#include "pyrowave_encoder.h"

namespace wivrn
{
class video_encoder_pyrowave : public video_encoder
{
	struct slot_t
	{
		vk::raii::Fence fence = nullptr;
		vk::raii::CommandBuffer cmd = nullptr;
		buffer_allocation meta, bitstream;
		buffer_allocation meta_staging, bitstream_staging;
		bool valid = false;
	};

	vk_bundle & vk;
	vk::raii::CommandPool cmd_pool;
	PyroWave::Encoder encoder;
	std::unordered_map<VkImage, std::array<vk::raii::ImageView, 3>> image_views;
	std::array<slot_t, num_slots> slots;
	size_t target_size;
	float current_fps;
	static constexpr size_t max_target_size = 4 * 1024 * 1024;
	std::vector<uint8_t> packet_buffer;
	std::vector<PyroWave::Encoder::Packet> packets;

public:
	video_encoder_pyrowave(vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);
	~video_encoder_pyrowave() override;
	void present_image(vk::Image image, vk::SemaphoreSubmitInfo semaphore, uint8_t slot, uint64_t frame_index,
	                   const to_headset::video_stream_data_shard::view_info_t & view_info) override;
	std::optional<data> encode(uint8_t slot, uint64_t frame_index) override;
};
} // namespace wivrn
