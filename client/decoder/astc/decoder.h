/*
 * WiVRn VR streaming
 * Copyright (C) 2025 Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "decoder/decoder.h"
#include "vk/allocation.h"
#include "wivrn_packets.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{
class astc_decoder final : public decoder
{
	static constexpr size_t image_count = 6;
	static constexpr size_t pending_limit = 2;

	struct image
	{
		image_allocation pixels;
		vk::raii::ImageView view = nullptr;
		buffer_allocation staging;
		uint8_t * mapped = nullptr;
		vk::ImageLayout layout = vk::ImageLayout::eUndefined;
		std::atomic_bool free = true;
		uint64_t semaphore_value = 0;
	};
	struct frame
	{
		std::vector<uint8_t> packet;
		from_headset::feedback feedback;
		to_headset::video_stream_data_shard::view_info_t view_info;
	};

	vk::raii::Device & device;
	vk::raii::Sampler sampler_ = nullptr;
	std::array<image, image_count> images;
	vk::Extent2D extent;
	vk::DeviceSize raw_bytes;
	std::vector<uint8_t> cpu_scratch;
	std::weak_ptr<scenes::stream> weak_scene;
	shard_accumulator * accumulator;

	std::mutex mutex;
	std::condition_variable wake;
	std::deque<frame> pending;
	std::vector<uint8_t> assembling;
	uint64_t assembling_frame = 0;
	bool have_frame = false;
	bool invalid_frame = false;
	bool exiting = false;
	std::thread worker;

public:
	astc_decoder(vk::raii::Device & device,
	             vk::raii::PhysicalDevice & physical_device,
	             uint32_t queue_family_index,
	             const to_headset::video_stream_description & description,
	             uint8_t stream_index,
	             std::weak_ptr<scenes::stream> scene,
	             shard_accumulator * accumulator);
	~astc_decoder() override;

	void push_data(std::span<std::span<const uint8_t>> data, uint64_t frame_index, bool partial) override;
	void frame_completed(const from_headset::feedback & feedback,
	                     const to_headset::video_stream_data_shard::view_info_t & view_info) override;
	vk::Sampler sampler() override { return *sampler_; }
	static std::vector<video_codec> supported_codecs();

private:
	image * get_free();
	void worker_function(uint32_t queue_family_index);
};
} // namespace wivrn
