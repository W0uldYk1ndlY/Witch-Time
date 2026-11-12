/* Reuse the existing disposable Vulkan loader/allocation helpers, not its FS test. */
#define main unused_fs_test_main
#include "sgsr_gpu_test.c"
#undef main
static PFN_vkCreateComputePipelines vkCreateComputePipelines;
static PFN_vkCmdDispatch vkCmdDispatch;

/* Independent constant-input oracle for the official packed-color/inverse-tone-map
 * equations. This checks integration fidelity, not HDR quality against input RGB. */
static void cs_constant_reference(float intensity, float expected[3]) {
    float rgb[3];
    for (int c = 0; c < 3; c++) rgb[c] = half_float(float_half(intensity / (float)(1 << c)));
    float color_max = rgb[0] + 1;
    for (int c = 0; c < 3; c++) rgb[c] /= color_max;
    float y = 0.25f * (rgb[0] + 2 * rgb[1] + rgb[2]);
    float co = 0.5f * rgb[0] + 0.5f - 0.5f * rgb[2], cg = y + co - rgb[0];
    y = (unsigned)(y * 2047.5f) / 2047.5f;
    co = (unsigned)(co * 2047.5f) / 2047.5f - 0.5f;
    cg = (unsigned)(cg * 1023.5f) / 1023.5f - 0.5f;
    expected[0] = fminf(1, fmaxf(0, y - cg + co));
    expected[1] = fminf(1, fmaxf(0, y + cg));
    expected[2] = fminf(1, fmaxf(0, y - cg - co));
    float maximum = fmaxf(expected[0], fmaxf(expected[1], expected[2]));
    float stored_max = half_float(float_half(color_max));
    float scale = stored_max > 4000 ? stored_max : 1 / ((1 + 600.0f / 65504.0f) - maximum);
    for (int c = 0; c < 3; c++) expected[c] *= scale;
}

static Image cs_image(uint32_t w, uint32_t h, VkFormat format) {
    Image im = {0};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(vkCreateImage(device, &ci, NULL, &im.image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, im.image, &req);
    im.memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkBindImageMemory(device, im.image, im.memory, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(vkCreateImageView(device, &vi, NULL, &im.view)); return im;
}

int main(int argc, char **argv) {
    if (argc != 8) return 2;
    int w = atoi(argv[3]), h = atoi(argv[4]), ow = atoi(argv[5]), oh = atoi(argv[6]);
    float intensity = (float)atof(argv[7]);
    if (w <= 0 || h <= 0 || ow < w || oh < h || ow > 4096 || oh > 4096 || !isfinite(intensity) || intensity < 0 || intensity > 60000) return 2;
#ifdef _WIN32
    HMODULE library = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr get = library ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(library, "vkGetInstanceProcAddr") : NULL;
#else
    void *library = dlopen("libvulkan.so.1", RTLD_NOW);
    PFN_vkGetInstanceProcAddr get = library ? (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr") : NULL;
#endif
    if (!get) return 2;
    vkCreateInstance = (PFN_vkCreateInstance)get(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    FUNCTIONS(LOAD)
    LOAD(CreateComputePipelines) LOAD(CmdDispatch)
    uint32_t count = 0; CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL)); if (!count) return 2;
    VkPhysicalDevice *gpus = calloc(count, sizeof *gpus); CHECK(vkEnumeratePhysicalDevices(instance, &count, gpus));
    VkPhysicalDevice gpu = gpus[0]; free(gpus);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(gpu, &properties);
    printf("GPU: %s\n", properties.deviceName); vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof *families); vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    uint32_t family; for (family = 0; family < count; family++) if (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
    free(families); if (family == count) return 2;
    float priority = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    CHECK(vkCreateDevice(gpu, &di, NULL, &device)); VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = family;
    VkCommandPool pool; CHECK(vkCreateCommandPool(device, &pi, NULL, &pool));
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    enum { COLOR, DEPTH, ZERO, MOTION, PACKED, HISTORY0, HISTORY1, OUTPUT, N };
    Image images[N];
    for (int i = 0; i < N; i++) images[i] = cs_image((uint32_t)(i >= HISTORY0 ? ow : w), (uint32_t)(i >= HISTORY0 ? oh : h), i == PACKED ? VK_FORMAT_R32_UINT : VK_FORMAT_R16G16B16A16_SFLOAT);
    Sgsr2CsParams params = {0};
    params.render_size[0] = (uint32_t)w; params.render_size[1] = (uint32_t)h;
    params.display_size[0] = (uint32_t)ow; params.display_size[1] = (uint32_t)oh;
    params.render_size_rcp[0] = 1.0f / w; params.render_size_rcp[1] = 1.0f / h;
    params.display_size_rcp[0] = 1.0f / ow; params.display_size_rcp[1] = 1.0f / oh;
    params.pre_exposure = params.camera_fov_angle_hor = 1;
    for (int i = 0; i < 16; i++) params.clip_to_prev_clip[i] = i % 5 == 0;
    Buffer uniform[3]; for (int f = 0; f < 3; f++) {
        uniform[f] = buffer_new(sizeof params, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        params.reset = f != 1; memcpy(uniform[f].mapped, &params, sizeof params);
    }
    Buffer readback[3]; for (int f = 0; f < 3; f++) readback[f] = buffer_new((VkDeviceSize)ow * oh * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.minFilter = sci.magFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler nearest, linear; CHECK(vkCreateSampler(device, &sci, NULL, &nearest));
    sci.minFilter = sci.magFilter = VK_FILTER_LINEAR; CHECK(vkCreateSampler(device, &sci, NULL, &linear));
    VkShaderModule modules[2] = {shader(argv[1]), shader(argv[2])};
    VkDescriptorSetLayout dl[2]; VkPipelineLayout pl[2]; VkPipeline pipelines[2];
    for (int p = 0; p < 2; p++) {
        VkDescriptorSetLayoutBinding bindings[6] = {0};
        for (int b = 0; b < 6; b++) {
            bindings[b].binding = !b ? 0 : b < 4 ? (uint32_t)(b + (p ? 6 : 0)) : (uint32_t)(b + 6);
            bindings[b].descriptorCount = 1; bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[b].descriptorType = !b ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : b < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 6; li.pBindings = bindings;
        CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &dl[p]));
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &dl[p];
        CHECK(vkCreatePipelineLayout(device, &pli, NULL, &pl[p]));
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout = pl[p];
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = modules[p]; ci.stage.pName = "main";
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipelines[p]));
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 18}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 6; dpi.poolSizeCount = 3; dpi.pPoolSizes = sizes;
    VkDescriptorPool descriptors; CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &descriptors));
    VkDescriptorSet sets[6]; VkDescriptorSetLayout layouts[] = {dl[0], dl[1], dl[0], dl[1], dl[0], dl[1]};
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sai.descriptorPool = descriptors; sai.descriptorSetCount = 6; sai.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(device, &sai, sets));
    for (int f = 0; f < 3; f++) for (int p = 0; p < 2; p++) {
        int previous = HISTORY0 + (f & 1), next = HISTORY0 + ((f & 1) ^ 1);
        int indices[5] = {p ? previous : COLOR, p ? MOTION : DEPTH, p ? PACKED : ZERO, p ? OUTPUT : MOTION, p ? next : PACKED};
        VkDescriptorBufferInfo bi = {uniform[f].buffer, 0, sizeof params};
        VkDescriptorImageInfo ii[5]; VkWriteDescriptorSet writes[6] = {0};
        for (int b = 0; b < 6; b++) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[b].dstSet = sets[f * 2 + p];
            writes[b].dstBinding = !b ? 0 : b < 4 ? (uint32_t)(b + (p ? 6 : 0)) : (uint32_t)(b + 6);
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = !b ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : b < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (!b) writes[b].pBufferInfo = &bi;
            else { ii[b - 1] = (VkDescriptorImageInfo){b >= 4 ? VK_NULL_HANDLE : p && b < 3 ? linear : nearest, images[indices[b - 1]].view, b >= 4 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; writes[b].pImageInfo = &ii[b - 1]; }
        }
        vkUpdateDescriptorSets(device, 6, writes, 0, NULL);
    }
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int i = 0; i < N; i++) {
        barrier(cmd, images[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue clear = {0};
        if (i == COLOR) { clear.float32[0] = intensity; clear.float32[1] = intensity * 0.5f; clear.float32[2] = intensity * 0.25f; }
        if (i == DEPTH) clear.float32[0] = 1;
        vkCmdClearColorImage(cmd, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        barrier(cmd, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (int f = 0; f < 3; f++) {
        /* Poison output each frame: a skipped dispatch must not pass using old pixels. */
        barrier(cmd, images[OUTPUT].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue poison = {{NAN, NAN, NAN, NAN}};
        vkCmdClearColorImage(cmd, images[OUTPUT].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &poison, 1, &range);
        barrier(cmd, images[OUTPUT].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (int p = 0; p < 2; p++) {
            int targets[] = {p ? OUTPUT : MOTION, p ? HISTORY0 + ((f & 1) ^ 1) : PACKED};
            for (int i = 0; i < 2; i++) barrier(cmd, images[targets[i]].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[p]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl[p], 0, 1, &sets[f * 2 + p], 0, NULL);
            if (!p || !getenv("WT_SGSR_TEST_SKIP_FINAL")) vkCmdDispatch(cmd, (uint32_t)((p ? ow : w) + 7) / 8, (uint32_t)((p ? oh : h) + 7) / 8, 1);
            for (int i = 0; i < 2; i++) barrier(cmd, images[targets[i]].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        barrier(cmd, images[OUTPUT].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy = {0}; copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = (VkExtent3D){(uint32_t)ow, (uint32_t)oh, 1};
        vkCmdCopyImageToBuffer(cmd, images[OUTPUT].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback[f].buffer, 1, &copy);
        barrier(cmd, images[OUTPUT].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    int failed = 0;
    int oracle = getenv("WT_SGSR_TEST_OFFICIAL_REFERENCE") != NULL;
    float official[3]; cs_constant_reference(intensity, official);
    for (int f = 0; f < 3; f++) for (int i = 0; i < ow * oh; i++) for (int c = 0; c < 3; c++) {
        float value = half_float(((uint16_t *)readback[f].mapped)[i * 4 + c]);
        float expected = oracle ? official[c] : intensity / (float)(1 << c);
        /* Official packed YCoCg + inverse tone map is lossy, especially bright HDR. */
        if (!isfinite(value) || fabsf(value - expected) > fmaxf(0.03f, expected * (oracle ? 0.002f : 0.02f))) {
            if (!failed) fprintf(stderr, "frame=%d pixel=%d channel=%d actual=%g expected=%g\n", f, i, c, value, expected);
            failed++;
        }
    }
    /* Constant frames exercise dispatches, not distinguishable history/reset behavior. */
    printf("SGSR 2-pass-CS %dx%d -> %dx%d intensity=%g %s constant-input dispatch/coverage readback: %s (%d channels)\n", w, h, ow, oh, intensity,
        oracle ? "official-equation fidelity" : "HDR quality against input RGB", failed ? "FAIL" : "PASS", failed);
    vkDestroyDescriptorPool(device, descriptors, NULL); vkDestroySampler(device, nearest, NULL); vkDestroySampler(device, linear, NULL);
    for (int p = 0; p < 2; p++) { vkDestroyPipeline(device, pipelines[p], NULL); vkDestroyPipelineLayout(device, pl[p], NULL); vkDestroyDescriptorSetLayout(device, dl[p], NULL); vkDestroyShaderModule(device, modules[p], NULL); }
    for (int i = 0; i < N; i++) { vkDestroyImageView(device, images[i].view, NULL); vkDestroyImage(device, images[i].image, NULL); vkFreeMemory(device, images[i].memory, NULL); }
    for (int f = 0; f < 3; f++) {
        Buffer buffers[] = {uniform[f], readback[f]};
        for (int i = 0; i < 2; i++) { vkUnmapMemory(device, buffers[i].memory); vkDestroyBuffer(device, buffers[i].buffer, NULL); vkFreeMemory(device, buffers[i].memory, NULL); }
    }
    vkDestroyCommandPool(device, pool, NULL); vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    return failed ? 1 : 0;
}
