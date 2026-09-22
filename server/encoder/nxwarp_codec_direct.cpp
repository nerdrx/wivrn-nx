/* SPDX-License-Identifier: GPL-3.0-or-later
 * Direct RGB blocks: GPU-only source processing, independent frames.
 */
#include "nxwarp_codec.h"
#include "nxwarp_direct_layout.h"
#include "wivrn-server_shaders.h"
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

namespace wivrn
{
namespace
{
void check(VkResult r, const char * what)
{
	if (r != VK_SUCCESS)
		throw std::runtime_error(std::string("NX direct: ") + what + " (" + std::to_string(r) + ")");
}
class direct_codec final : public nxwarp_codec
{
	nxwarp_direct::layout geometry;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	struct buffer
	{
		VkBuffer handle{};
		VkDeviceMemory memory{};
		void * mapped{};
		VkDeviceSize size{};
	} jobs, output;
	VkDescriptorSetLayout set_layout{};
	VkDescriptorPool descriptor_pool{};
	VkDescriptorSet descriptor{};
	VkPipelineLayout pipeline_layout{};
	VkPipeline pipeline{};
	VkCommandPool command_pool{};
	VkCommandBuffer command{};
	VkFence fence{};
	VkShaderModule module{};
	std::map<std::pair<VkImage, uint32_t>, std::array<VkImageView, 2>> views;
	std::vector<uint8_t> header;
	nxwarp_direct::plan plan;
	std::vector<nxwarp_tile_desc> tile_info;
	uint32_t target = 500'000'000;
	float refresh = 90;
	int64_t next_due = 0;
	bool submitted = false;

	void allocate(buffer & b, VkDeviceSize bytes)
	{
		b.size = bytes;
		VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		                      .size = bytes,
		                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
		check(vkCreateBuffer(device, &ci, nullptr, &b.handle), "create buffer");
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, b.handle, &req);
		VkPhysicalDeviceMemoryProperties props;
		vkGetPhysicalDeviceMemoryProperties(physical, &props);
		uint32_t type = UINT32_MAX;
		constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t i = 0; i < props.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
			{
				type = i;
				break;
			}
		if (type == UINT32_MAX)
			throw std::runtime_error("NX direct: no coherent host-visible storage memory");
		VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
		check(vkAllocateMemory(device, &ai, nullptr, &b.memory), "allocate buffer");
		check(vkBindBufferMemory(device, b.handle, b.memory, 0), "bind buffer");
		check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "map buffer");
	}
	void release(buffer & b)
	{
		if (b.mapped)
			vkUnmapMemory(device, b.memory);
		if (b.handle)
			vkDestroyBuffer(device, b.handle, nullptr);
		if (b.memory)
			vkFreeMemory(device, b.memory, nullptr);
	}
	void update_plan()
	{
		plan = nxwarp_direct::select_plan(geometry, target, refresh);
		tile_info.resize(geometry.tile_count());
		for (uint32_t i = 0; i < tile_info.size(); i++)
			tile_info[i] = {.index = i, .qp = 0, .mode = 0, .res_level = uint8_t(plan.descriptors[i] >> 30), .ref_delta = 3};
		std::memcpy(jobs.mapped, plan.jobs.data(), plan.jobs.size() * sizeof(plan.jobs[0]));
	}
	std::array<VkImageView, 2> image_views(VkImage image, uint32_t layers)
	{
		auto key = std::make_pair(image, layers);
		if (auto it = views.find(key); it != views.end())
			return it->second;
		std::array<VkImageView, 2> result{};
		try
		{
			for (uint32_t i = 0; i < 2; i++)
			{
				VkImageViewUsageCreateInfo usage{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_STORAGE_BIT};
				VkImageViewCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = i ? VK_FORMAT_R8G8_UINT : VK_FORMAT_R8_UINT, .subresourceRange = {VkImageAspectFlags(i ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT), 0, 1, 0, layers}};
				check(vkCreateImageView(device, &ci, nullptr, &result[i]), "source plane view");
			}
		}
		catch (...)
		{
			for (auto v: result)
				if (v)
					vkDestroyImageView(device, v, nullptr);
			throw;
		}
		views.emplace(key, result);
		return result;
	}

public:
	direct_codec(const nxwarp_codec_config & c, VkPhysicalDevice p, VkDevice d, VkQueue q, uint32_t f) :
	        geometry{c.width, c.height, c.eyes}, physical(p), device(d), queue(q), family(f), header(nxwarp_direct::stream_header(geometry))
	{
		if (header.empty())
			throw std::runtime_error("NX direct: eye geometry must be multiples of 32, <=4096");
	}
	void initialize()
	{
		allocate(jobs, VkDeviceSize(geometry.tile_count()) * 16 * 16);
		allocate(output, geometry.max_frame_bytes());
		std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
		for (uint32_t i = 0; i < 4; i++)
			bindings[i] = {i, i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo sl{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bindings.data()};
		check(vkCreateDescriptorSetLayout(device, &sl, nullptr, &set_layout), "descriptor layout");
		VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
		VkPipelineLayoutCreateInfo pl{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
		check(vkCreatePipelineLayout(device, &pl, nullptr, &pipeline_layout), "pipeline layout");
		const auto & code = ::shaders.at("direct_blocks_encode");
		VkShaderModuleCreateInfo sm{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code.size() * 4, .pCode = code.data()};
		check(vkCreateShaderModule(device, &sm, nullptr, &module), "shader module");
		VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		                               .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
		                               .layout = pipeline_layout};
		check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline), "compute pipeline");
		std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}};
		VkDescriptorPoolCreateInfo dp{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes.data()};
		check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptor_pool), "descriptor pool");
		VkDescriptorSetAllocateInfo ds{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
		check(vkAllocateDescriptorSets(device, &ds, &descriptor), "descriptor set");
		VkCommandPoolCreateInfo cp{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
		check(vkCreateCommandPool(device, &cp, nullptr, &command_pool), "command pool");
		VkCommandBufferAllocateInfo ca{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
		check(vkAllocateCommandBuffers(device, &ca, &command), "command buffer");
		VkFenceCreateInfo fc{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		check(vkCreateFence(device, &fc, nullptr, &fence), "fence");
		update_plan();
	}
	~direct_codec() override
	{
		if (submitted)
			vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
		for (auto & [key, v]: views)
			for (auto view: v)
				vkDestroyImageView(device, view, nullptr);
		if (fence)
			vkDestroyFence(device, fence, nullptr);
		if (command_pool)
			vkDestroyCommandPool(device, command_pool, nullptr);
		if (pipeline)
			vkDestroyPipeline(device, pipeline, nullptr);
		if (module)
			vkDestroyShaderModule(device, module, nullptr);
		if (descriptor_pool)
			vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
		if (pipeline_layout)
			vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
		if (set_layout)
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		release(output);
		release(jobs);
	}
	bool direct_blocks() const override
	{
		return true;
	}
	bool accepts_image() const override
	{
		return true;
	}
	void set_target_bitrate(uint32_t bps, float fps) override
	{
		bps = std::clamp(bps, 1u, 800'000'000u);
		fps = std::isfinite(fps) ? std::clamp(fps, 1.f, 240.f) : 90.f;
		if (bps == target && fps == refresh)
			return;
		target = bps;
		refresh = fps;
		update_plan();
		next_due = 0;
	}
	bool admit_frame(int64_t now) override
	{
		int64_t interval = int64_t(std::ceil(plan.bytes() * 8.0 * 1.25 * 1e9 / target));
		if (now < next_due)
			return false;
		next_due = std::max(next_due, now - interval) + interval;
		return true;
	}
	std::span<const uint8_t> stream_header() const override
	{
		return header;
	}
	void tile_grid(uint32_t & cols, uint32_t & rows) const override
	{
		cols = geometry.width / 32 * geometry.eyes;
		rows = geometry.height / 32;
	}
	void set_view(const nxwarp_codec_view &) override {}
	bool set_qp(uint32_t) override
	{
		return false;
	}
	void set_received_tiles(std::span<const uint8_t>) override {}
	std::span<const uint8_t> encode(const uint8_t *, size_t, const uint8_t *, const uint8_t *, size_t) override
	{
		return {};
	}
	std::span<const nxwarp_tile_desc> tiles() const override
	{
		return tile_info;
	}
	std::string description() const override
	{
		return "NX direct RGB blocks v1 (GPU source, independent frames)";
	}
	std::span<const uint8_t> encode_image(VkImage image, uint32_t layer) override
	{
		return encode_image_pair(image, layer, layer, 0);
	}
	std::span<const uint8_t> encode_image_pair(VkImage image, uint32_t left, uint32_t right, uint64_t) override
	{
		if (submitted)
			throw std::runtime_error("NX direct: unfinished GPU work");
		auto source = image_views(image, std::max(left, right) + 1);
		auto * words = static_cast<uint32_t *>(output.mapped);
		words[0] = nxwarp_direct::frame_magic;
		words[1] = nxwarp_direct::version;
		words[2] = geometry.tile_count();
		words[3] = plan.words;
		std::memcpy(words + 4, plan.descriptors.data(), plan.descriptors.size() * 4);
		std::array<VkDescriptorImageInfo, 2> images{{{VK_NULL_HANDLE, source[0], VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, source[1], VK_IMAGE_LAYOUT_GENERAL}}};
		std::array<VkDescriptorBufferInfo, 2> buffers{{{jobs.handle, 0, jobs.size}, {output.handle, 0, output.size}}};
		std::array<VkWriteDescriptorSet, 4> writes{};
		for (uint32_t i = 0; i < 4; i++)
			writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor, .dstBinding = i, .descriptorCount = 1, .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pImageInfo = i < 2 ? &images[i] : nullptr, .pBufferInfo = i >= 2 ? &buffers[i - 2] : nullptr};
		vkUpdateDescriptorSets(device, 4, writes.data(), 0, nullptr);
		check(vkResetCommandBuffer(command, 0), "reset commands");
		VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		check(vkBeginCommandBuffer(command, &begin), "begin commands");
		VkMemoryBarrier input{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &input, 0, nullptr, 0, nullptr);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor, 0, nullptr);
		uint32_t push[5] = {geometry.width, geometry.height, uint32_t(plan.jobs.size()), left, right};
		vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(command, (push[2] + 63) / 64, 1, 1);
		VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
		check(vkEndCommandBuffer(command), "end commands");
		check(vkResetFences(device, 1, &fence), "reset fence");
		VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
		check(vkQueueSubmit(queue, 1, &submit, fence), "submit");
		submitted = true;
		check(vkWaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000), "encode timeout");
		submitted = false;
		return {static_cast<const uint8_t *>(output.mapped), plan.bytes()};
	}
};
} // namespace
std::unique_ptr<nxwarp_codec> nxwarp_codec::make_direct(const nxwarp_codec_config & c, VkInstance, VkPhysicalDevice p, VkDevice d, VkQueue q, uint32_t f)
{
	auto result = std::make_unique<direct_codec>(c, p, d, q, f);
	result->initialize();
	return result;
}
} // namespace wivrn
