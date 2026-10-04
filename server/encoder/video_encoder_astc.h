#pragma once

#include "video_encoder.h"
#include "astc_rate_control.h"
#include "vk/allocation.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>
#include <zstd.h>

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
		uint32_t quality = 6;
		uint32_t block = 8;
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
	std::vector<uint8_t> zstd_compressed;
	std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> zstd_context{ZSTD_createCCtx(), ZSTD_freeCCtx};
	std::atomic_uint32_t quality{6};
	astc_rate_control rate_control;
	std::array<double, 4> sampled_cpu_ms{};
	uint64_t sampled_bytes = 0;
	uint32_t sampled_frames = 0;
	uint32_t sampled_expansion_drops = 0;
	std::array<uint32_t, astc_rate_control::rungs> sampled_quality{};
	std::array<uint32_t, 3> sampled_encoding{};
	const float initial_fps;
	const bool direct_rgb_input;

public:
	video_encoder_astc(vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);
	~video_encoder_astc() override;
	void present_image(vk::Image image, vk::SemaphoreSubmitInfo semaphore, uint8_t slot, uint64_t frame_index, const to_headset::video_stream_data_shard::view_info_t & view_info) override;
	std::optional<data> encode(uint8_t slot, uint64_t frame_index) override;
};
} // namespace wivrn
