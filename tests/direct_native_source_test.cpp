#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <vulkan/vulkan.h>
VkDevice d;
VkPhysicalDevice g;
void ok(VkResult r)
{
	assert(r == VK_SUCCESS);
}
uint32_t mt(uint32_t bits, VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties p;
	vkGetPhysicalDeviceMemoryProperties(g, &p);
	for (unsigned i = 0; i < p.memoryTypeCount; i++)
		if ((bits & (1 << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	abort();
}
struct B
{
	VkBuffer b;
	VkDeviceMemory m;
	void * p;
};
B buf(size_t n, VkBufferUsageFlags use)
{
	B b{};
	VkBufferCreateInfo c{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	c.size = n;
	c.usage = use;
	ok(vkCreateBuffer(d, &c, 0, &b.b));
	VkMemoryRequirements r;
	vkGetBufferMemoryRequirements(d, b.b, &r);
	VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	a.allocationSize = r.size;
	a.memoryTypeIndex = mt(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	ok(vkAllocateMemory(d, &a, 0, &b.m));
	ok(vkBindBufferMemory(d, b.b, b.m, 0));
	ok(vkMapMemory(d, b.m, 0, n, 0, &b.p));
	return b;
}
struct I
{
	VkImage i;
	VkDeviceMemory m;
	VkImageView v;
};
I img(VkFormat f, unsigned w, unsigned h, VkImageUsageFlags use)
{
	I i{};
	VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	c.imageType = VK_IMAGE_TYPE_2D;
	c.format = f;
	c.extent = {w, h, 1};
	c.mipLevels = 1;
	c.arrayLayers = 2;
	c.samples = VK_SAMPLE_COUNT_1_BIT;
	c.tiling = VK_IMAGE_TILING_OPTIMAL;
	c.usage = use;
	ok(vkCreateImage(d, &c, 0, &i.i));
	VkMemoryRequirements r;
	vkGetImageMemoryRequirements(d, i.i, &r);
	VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	a.allocationSize = r.size;
	a.memoryTypeIndex = mt(r.memoryTypeBits, 0);
	ok(vkAllocateMemory(d, &a, 0, &i.m));
	ok(vkBindImageMemory(d, i.i, i.m, 0));
	VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	v.image = i.i;
	v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	v.format = f;
	v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
	ok(vkCreateImageView(d, &v, 0, &i.v));
	return i;
}
int main(int argc, char ** argv)
{
	assert(argc == 2);
	VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ai.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ic.pApplicationInfo = &ai;
	VkInstance instance;
	ok(vkCreateInstance(&ic, 0, &instance));
	unsigned ng = 0;
	vkEnumeratePhysicalDevices(instance, &ng, 0);
	std::vector<VkPhysicalDevice> gs(ng);
	vkEnumeratePhysicalDevices(instance, &ng, gs.data());
	g = gs[0];
	unsigned nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g, &nq, 0);
	std::vector<VkQueueFamilyProperties> qs(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(g, &nq, qs.data());
	unsigned q = 0;
	while (!(qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT))
		q++;
	float pr = 1;
	VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueFamilyIndex = q;
	qi.queueCount = 1;
	qi.pQueuePriorities = &pr;
	VkPhysicalDeviceFeatures feat{};
	feat.shaderStorageImageWriteWithoutFormat = 1;
	VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dc.queueCreateInfoCount = 1;
	dc.pQueueCreateInfos = &qi;
	dc.pEnabledFeatures = &feat;
	ok(vkCreateDevice(g, &dc, 0, &d));
	VkQueue queue;
	vkGetDeviceQueue(d, q, 0, &queue);
	auto src = img(VK_FORMAT_R8G8B8A8_UNORM, 256, 256, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	auto y = img(VK_FORMAT_R8_UNORM, 256, 256, VK_IMAGE_USAGE_STORAGE_BIT);
	auto uv = img(VK_FORMAT_R8G8_UNORM, 128, 128, VK_IMAGE_USAGE_STORAGE_BIT);
	auto upload = buf(256 * 256 * 2 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	auto bytes = (unsigned char *)upload.p;
	for (unsigned e = 0; e < 2; e++)
		for (unsigned yy = 0; yy < 256; yy++)
			for (unsigned x = 0; x < 256; x++)
			{
				unsigned k = ((e * 256 + yy) * 256 + x) * 4;
				bool red = (x + yy + e) & 1;
				bytes[k] = red ? 255 : 0;
				bytes[k + 1] = red ? 0 : 255;
				bytes[k + 2] = 0;
				bytes[k + 3] = 255;
			}
	constexpr unsigned md = 4097, tail = 4 * md + 256 + 4;
	auto config = buf((tail + 4) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	memset(config.p, 0, (tail + 4) * 4);
	auto u = (uint32_t *)config.p;
	for (unsigned e = 0; e < 2; e++)
		for (unsigned x = 0; x <= 256; x++)
		{
			u[e * md + x] = x;
			u[2 * md + e * md + x] = x;
		}
	u[tail] = 1;
	u[tail + 1] = 64;
	u[tail + 2] = 64;
	u[tail + 3] = 128;
	auto capture = buf(2 * 128 * 128 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	memset(capture.p, 0xa5, 2 * 128 * 128 * 4);
	VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
	sc.maxLod = 0;
	VkSampler sampler;
	ok(vkCreateSampler(d, &sc, 0, &sampler));
	VkImageView eyeviews[2];
	for (unsigned e = 0; e < 2; e++)
	{
		VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		v.image = src.i;
		v.viewType = VK_IMAGE_VIEW_TYPE_2D;
		v.format = VK_FORMAT_R8G8B8A8_UNORM;
		v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, e, 1};
		ok(vkCreateImageView(d, &v, 0, &eyeviews[e]));
	}
	std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
	for (unsigned b = 0; b < 5; b++)
	{
		bindings[b].binding = b;
		bindings[b].descriptorCount = b == 0 ? 2 : 1;
		bindings[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : (b == 1 || b == 4) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
		                                                                                                     : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	sl.bindingCount = 5;
	sl.pBindings = bindings.data();
	VkDescriptorSetLayout setlayout;
	ok(vkCreateDescriptorSetLayout(d, &sl, 0, &setlayout));
	VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &setlayout;
	VkPipelineLayout layout;
	ok(vkCreatePipelineLayout(d, &pl, 0, &layout));
	std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
	assert(f);
	std::vector<uint32_t> code(size_t(f.tellg()) / 4);
	f.seekg(0);
	f.read((char *)code.data(), code.size() * 4);
	VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	sm.codeSize = code.size() * 4;
	sm.pCode = code.data();
	VkShaderModule mod;
	ok(vkCreateShaderModule(d, &sm, 0, &mod));
	VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	pc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	pc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pc.stage.module = mod;
	pc.stage.pName = "main";
	pc.layout = layout;
	VkPipeline pipeline;
	ok(vkCreateComputePipelines(d, 0, 1, &pc, 0, &pipeline));
	VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
	VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dp.maxSets = 1;
	dp.poolSizeCount = 3;
	dp.pPoolSizes = sizes;
	VkDescriptorPool pool;
	ok(vkCreateDescriptorPool(d, &dp, 0, &pool));
	VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	sa.descriptorPool = pool;
	sa.descriptorSetCount = 1;
	sa.pSetLayouts = &setlayout;
	VkDescriptorSet set;
	ok(vkAllocateDescriptorSets(d, &sa, &set));
	VkDescriptorImageInfo imgs[] = {{sampler, eyeviews[0], VK_IMAGE_LAYOUT_GENERAL}, {sampler, eyeviews[1], VK_IMAGE_LAYOUT_GENERAL}, {0, y.v, VK_IMAGE_LAYOUT_GENERAL}, {0, uv.v, VK_IMAGE_LAYOUT_GENERAL}};
	VkDescriptorBufferInfo bs[] = {{config.b, 0, VK_WHOLE_SIZE}, {capture.b, 0, VK_WHOLE_SIZE}};
	std::array<VkWriteDescriptorSet, 5> writes{};
	for (unsigned b = 0; b < 5; b++)
	{
		writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[b].dstSet = set;
		writes[b].dstBinding = b;
		writes[b].descriptorType = bindings[b].descriptorType;
		writes[b].descriptorCount = bindings[b].descriptorCount;
		if (b == 0)
			writes[b].pImageInfo = imgs;
		else if (b == 1 || b == 4)
			writes[b].pBufferInfo = &bs[b == 4];
		else
			writes[b].pImageInfo = &imgs[b];
	}
	vkUpdateDescriptorSets(d, 5, writes.data(), 0, 0);
	VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cp.queueFamilyIndex = q;
	VkCommandPool cmdpool;
	ok(vkCreateCommandPool(d, &cp, 0, &cmdpool));
	VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ca.commandPool = cmdpool;
	ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ca.commandBufferCount = 1;
	VkCommandBuffer cmd;
	ok(vkAllocateCommandBuffers(d, &ca, &cmd));
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	ok(vkBeginCommandBuffer(cmd, &begin));
	for (auto im: {src.i, y.i, uv.i})
	{
		VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		bar.image = im;
		bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 0, 0, 1, &bar);
	}
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
	copy.imageExtent = {256, 256, 1};
	vkCmdCopyBufferToImage(cmd, upload.b, src.i, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, 0, 0, 0);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, 0);
	vkCmdDispatch(cmd, 32, 32, 2);
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, 0, 0, 0);
	ok(vkEndCommandBuffer(cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	ok(vkQueueSubmit(queue, 1, &si, 0));
	ok(vkQueueWaitIdle(queue));
	auto result = (uint32_t *)capture.p;
	for (unsigned e = 0; e < 2; e++)
		for (unsigned yy = 0; yy < 128; yy++)
			for (unsigned x = 0; x < 128; x++)
			{
				uint32_t expected = ((x + yy + e) & 1) ? 0xff0000u : 0x00ff00u;
				unsigned k = (e * 128 + yy) * 128 + x;
				if (result[k] != expected)
				{
					printf("FAIL %u got %x expected %x\n", k, result[k], expected);
					return 1;
				}
			}
	puts("actual foveation source capture: all 32768 RGB pixels exact");
}
