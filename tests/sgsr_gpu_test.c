/* Headless SGSR FS constant-HDR/readback check. No surface, game or app files. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "sgsr2_params.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define FUNCTIONS(X) \
 X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceQueueFamilyProperties) \
 X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceProperties) X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) \
 X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(QueueSubmit) X(QueueWaitIdle) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) \
 X(FreeMemory) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer) \
 X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateShaderModule) X(DestroyShaderModule) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateSampler) X(DestroySampler) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdPipelineBarrier) X(CmdClearColorImage) X(CmdBeginRendering) \
 X(CmdEndRendering) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) X(CmdCopyImageToBuffer) X(CmdCopyBufferToImage)
#define DECLARE(name) static PFN_vk##name vk##name;
FUNCTIONS(DECLARE)
#define CHECK(call) do { VkResult result_ = (call); if (result_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d at %d\n", #call, result_, __LINE__); exit(1); } } while (0)
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fprintf(stderr, "required memory type absent\n"); exit(2);
}
static VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags) {
    VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size; info.memoryTypeIndex = memory_type(requirements.memoryTypeBits, flags);
    VkDeviceMemory result; CHECK(vkAllocateMemory(device, &info, NULL, &result)); return result;
}
typedef struct { VkImage image; VkImageView view; VkDeviceMemory memory; } Image;
static Image image_new(uint32_t w, uint32_t h) {
    Image image = {0};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = (VkExtent3D){w, h, 1}; ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(vkCreateImage(device, &ci, NULL, &image.image));
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, image.image, &requirements);
    image.memory = allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkBindImageMemory(device, image.image, image.memory, 0));
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = ci.format;
    view.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(vkCreateImageView(device, &view, NULL, &image.view)); return image;
}
typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; } Buffer;
static Buffer buffer_new(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer buffer = {0}; VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage;
    CHECK(vkCreateBuffer(device, &ci, NULL, &buffer.buffer));
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    buffer.memory = allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0));
    CHECK(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped)); return buffer;
}
static VkShaderModule shader(const char *path) {
    FILE *file = fopen(path, "rb"); if (!file) exit(2);
    fseek(file, 0, SEEK_END); long n = ftell(file); rewind(file); if (n <= 0 || n % 4) exit(2);
    uint32_t *bytes = malloc((size_t)n); if (!bytes || fread(bytes, 1, (size_t)n, file) != (size_t)n) exit(2); fclose(file);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = (size_t)n; ci.pCode = bytes;
    VkShaderModule module; CHECK(vkCreateShaderModule(device, &ci, NULL, &module)); free(bytes); return module;
}
static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static float half_float(uint16_t bits) {
    int e = (bits >> 10) & 31; float f = (float)(bits & 1023);
    float value = e == 31 ? (f ? NAN : INFINITY) : e ? ldexpf(1.0f + f / 1024.0f, e - 15) : ldexpf(f, -24);
    return bits & 32768 ? -value : value;
}
static uint16_t float_half(float value) {
    if (value == 0) return 0;
    int exponent; float fraction = frexpf(value, &exponent);
    int e = exponent + 14; unsigned mantissa = (unsigned)lroundf((fraction * 2 - 1) * 1024);
    if (mantissa == 1024) { mantissa = 0; e++; }
    return (uint16_t)(((unsigned)e << 10) | mantissa);
}
static float pattern_value(int x, int y, int channel) {
    return channel == 0 ? (x + 1) * 0.25f : channel == 1 ? (y + 1) * 0.25f : ((x + y) % 3) * 0.5f;
}
static float pattern_reference(int x, int y, int w, int h, int ow, int oh, int channel) {
    float px = (x + 0.5f) * w / ow, py = (y + 0.5f) * h / oh;
    int sx = (int)px, sy = (int)py;
    const int offsets[5][2] = {{0, 1}, {1, 0}, {-1, 0}, {0, 0}, {0, -1}};
    float sum = 0, weights = 0, lo = INFINITY, hi = -INFINITY;
    float bias = 0.5f * ow / w;
    for (int i = 0; i < 5; i++) {
        int ix = sx + offsets[i][0], iy = sy + offsets[i][1];
        if (ix < 0) ix = 0;
        if (ix >= w) ix = w - 1;
        if (iy < 0) iy = 0;
        if (iy >= h) iy = h - 1;
        float value = pattern_value(ix, iy, channel);
        float dx = sx + 0.5f - px + offsets[i][0], dy = sy + 0.5f - py + offsets[i][1];
        float base = fminf(1, (dx * dx + dy * dy) * bias * bias);
        float a = base - 1, a2 = a * a, weight = (0.75f * a + a2) * a2;
        sum += value * weight; weights += weight; lo = fminf(lo, value); hi = fmaxf(hi, value);
    }
    return fminf(hi + 0.075f, fmaxf(lo - 0.075f, sum / weights));
}
int main(int argc, char **argv) {
    if (argc != 4 && argc != 9 && argc != 10 && argc != 11 && argc != 12) { fprintf(stderr, "vertex.spv convert.spv upscale.spv [w h ow oh intensity [reset [pattern [historyIntensity]]]] required\n"); return 2; }
    int W = argc >= 9 ? atoi(argv[4]) : 17, H = argc >= 9 ? atoi(argv[5]) : 13;
    int OW = argc >= 9 ? atoi(argv[6]) : W, OH = argc >= 9 ? atoi(argv[7]) : H;
    float intensity = argc >= 9 ? (float)atof(argv[8]) : 4.0f;
    int reset = argc >= 10 ? atoi(argv[9]) : 1; if (reset != 0 && reset != 1) return 2;
    int pattern = argc >= 11 ? atoi(argv[10]) : 0;
    int camera_case = getenv("WT_SGSR_TEST_CAMERA") ? atoi(getenv("WT_SGSR_TEST_CAMERA")) : 0;
    if (camera_case < 0 || camera_case > 5 || (camera_case && pattern)) return 2;
    float history_intensity = argc == 12 ? (float)atof(argv[11]) : intensity;
    if (!isfinite(history_intensity) || history_intensity < 0 || history_intensity > 60000) return 2;
    if (pattern && (!reset || W > 100 || H > 100)) return 2;
    if (W < 1 || H < 1 || OW < W || OH < H || OW > 4096 || OH > 4096 || !isfinite(intensity) || intensity < 0 || intensity > 60000) return 2;
#ifdef _WIN32
    HMODULE library = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr get = library ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(library, "vkGetInstanceProcAddr") : NULL;
#else
    void *library = dlopen("libvulkan.so.1", RTLD_NOW);
    PFN_vkGetInstanceProcAddr get = library ? (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr") : NULL;
#endif
    if (!get) { fprintf(stderr, "Vulkan loader unavailable\n"); return 2; }
    vkCreateInstance = (PFN_vkCreateInstance)get(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
#define LOAD(name) if (!vk##name) vk##name = (PFN_vk##name)get(instance, "vk" #name); if (!vk##name) { fprintf(stderr, "missing vk%s\n", #name); return 2; }
    FUNCTIONS(LOAD)
    uint32_t count = 0; CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL)); if (!count) return 2;
    VkPhysicalDevice *gpus = calloc(count, sizeof *gpus); CHECK(vkEnumeratePhysicalDevices(instance, &count, gpus)); VkPhysicalDevice gpu = gpus[0]; free(gpus);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(gpu, &properties); printf("GPU: %s\n", properties.deviceName);
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof *families); vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    uint32_t family; for (family = 0; family < count; family++) if (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    free(families); if (family == count) return 2;
    float priority = 1; VkDeviceQueueCreateInfo queue_ci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_ci.queueFamilyIndex = family; queue_ci.queueCount = 1; queue_ci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &features; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &queue_ci;
    CHECK(vkCreateDevice(gpu, &dci, NULL, &device)); VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = family;
    VkCommandPool pool; CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    enum { IMAGES = 6 }; Image images[IMAGES];
    for (int i = 0; i < IMAGES; i++) images[i] = image_new(i == 3 || i == 5 ? (uint32_t)OW : (uint32_t)W, i == 3 || i == 5 ? (uint32_t)OH : (uint32_t)H);
    Buffer uniform = buffer_new(sizeof(Sgsr2FsParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    Buffer readback = buffer_new((VkDeviceSize)OW * OH * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer upload = buffer_new((VkDeviceSize)W * H * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Buffer motion_readback = buffer_new((VkDeviceSize)W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memset(motion_readback.mapped, 0xff, (size_t)W * H * 8);
    uint16_t *input = upload.mapped;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 4; c++)
        input[(y * W + x) * 4 + c] = c < 3 ? float_half(pattern_value(x, y, c)) : 0;
    memset(readback.mapped, 0xff, (size_t)OW * OH * 8);
    Sgsr2FsParams params = {0}; for (int i = 0; i < 16; i++) params.clip_to_prev_clip[i] = i % 5 == 0;
    params.render_size[0] = (float)W; params.render_size[1] = (float)H; params.output_size[0] = (float)OW; params.output_size[1] = (float)OH;
    params.render_size_rcp[0] = 1.0f / W; params.render_size_rcp[1] = 1.0f / H;
    params.output_size_rcp[0] = 1.0f / OW; params.output_size_rcp[1] = 1.0f / OH;
    params.scale_ratio[0] = (float)OW / W;
    float area = (float)OW * OH / ((float)W * H); params.scale_ratio[1] = fminf(20.0f, area * area * area);
    params.camera_fov_angle_hor = 1; params.reset = (float)reset;
    if (camera_case) {
        params.clip_to_prev_clip[0] = 1.1f;
        params.clip_to_prev_clip[5] = 0.9f;
        params.clip_to_prev_clip[8] = 0.2f;
        params.clip_to_prev_clip[9] = -0.1f;
        params.clip_to_prev_clip[12] = 0.05f;
        params.clip_to_prev_clip[13] = -0.03f;
        params.jitter_offset[0] = camera_case == 1 ? 0.375f : -0.125f;
        params.jitter_offset[1] = camera_case == 1 ? -0.25f : 0.375f;
        if (camera_case == 5) {
            params.clip_to_prev_clip[3] = 0.15f;
            params.clip_to_prev_clip[7] = -0.08f;
            params.clip_to_prev_clip[15] = 1.2f;
        }
    }
    memcpy(uniform.mapped, &params, sizeof params);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler; CHECK(vkCreateSampler(device, &sci, NULL, &sampler));
    VkShaderModule modules[3] = {shader(argv[1]), shader(argv[2]), shader(argv[3])};
    VkDescriptorSetLayout layouts[2]; VkPipelineLayout pipeline_layouts[2]; VkPipeline pipelines[2]; VkDescriptorSet sets[3];
    for (int pass = 0; pass < 2; pass++) {
        VkDescriptorSetLayoutBinding binding[4] = {0}; uint32_t n = pass ? 4 : 3;
        for (uint32_t i = 0; i < n; i++) { binding[i].binding = i; binding[i].descriptorCount = 1; binding[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; binding[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; }
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = n; li.pBindings = binding;
        CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &layouts[pass]));
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &layouts[pass];
        CHECK(vkCreatePipelineLayout(device, &pli, NULL, &pipeline_layouts[pass]));
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = modules[0]; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = modules[pass + 1]; stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba = {0}; ba.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &ba;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dyn.dynamicStateCount = 2; dyn.pDynamicStates = states;
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; ci.pNext = &ri; ci.stageCount = 2; ci.pStages = stages;
        ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &blend; ci.pDynamicState = &dyn; ci.layout = pipeline_layouts[pass];
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipelines[pass]));
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 3; dpi.poolSizeCount = 2; dpi.pPoolSizes = sizes;
    VkDescriptorPool descriptors; CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &descriptors));
    VkDescriptorSetLayout set_layouts[3] = {layouts[0], layouts[1], layouts[1]};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = descriptors; ai.descriptorSetCount = 3; ai.pSetLayouts = set_layouts;
    CHECK(vkAllocateDescriptorSets(device, &ai, sets));
    for (int pass = 0; pass < 3; pass++) {
        int indices[3] = {pass == 2 ? 5 : pass ? 3 : 0, pass ? 4 : 1, 2}; int n = pass ? 4 : 3;
        VkDescriptorBufferInfo bi = {uniform.buffer, 0, sizeof params}; VkDescriptorImageInfo ii[3]; VkWriteDescriptorSet writes[4] = {0};
        for (int i = 0; i < n; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = sets[pass]; writes[i].dstBinding = (uint32_t)i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (i) { ii[i - 1] = (VkDescriptorImageInfo){sampler, images[indices[i - 1]].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; writes[i].pImageInfo = &ii[i - 1]; } else writes[i].pBufferInfo = &bi;
        }
        vkUpdateDescriptorSets(device, (uint32_t)n, writes, 0, NULL);
    }
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
    for (int i = 0; i < IMAGES; i++) {
        barrier(cmd, images[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue color = {{0, 0, 0, 0}};
        if (i == 1 && (camera_case == 3 || camera_case == 4)) {
            color.float32[0] = camera_case == 3 ? 0.5f : 0.5625f;
            color.float32[1] = camera_case == 3 ? 0.5f : 0.4375f;
        }
        if (i == 0) color.float32[0] = camera_case == 5 ? 0.5f : 1;
        if (i == 2 || (i == 3 && !reset)) {
            float value = i == 2 ? intensity : history_intensity;
            color.float32[0] = value; color.float32[1] = value * 0.5f; color.float32[2] = value * 0.25f;
        }
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        if (pattern && i == 2) {
            VkBufferImageCopy input_copy = {0}; input_copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            input_copy.imageExtent = (VkExtent3D){(uint32_t)W, (uint32_t)H, 1};
            VkMemoryBarrier transfer = {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; transfer.srcAccessMask = transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &transfer, 0, NULL, 0, NULL);
            vkCmdCopyBufferToImage(cmd, upload.buffer, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &input_copy);
        }
        barrier(cmd, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (int pass = 0; pass < 3; pass++) {
        int target = pass == 2 ? 3 : pass ? 5 : 4;
        uint32_t tw = (uint32_t)(pass ? OW : W), th = (uint32_t)(pass ? OH : H);
        if (pass == 2) {
            /* History was consumed by the first Upscale; poison its destination before reusing it. */
            barrier(cmd, images[target].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue poison = {{NAN, NAN, NAN, NAN}};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, images[target].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &poison, 1, &range);
            barrier(cmd, images[target].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        barrier(cmd, images[target].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = images[target].view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = pass == 2 ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea.extent = (VkExtent2D){tw, th}; ri.layerCount = ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
        VkViewport viewport = {0, 0, (float)tw, (float)th, 0, 1}; VkRect2D scissor = {{0, 0}, {tw, th}};
        vkCmdBeginRendering(cmd, &ri); vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[pass ? 1 : 0]);
        vkCmdSetViewport(cmd, 0, 1, &viewport); vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layouts[pass ? 1 : 0], 0, 1, &sets[pass], 0, NULL);
        if (pass != 2 || !getenv("WT_SGSR_TEST_SKIP_FINAL")) vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        barrier(cmd, images[target].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    barrier(cmd, images[3].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy = {0}; copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = (VkExtent3D){(uint32_t)OW, (uint32_t)OH, 1};
    vkCmdCopyImageToBuffer(cmd, images[3].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    if (camera_case) {
        barrier(cmd, images[4].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        copy.imageExtent = (VkExtent3D){(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyImageToBuffer(cmd, images[4].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, motion_readback.buffer, 1, &copy);
    }
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    int failed = 0; const uint16_t *pixels = readback.mapped; float expected[3] = {intensity, intensity * 0.5f, intensity * 0.25f};
    for (int i = 0; i < OW * OH; i++) for (int c = 0; c < 3; c++) {
        float value = half_float(pixels[i * 4 + c]);
        float reference = pattern ? pattern_reference(i % OW, i / OW, W, H, OW, OH, c) : expected[c];
        if (!isfinite(value) || fabsf(value - reference) > fmaxf(0.02f, fabsf(reference) * 0.002f)) { if (!failed) fprintf(stderr, "pixel=%d channel=%d value=%g expected=%g\n", i, c, value, reference); failed++; }
    }
    if (camera_case) {
        const uint16_t *motion = motion_readback.mapped;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            float sx = 2.0f * (x + 0.5f) / W - 1.0f - 2.0f * params.jitter_offset[0] / W;
            float sy = 1.0f - 2.0f * (y + 0.5f) / H + 2.0f * params.jitter_offset[1] / H;
            float depth = camera_case == 5 ? 0.5f : 1.0f;
            float previous_w = camera_case == 5 ? 0.15f * sx - 0.08f * sy + 1.2f : 1.0f;
            float reference[4] = {sx - (1.1f * sx + 0.2f * depth + 0.05f) / previous_w,
                                  sy - (0.9f * sy - 0.1f * depth - 0.03f) / previous_w, 0, 0};
            if (camera_case == 3 || camera_case == 4) {
                reference[0] = ((camera_case == 3 ? 0.5f : 0.5625f) - 32767.0f / 65535.0f) / 0.2495f;
                reference[1] = ((camera_case == 3 ? 0.5f : 0.4375f) - 32767.0f / 65535.0f) / 0.2495f;
            }
            for (int c = 0; c < 4; c++) {
                float value = half_float(motion[(y * W + x) * 4 + c]);
                if (!isfinite(value) || fabsf(value - reference[c]) > (camera_case == 3 ? 0.000001f : 0.001f)) {
                    if (!failed) fprintf(stderr, "camera=%d motion(%d,%d)[%d]=%g expected=%g\n", camera_case, x, y, c, value, reference[c]);
                    failed++;
                }
            }
        }
        printf("SGSR Convert nonidentity+jitter case=%d analytic readback: %s\n", camera_case, failed ? "FAIL" : "PASS");
    }
    vkDestroyDescriptorPool(device, descriptors, NULL); vkDestroySampler(device, sampler, NULL);
    for (int i = 0; i < 2; i++) { vkDestroyPipeline(device, pipelines[i], NULL); vkDestroyPipelineLayout(device, pipeline_layouts[i], NULL); vkDestroyDescriptorSetLayout(device, layouts[i], NULL); }
    for (int i = 0; i < 3; i++) vkDestroyShaderModule(device, modules[i], NULL);
    for (int i = 0; i < IMAGES; i++) { vkDestroyImageView(device, images[i].view, NULL); vkDestroyImage(device, images[i].image, NULL); vkFreeMemory(device, images[i].memory, NULL); }
    Buffer buffers[] = {uniform, readback, upload, motion_readback}; for (int i = 0; i < 4; i++) { vkUnmapMemory(device, buffers[i].memory); vkDestroyBuffer(device, buffers[i].buffer, NULL); vkFreeMemory(device, buffers[i].memory, NULL); }
    vkDestroyCommandPool(device, pool, NULL); vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    printf("SGSR FS %dx%d -> %dx%d intensity=%g history=%g reset=%d pattern=%d ping-pong readback: %s (%d failed channels)\n", W, H, OW, OH, intensity, history_intensity, reset, pattern, failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
