// Run the production sample_block() on Vulkan and compare every texel to the old CPU path.
#define main checkerboard_sampling_reference_main
#include "direct_checkerboard_upload_sampling_test.cpp"
#undef main

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vulkan/vulkan.h>

namespace
{
void vk_ok(VkResult result, const char * what)
{
	if (result != VK_SUCCESS)
	{
		std::fprintf(stderr, "%s: Vulkan error %d\n", what, result);
		std::exit(2);
	}
}

std::string extract_function(const std::string & source, std::string_view signature)
{
	size_t at = source.find(signature);
	if (at == std::string::npos)
		throw std::runtime_error("shader function missing");
	size_t open = source.find('{', at), depth = 0;
	if (open == std::string::npos)
		throw std::runtime_error("shader function body missing");
	for (size_t i = open; i < source.size(); ++i)
	{
		if (source[i] == '{')
			++depth;
		else if (source[i] == '}' && --depth == 0)
			return source.substr(at, i - at + 1);
	}
	throw std::runtime_error("unterminated shader function");
}

uint32_t memory_type(VkPhysicalDevice gpu, uint32_t bits)
{
	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags &
		                           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		                                  (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			return i;
	throw std::runtime_error("no host-coherent Vulkan memory type");
}

struct gpu_buffer
{
	VkBuffer buffer{};
	VkDeviceMemory memory{};
	void * mapped{};
};

gpu_buffer make_buffer(VkDevice device, VkPhysicalDevice gpu, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
	gpu_buffer result;
	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = bytes;
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vk_ok(vkCreateBuffer(device, &bi, nullptr, &result.buffer), "vkCreateBuffer");
	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(device, result.buffer, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memory_type(gpu, req.memoryTypeBits);
	vk_ok(vkAllocateMemory(device, &ai, nullptr, &result.memory), "vkAllocateMemory");
	vk_ok(vkBindBufferMemory(device, result.buffer, result.memory, 0), "vkBindBufferMemory");
	vk_ok(vkMapMemory(device, result.memory, 0, bytes, 0, &result.mapped), "vkMapMemory");
	return result;
}

void drop_buffer(VkDevice device, gpu_buffer & b)
{
	if (b.mapped)
		vkUnmapMemory(device, b.memory);
	if (b.buffer)
		vkDestroyBuffer(device, b.buffer, nullptr);
	if (b.memory)
		vkFreeMemory(device, b.memory, nullptr);
}

std::vector<uint32_t> run_shader(const std::string & shader_path, std::span<const uint8_t> upload, const layout & l, bool precompiled)
{
	std::vector<uint32_t> code;
	if (precompiled)
	{
		std::ifstream spv_file(shader_path, std::ios::binary);
		const std::vector<char> bytes(std::istreambuf_iterator<char>(spv_file), {});
		if (bytes.size() % sizeof(uint32_t))
			throw std::runtime_error("bad SPIR-V byte length");
		code.resize(bytes.size() / sizeof(uint32_t));
		std::memcpy(code.data(), bytes.data(), bytes.size());
	}
	else
	{
		std::ifstream frag_file(shader_path);
		const std::string frag((std::istreambuf_iterator<char>(frag_file)), {});
		const std::string shader =
		        "#version 450\n"
		        "layout(local_size_x=8,local_size_y=8) in;\n"
		        "layout(binding=3,std430) readonly buffer direct_tiles_t { uint tile[]; } tiles;\n"
		        "layout(binding=4,std430) readonly buffer direct_blocks_t { uint block[]; } blocks;\n"
		        "layout(binding=0,std430) writeonly buffer result_t { uint pixel[]; } result;\n"
		        "layout(push_constant) uniform params_t { vec4 motion; uvec4 dims; } params;\n"
		        "#define motion params.motion\n" +
		        extract_function(frag, "uvec3 rgb565(") + "\n" +
		        extract_function(frag, "vec3 sample_block(") +
		        "\nvoid main(){ uvec2 p=gl_GlobalInvocationID.xy; if(p.x>=params.dims.x||p.y>=params.dims.y)return;"
		        " vec3 c=sample_block(p,1u); uvec3 v=uvec3(round(clamp(c,0.0,1.0)*255.0));"
		        " result.pixel[p.y*params.dims.x+p.x]=(v.r<<16)|(v.g<<8)|v.b; }\n";
		const auto dir = std::filesystem::temp_directory_path() / "checkerboard-gpu-sample";
		std::filesystem::create_directories(dir);
		const auto src = dir / "sample.comp", spv = dir / "sample.spv";
		{
			std::ofstream out(src);
			out << shader;
		}
		const std::string cmd = "glslangValidator -V --target-env vulkan1.1 -S comp -o '" + spv.string() + "' '" + src.string() + "' >/dev/null";
		if (std::system(cmd.c_str()) != 0)
			throw std::runtime_error("glslangValidator failed");
		std::ifstream spv_file(spv, std::ios::binary);
		const std::vector<char> spv_bytes(std::istreambuf_iterator<char>(spv_file), {});
		if (spv_bytes.size() % sizeof(uint32_t))
			throw std::runtime_error("bad SPIR-V byte length");
		code.resize(spv_bytes.size() / sizeof(uint32_t));
		std::memcpy(code.data(), spv_bytes.data(), spv_bytes.size());
	}
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "checkerboard-gpu-sample-test";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance instance{};
	vk_ok(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
	uint32_t count = 0;
	vk_ok(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPUs");
	if (!count)
		throw std::runtime_error("no Vulkan GPU");
	std::vector<VkPhysicalDevice> gpus(count);
	vk_ok(vkEnumeratePhysicalDevices(instance, &count, gpus.data()), "enumerate GPUs");
	VkPhysicalDevice gpu = gpus[0];
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(gpu, &props);
	std::fprintf(stderr, "GPU: %s\n", props.deviceName);
	uint32_t nf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, nullptr);
	std::vector<VkQueueFamilyProperties> families(nf);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, families.data());
	uint32_t family = 0;
	while (family < nf && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
		++family;
	if (family == nf)
		throw std::runtime_error("no compute queue");
	float priority = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VkDevice device{};
	vk_ok(vkCreateDevice(gpu, &dci, nullptr, &device), "vkCreateDevice");
	VkQueue queue{};
	vkGetDeviceQueue(device, family, 0, &queue);
	const size_t descriptors_at = frame_header_bytes;
	const size_t blocks_at = descriptors_at + size_t(l.tile_count()) * 4;
	const size_t block_bytes = upload.size() - blocks_at;
	const size_t pixels = size_t(l.width) * l.height * l.eyes;
	auto tiles = make_buffer(device, gpu, size_t(l.tile_count()) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	auto blocks = make_buffer(device, gpu, block_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	auto output = make_buffer(device, gpu, pixels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	std::memcpy(tiles.mapped, upload.data() + descriptors_at, size_t(l.tile_count()) * 4);
	std::memcpy(blocks.mapped, upload.data() + blocks_at, block_bytes);
	VkDescriptorSetLayoutBinding bindings[3]{};
	for (uint32_t i = 0; i < 3; ++i)
	{
		bindings[i].binding = i == 0 ? 0u : i + 2;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	sli.bindingCount = 3;
	sli.pBindings = bindings;
	VkDescriptorSetLayout set_layout{};
	vk_ok(vkCreateDescriptorSetLayout(device, &sli, nullptr, &set_layout), "descriptor layout");
	VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 1;
	pci.poolSizeCount = 1;
	pci.pPoolSizes = &pool_size;
	VkDescriptorPool pool{};
	vk_ok(vkCreateDescriptorPool(device, &pci, nullptr, &pool), "descriptor pool");
	VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	sai.descriptorPool = pool;
	sai.descriptorSetCount = 1;
	sai.pSetLayouts = &set_layout;
	VkDescriptorSet set{};
	vk_ok(vkAllocateDescriptorSets(device, &sai, &set), "descriptor set");
	VkDescriptorBufferInfo infos[3]{{output.buffer, 0, pixels * 4}, {tiles.buffer, 0, size_t(l.tile_count()) * 4}, {blocks.buffer, 0, block_bytes}};
	VkWriteDescriptorSet writes[3]{};
	const uint32_t bind_index[3]{0, 3, 4};
	for (uint32_t i = 0; i < 3; ++i)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = bind_index[i];
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &infos[i];
	}
	vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
	VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &set_layout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &range;
	VkPipelineLayout pipeline_layout{};
	vk_ok(vkCreatePipelineLayout(device, &pli, nullptr, &pipeline_layout), "pipeline layout");
	VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = code.size() * 4;
	smi.pCode = code.data();
	VkShaderModule module{};
	vk_ok(vkCreateShaderModule(device, &smi, nullptr, &module), "shader module");
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = module;
	cpi.stage.pName = "main";
	cpi.layout = pipeline_layout;
	VkPipeline pipeline{};
	vk_ok(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline), "compute pipeline");
	VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpci.queueFamilyIndex = family;
	VkCommandPool command_pool{};
	vk_ok(vkCreateCommandPool(device, &cpci, nullptr, &command_pool), "command pool");
	VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = command_pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer command{};
	vk_ok(vkAllocateCommandBuffers(device, &cbai, &command), "command buffer");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vk_ok(vkBeginCommandBuffer(command, &begin), "begin command buffer");
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
	struct
	{
		float motion[4];
		uint32_t dims[4];
	} push{{float(l.width * l.eyes), 0, 0, 0}, {l.width * l.eyes, l.height, 0, 0}};
	vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(command, (l.width * l.eyes + 7) / 8, (l.height + 7) / 8, 1);
	VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
	vk_ok(vkEndCommandBuffer(command), "end command buffer");
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	vk_ok(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit");
	vk_ok(vkQueueWaitIdle(queue), "wait GPU");
	std::vector<uint32_t> result(pixels);
	std::memcpy(result.data(), output.mapped, pixels * 4);
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyShaderModule(device, module, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyDescriptorPool(device, pool, nullptr);
	vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
	vkDestroyCommandPool(device, command_pool, nullptr);
	drop_buffer(device, output);
	drop_buffer(device, blocks);
	drop_buffer(device, tiles);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	return result;
}
} // namespace

int main(int argc, char ** argv)
{
	const bool precompiled = argc > 1 && std::string_view(argv[1]) == "--spv";
	if (argc != (precompiled ? 6 : 5))
	{
		std::fprintf(stderr, "usage: %s reprojection_direct.frag.glsl checker-now.nxdf checker-old.nxdf checker-upload.nxdu\n"
		                     "       %s --spv wrapper.spv checker-now.nxdf checker-old.nxdf checker-upload.nxdu\n",
		             argv[0],
		             argv[0]);
		return 2;
	}
	const layout l{2176, 2176, 2, true, 256, false, false, false, true};
	const Bytes now = read_file(argv[precompiled ? 3 : 2]), old = read_file(argv[precompiled ? 4 : 3]), upload = read_file(argv[precompiled ? 5 : 4]);
	const auto now_view = parse_frame(l, now), old_view = parse_frame(l, old);
	if (!now_view || !old_view || !checker_frame(now) || !checker_frame(old))
		return 2;
	auto gpu = run_shader(argv[precompiled ? 2 : 1], upload, l, precompiled);
	mismatch_counts result;
	for (uint32_t y = 0; y < l.height; ++y)
		for (uint32_t x = 0; x < l.width * l.eyes; ++x)
		{
			const uint32_t expected = sample_old_shader(l, now, *now_view, old, &*old_view, x, y);
			compare_pixel(result, gpu[size_t(y) * l.width * l.eyes + x], expected);
		}
	std::printf("{\"scenario\":\"production-sample_block-GPU-vs-old-CPU\",\"pixels\":%llu,\"mismatches\":%llu,\"max_channel_delta\":%u}\n",
	            (unsigned long long)result.pixels,
	            (unsigned long long)result.mismatched,
	            result.max_channel_delta);
	return result.mismatched ? 1 : 0;
}
