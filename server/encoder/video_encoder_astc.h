#pragma once

#include "video_encoder.h"
#include "vk/allocation.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace wivrn
{
class video_encoder_astc : public video_encoder
{
	struct slot_t
	{
		vk::raii::Fence fence = nullptr;
		vk::raii::CommandBuffer cmd = nullptr;
		buffer_allocation blocks, readback;
		vk::DescriptorSet descriptor_set{};
		bool valid = false;
	};

	vk_bundle & vk;
	vk::raii::CommandPool cmd_pool;
	vk::raii::Sampler sampler;
	vk::raii::DescriptorSetLayout ds_layout;
	vk::raii::PipelineLayout pipeline_layout;
	vk::raii::Pipeline pipeline;
	vk::raii::DescriptorPool ds_pool;
	std::unordered_map<VkImage, std::array<vk::raii::ImageView, 2>> image_views;
	std::array<slot_t, num_slots> slots;
	std::vector<uint8_t> compressed;

public:
	video_encoder_astc(vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);
	~video_encoder_astc() override;
	void present_image(vk::Image image, vk::SemaphoreSubmitInfo semaphore, uint8_t slot, uint64_t frame_index, const to_headset::video_stream_data_shard::view_info_t & view_info) override;
	std::optional<data> encode(uint8_t slot, uint64_t frame_index) override;
};
} // namespace wivrn
