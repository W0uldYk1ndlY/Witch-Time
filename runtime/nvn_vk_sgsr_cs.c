/* Include after nvn_vk_sgsr.c. Scene selection/camera/reset remain owned by the caller.
 * Identity: official SGSR 2-pass-CS, camera-only, no external reactivity. */
#include "sgsr2_params.h"
#include "sgsr2_cs_source.h"

static const char k_sgsr_cs_convert[] = {
#embed "third_party/sgsr2/glsl_2_pass_cs/sgsr2_convert.comp"
, 0};
static const char k_sgsr_cs_upscale[] = {
#embed "third_party/sgsr2/glsl_2_pass_cs/sgsr2_upscale.comp"
, 0};

enum { SC_MOTION, SC_PACKED, SC_HISTORY0, SC_HISTORY1, SC_OUTPUT, SC_ZERO, SC_IMAGES };
static struct {
    int ok, w, h, ow, oh, history, have_history;
    FImg image[SC_IMAGES];
    VkPipeline pipe[2];
    VkPipelineLayout pl[2];
    VkDescriptorSetLayout dl[2];
    VkSampler nearest, linear;
    double previous_vp[16];
    float previous_position[3], previous_forward[3], previous_projection[16];
    Tex *previous_color, *previous_depth;
    const VTex *previous_color_image, *previous_depth_image;
    int still_frames, frame_reset, observations, last_state, last_reset;
} SC;

static VkFormat sgsr_cs_format(int i) {
    return i == SC_PACKED ? VK_FORMAT_R32_UINT : VK_FORMAT_R16G16B16A16_SFLOAT;
}

static VkImageUsageFlags sgsr_cs_usage(int i) {
    return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (i == SC_ZERO ? 0 : VK_IMAGE_USAGE_STORAGE_BIT) |
        (i == SC_OUTPUT ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
}

static const char *sgsr_cs_unable(void) {
    const VkPhysicalDeviceLimits *l = &V.props.limits;
    if (!D.has_push || D.fsr_push_descriptors < 6) return "SGSR CS requires six push descriptors";
    if (l->maxComputeWorkGroupInvocations < 64 || l->maxComputeWorkGroupSize[0] < 8 ||
        l->maxComputeWorkGroupSize[1] < 8) return "SGSR CS requires 8x8 compute workgroups";
    if (l->maxPerStageDescriptorSamplers < 3 || l->maxPerStageDescriptorSampledImages < 3 ||
        l->maxDescriptorSetSamplers < 3 || l->maxDescriptorSetSampledImages < 3 ||
        l->maxPerStageDescriptorStorageImages < 2 || l->maxDescriptorSetStorageImages < 2 ||
        l->maxPerStageDescriptorUniformBuffers < 1 || l->maxDescriptorSetUniformBuffers < 1 ||
        l->maxPerStageResources < 6 || l->maxUniformBufferRange < sizeof(Sgsr2CsParams))
        return "SGSR CS descriptor or uniform limits are insufficient";
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (!fmt_supported(VK_FORMAT_R32_UINT, need)) return "SGSR CS requires sampled/storage R32UI";
    if (!fmt_supported(VK_FORMAT_R16G16B16A16_SFLOAT, need | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        return "SGSR CS requires sampled/filtered/storage RGBA16F";
    /* Both storage formats are in Vulkan's mandatory shaderStorageImageExtendedFormats-free list. */
    return NULL;
}

static void sgsr_cs_release_setup(void) {
    for (int p = 0; p < 2; p++) {
        if (SC.pipe[p]) vkDestroyPipeline(V.device, SC.pipe[p], NULL);
        if (SC.pl[p]) vkDestroyPipelineLayout(V.device, SC.pl[p], NULL);
        if (SC.dl[p]) vkDestroyDescriptorSetLayout(V.device, SC.dl[p], NULL);
        SC.pipe[p] = VK_NULL_HANDLE; SC.pl[p] = VK_NULL_HANDLE; SC.dl[p] = VK_NULL_HANDLE;
    }
    if (SC.nearest) vkDestroySampler(V.device, SC.nearest, NULL);
    if (SC.linear) vkDestroySampler(V.device, SC.linear, NULL);
    SC.nearest = SC.linear = VK_NULL_HANDLE;
}

static int sgsr_cs_setup(void) {
    if (SC.ok) return SC.ok > 0;
    const char *why = sgsr_cs_unable();
    if (why) { printf("nvn_vk_sgsr_cs: unavailable: %s\n", why); SC.ok = -1; return 0; }
    VkResult result = VK_SUCCESS;
    VkShaderModule module = VK_NULL_HANDLE;
    const char *operation = "sampler";
    int pass = -1;
    for (int i = 0; i < 2; i++) {
        VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.minFilter = ci.magFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        result = vkCreateSampler(V.device, &ci, NULL, i ? &SC.linear : &SC.nearest);
        if (result != VK_SUCCESS) goto fail;
    }
    for (pass = 0; pass < 2; pass++) {
        VkDescriptorSetLayoutBinding bindings[6] = {0};
        for (int b = 0; b < 6; b++) {
            bindings[b].binding = b == 0 ? 0 : b < 4 ? (uint32_t)(b + (pass ? 6 : 0)) : (uint32_t)(b + 6);
            bindings[b].descriptorCount = 1; bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[b].descriptorType = !b ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                b < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        dl.bindingCount = 6; dl.pBindings = bindings;
        operation = "vkCreateDescriptorSetLayout";
        result = vkCreateDescriptorSetLayout(V.device, &dl, NULL, &SC.dl[pass]);
        if (result != VK_SUCCESS) goto fail;
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &SC.dl[pass];
        operation = "vkCreatePipelineLayout";
        result = vkCreatePipelineLayout(V.device, &pl, NULL, &SC.pl[pass]);
        if (result != VK_SUCCESS) goto fail;
        char *source = sgsr_cs_source(pass ? k_sgsr_cs_upscale : k_sgsr_cs_convert, pass);
        uint64_t hash;
        operation = "compute compilation";
        module = source ? module_from_glsl(5, source, pass ? "SGSR CS Upscale" : "SGSR CS Convert", &hash) : VK_NULL_HANDLE;
        free(source);
        if (!module) goto fail;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
        ci.layout = SC.pl[pass];
        operation = "vkCreateComputePipelines";
        result = vkCreateComputePipelines(V.device, g_pcache, 1, &ci, NULL, &SC.pipe[pass]);
        vkDestroyShaderModule(V.device, module, NULL); module = VK_NULL_HANDLE;
        if (result != VK_SUCCESS) goto fail;
    }
    SC.ok = 1; return 1;
fail:
    printf("nvn_vk_sgsr_cs: pass=%d %s failed: %s (%d)\n", pass, operation,
        result == VK_SUCCESS ? "compiler failure" : vk_result_name(result), (int)result);
    if (result == VK_ERROR_DEVICE_LOST) vk_fatal(operation, result);
    if (module) vkDestroyShaderModule(V.device, module, NULL);
    sgsr_cs_release_setup(); SC.ok = -1; return 0;
}

static void sgsr_cs_retire_images(void) {
    for (int i = 0; i < SC_IMAGES; i++) {
        if (SC.image[i].vt) vt_garbage(SC.image[i].vt);
        memset(&SC.image[i], 0, sizeof SC.image[i]);
    }
    SC.w = SC.h = SC.ow = SC.oh = SC.history = SC.have_history = 0;
}

static int sgsr_cs_image_new(FImg *out, int i, int w, int h) {
    VTex *vt = calloc(1, sizeof *vt);
    VView *view = calloc(1, sizeof *view);
    if (!vt || !view) { free(vt); free(view); return 0; }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = sgsr_cs_format(i);
    ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1;
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.usage = sgsr_cs_usage(i);
    VkResult result = vkCreateImage(V.device, &ci, NULL, &vt->img);
    if (result != VK_SUCCESS) goto fail;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(V.device, vt->img, &requirements);
    vt->mem = vmem_alloc(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    result = vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off);
    if (result != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = vt->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    result = vkCreateImageView(V.device, &vi, NULL, &view->view);
    if (result != VK_SUCCESS) goto fail;
    vt->magic = 0x56544558; vt->fmt = ci.format; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    vt->w = w; vt->h = h; vt->d = vt->levels = vt->layers = vt->samples = 1;
    view->vtype = -2; vt->views = view;
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    R.images_live++; out->vt = vt; out->all = view->view; return 1;
fail:
    printf("nvn_vk_sgsr_cs: image=%d %dx%d failed: %s (%d)\n", i, w, h, vk_result_name(result), (int)result);
    if (result == VK_ERROR_DEVICE_LOST) vk_fatal("SGSR CS image", result);
    if (vt->img) vkDestroyImage(V.device, vt->img, NULL);
    if (vt->mem.mem) vmem_free(vt->mem);
    free(vt); free(view); return 0;
}

static int sgsr_cs_resize(int w, int h, int ow, int oh) {
    FImg next[SC_IMAGES] = {0};
    for (int i = 0; i < SC_IMAGES; i++) {
        int iw = i >= SC_HISTORY0 && i <= SC_OUTPUT ? ow : w;
        int ih = i >= SC_HISTORY0 && i <= SC_OUTPUT ? oh : h;
        VkImageFormatProperties props;
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(V.gpu, sgsr_cs_format(i), VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, sgsr_cs_usage(i), 0, &props);
        if (iw <= 0 || ih <= 0 || result != VK_SUCCESS || (uint32_t)iw > props.maxExtent.width ||
            (uint32_t)ih > props.maxExtent.height || !props.maxExtent.depth || !props.maxMipLevels || !props.maxArrayLayers ||
            !(props.sampleCounts & VK_SAMPLE_COUNT_1_BIT) || !sgsr_cs_image_new(&next[i], i, iw, ih)) {
            for (int j = 0; j < SC_IMAGES; j++) if (next[j].vt) vt_destroy(next[j].vt);
            printf("nvn_vk_sgsr_cs: unsupported image extent/usage or allocation failure image=%d\n", i);
            return 0;
        }
    }
    sgsr_cs_retire_images(); memcpy(SC.image, next, sizeof next);
    SC.w = w; SC.h = h; SC.ow = ow; SC.oh = oh;
    VkClearColorValue zero = {0};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int i = 0; i < SC_IMAGES; i++) {
        VTex *vt = SC.image[i].vt;
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(R.cmd, vt->img, vt->layout, &zero, 1, &range);
        vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return 1;
}

static void sgsr_cs_pass(int pass, const VkDescriptorBufferInfo *ubo, const VkDescriptorImageInfo *images, int w, int h) {
    VkWriteDescriptorSet writes[6] = {0};
    for (int b = 0; b < 6; b++) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[b].descriptorCount = 1;
        writes[b].dstBinding = !b ? 0 : b < 4 ? (uint32_t)(b + (pass ? 6 : 0)) : (uint32_t)(b + 6);
        writes[b].descriptorType = !b ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
            b < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (!b) writes[b].pBufferInfo = ubo; else writes[b].pImageInfo = &images[b - 1];
    }
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, SC.pipe[pass]);
    vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, SC.pl[pass], 0, 6, writes);
    pt_begin(PT_SGSR_CS_BASE + pass, w, h);
    vkCmdDispatch(R.cmd, ((uint32_t)w + 7) / 8, ((uint32_t)h + 7) / 8, 1);
    pt_end();
    /* Compute bindings are not compatible with translated guest compute pipelines. */
    R.pipe_bound = NULL; dsh_forget();
}

/* Caller supplies matching sampled color/depth views, unjittered camera transform and
 * scene reset. Returns separate HDR output; never advances on a held presentation. */
static FImg *sgsr_cs_dispatch(VTex *color, VkImageView color_view, VTex *depth, VkImageView depth_view,
                         const Sgsr2CsParams *input) {
    if (!input || !color || !depth) return NULL;
    int w = (int)input->render_size[0], h = (int)input->render_size[1];
    int ow = (int)input->display_size[0], oh = (int)input->display_size[1];
    const VkPhysicalDeviceLimits *limits = &V.props.limits;
    if (w <= 0 || h <= 0 || ow < w || oh < h || color == depth ||
        color->w != w || color->h != h || depth->w != w || depth->h != h || !color_view || !depth_view ||
        !(input->pre_exposure > 0) || !isfinite(input->pre_exposure) ||
        ((uint32_t)ow + 7) / 8 > limits->maxComputeWorkGroupCount[0] ||
        ((uint32_t)oh + 7) / 8 > limits->maxComputeWorkGroupCount[1] || !sgsr_cs_setup()) return NULL;
    Sgsr2CsParams params = *input;
    int resize = SC.w != w || SC.h != h || SC.ow != ow || SC.oh != oh;
    params.reset = params.reset || resize || !SC.have_history;
    if (params.reset) params.same_camera = 0;
    params.render_size_rcp[0] = 1.0f / w; params.render_size_rcp[1] = 1.0f / h;
    params.display_size_rcp[0] = 1.0f / ow; params.display_size_rcp[1] = 1.0f / oh;
    /* Convert reads bSameCamera at this slot; only Upscale consumes its frame count. */
    uint64_t alignment = V.props.limits.minUniformBufferOffsetAlignment;
    if (!alignment) alignment = 1;
    pass_end();
    uint64_t raw = ring_raw(NULL, sizeof params + alignment - 1);
    uint64_t offset = (raw + alignment - 1) / alignment * alignment;
    memcpy((uint8_t *)R.ring_mem.map + offset, &params, sizeof params);
    cmd_begin();
    if (resize && !sgsr_cs_resize(w, h, ow, oh)) { SC.ok = -1; return NULL; }
    vt_layout(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vt_layout(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (int i = 0; i < 2; i++) vt_layout(SC.image[i].vt, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorBufferInfo ubo = {R.ring, offset, sizeof params};
    VkDescriptorImageInfo convert[] = {
        {SC.nearest, color_view, color->layout}, {SC.nearest, depth_view, depth->layout},
        {SC.nearest, SC.image[SC_ZERO].all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {0, SC.image[SC_MOTION].all, VK_IMAGE_LAYOUT_GENERAL}, {0, SC.image[SC_PACKED].all, VK_IMAGE_LAYOUT_GENERAL}
    };
    sgsr_cs_pass(0, &ubo, convert, w, h);
    for (int i = 0; i < 2; i++) vt_layout(SC.image[i].vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    int output = SC.history ^ 1;
    vt_layout(SC.image[SC_HISTORY0 + output].vt, VK_IMAGE_LAYOUT_GENERAL);
    vt_layout(SC.image[SC_OUTPUT].vt, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorImageInfo upscale[] = {
        {SC.linear, SC.image[SC_HISTORY0 + SC.history].all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {SC.linear, SC.image[SC_MOTION].all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {SC.nearest, SC.image[SC_PACKED].all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {0, SC.image[SC_OUTPUT].all, VK_IMAGE_LAYOUT_GENERAL},
        {0, SC.image[SC_HISTORY0 + output].all, VK_IMAGE_LAYOUT_GENERAL}
    };
    sgsr_cs_pass(1, &ubo, upscale, ow, oh);
    vt_layout(SC.image[SC_HISTORY0 + output].vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vt_layout(SC.image[SC_OUTPUT].vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    SC.history = output; SC.have_history = 1;
    return &SC.image[SC_OUTPUT];
}

static void sgsr_cs_shutdown(void) {
    /* Called after global image destruction and after all GPU users have retired. */
    sgsr_cs_release_setup(); memset(&SC, 0, sizeof SC);
}

static int sgsr_cs_want(void) {
    return g_rs_mode >= 6 && g_rs_mode <= 8 && gfx_sgsr_variant() == 1 && SC.ok >= 0;
}

static void sgsr_cs_run(Tex *color, Tex *depth, int effect) {
    (void)effect;
    VTex *vc = vt_get(color), *vd = vt_get(depth);
    int w = vc->w, h = vc->h, ow = w, oh = h;
    if (!sgsr_cs_want() || w <= 0 || h <= 0 || vd->w != w || vd->h != h) return;
    if (vc->rs) rs_size(color->b.width, color->b.height, 1, &ow, &oh);
    if (ow < w || oh < h) { ow = w; oh = h; }
    if (!fmt_supported(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !fmt_supported(vc->fmt, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        printf("nvn_vk_sgsr_cs: scene format cannot receive SGSR output; native fallback\n"); SC.ok = -1; return;
    }
    double jx, jy, vp[16], inverse[16], transform[16];
    upscale_scene_jitter(&U, w, h, &jx, &jy);
    const float *camera = U.c3;
    for (int i = 0; i < 16; i++) vp[i] = camera[32 + i];
    for (int c = 0; c < 4; c++) { vp[c] -= jx * vp[12 + c]; vp[4 + c] -= jy * vp[12 + c]; }
    float position[3] = {camera[51], camera[55], camera[59]};
    float forward[3] = {-camera[8], -camera[9], -camera[10]};
    float distance = 0, dot = 0, fl = 0, pl = 0;
    for (int i = 0; i < 3; i++) {
        float delta = position[i] - SC.previous_position[i]; distance += delta * delta;
        dot += forward[i] * SC.previous_forward[i]; fl += forward[i] * forward[i]; pl += SC.previous_forward[i] * SC.previous_forward[i];
    }
    /* shortcut: use FSR's camera-cut heuristic until a verified game signal exists. */
    int cut = distance > 25 || (fl > 0 && pl > 0 && dot / sqrtf(fl * pl) < 0.9f);
    int incompatible = SC.w != w || SC.h != h || SC.ow != ow || SC.oh != oh ||
        SC.previous_color != color || SC.previous_depth != depth || SC.previous_color_image != vc || SC.previous_depth_image != vd;
    for (int i = 0; i < 16; i++) if (i != 2 && i != 6 && SC.previous_projection[i] != camera[16 + i]) incompatible = 1;
    int reset = upscale_scene_reset(&U, incompatible, SC.have_history, cut, g_diag_surface_gen);
    if (reset) for (int i = 0; i < 16; i++) transform[i] = i % 5 == 0;
    else {
        fsr_inv4(vp, inverse);
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
            double value = 0; for (int k = 0; k < 4; k++) value += SC.previous_vp[r * 4 + k] * inverse[k * 4 + c];
            transform[r * 4 + c] = value;
        }
    }
    Sgsr2CsParams params = {0};
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) params.clip_to_prev_clip[c * 4 + r] = (float)transform[r * 4 + c];
    params.render_size[0] = (uint32_t)w; params.render_size[1] = (uint32_t)h;
    params.display_size[0] = (uint32_t)ow; params.display_size[1] = (uint32_t)oh;
    params.jitter_offset[0] = (float)(jx * w * 0.5); params.jitter_offset[1] = (float)(-jy * h * 0.5);
    params.pre_exposure = 1; /* Scene is linear HDR; no hidden exposure or clipping treatment. */
    params.camera_fov_angle_hor = camera[21] > 0 ? (float)w / (h * camera[21]) : 1.0f;
    params.reset = (uint32_t)reset;
    int same = !reset && memcmp(vp, SC.previous_vp, sizeof vp) == 0;
    int still = same ? (SC.still_frames < 6 ? SC.still_frames + 1 : 6) : 0;
    params.same_camera = (uint32_t)still;
    params.min_lerp_contribution = still > 5 ? 0.3f : 0;
    EView view = sgsr_scene_view(depth->b.format);
    VkImageView depth_view = vt_view(vd, &view, DIM_2D, 0);
    view.format = color->b.format;
    VkImageView color_view = vt_view(vc, &view, DIM_2D, 0);
    FImg *out = sgsr_cs_dispatch(vc, color_view, vd, depth_view, &params);
    if (!out) return;
    VTex *result = out->vt;
    vt_layout(result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[1] = (VkOffset3D){ow, oh, 1}; blit.dstOffsets[1] = (VkOffset3D){w, h, 1};
    pt_begin(PT_COPY_CONVERT, w, h);
    vkCmdBlitImage(R.cmd, result->img, result->layout, vc->img, vc->layout, 1, &blit, ow == w && oh == h ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    pt_end();
    vt_layout(result, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); vt_layout(vc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    memcpy(SC.previous_vp, vp, sizeof vp); memcpy(SC.previous_position, position, sizeof position); memcpy(SC.previous_forward, forward, sizeof forward);
    memcpy(SC.previous_projection, camera + 16, sizeof SC.previous_projection);
    SC.previous_color = color; SC.previous_depth = depth; SC.previous_color_image = vc; SC.previous_depth_image = vd;
    SC.still_frames = still; SC.frame_reset = reset;
    U.col = color; U.output_larger = ow != w || oh != h; U.ran = U.frame;
    upscale_diag_dispatched(1);
    U.surface_generation = g_diag_surface_gen; U.have_surface = 1;
    game_fsr_w = w; game_fsr_h = h; game_fsr_phases = (int)(8.0 * ow / w * ow / w + 0.5);
}

static void sgsr_cs_present(void) {
    if (g_rs_mode < 6 || g_rs_mode > 8 || gfx_sgsr_variant() != 1) return;
    int state = SC.ok < 0 ? 0 : U.ran != U.frame ? 1 : 2;
    if (SC.observations < 32 && (!SC.observations || state != SC.last_state || SC.frame_reset != SC.last_reset)) {
        printf("nvn_vk_sgsr_cs: evidence frame=%lld variant=2-pass-cs initialized=%d render=%dx%d output=%dx%d reset=%d result=%s motion=camera-only reactive=unavailable HDR=unqualified (recording, not GPU completion/consumption proof)\n",
            U.frame, SC.ok == 1, SC.w, SC.h, SC.ow, SC.oh, SC.frame_reset,
            state == 0 ? "backend-unavailable" : state == 1 ? "no-recorded-scene" : "passes-and-scene-copy-recorded");
        SC.observations++;
    }
    SC.last_state = state; SC.last_reset = SC.frame_reset;
}

static VkImageView sgsr_cs_up_view(int stage, Tex *texture, VTex *image, const View *view) {
    if (stage != 1 || !sgsr_cs_want() || SC.ok != 1 || !SC.have_history || U.ran != U.frame ||
        texture != U.col || !U.output_larger || !R.prog[1] || stage_fx(&R.prog[1]->st[1]) != FX_GRAIN ||
        !R.ncol || !R.col[0].tex) return VK_NULL_HANDLE;
    Tex *target = R.col[0].tex;
    if (!rs_post_has(target) && !(target->vk && ((VTex *)target->vk)->post)) return VK_NULL_HANDLE;
    EView effective; eview_of(texture, image, view, &effective);
    if (effective.format != texture->b.format || effective.lvl0 != 0 || effective.nlvl != 1 ||
        effective.lay0 != 0 || effective.nlay != 1 || effective.swz[0] != 2 || effective.swz[1] != 3 ||
        effective.swz[2] != 4 || effective.swz[3] != 5) return VK_NULL_HANDLE;
    return SC.image[SC_OUTPUT].all;
}
