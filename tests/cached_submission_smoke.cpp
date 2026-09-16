// Host-only Vulkan API lifecycle smoke for cached presentation submissions.
// This models the synchronization sequence; it does not run the OpenXR scene.
// Build: c++ -std=c++17 -O2 -I <Vulkan-Headers/include> tests/cached_submission_smoke.cpp -lvulkan -o cached_submission_smoke
// Run without NDEBUG so assertions remain active. No window or headset is used.
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int validation_errors = 0;
static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        std::fprintf(stderr, "validation error: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static void ok(VkResult r, const char *what)
{
    if (r != VK_SUCCESS) { std::fprintf(stderr, "%s: %d\n", what, int(r)); std::abort(); }
}

int main()
{
    uint32_t layer_count = 0;
    ok(vkEnumerateInstanceLayerProperties(&layer_count, nullptr), "layers");
    std::vector<VkLayerProperties> layers(layer_count);
    ok(vkEnumerateInstanceLayerProperties(&layer_count, layers.data()), "layers");
    bool validation = false;
    for (const auto &layer : layers)
        validation |= std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;

    std::vector<const char *> enabled_layers;
    if (validation) enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "headroom-submit-smoke", 1,
                          "headroom-submit-smoke", 1, VK_API_VERSION_1_0};
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = report;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = uint32_t(enabled_layers.size());
    ici.ppEnabledLayerNames = enabled_layers.data();
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = extensions;
    ici.pNext = &debug;
    VkInstance instance{};
    ok(vkCreateInstance(&ici, nullptr, &instance), "instance");
    auto create_debug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroy_debug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger{};
    if (create_debug) ok(create_debug(instance, &debug, nullptr, &messenger), "debug messenger");

    uint32_t physical_count = 0;
    ok(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr), "physical devices");
    std::vector<VkPhysicalDevice> physical(physical_count);
    ok(vkEnumeratePhysicalDevices(instance, &physical_count, physical.data()), "physical devices");
    assert(!physical.empty());
    VkPhysicalDevice gpu = physical.front();
    VkPhysicalDeviceProperties gpu_info{}; vkGetPhysicalDeviceProperties(gpu, &gpu_info);
    std::printf("device=%s\n", gpu_info.deviceName);
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    assert(family < family_count && families[family].timestampValidBits > 0);
    float priority = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice device{}; ok(vkCreateDevice(gpu, &dci, nullptr, &device), "device");
    VkQueue queue{}; vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = family;
    VkCommandPool pool{}; ok(vkCreateCommandPool(device, &pci, nullptr, &pool), "pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd{}; ok(vkAllocateCommandBuffers(device, &cai, &cmd), "command buffer");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence{}; ok(vkCreateFence(device, &fci, nullptr, &fence), "fence");
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP; qpci.queryCount = 2;
    VkQueryPool queries{}; ok(vkCreateQueryPool(device, &qpci, nullptr, &queries), "queries");

    VkImageCreateInfo ici1{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici1.imageType = VK_IMAGE_TYPE_2D; ici1.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici1.extent = {1, 1, 1}; ici1.mipLevels = 1; ici1.arrayLayers = 1;
    ici1.samples = VK_SAMPLE_COUNT_1_BIT; ici1.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici1.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici1.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image{}; ok(vkCreateImage(device, &ici1, nullptr, &image), "image");
    VkMemoryRequirements image_req{}; vkGetImageMemoryRequirements(device, image, &image_req);
    VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    uint32_t image_type = 0;
    while (image_type < memory.memoryTypeCount && (!(image_req.memoryTypeBits & (1u << image_type)) ||
           !(memory.memoryTypes[image_type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) ++image_type;
    assert(image_type < memory.memoryTypeCount);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = image_req.size; mai.memoryTypeIndex = image_type;
    VkDeviceMemory image_memory{}; ok(vkAllocateMemory(device, &mai, nullptr, &image_memory), "image memory");
    ok(vkBindImageMemory(device, image, image_memory, 0), "bind image");

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sizeof(uint32_t); bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer sentinel{}; ok(vkCreateBuffer(device, &bci, nullptr, &sentinel), "sentinel");
    VkMemoryRequirements buffer_req{}; vkGetBufferMemoryRequirements(device, sentinel, &buffer_req);
    uint32_t buffer_type = 0;
    while (buffer_type < memory.memoryTypeCount && (!(buffer_req.memoryTypeBits & (1u << buffer_type)) ||
           !(memory.memoryTypes[buffer_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
           !(memory.memoryTypes[buffer_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) ++buffer_type;
    assert(buffer_type < memory.memoryTypeCount);
    mai.allocationSize = buffer_req.size; mai.memoryTypeIndex = buffer_type;
    VkDeviceMemory buffer_memory{}; ok(vkAllocateMemory(device, &mai, nullptr, &buffer_memory), "buffer memory");
    ok(vkBindBufferMemory(device, sentinel, buffer_memory, 0), "bind sentinel");
    uint32_t *mapped{}; ok(vkMapMemory(device, buffer_memory, 0, sizeof(*mapped), 0, reinterpret_cast<void **>(&mapped)), "map");

    bool query_filled = false, image_transition = false;
    uint32_t expected = 0; *mapped = expected;
    constexpr int iterations = 48;
    int submissions = 0, skipped = 0, query_reads = 0;
    for (uint32_t i = 0; i < iterations; ++i) {
        ok(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait fence");
        if (query_filled) {
            uint64_t values[2]{};
            ok(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(values), values, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "query");
            query_filled = false; ++query_reads;
        }
        const bool cache_hit = i % 4 != 0;
        ok(vkResetCommandBuffer(cmd, 0), "reset command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ok(vkBeginCommandBuffer(cmd, &begin), "begin command");
        vkCmdResetQueryPool(cmd, queries, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        image_transition = i % 4 == 2; // A cached image can still require a first-use transition.
        if (image_transition) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.image = image;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1; barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &barrier);
        }
        if (!cache_hit) {
            expected = 0xA5000000u | i;
            vkCmdFillBuffer(cmd, sentinel, 0, sizeof(uint32_t), expected);
            VkMemoryBarrier host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host_read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &host_read, 0, nullptr, 0, nullptr);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        ok(vkEndCommandBuffer(cmd), "end command");
        if (!cache_hit || image_transition) {
            ok(vkResetFences(device, 1, &fence), "reset fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
            ok(vkQueueSubmit(queue, 1, &submit, fence), "submit");
            query_filled = true; ++submissions;
        } else {
            assert(vkGetFenceStatus(device, fence) == VK_SUCCESS);
            assert(!query_filled);
            ++skipped;
        }
        ok(vkWaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000), "sentinel wait");
        assert(*mapped == expected);
    }
    ok(vkDeviceWaitIdle(device), "idle");
    std::printf("validation=%s errors=%d iterations=%d submissions=%d skipped=%d query_reads=%d sentinel=ok\n",
                validation ? "on" : "off", validation_errors, iterations, submissions, skipped, query_reads);
    assert(submissions == iterations / 2 && skipped == iterations / 2 && query_reads == submissions && validation_errors == 0);
    vkUnmapMemory(device, buffer_memory);
    vkDestroyBuffer(device, sentinel, nullptr); vkFreeMemory(device, buffer_memory, nullptr);
    vkDestroyImage(device, image, nullptr); vkFreeMemory(device, image_memory, nullptr);
    vkDestroyQueryPool(device, queries, nullptr); vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &cmd); vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    if (destroy_debug && messenger) destroy_debug(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
}
