#pragma once

#include "video_encoder.h"
#include "astc_rate_control.h"
#include "nxastc_motion.h"
#include "vk/allocation.h"

#include <array>
#include <memory>
#include <mutex>
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
		bool valid = false;
	};

	vk_bundle & vk;
	vk::raii::CommandPool cmd_pool;
	vk::raii::Sampler sampler;
	vk::raii::DescriptorSetLayout ds_layout;
	vk::raii::PipelineLayout pipeline_layout;
	vk::raii::Pipeline pipeline;
	vk::raii::DescriptorPool ds_pool;
	vk::raii::QueryPool gpu_timing_queries = nullptr;
	uint32_t gpu_timestamp_valid_bits = 0;
	double gpu_timestamp_period_ns = 0;
	std::unordered_map<VkImage, std::array<vk::raii::ImageView, 2>> image_views;
	std::array<slot_t, num_slots> slots;
	std::vector<uint8_t> compressed;
	std::vector<uint8_t> zstd_compressed;
	std::vector<uint8_t> compact_blocks;
	std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> zstd_context{ZSTD_createCCtx(), ZSTD_freeCCtx};
	std::atomic_uint32_t quality{6};
	astc_rate_control rate_control;
	std::array<double, 4> sampled_cpu_ms{};
	std::array<double, 180> sampled_gpu_ms{};
	uint32_t sampled_gpu_count = 0;
	uint64_t sampled_bytes = 0;
	uint32_t sampled_frames = 0;
	std::array<uint32_t, 7> sampled_quality{};
	std::array<uint32_t, 3> sampled_encoding{};
	uint32_t sampled_compact_frames = 0;
	double sampled_compact_ms = 0;
	struct motion_reference
	{
		uint64_t frame_index = 0;
		bool valid = false;
		std::vector<uint8_t> blocks;
	};
	std::array<motion_reference, nxastc_packet::motion_reference_capacity> motion_references;
	std::mutex motion_mutex;
	size_t motion_reference_next = 0;
	uint8_t motion_probe_cooldown = 0;
	std::vector<uint8_t> motion_scratch, motion_zstd_compressed;
	double sampled_motion_candidate_ms = 0;
	double sampled_worker_ms = 0;
	uint32_t sampled_motion_wins = 0, sampled_motion_missing_ref = 0;
	const float initial_fps;
	const bool direct_rgb_input;
	const bool motion_delta_enabled;
	const bool compact_enabled;
	const int independent_zstd_level;
	bool independent_zstd_jobs = false;

public:
	video_encoder_astc(vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);
	~video_encoder_astc() override;
	void reset() override;
	void present_image(vk::Image image, vk::SemaphoreSubmitInfo semaphore, uint8_t slot, uint64_t frame_index, const to_headset::video_stream_data_shard::view_info_t & view_info) override;
	std::optional<data> encode(uint8_t slot, uint64_t frame_index) override;
};
} // namespace wivrn
