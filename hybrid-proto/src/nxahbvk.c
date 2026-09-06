// nxahbvk -- what does it cost to get a MediaCodec-decoded HEVC frame into an
// NX Warp atlas tile, on the Pico 4?
//
// Decodes the same base stream as nxhevcbench through the same AImageReader the
// WiVRn client uses, then for each decoded frame:
//   * imports the AHardwareBuffer into Vulkan exactly as
//     client/decoder/android/android_decoder.cpp map_hardware_buffer() does
//     (VkExternalFormatANDROID, dedicated allocation, cached by AHB pointer),
//     timing the cold import and the cached lookup separately;
//   * dispatches atlas_patch.comp over a list of 64x64 tiles, converting the
//     decoder's YCbCr to the codec's coded sample domain (Y/Co/Cg) in integer
//     arithmetic, and timing the dispatch with GPU timestamp queries.
//
// The tile count is swept, so the result is a per-tile cost and a full-frame
// cost, which is what ADR-0029's 4.2 ms budget needs.
//
// --dump-atlas writes the converted atlas back to a file so the integer
// conversion can be checked against a CPU computation from the reference decode.
//
// Standalone: no app, no window, no wivrn-server, no installed package.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>
#include <android/hardware_buffer.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#define MAXF 2048
#define TILE 64
#define VKC(x)                                                             \
	do {                                                                   \
		VkResult _r = (x);                                                 \
		if (_r != VK_SUCCESS)                                              \
		{                                                                  \
			fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, _r); \
			exit(5);                                                       \
		}                                                                  \
	} while (0)

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int cmpd(const void * a, const void * b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

// ------------------------------------------------------------------ Vulkan bits

static VkInstance vki;
static VkPhysicalDevice vkpd;
static VkDevice vkd;
static VkQueue vkq;
static uint32_t qfam;
static float ts_period;
static VkQueryPool qpool;
static VkCommandPool cpool;
static VkCommandBuffer cmd;
static VkFence fence;

static PFN_vkGetAndroidHardwareBufferPropertiesANDROID pfnAhbProps;

static VkSamplerYcbcrConversion ycbcr;
static VkSampler sampler;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout plyt;
static VkPipeline cpipe;
static VkDescriptorPool dpool;

static VkImage atlasY, atlasC;
static VkDeviceMemory atlasYm, atlasCm;
static VkImageView atlasYv, atlasCv;
static VkBuffer tileBuf, readBuf;
static VkDeviceMemory tileMem, readMem;

struct pc_t {
	int32_t bw, bh, nTiles, planes;
};

// one imported AHardwareBuffer
struct imp {
	AHardwareBuffer * ahb;
	VkImage img;
	VkDeviceMemory mem;
	VkImageView view;
	VkDescriptorSet ds;
};
static struct imp imps[16];
static int nimps;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(vkpd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type for bits %x want %x\n", bits, want);
	exit(5);
}

static void vk_init(void)
{
	VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	                        .pApplicationName = "nxahbvk",
	                        .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	                            .pApplicationInfo = &ai};
	VKC(vkCreateInstance(&ici, NULL, &vki));

	uint32_t n = 0;
	vkEnumeratePhysicalDevices(vki, &n, NULL);
	VkPhysicalDevice pds[8];
	if (n > 8)
		n = 8;
	vkEnumeratePhysicalDevices(vki, &n, pds);
	vkpd = pds[0];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(vkpd, &props);
	ts_period = props.limits.timestampPeriod;
	fprintf(stderr, "[vk] %s  api %u.%u.%u  timestampPeriod %.3f ns\n", props.deviceName,
	        VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
	        VK_VERSION_PATCH(props.apiVersion), ts_period);

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(vkpd, &qn, NULL);
	VkQueueFamilyProperties qs[8];
	if (qn > 8)
		qn = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(vkpd, &qn, qs);
	qfam = 0;
	for (uint32_t i = 0; i < qn; i++)
		if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
		{
			qfam = i;
			break;
		}
	fprintf(stderr, "[vk] queue family %u  timestampValidBits %u\n", qfam, qs[qfam].timestampValidBits);

	const char * dex[] = {
	        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
	        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
	        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
	        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
	        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
	        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
	        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
	};
	float prio = 1.f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                               .queueFamilyIndex = qfam,
	                               .queueCount = 1,
	                               .pQueuePriorities = &prio};
	VkPhysicalDeviceSamplerYcbcrConversionFeatures yf = {
	        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
	        .samplerYcbcrConversion = VK_TRUE};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	                          .pNext = &yf,
	                          .queueCreateInfoCount = 1,
	                          .pQueueCreateInfos = &qci,
	                          .enabledExtensionCount = sizeof dex / sizeof *dex,
	                          .ppEnabledExtensionNames = dex};
	VKC(vkCreateDevice(vkpd, &dci, NULL, &vkd));
	vkGetDeviceQueue(vkd, qfam, 0, &vkq);

	pfnAhbProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
	        vkd, "vkGetAndroidHardwareBufferPropertiesANDROID");

	VkQueryPoolCreateInfo qp = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
	                            .queryType = VK_QUERY_TYPE_TIMESTAMP,
	                            .queryCount = 2};
	VKC(vkCreateQueryPool(vkd, &qp, NULL, &qpool));
	VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                              .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                              .queueFamilyIndex = qfam};
	VKC(vkCreateCommandPool(vkd, &cp, NULL, &cpool));
	VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                   .commandPool = cpool,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                   .commandBufferCount = 1};
	VKC(vkAllocateCommandBuffers(vkd, &cba, &cmd));
	VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VKC(vkCreateFence(vkd, &fc, NULL, &fence));
}

static void mk_image(int w, int h, VkFormat f, VkImage * im, VkDeviceMemory * mm, VkImageView * iv)
{
	VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                        .imageType = VK_IMAGE_TYPE_2D,
	                        .format = f,
	                        .extent = {w, h, 1},
	                        .mipLevels = 1,
	                        .arrayLayers = 1,
	                        .samples = VK_SAMPLE_COUNT_1_BIT,
	                        .tiling = VK_IMAGE_TILING_OPTIMAL,
	                        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	VKC(vkCreateImage(vkd, &ic, NULL, im));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(vkd, *im, &mr);
	VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                           .allocationSize = mr.size,
	                           .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VKC(vkAllocateMemory(vkd, &ma, NULL, mm));
	VKC(vkBindImageMemory(vkd, *im, *mm, 0));
	VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                            .image = *im,
	                            .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                            .format = f,
	                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VKC(vkCreateImageView(vkd, &vc, NULL, iv));
}

static void mk_buffer(VkDeviceSize sz, VkBufferUsageFlags u, VkMemoryPropertyFlags mp,
                      VkBuffer * b, VkDeviceMemory * m)
{
	VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                         .size = sz,
	                         .usage = u,
	                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VKC(vkCreateBuffer(vkd, &bc, NULL, b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(vkd, *b, &mr);
	VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                           .allocationSize = mr.size,
	                           .memoryTypeIndex = mem_type(mr.memoryTypeBits, mp)};
	VKC(vkAllocateMemory(vkd, &ma, NULL, m));
	VKC(vkBindBufferMemory(vkd, *b, *m, 0));
}

// The pipeline can only be built once an AHB has told us the external format,
// because the ycbcr conversion is an immutable sampler in the layout. This is
// the same ordering constraint android_decoder.cpp has.
static void build_pipeline(uint64_t extfmt, VkSamplerYcbcrConversionCreateInfo base,
                           const char * spv_path)
{
	VkExternalFormatANDROID ef = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
	                              .externalFormat = extfmt};
	base.pNext = &ef;
	// RGB_IDENTITY + NEAREST + full range: the sampler does no colour
	// conversion and no chroma filtering, so .rgb are the decoder's raw
	// Y/Cb/Cr samples and the conversion below is ours and integer.
	base.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
	base.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
	base.chromaFilter = VK_FILTER_NEAREST;
	base.forceExplicitReconstruction = VK_FALSE;
	VKC(vkCreateSamplerYcbcrConversion(vkd, &base, NULL, &ycbcr));

	VkSamplerYcbcrConversionInfo yi = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
	                                   .conversion = ycbcr};
	VkSamplerCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	                          .pNext = &yi,
	                          .magFilter = VK_FILTER_NEAREST,
	                          .minFilter = VK_FILTER_NEAREST,
	                          .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	                          .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                          .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                          .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                          .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
	                          .unnormalizedCoordinates = VK_FALSE};
	VKC(vkCreateSampler(vkd, &sc, NULL, &sampler));

	VkDescriptorSetLayoutBinding b[4] = {
	        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler},
	        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	};
	VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	                                      .bindingCount = 4,
	                                      .pBindings = b};
	VKC(vkCreateDescriptorSetLayout(vkd, &dl, NULL, &dsl));

	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct pc_t)};
	VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                 .setLayoutCount = 1,
	                                 .pSetLayouts = &dsl,
	                                 .pushConstantRangeCount = 1,
	                                 .pPushConstantRanges = &pcr};
	VKC(vkCreatePipelineLayout(vkd, &pl, NULL, &plyt));

	FILE * f = fopen(spv_path, "rb");
	if (!f)
	{
		fprintf(stderr, "open %s: %s\n", spv_path, strerror(errno));
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t * code = malloc(sz);
	if (fread(code, 1, sz, f) != (size_t)sz)
		exit(2);
	fclose(f);
	VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                              .codeSize = sz,
	                              .pCode = code};
	VkShaderModule mod;
	VKC(vkCreateShaderModule(vkd, &sm, NULL, &mod));
	VkComputePipelineCreateInfo cpi = {
	        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	                  .module = mod,
	                  .pName = "main"},
	        .layout = plyt};
	VKC(vkCreateComputePipelines(vkd, VK_NULL_HANDLE, 1, &cpi, NULL, &cpipe));
	vkDestroyShaderModule(vkd, mod, NULL);

	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32}};
	VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	                                 .maxSets = 32,
	                                 .poolSizeCount = 3,
	                                 .pPoolSizes = ps};
	VKC(vkCreateDescriptorPool(vkd, &dp, NULL, &dpool));
}

// ------------------------------------------------------------------ AHB import

static int g_bw = 1088, g_bh = 1088;
static const char * g_spv = "/data/local/tmp/nxhybrid/atlas_patch.spv";
static double imp_cold[64];
static int n_cold;
static double imp_warm[MAXF];
static int n_warm;

static struct imp * import_ahb(AHardwareBuffer * ahb)
{
	double t0 = now_ms();
	for (int i = 0; i < nimps; i++)
		if (imps[i].ahb == ahb)
		{
			if (n_warm < MAXF)
				imp_warm[n_warm++] = now_ms() - t0;
			return &imps[i];
		}

	AHardwareBuffer_Desc d;
	AHardwareBuffer_describe(ahb, &d);

	VkAndroidHardwareBufferFormatPropertiesANDROID fp = {
	        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
	VkAndroidHardwareBufferPropertiesANDROID hp = {
	        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp};
	VKC(pfnAhbProps(vkd, ahb, &hp));

	if (!cpipe)
	{
		fprintf(stderr, "[ahb] %ux%u fmt=0x%x layers=%u  vkFormat=%d externalFormat=%llu\n",
		        d.width, d.height, d.format, d.layers, fp.format,
		        (unsigned long long)fp.externalFormat);
		fprintf(stderr, "[ahb] suggestedYcbcrModel=%d range=%d xChroma=%d yChroma=%d "
		                "formatFeatures=0x%llx\n",
		        fp.suggestedYcbcrModel, fp.suggestedYcbcrRange, fp.suggestedXChromaOffset,
		        fp.suggestedYChromaOffset, (unsigned long long)fp.formatFeatures);
		VkSamplerYcbcrConversionCreateInfo base = {
		        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		        .format = VK_FORMAT_UNDEFINED,
		        .components = fp.samplerYcbcrConversionComponents,
		        .xChromaOffset = fp.suggestedXChromaOffset,
		        .yChromaOffset = fp.suggestedYChromaOffset};
		build_pipeline(fp.externalFormat, base, g_spv);
	}

	struct imp * im = &imps[nimps];
	im->ahb = ahb;

	VkExternalFormatANDROID ef = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
	                              .externalFormat = fp.externalFormat};
	VkExternalMemoryImageCreateInfo emi = {
	        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	        .pNext = &ef,
	        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
	VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                        .pNext = &emi,
	                        .imageType = VK_IMAGE_TYPE_2D,
	                        .format = VK_FORMAT_UNDEFINED,
	                        .extent = {d.width, d.height, 1},
	                        .mipLevels = 1,
	                        .arrayLayers = 1,
	                        .samples = VK_SAMPLE_COUNT_1_BIT,
	                        .tiling = VK_IMAGE_TILING_OPTIMAL,
	                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	VKC(vkCreateImage(vkd, &ic, NULL, &im->img));

	VkMemoryDedicatedAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
	                                     .image = im->img};
	VkImportAndroidHardwareBufferInfoANDROID iai = {
	        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
	        .pNext = &dai,
	        .buffer = ahb};
	// same memory-type choice as android_decoder.cpp:456
	uint32_t mt = 0;
	for (uint32_t i = 0; i < 32; i++)
		if (hp.memoryTypeBits & (1u << i))
		{
			mt = i;
			break;
		}
	VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                           .pNext = &iai,
	                           .allocationSize = hp.allocationSize,
	                           .memoryTypeIndex = mt};
	VKC(vkAllocateMemory(vkd, &ma, NULL, &im->mem));
	VKC(vkBindImageMemory(vkd, im->img, im->mem, 0));

	VkSamplerYcbcrConversionInfo yi = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
	                                   .conversion = ycbcr};
	VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                            .pNext = &yi,
	                            .image = im->img,
	                            .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                            .format = VK_FORMAT_UNDEFINED,
	                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VKC(vkCreateImageView(vkd, &vc, NULL, &im->view));

	VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	                                   .descriptorPool = dpool,
	                                   .descriptorSetCount = 1,
	                                   .pSetLayouts = &dsl};
	VKC(vkAllocateDescriptorSets(vkd, &dsa, &im->ds));

	VkDescriptorImageInfo di0 = {.sampler = sampler,
	                             .imageView = im->view,
	                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
	VkDescriptorImageInfo di1 = {.imageView = atlasYv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
	VkDescriptorImageInfo di2 = {.imageView = atlasCv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
	VkDescriptorBufferInfo db = {.buffer = tileBuf, .offset = 0, .range = VK_WHOLE_SIZE};
	VkWriteDescriptorSet w[4] = {
	        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = im->ds, .dstBinding = 0,
	         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	         .pImageInfo = &di0},
	        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = im->ds, .dstBinding = 1,
	         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	         .pImageInfo = &di1},
	        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = im->ds, .dstBinding = 2,
	         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	         .pImageInfo = &di2},
	        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = im->ds, .dstBinding = 3,
	         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	         .pBufferInfo = &db},
	};
	vkUpdateDescriptorSets(vkd, 4, w, 0, NULL);

	nimps++;
	if (n_cold < 64)
		imp_cold[n_cold++] = now_ms() - t0;
	return im;
}

// ------------------------------------------------------------------ dispatch

static double gpu_ms[MAXF];
static int n_gpu;

static void run_patch(struct imp * im, int nTiles, int planes, int readback)
{
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	vkResetCommandBuffer(cmd, 0);
	VKC(vkBeginCommandBuffer(cmd, &bi));
	vkCmdResetQueryPool(cmd, qpool, 0, 2);

	VkImageMemoryBarrier bar[3] = {
	        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	         .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = qfam,
	         .image = im->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	         .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	         .image = atlasY, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	         .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	         .image = atlasC, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 3, bar);

	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, plyt, 0, 1, &im->ds, 0, NULL);
	struct pc_t pc = {g_bw, g_bh, nTiles, planes};
	vkCmdPushConstants(cmd, plyt, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
	vkCmdDispatch(cmd, TILE / 16, TILE / 16, nTiles);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);

	if (readback)
	{
		VkImageMemoryBarrier rb = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		                           .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		                           .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		                           .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		                           .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		                           .image = atlasY,
		                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &rb);
		VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		                       .imageExtent = {g_bw, g_bh, 1}};
		vkCmdCopyImageToBuffer(cmd, atlasY, VK_IMAGE_LAYOUT_GENERAL, readBuf, 1, &c);
	}
	VKC(vkEndCommandBuffer(cmd));

	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
	                   .pCommandBuffers = &cmd};
	vkResetFences(vkd, 1, &fence);
	VKC(vkQueueSubmit(vkq, 1, &si, fence));
	VKC(vkWaitForFences(vkd, 1, &fence, VK_TRUE, UINT64_MAX));

	uint64_t ts[2] = {0, 0};
	if (vkGetQueryPoolResults(vkd, qpool, 0, 2, sizeof ts, ts, sizeof ts[0],
	                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
	{
		if (n_gpu < MAXF)
			gpu_ms[n_gpu++] = (double)(ts[1] - ts[0]) * ts_period / 1e6;
	}
}

// ------------------------------------------------------------------ MediaCodec

static AMediaCodec * codec;
static AImageReader * reader;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int out_n, err_flag, stop_flag;
static AImage * pending;

static void on_image(void * ud, AImageReader * r)
{
	(void)ud;
	AImage * img = NULL;
	if (AImageReader_acquireNextImage(r, &img) != AMEDIA_OK || !img)
		return;
	pthread_mutex_lock(&mu);
	if (pending)
	{
		AImage_delete(pending); // drop, we are slower than the decoder
	}
	pending = img;
	out_n++;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

static void * drain_thread(void * ud)
{
	(void)ud;
	while (!stop_flag && !err_flag)
	{
		AMediaCodecBufferInfo bi;
		ssize_t i = AMediaCodec_dequeueOutputBuffer(codec, &bi, 2000);
		if (i >= 0)
			AMediaCodec_releaseOutputBuffer(codec, i, true);
	}
	return NULL;
}

struct au {
	size_t off, size;
	int key;
};
static struct au aus[MAXF];
static int naus;
static uint8_t * stream_buf;

static uint8_t * slurp(const char * p, size_t * n)
{
	FILE * f = fopen(p, "rb");
	if (!f)
	{
		fprintf(stderr, "open %s: %s\n", p, strerror(errno));
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t * b = malloc(sz);
	if (fread(b, 1, sz, f) != (size_t)sz)
		exit(2);
	fclose(f);
	*n = sz;
	return b;
}

int main(int argc, char ** argv)
{
	const char *sp = NULL, *ip = NULL, *cp = NULL, *dump = NULL;
	int frames = 120, warmup = 20, planes = 3, sweep = 0;
	int tiles_arg = 289;

	for (int i = 1; i < argc; i++)
	{
#define A(s) (!strcmp(argv[i], s))
		if (A("--stream") && i + 1 < argc)
			sp = argv[++i];
		else if (A("--index") && i + 1 < argc)
			ip = argv[++i];
		else if (A("--csd") && i + 1 < argc)
			cp = argv[++i];
		else if (A("--spv") && i + 1 < argc)
			g_spv = argv[++i];
		else if (A("--frames") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (A("--warmup") && i + 1 < argc)
			warmup = atoi(argv[++i]);
		else if (A("--tiles") && i + 1 < argc)
			tiles_arg = atoi(argv[++i]);
		else if (A("--planes") && i + 1 < argc)
			planes = atoi(argv[++i]);
		else if (A("--sweep"))
			sweep = 1;
		else if (A("--dump-atlas") && i + 1 < argc)
			dump = argv[++i];
		else
		{
			fprintf(stderr, "unknown arg %s\n", argv[i]);
			return 1;
		}
#undef A
	}
	if (!sp || !ip)
	{
		fprintf(stderr, "nxahbvk --stream S.hevc --index S.idx [--csd S.csd] [--spv F]\n"
		                "  [--frames N] [--warmup N] [--tiles N] [--planes 1|3] [--sweep]\n"
		                "  [--dump-atlas FILE]\n");
		return 1;
	}

	size_t n;
	stream_buf = slurp(sp, &n);
	FILE * f = fopen(ip, "r");
	unsigned long o, s;
	int k;
	while (naus < MAXF && fscanf(f, "%lu %lu %d", &o, &s, &k) == 3)
		aus[naus++] = (struct au){o, s, k};
	fclose(f);
	uint8_t * csd = NULL;
	size_t csd_n = 0;
	if (cp)
		csd = slurp(cp, &csd_n);

	vk_init();
	mk_image(g_bw, g_bh, VK_FORMAT_R8_UINT, &atlasY, &atlasYm, &atlasYv);
	mk_image(g_bw / 2, g_bh / 2, VK_FORMAT_R16G16_SINT, &atlasC, &atlasCm, &atlasCv);
	mk_buffer(289 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	          &tileBuf, &tileMem);
	mk_buffer((VkDeviceSize)g_bw * g_bh, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	          &readBuf, &readMem);
	{ // the 17x17 tile grid, raster order
		int32_t * tp;
		VKC(vkMapMemory(vkd, tileMem, 0, VK_WHOLE_SIZE, 0, (void **)&tp));
		int t = 0;
		for (int ty = 0; ty < g_bh / TILE; ty++)
			for (int tx = 0; tx < g_bw / TILE; tx++)
			{
				tp[2 * t] = tx * TILE;
				tp[2 * t + 1] = ty * TILE;
				t++;
			}
		fprintf(stderr, "[atlas] %d tiles of %dx%d over %dx%d\n", t, TILE, TILE, g_bw, g_bh);
		vkUnmapMemory(vkd, tileMem);
	}

	VKC(AImageReader_newWithUsage(g_bw, g_bh, AIMAGE_FORMAT_PRIVATE,
	                              AHARDWAREBUFFER_USAGE_CPU_READ_NEVER |
	                                      AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER |
	                                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
	                              7, &reader) == AMEDIA_OK
	            ? VK_SUCCESS
	            : VK_ERROR_INITIALIZATION_FAILED);
	AImageReader_ImageListener il = {NULL, on_image};
	AImageReader_setImageListener(reader, &il);
	ANativeWindow * win = NULL;
	AImageReader_getWindow(reader, &win);

	codec = AMediaCodec_createDecoderByType("video/hevc");
	char * cname = NULL;
	AMediaCodec_getName(codec, &cname);
	AMediaFormat * fmt = AMediaFormat_new();
	AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, g_bw);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, g_bh);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_OPERATING_RATE, 90);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_PRIORITY, 0);
	AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);
	if (csd)
		AMediaFormat_setBuffer(fmt, "csd-0", csd, csd_n);
	if (AMediaCodec_configure(codec, fmt, win, NULL, 0) != AMEDIA_OK)
	{
		fprintf(stderr, "configure failed\n");
		return 3;
	}
	AMediaCodec_start(codec);
	pthread_t dt;
	pthread_create(&dt, NULL, drain_thread, NULL);
	fprintf(stderr, "[codec] %s\n", cname ? cname : "?");

	int tile_list[] = {289, 144, 72, 36, 18, 9};
	int ntl = sweep ? (int)(sizeof tile_list / sizeof *tile_list) : 1;
	if (!sweep)
		tile_list[0] = tiles_arg;

	printf("== nxahbvk  %dx%d  planes=%d  stream=%s\n", g_bw, g_bh, planes, sp);

	for (int tl = 0; tl < ntl; tl++)
	{
		int nT = tile_list[tl];
		n_gpu = 0;
		int total = warmup + frames;
		for (int i = 0; i < total && !err_flag; i++)
		{
			const struct au * a = &aus[i % naus];
			ssize_t bidx = -1;
			for (int spin = 0; spin < 1000; spin++)
				if ((bidx = AMediaCodec_dequeueInputBuffer(codec, 5000)) >= 0)
					break;
			if (bidx < 0)
			{
				fprintf(stderr, "no input buffer\n");
				err_flag = 1;
				break;
			}
			size_t cap = 0;
			uint8_t * ib = AMediaCodec_getInputBuffer(codec, bidx, &cap);
			memcpy(ib, stream_buf + a->off, a->size);
			AMediaCodec_queueInputBuffer(codec, bidx, 0, a->size, (uint64_t)i * 10000, 0);

			AImage * img = NULL;
			pthread_mutex_lock(&mu);
			for (int w = 0; w < 500 && !pending; w++)
			{
				struct timespec to;
				clock_gettime(CLOCK_REALTIME, &to);
				to.tv_nsec += 10000000;
				if (to.tv_nsec > 999999999)
				{
					to.tv_nsec -= 1000000000;
					to.tv_sec++;
				}
				pthread_cond_timedwait(&cv, &mu, &to);
			}
			img = pending;
			pending = NULL;
			pthread_mutex_unlock(&mu);
			if (!img)
				continue;

			AHardwareBuffer * ahb = NULL;
			AImage_getHardwareBuffer(img, &ahb);
			if (ahb)
			{
				struct imp * im = import_ahb(ahb);
				int last = (i == total - 1) && dump && tl == 0;
				if (i >= warmup || last)
					run_patch(im, nT, planes, last);
				if (last)
				{
					void * mp;
					vkMapMemory(vkd, readMem, 0, VK_WHOLE_SIZE, 0, &mp);
					FILE * o2 = fopen(dump, "wb");
					fwrite(mp, 1, (size_t)g_bw * g_bh, o2);
					fclose(o2);
					vkUnmapMemory(vkd, readMem);
					fprintf(stderr, "[dump] wrote atlas Y plane -> %s (frame %d)\n", dump, i);
				}
			}
			AImage_delete(img);
		}
		double * v = malloc(n_gpu * sizeof *v);
		memcpy(v, gpu_ms, n_gpu * sizeof *v);
		qsort(v, n_gpu, sizeof *v, cmpd);
		double sum = 0;
		for (int i = 0; i < n_gpu; i++)
			sum += v[i];
		double mean = n_gpu ? sum / n_gpu : 0;
		printf("   tiles %3d  (%6d px)  gpu ms: mean %.4f  p50 %.4f  p95 %.4f  max %.4f  "
		       "| per tile %.1f us  (n=%d)\n",
		       nT, nT * TILE * TILE, mean, n_gpu ? v[n_gpu / 2] : 0,
		       n_gpu ? v[(int)(n_gpu * 0.95)] : 0, n_gpu ? v[n_gpu - 1] : 0,
		       nT ? mean * 1000.0 / nT : 0, n_gpu);
		free(v);
	}

	{
		double sum = 0;
		for (int i = 0; i < n_cold; i++)
			sum += imp_cold[i];
		double wsum = 0;
		for (int i = 0; i < n_warm; i++)
			wsum += imp_warm[i];
		printf("   AHB import: %d cold imports, mean %.3f ms each; %d cached lookups, "
		       "mean %.4f ms\n",
		       n_cold, n_cold ? sum / n_cold : 0, n_warm, n_warm ? wsum / n_warm : 0);
	}

	stop_flag = 1;
	pthread_join(dt, NULL);
	AMediaCodec_stop(codec);
	AMediaCodec_delete(codec);
	AImageReader_delete(reader);
	return 0;
}
