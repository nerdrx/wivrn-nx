// test_base_patch -- host byte-exactness test for the base patch import.
//
// Runs base_patch.comp's three storage variants against the CPU model in
// base_patch_layout.h, on the host's Vulkan (RADV here; lavapipe works too).
//
// The base picture is a real VK_FORMAT_G8_B8R8_2PLANE_420_UNORM image, which is
// the format WiVRn's compositor actually produces (compositor.cpp:209), sampled
// through a VkSamplerYcbcrConversion set to RGB_IDENTITY + NEAREST + full
// range -- the same sampler configuration the Android path uses on the imported
// AHardwareBuffer. So this exercises everything about the kernel except the
// AHardwareBuffer import itself, and it needs no headset.
//
// What it checks:
//   1. all three variants reproduce the CPU model's atlas bit for bit;
//   2. all three agree with each other (same logical layout);
//   3. the per-tile table matches, all 64 bytes including the 20 reserved;
//   4. the swizzle push constant is honoured -- the test re-runs with a
//      deliberately permuted swizzle and requires the output to change, so a
//      kernel that ignored it could not pass.
//
// Exit status 0 = pass.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "base_patch_layout.h"

#define VKC(x)                                                                \
	do {                                                                      \
		VkResult r_ = (x);                                                    \
		if (r_ != VK_SUCCESS) {                                               \
			fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_);  \
			exit(2);                                                          \
		}                                                                     \
	} while (0)

#define EW 256           // per-eye width  (4 tiles)
#define EH 192           // per-eye height (3 tiles)
#define EYES 2

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static uint32_t qf;
static VkCommandPool cpool;
static VkCommandBuffer cb;
static VkSamplerYcbcrConversion conv;
static VkSampler samp;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout plyt;
static VkDescriptorPool dpool;
static VkDescriptorSet dset;

static nxbp_layout L;

struct pc_t {
	int32_t baseSize[2];
	int32_t nPatches;
	int32_t srcFrame;
	int32_t planeOff[4];
	int32_t planeStride[4];
	int32_t planeW[4];
	int32_t planeRowOff[4];
	int32_t swz;
	int32_t writeTable;
	int32_t pad0, pad1;
};

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(2);
}

struct buf {
	VkBuffer b;
	VkDeviceMemory m;
	void * p;
	VkDeviceSize sz;
};

static struct buf mkbuf(VkDeviceSize sz, VkBufferUsageFlags u)
{
	struct buf r = {.sz = sz};
	VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                         .size = sz, .usage = u,
	                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VKC(vkCreateBuffer(dev, &bc, NULL, &r.b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, r.b, &mr);
	VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                           .allocationSize = mr.size,
	                           .memoryTypeIndex = memtype(mr.memoryTypeBits,
	                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	VKC(vkAllocateMemory(dev, &ma, NULL, &r.m));
	VKC(vkBindBufferMemory(dev, r.b, r.m, 0));
	VKC(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p));
	memset(r.p, 0, sz);
	return r;
}

struct img {
	VkImage i;
	VkDeviceMemory m;
	VkImageView v;
};

static struct img mkimg(int w, int h, VkFormat f, VkImageUsageFlags u,
                        VkImageAspectFlags asp)
{
	struct img r = {0};
	VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                        .imageType = VK_IMAGE_TYPE_2D, .format = f,
	                        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
	                        .samples = VK_SAMPLE_COUNT_1_BIT,
	                        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u,
	                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	VKC(vkCreateImage(dev, &ic, NULL, &r.i));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, r.i, &mr);
	VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                           .allocationSize = mr.size,
	                           .memoryTypeIndex = memtype(mr.memoryTypeBits,
	                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VKC(vkAllocateMemory(dev, &ma, NULL, &r.m));
	VKC(vkBindImageMemory(dev, r.i, r.m, 0));
	VkSamplerYcbcrConversionInfo yi = {
	        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .conversion = conv};
	VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                            .pNext = (f == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) ? &yi : NULL,
	                            .image = r.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                            .format = f,
	                            .subresourceRange = {asp, 0, 1, 0, 1}};
	VKC(vkCreateImageView(dev, &vc, NULL, &r.v));
	return r;
}

static VkCommandBuffer begin(void)
{
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	vkResetCommandBuffer(cb, 0);
	VKC(vkBeginCommandBuffer(cb, &bi));
	return cb;
}

static void submit(void)
{
	VKC(vkEndCommandBuffer(cb));
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	                   .commandBufferCount = 1, .pCommandBuffers = &cb};
	VKC(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	VKC(vkQueueWaitIdle(q));
}

static void barrier(VkImage im, VkImageAspectFlags asp, VkImageLayout o,
                    VkImageLayout n, VkAccessFlags sa, VkAccessFlags da)
{
	VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	                          .srcAccessMask = sa, .dstAccessMask = da,
	                          .oldLayout = o, .newLayout = n,
	                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .image = im,
	                          .subresourceRange = {asp, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkPipeline mkpipe(const char * path)
{
	FILE * f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "open %s\n", path); exit(2); }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint32_t * code = malloc(n);
	if (fread(code, 1, n, f) != (size_t)n) exit(2);
	fclose(f);
	VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                              .codeSize = n, .pCode = code};
	VkShaderModule mod;
	VKC(vkCreateShaderModule(dev, &sm, NULL, &mod));
	VkComputePipelineCreateInfo ci = {
	        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"},
	        .layout = plyt};
	VkPipeline p;
	VKC(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
	vkDestroyShaderModule(dev, mod, NULL);
	free(code);
	return p;
}

int main(int argc, char ** argv)
{
	const char * spvdir = argc > 1 ? argv[1] : "build";

	// ---- device
	VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	                        .pApplicationName = "test_base_patch",
	                        .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	                            .pApplicationInfo = &ai};
	VKC(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	vkEnumeratePhysicalDevices(inst, &n, pds);
	pd = pds[0];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	uint32_t qn = 8;
	VkQueueFamilyProperties qs[8];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs);
	for (uint32_t i = 0; i < qn; i++)
		if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
	float prio = 1.f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                               .queueFamilyIndex = qf, .queueCount = 1,
	                               .pQueuePriorities = &prio};
	VkPhysicalDeviceSamplerYcbcrConversionFeatures yf = {
	        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
	        .samplerYcbcrConversion = VK_TRUE};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	                          .pNext = &yf, .queueCreateInfoCount = 1,
	                          .pQueueCreateInfos = &qci};
	VKC(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, qf, 0, &q);
	printf("device: %s\n", props.deviceName);

	VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                              .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                              .queueFamilyIndex = qf};
	VKC(vkCreateCommandPool(dev, &cp, NULL, &cpool));
	VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                   .commandPool = cpool,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                   .commandBufferCount = 1};
	VKC(vkAllocateCommandBuffers(dev, &cba, &cb));

	// ---- the ycbcr conversion: identical configuration to the Android path
	VkSamplerYcbcrConversionCreateInfo cc = {
	        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
	        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
	        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
	        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
	        .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
	        .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
	        .chromaFilter = VK_FILTER_NEAREST,
	        .forceExplicitReconstruction = VK_FALSE};
	VKC(vkCreateSamplerYcbcrConversion(dev, &cc, NULL, &conv));
	VkSamplerYcbcrConversionInfo yi = {
	        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .conversion = conv};
	VkSamplerCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	                          .pNext = &yi,
	                          .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
	                          .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	                          .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                          .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                          .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                          .unnormalizedCoordinates = VK_FALSE};
	VKC(vkCreateSampler(dev, &sc, NULL, &samp));

	// ---- layout
	nxbp_layout_init(&L, EW, EH, EW / 2, EH / 2, EYES, 3);
	printf("atlas: %d u16 (%d planes), image %dx%d, grid %dx%d per eye, cols %d\n",
	       L.slot_u16, L.nplanes, L.imageW, L.imageH, L.cols_per_eye, L.rows, L.cols);

	// ---- a synthetic base picture, per-eye geometry, deterministic
	uint8_t * by = malloc((size_t)EW * EH);
	uint8_t * bcb = malloc((size_t)(EW / 2) * (EH / 2));
	uint8_t * bcr = malloc((size_t)(EW / 2) * (EH / 2));
	for (int y = 0; y < EH; y++)
		for (int x = 0; x < EW; x++)
			by[(size_t)y * EW + x] = (uint8_t)((x * 7 + y * 13 + ((x ^ y) & 31)) & 0xff);
	for (int y = 0; y < EH / 2; y++)
		for (int x = 0; x < EW / 2; x++)
		{
			bcb[(size_t)y * (EW / 2) + x] = (uint8_t)((x * 3 + y * 5 + 40) & 0xff);
			bcr[(size_t)y * (EW / 2) + x] = (uint8_t)((x * 11 + y * 2 + 90) & 0xff);
		}

	// upload into a real 2-plane 4:2:0 image, the compositor's own format
	struct img base = mkimg(EW, EH, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                        VK_IMAGE_ASPECT_COLOR_BIT);
	struct buf stage = mkbuf((VkDeviceSize)EW * EH * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	{
		uint8_t * s = stage.p;
		memcpy(s, by, (size_t)EW * EH);
		uint8_t * uv = s + (size_t)EW * EH;
		for (int i = 0; i < (EW / 2) * (EH / 2); i++)
		{
			uv[2 * i] = bcb[i];      // plane 1 is CbCr interleaved
			uv[2 * i + 1] = bcr[i];
		}
		begin();
		barrier(base.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkBufferImageCopy c[2] = {
		        {.bufferOffset = 0,
		         .imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
		         .imageExtent = {EW, EH, 1}},
		        {.bufferOffset = (VkDeviceSize)EW * EH,
		         .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
		         .imageExtent = {EW / 2, EH / 2, 1}},
		};
		vkCmdCopyBufferToImage(cb, stage.b, base.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, c);
		barrier(base.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		        VK_ACCESS_SHADER_READ_BIT);
		submit();
	}

	// ---- atlas targets
	const VkDeviceSize atlasBytes = (VkDeviceSize)L.slot_u16 * 2;
	struct buf atlasSSBO = mkbuf(atlasBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
	                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	struct img atlas16 = mkimg(L.imageW, L.imageH, VK_FORMAT_R16_UINT,
	                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                           VK_IMAGE_ASPECT_COLOR_BIT);
	struct img atlas8 = mkimg(L.imageW, L.imageH, VK_FORMAT_R8_UNORM,
	                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                          VK_IMAGE_ASPECT_COLOR_BIT);
	const int ntiles = L.cols * L.rows;
	struct buf tableBuf = mkbuf((VkDeviceSize)ntiles * NXBP_TABLE_BYTES,
	                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	// every tile of both eyes
	struct buf patchBuf = mkbuf((VkDeviceSize)ntiles * sizeof(nxbp_patch),
	                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	nxbp_patch * pl = patchBuf.p;
	int npatch = 0;
	for (int e = 0; e < EYES; e++)
		for (int r = 0; r < L.rows; r++)
			for (int c = 0; c < L.cols_per_eye; c++)
				pl[npatch++] = (nxbp_patch){c * NXBP_TILE, r * NXBP_TILE,
				                            nxbp_tile_index(&L, e, c, r), e};
	struct buf readback = mkbuf((VkDeviceSize)L.imageW * L.imageH * 2,
	                            VK_BUFFER_USAGE_TRANSFER_DST_BIT);

	// ---- one descriptor layout for all three variants
	VkDescriptorSetLayoutBinding b[6] = {
	        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &samp},
	        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	};
	VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	                                      .bindingCount = 6, .pBindings = b};
	VKC(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl));
	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct pc_t)};
	VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                  .setLayoutCount = 1, .pSetLayouts = &dsl,
	                                  .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
	VKC(vkCreatePipelineLayout(dev, &pli, NULL, &plyt));
	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}};
	VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	                                 .maxSets = 4, .poolSizeCount = 3, .pPoolSizes = ps};
	VKC(vkCreateDescriptorPool(dev, &dp, NULL, &dpool));
	VkDescriptorSetAllocateInfo da = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	                                  .descriptorPool = dpool, .descriptorSetCount = 1,
	                                  .pSetLayouts = &dsl};
	VKC(vkAllocateDescriptorSets(dev, &da, &dset));
	{
		VkDescriptorImageInfo i0 = {samp, base.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorImageInfo i2 = {VK_NULL_HANDLE, atlas16.v, VK_IMAGE_LAYOUT_GENERAL};
		VkDescriptorImageInfo i3 = {VK_NULL_HANDLE, atlas8.v, VK_IMAGE_LAYOUT_GENERAL};
		VkDescriptorBufferInfo b1 = {atlasSSBO.b, 0, VK_WHOLE_SIZE};
		VkDescriptorBufferInfo b4 = {patchBuf.b, 0, VK_WHOLE_SIZE};
		VkDescriptorBufferInfo b5 = {tableBuf.b, 0, VK_WHOLE_SIZE};
		VkWriteDescriptorSet w[6] = {
		    {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=dset,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&i0},
		    {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=dset,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&b1},
		    {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=dset,.dstBinding=2,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&i2},
		    {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=dset,.dstBinding=3,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&i3},
		    {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=dset,.dstBinding=4,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&b4},
		    {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=dset,.dstBinding=5,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&b5},
		};
		vkUpdateDescriptorSets(dev, 6, w, 0, NULL);
	}

	char path[512];
	VkPipeline pipes[3];
	for (int s = 0; s < 3; s++)
	{
		snprintf(path, sizeof path, "%s/base_patch_s%d.spv", spvdir, s);
		pipes[s] = mkpipe(path);
	}

	// ---- the CPU model
	uint16_t * ref = calloc(L.slot_u16, 2);
	uint32_t * reftbl = calloc((size_t)ntiles, NXBP_TABLE_BYTES);
	nxbp_base_planes bp = {by, bcb, bcr, EW, EW / 2};
	const uint32_t SRC_FRAME = 0x1234;
	nxbp_apply_cpu(&L, &bp, pl, npatch, SRC_FRAME, ref, reftbl);

	struct pc_t pc = {0};
	pc.baseSize[0] = EW; pc.baseSize[1] = EH;
	pc.nPatches = npatch;
	pc.srcFrame = SRC_FRAME;
	for (int i = 0; i < 4; i++)
	{
		pc.planeOff[i] = L.off[i];
		pc.planeStride[i] = L.stride[i];
		pc.planeW[i] = L.planeW[i];
		pc.planeRowOff[i] = L.rowOff[i];
	}
	// The channel mapping is a property of the FORMAT, not of the driver:
	// G8_B8R8_2PLANE_420_UNORM puts luma in G, Cb in B and Cr in R, so a
	// RGB_IDENTITY sampler returns .r/.g/.b = Cr/Y/Cb. Measured identically on
	// RADV here and on the Pico 4's Adreno 650 through an AHardwareBuffer's
	// external format, which is why it is normative rather than a vendor quirk.
	pc.swz = (1 << 0) | (2 << 2) | (0 << 4);   // Y = .g, Cb = .b, Cr = .r
	pc.writeTable = 1;

	int fails = 0;
	uint16_t * got = malloc((size_t)L.slot_u16 * 2);

	for (int s = 0; s < 3; s++)
	{
		memset(atlasSSBO.p, 0, atlasBytes);
		memset(tableBuf.p, 0, (size_t)ntiles * NXBP_TABLE_BYTES);
		begin();
		barrier(atlas16.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		        VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
		barrier(atlas8.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		        VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[s]);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plyt, 0, 1, &dset, 0, NULL);
		vkCmdPushConstants(cb, plyt, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
		vkCmdDispatch(cb, NXBP_TILE / 2 / 16, NXBP_TILE / 16, npatch);
		if (s > 0)
		{
			VkImage src = s == 1 ? atlas16.i : atlas8.i;
			barrier(src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
			        VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
			        VK_ACCESS_TRANSFER_READ_BIT);
			VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			                       .imageExtent = {L.imageW, L.imageH, 1}};
			vkCmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_GENERAL, readback.b, 1, &c);
		}
		submit();

		// bring the variant's output into the SSBO's logical layout
		memset(got, 0, (size_t)L.slot_u16 * 2);
		if (s == 0)
		{
			memcpy(got, atlasSSBO.p, (size_t)L.slot_u16 * 2);
		}
		else
		{
			for (int p = 0; p < L.nplanes; p++)
				for (int y = 0; y < L.planeH[p]; y++)
					for (int x = 0; x < L.stride[p]; x++)
					{
						uint32_t v;
						if (s == 1)
							v = ((uint16_t *)readback.p)[(size_t)(L.rowOff[p] + y) * L.imageW + x];
						else
							v = ((uint8_t *)readback.p)[(size_t)(L.rowOff[p] + y) * L.imageW + x];
						got[L.off[p] + (size_t)y * L.stride[p] + x] = (uint16_t)v;
					}
		}

		size_t bad = 0, firstbad = 0;
		for (int i = 0; i < L.slot_u16; i++)
			if (got[i] != ref[i]) { if (!bad) firstbad = i; bad++; }
		const int tbad = memcmp(tableBuf.p, reftbl, (size_t)ntiles * NXBP_TABLE_BYTES) != 0;
		printf("  STORE=%d  pixels %s (%zu/%d differ%s)   table %s\n", s,
		       bad ? "FAIL" : "ok", bad, L.slot_u16,
		       bad ? "" : "", tbad ? "FAIL" : "ok");
		if (bad)
		{
			printf("      first at u16 %zu: got %u want %u\n", firstbad,
			       got[firstbad], ref[firstbad]);
			fails++;
		}
		if (tbad) fails++;
	}

	// ---- the swizzle must actually be consumed
	{
		memset(atlasSSBO.p, 0, atlasBytes);
		struct pc_t p2 = pc;
		p2.swz = (0 << 0) | (2 << 2) | (1 << 4);   // Y<->Cr swapped
		begin();
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[0]);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plyt, 0, 1, &dset, 0, NULL);
		vkCmdPushConstants(cb, plyt, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p2, &p2);
		vkCmdDispatch(cb, NXBP_TILE / 2 / 16, NXBP_TILE / 16, npatch);
		submit();
		const int same = memcmp(atlasSSBO.p, ref, (size_t)L.slot_u16 * 2) == 0;
		printf("  swizzle is consumed: %s\n", same ? "FAIL (output unchanged)" : "ok");
		if (same) fails++;
	}

	printf("%s\n", fails ? "FAILED" : "PASS");
	return fails ? 1 : 0;
}
