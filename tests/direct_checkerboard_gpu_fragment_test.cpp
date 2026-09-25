// Render the production direct fragment shader to a headless RGBA8 target and compare it.
#define main checkerboard_sampling_reference_main
#include "direct_checkerboard_upload_sampling_test.cpp"
#undef main

#include <cstring>
#include <vulkan/vulkan.h>

namespace
{
void vk_ok(VkResult r, const char * what)
{
	if (r != VK_SUCCESS)
	{
		std::fprintf(stderr, "%s: Vulkan error %d\n", what, r);
		std::exit(2);
	}
}

uint32_t mem_type(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags wanted)
{
	VkPhysicalDeviceMemoryProperties p{};
	vkGetPhysicalDeviceMemoryProperties(gpu, &p);
	for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & wanted) == wanted)
			return i;
	throw std::runtime_error("required Vulkan memory type unavailable");
}

struct buffer
{
	VkBuffer handle{};
	VkDeviceMemory memory{};
	void * mapped{};
};

buffer make_buffer(VkDevice d, VkPhysicalDevice g, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
	buffer b;
	VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = bytes;
	ci.usage = usage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vk_ok(vkCreateBuffer(d, &ci, nullptr, &b.handle), "buffer");
	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(d, b.handle, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = mem_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	vk_ok(vkAllocateMemory(d, &ai, nullptr, &b.memory), "buffer memory");
	vk_ok(vkBindBufferMemory(d, b.handle, b.memory, 0), "bind buffer");
	vk_ok(vkMapMemory(d, b.memory, 0, bytes, 0, &b.mapped), "map buffer");
	return b;
}

void drop_buffer(VkDevice d, buffer & b)
{
	if (b.mapped)
		vkUnmapMemory(d, b.memory);
	if (b.handle)
		vkDestroyBuffer(d, b.handle, nullptr);
	if (b.memory)
		vkFreeMemory(d, b.memory, nullptr);
}

std::vector<uint32_t> read_spv(const char * path)
{
	std::ifstream f(path, std::ios::binary);
	const std::vector<char> bytes(std::istreambuf_iterator<char>(f), {});
	if (bytes.empty() || bytes.size() % 4)
		throw std::runtime_error("invalid SPIR-V file");
	std::vector<uint32_t> words(bytes.size() / 4);
	std::memcpy(words.data(), bytes.data(), bytes.size());
	return words;
}

struct push_constants
{
	int32_t rgb_rect[4]{}, a_rect[4]{};
	float scale[4]{1, 1, 1, 1}, bias[4]{}, post[4]{};
	float motion[4]{4352, 2176, 0, 0}, glow[4]{1, 0, 0, 0}, deband[4]{};
};
static_assert(sizeof(push_constants) == 128);

std::vector<uint8_t> render(const std::vector<uint32_t> & vert, const std::vector<uint32_t> & frag, std::span<const uint8_t> tile_data, std::span<const uint8_t> block_data, uint32_t width, uint32_t height, bool srgb, const push_constants & pc)
{
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "checkerboard-fragment-parity";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance instance{};
	vk_ok(vkCreateInstance(&ici, nullptr, &instance), "instance");
	uint32_t ng = 0;
	vk_ok(vkEnumeratePhysicalDevices(instance, &ng, nullptr), "enumerate GPUs");
	if (!ng)
		throw std::runtime_error("no Vulkan GPU");
	std::vector<VkPhysicalDevice> gs(ng);
	vk_ok(vkEnumeratePhysicalDevices(instance, &ng, gs.data()), "enumerate GPUs");
	VkPhysicalDevice gpu = gs[0];
	const VkFormat color_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(gpu, &props);
	std::fprintf(stderr, "GPU: %s\n", props.deviceName);
	uint32_t nf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, nullptr);
	std::vector<VkQueueFamilyProperties> fs(nf);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, fs.data());
	uint32_t family = 0;
	while (family < nf && !(fs[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		++family;
	if (family == nf)
		throw std::runtime_error("no graphics queue");
	float priority = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VkDevice device{};
	vk_ok(vkCreateDevice(gpu, &dci, nullptr, &device), "device");
	VkQueue queue{};
	vkGetDeviceQueue(device, family, 0, &queue);
	const size_t pixels = size_t(width) * height;
	const size_t tile_bytes = tile_data.size(), block_bytes = block_data.size();
	auto tiles = make_buffer(device, gpu, tile_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	auto blocks = make_buffer(device, gpu, block_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	auto readback = make_buffer(device, gpu, pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	std::memcpy(tiles.mapped, tile_data.data(), tile_bytes);
	std::memcpy(blocks.mapped, block_data.data(), block_bytes);
	VkImage image{};
	VkDeviceMemory image_mem{};
	VkImageCreateInfo ici_img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici_img.imageType = VK_IMAGE_TYPE_2D;
	ici_img.format = color_format;
	ici_img.extent = {width, height, 1};
	ici_img.mipLevels = 1;
	ici_img.arrayLayers = 1;
	ici_img.samples = VK_SAMPLE_COUNT_1_BIT;
	ici_img.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici_img.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici_img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici_img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	vk_ok(vkCreateImage(device, &ici_img, nullptr, &image), "image");
	VkMemoryRequirements imreq{};
	vkGetImageMemoryRequirements(device, image, &imreq);
	VkMemoryAllocateInfo imai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	imai.allocationSize = imreq.size;
	imai.memoryTypeIndex = mem_type(gpu, imreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	vk_ok(vkAllocateMemory(device, &imai, nullptr, &image_mem), "image memory");
	vk_ok(vkBindImageMemory(device, image, image_mem, 0), "bind image");
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = color_format;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView view{};
	vk_ok(vkCreateImageView(device, &vci, nullptr, &view), "image view");
	VkAttachmentDescription attachment{};
	attachment.format = color_format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	VkAttachmentReference aref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &aref;
	VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpci.attachmentCount = 1;
	rpci.pAttachments = &attachment;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	VkRenderPass render_pass{};
	vk_ok(vkCreateRenderPass(device, &rpci, nullptr, &render_pass), "render pass");
	VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fbci.renderPass = render_pass;
	fbci.attachmentCount = 1;
	fbci.pAttachments = &view;
	fbci.width = width;
	fbci.height = height;
	fbci.layers = 1;
	VkFramebuffer framebuffer{};
	vk_ok(vkCreateFramebuffer(device, &fbci, nullptr, &framebuffer), "framebuffer");
	VkDescriptorSetLayoutBinding db[2]{};
	db[0] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	db[1] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo dsci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dsci.bindingCount = 2;
	dsci.pBindings = db;
	VkDescriptorSetLayout dsl{};
	vk_ok(vkCreateDescriptorSetLayout(device, &dsci, nullptr, &dsl), "descriptor layout");
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &dsl;
	VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VkPipelineLayout layout{};
	vk_ok(vkCreatePipelineLayout(device, &plci, nullptr, &layout), "pipeline layout");
	VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &dps;
	VkDescriptorPool pool{};
	vk_ok(vkCreateDescriptorPool(device, &dpci, nullptr, &pool), "descriptor pool");
	VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = pool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &dsl;
	VkDescriptorSet set{};
	vk_ok(vkAllocateDescriptorSets(device, &dsai, &set), "descriptor set");
	VkDescriptorBufferInfo bi[2]{{tiles.handle, 0, tile_bytes}, {blocks.handle, 0, block_bytes}};
	VkWriteDescriptorSet wr[2]{};
	for (uint32_t i = 0; i < 2; ++i)
	{
		wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[i].dstSet = set;
		wr[i].dstBinding = 3 + i;
		wr[i].descriptorCount = 1;
		wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		wr[i].pBufferInfo = &bi[i];
	}
	vkUpdateDescriptorSets(device, 2, wr, 0, nullptr);
	auto module = [&](const std::vector<uint32_t> & code) {VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=code.size()*4;ci.pCode=code.data();VkShaderModule m{};vk_ok(vkCreateShaderModule(device,&ci,nullptr,&m),"shader module");return m; };
	VkShaderModule vs = module(vert), fsmod = module(frag);
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vs;
	stages[0].pName = "main";
	stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fsmod;
	stages[1].pName = "main";
	VkBool32 srgb_conversion = srgb;
	VkSpecializationMapEntry srgb_entry{1, 0, sizeof(srgb_conversion)};
	VkSpecializationInfo srgb_specialization{1, &srgb_entry, sizeof(srgb_conversion), &srgb_conversion};
	stages[1].pSpecializationInfo = &srgb_specialization;
	VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport viewport{0, 0, float(width), float(height), 0, 1};
	VkRect2D scissor{{0, 0}, {width, height}};
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.pViewports = &viewport;
	vp.scissorCount = 1;
	vp.pScissors = &scissor;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cbs.attachmentCount = 1;
	cbs.pAttachments = &cba;
	VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vis;
	gp.pInputAssemblyState = &ias;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pColorBlendState = &cbs;
	gp.layout = layout;
	gp.renderPass = render_pass;
	gp.subpass = 0;
	VkPipeline pipeline{};
	vk_ok(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline), "graphics pipeline");
	VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpci.queueFamilyIndex = family;
	VkCommandPool cp{};
	vk_ok(vkCreateCommandPool(device, &cpci, nullptr, &cp), "command pool");
	VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = cp;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cmd{};
	vk_ok(vkAllocateCommandBuffers(device, &cbai, &cmd), "command buffer");
	VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vk_ok(vkBeginCommandBuffer(cmd, &cbi), "begin command");
	VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rbi.renderPass = render_pass;
	rbi.framebuffer = framebuffer;
	rbi.renderArea = {{0, 0}, {width, height}};
	vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, &pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {width, height, 1};
	vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &copy);
	VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
	vk_ok(vkEndCommandBuffer(cmd), "end command");
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vk_ok(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "submit");
	vk_ok(vkQueueWaitIdle(queue), "wait");
	std::vector<uint8_t> result(pixels * 4);
	std::memcpy(result.data(), readback.mapped, result.size());
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyShaderModule(device, fsmod, nullptr);
	vkDestroyShaderModule(device, vs, nullptr);
	vkDestroyCommandPool(device, cp, nullptr);
	vkDestroyDescriptorPool(device, pool, nullptr);
	vkDestroyPipelineLayout(device, layout, nullptr);
	vkDestroyDescriptorSetLayout(device, dsl, nullptr);
	vkDestroyFramebuffer(device, framebuffer, nullptr);
	vkDestroyRenderPass(device, render_pass, nullptr);
	vkDestroyImageView(device, view, nullptr);
	vkDestroyImage(device, image, nullptr);
	vkFreeMemory(device, image_mem, nullptr);
	drop_buffer(device, readback);
	drop_buffer(device, blocks);
	drop_buffer(device, tiles);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	return result;
}

uint32_t output_dimension(const char * name, uint32_t fallback)
{
	const char * value = std::getenv(name);
	if (!value || !*value) return fallback;
	char * end = nullptr;
	const unsigned long parsed = std::strtoul(value, &end, 10);
	if (!end || *end || !parsed || parsed > UINT32_MAX)
		throw std::runtime_error(std::string("invalid ") + name + ": " + value);
	return uint32_t(parsed);
}

bool output_srgb()
{
	const char * value = std::getenv("NX_CHECKER_SRGB");
	return value && std::string_view(value) == "1";
}
} // namespace

int main(int argc, char ** argv)
{
	const bool merged = argc > 1 && std::string_view(argv[1]) == "--merged";
	const bool old_phase_in_deband = argc > 1 && std::string_view(argv[1]) == "--legacy-deband-phase";
	const bool legacy = old_phase_in_deband || (argc > 1 && std::string_view(argv[1]) == "--legacy-glow-phase");
	const bool old_phase_in_glow = legacy && !old_phase_in_deband;
	if ((!merged && !legacy && argc != 6) || ((merged || legacy) && argc != (merged ? 7 : 6)))
	{
		std::fprintf(stderr, "usage: %s frag.spv fullscreen.vert.spv checker-now.nxdf checker-old.nxdf checker-upload.nxdu\n"
		                     "       %s --merged frag.spv fullscreen.vert.spv checker-now.nxdf checker-old.nxdf checker-upload.nxdu\n"
		                     "       %s --legacy-deband-phase frag.spv fullscreen.vert.spv checker-now.nxdf checker-old.nxdf\n"
		                     "       %s --legacy-glow-phase frag.spv fullscreen.vert.spv checker-now.nxdf checker-old.nxdf\n",
		             argv[0],
		             argv[0],
		             argv[0],
		             argv[0]);
		return 2;
	}
	const layout l{2176, 2176, 2, true, 256, false, false, false, true};
	const uint32_t output_width = output_dimension("NX_CHECKER_WIDTH", l.width * l.eyes);
	const uint32_t output_height = output_dimension("NX_CHECKER_HEIGHT", l.height);
	const bool srgb = output_srgb();
	const int path_base = (merged || legacy) ? 2 : 1;
	Bytes now = read_file(argv[path_base + 2]), old = read_file(argv[path_base + 3]);
	auto nv = parse_frame(l, now), ov = parse_frame(l, old);
	if (!nv || !ov || !checker_frame(now) || !checker_frame(old))
		return 2;
	Bytes tiles, blocks, upload;
	push_constants pc;
	pc.motion[0] = float(l.width * l.eyes);
	pc.motion[1] = float(l.height);
	if (legacy)
	{
		tiles.insert(tiles.end(), nv->descriptors.begin(), nv->descriptors.end());
		tiles.insert(tiles.end(), ov->descriptors.begin(), ov->descriptors.end());
		blocks.insert(blocks.end(), nv->blocks.begin(), nv->blocks.end());
		blocks.insert(blocks.end(), ov->blocks.begin(), ov->blocks.end());
		pc.glow[0] = float(l.tile_count());
		pc.glow[1] = float(nv->blocks.size() / 4);
		pc.glow[2] = old_phase_in_glow ? float(checker_phase(now) + 1) : 0.0f;
		pc.glow[3] = float(checker_phase(old) + 2);
		pc.deband[2] = old_phase_in_deband ? float(checker_phase(now) + 1) : 0.0f;
	}
	else
	{
		upload = read_file(argv[path_base + 4]);
		if (upload.size() < frame_header_bytes + size_t(l.tile_count()) * 4 || read32(upload, 0) != checker_upload_magic)
			return 2;
		tiles.assign(upload.begin() + frame_header_bytes,
		             upload.begin() + frame_header_bytes + size_t(l.tile_count()) * 4);
		blocks.assign(upload.begin() + frame_header_bytes + size_t(l.tile_count()) * 4, upload.end());
		pc.glow[0] = 1.0f;
	}
	const char *frag_path = argv[path_base], *vert_path = argv[path_base + 1];
	auto rgba = render(read_spv(vert_path), read_spv(frag_path), tiles, blocks, output_width, output_height, srgb, pc);
	if (const char * path = std::getenv("NX_CHECKER_DUMP_RGBA"))
	{
		std::ofstream dump(path, std::ios::binary);
		dump.write(reinterpret_cast<const char *>(rgba.data()), rgba.size());
	}
	mismatch_counts mismatches;
	for (uint32_t y = 0; y < output_height; ++y)
		for (uint32_t x = 0; x < output_width; ++x)
		{
			const size_t at = (size_t(y) * output_width + x) * 4;
			const uint32_t actual = (uint32_t(rgba[at]) << 16) | (uint32_t(rgba[at + 1]) << 8) | rgba[at + 2];
			const uint32_t source_x = std::min(uint32_t((uint64_t(x) * 2 + 1) * (l.width * l.eyes) / (uint64_t(output_width) * 2)), l.width * l.eyes - 1);
			const uint32_t source_y = std::min(uint32_t((uint64_t(y) * 2 + 1) * l.height / (uint64_t(output_height) * 2)), l.height - 1);
			const uint32_t expected = sample_old_shader(l, now, *nv, old, &*ov, source_x, source_y);
			++mismatches.pixels;
			const uint32_t delta = std::max({
			        uint32_t(std::abs(int((actual >> 16) & 255u) - int((expected >> 16) & 255u))),
			        uint32_t(std::abs(int((actual >> 8) & 255u) - int((expected >> 8) & 255u))),
			        uint32_t(std::abs(int(actual & 255u) - int(expected & 255u)))});
			mismatches.max_channel_delta = std::max(mismatches.max_channel_delta, delta);
			if (delta > (srgb ? 1u : 0u)) ++mismatches.mismatched;
		}
	std::printf("{\"scenario\":\"%s\",\"width\":%u,\"height\":%u,\"srgb\":%s,\"pixels\":%llu,\"mismatches\":%llu,\"max_channel_delta\":%u}\n",
	            legacy ? "old-fragment-RGBA8-vs-old-CPU" : "new-fragment-RGBA8-vs-old-CPU",
	            output_width,
	            output_height,
	            srgb ? "true" : "false",
	            (unsigned long long)mismatches.pixels,
	            (unsigned long long)mismatches.mismatched,
	            mismatches.max_channel_delta);
	return mismatches.mismatched ? 1 : 0;
}
