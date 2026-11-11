/* SGSR fragment backend. Included after nvn_vk_fsr.c in the Vulkan translation unit. */
#include "sgsr2_params.h"
#include "sgsr2_vertex.h"

static const char k_sgsr_convert[] = {
#embed "third_party/sgsr2/glsl_2_pass_fs/sgsr2_convert.fs"
, 0};
static const char k_sgsr_upscale[] = {
#embed "third_party/sgsr2/glsl_2_pass_fs/sgsr2_upscale.fs"
, 0};

enum { SG_MOTION, SG_HISTORY0, SG_HISTORY1, SG_ZERO, SG_IMAGES };
static struct {
    int ok, w, h, ow, oh, history, have_history, still_frames;
    FImg image[SG_IMAGES];
    VkPipeline pipe[2];
    VkPipelineLayout pl[2];
    VkDescriptorSetLayout dl[2];
    VkSampler nearest, linear;
    double previous_vp[16];
    float previous_position[3], previous_forward[3];
    Tex *previous_color, *previous_depth;
    const VTex *previous_color_image, *previous_depth_image;
    float previous_projection[16];
    int observations, last_state, last_mode, last_variant, last_reset, frame_reset;
} SG;

static char *sgsr_fragment_source(const char *upstream) {
    FsrStr text = {0};
    const char *p = strchr(upstream, '\n');
    if (!p) return NULL;
    const char *prefix = "#version 450\n#define REQUEST_NDC_Y_UP 1\n"
        "#define texelFetch(s,p,l) texelFetch(s,clamp(p,ivec2(0),textureSize(s,l)-ivec2(1)),l)\n";
    fsr_put(&text, prefix, strlen(prefix));
    p++;
    const char *position = strstr(p, "vec3 Position = vec3(ScreenPos,");
    /* HDR variance squares exceed mediump range well before FP16 color storage does. */
    for (;;) {
        const char *next = strstr(p, "mediump");
        if (position && (!next || position < next)) {
            fsr_put(&text, p, (size_t)(position - p));
            const char *unjitter = "ScreenPos -= vec2(2.0 * params.jitterOffset.x * params.renderSizeRcp.x, -2.0 * params.jitterOffset.y * params.renderSizeRcp.y);\n";
            fsr_put(&text, unjitter, strlen(unjitter)); p = position; position = NULL; continue;
        }
        if (!next) { fsr_put(&text, p, strlen(p)); break; }
        fsr_put(&text, p, (size_t)(next - p));
        fsr_put(&text, "highp", 5); p = next + 7;
    }
    return text.p;
}

static const char *sgsr_unable(void) {
    if (!D.has_push || D.fsr_push_descriptors < 4) return "SGSR FS requires four push descriptors";
    const VkPhysicalDeviceLimits *l = &V.props.limits;
    if (l->maxPerStageDescriptorSamplers < 3 || l->maxPerStageDescriptorSampledImages < 3 ||
        l->maxDescriptorSetSamplers < 3 || l->maxDescriptorSetSampledImages < 3 ||
        l->maxPerStageDescriptorUniformBuffers < 1 || l->maxDescriptorSetUniformBuffers < 1 ||
        l->maxPerStageResources < 4 || l->maxUniformBufferRange < sizeof(Sgsr2FsParams))
        return "SGSR FS descriptor or uniform limits are insufficient";
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (!fmt_supported(VK_FORMAT_R16G16B16A16_SFLOAT, need)) return "SGSR FS needs sampled, filtered RGBA16F attachments";
    return NULL;
}

static void sgsr_release_setup(void) {
    for (int i = 0; i < 2; i++) {
        if (SG.pipe[i]) vkDestroyPipeline(V.device, SG.pipe[i], NULL);
        if (SG.pl[i]) vkDestroyPipelineLayout(V.device, SG.pl[i], NULL);
        if (SG.dl[i]) vkDestroyDescriptorSetLayout(V.device, SG.dl[i], NULL);
        SG.pipe[i] = VK_NULL_HANDLE; SG.pl[i] = VK_NULL_HANDLE; SG.dl[i] = VK_NULL_HANDLE;
    }
    if (SG.nearest) vkDestroySampler(V.device, SG.nearest, NULL);
    if (SG.linear) vkDestroySampler(V.device, SG.linear, NULL);
    SG.nearest = SG.linear = VK_NULL_HANDLE;
}

static int sgsr_setup(void) {
    if (SG.ok) return SG.ok > 0;
    const char *why = sgsr_unable();
    if (why) { printf("nvn_vk_sgsr: unavailable: %s\n", why); SG.ok = -1; return 0; }
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    uint64_t hash;
    VkResult result = VK_SUCCESS;
    const char *operation = "vertex compilation";
    int pass = -1;
    vertex = module_from_glsl(0, k_sgsr2_vs, "SGSR FS fullscreen vertex", &hash);
    if (!vertex) goto fail;
    for (int i = 0; i < 2; i++) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        operation = "vkCreateSampler";
        result = vkCreateSampler(V.device, &si, NULL, i ? &SG.linear : &SG.nearest);
        if (result != VK_SUCCESS) goto fail;
    }
    for (pass = 0; pass < 2; pass++) {
        VkDescriptorSetLayoutBinding binding[4];
        memset(binding, 0, sizeof binding);
        int n = pass ? 4 : 3;
        for (int b = 0; b < n; b++) {
            binding[b].binding = (uint32_t)b; binding[b].descriptorCount = 1;
            binding[b].descriptorType = b ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binding[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        dl.bindingCount = (uint32_t)n; dl.pBindings = binding;
        operation = "vkCreateDescriptorSetLayout";
        result = vkCreateDescriptorSetLayout(V.device, &dl, NULL, &SG.dl[pass]);
        if (result != VK_SUCCESS) goto fail;
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &SG.dl[pass];
        operation = "vkCreatePipelineLayout";
        result = vkCreatePipelineLayout(V.device, &pl, NULL, &SG.pl[pass]);
        if (result != VK_SUCCESS) goto fail;
        char *source = sgsr_fragment_source(pass ? k_sgsr_upscale : k_sgsr_convert);
        operation = "fragment compilation";
        fragment = source ? module_from_glsl(1, source, pass ? "SGSR FS Upscale" : "SGSR FS Convert", &hash) : VK_NULL_HANDLE;
        free(source);
        if (!fragment) goto fail;
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment; stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment = {0}; attachment.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1; blend.pAttachments = &attachment;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &rendering; ci.stageCount = 2; ci.pStages = stages;
        ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
        ci.pRasterizationState = &raster; ci.pMultisampleState = &ms; ci.pColorBlendState = &blend;
        ci.pDynamicState = &dynamic; ci.layout = SG.pl[pass];
        operation = "vkCreateGraphicsPipelines";
        result = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &ci, NULL, &SG.pipe[pass]);
        vkDestroyShaderModule(V.device, fragment, NULL); fragment = VK_NULL_HANDLE;
        if (result != VK_SUCCESS) goto fail;
    }
    vkDestroyShaderModule(V.device, vertex, NULL);
    SG.ok = 1;
    printf("nvn_vk_sgsr: 2-pass-fs pipelines initialized (HDR quality unqualified)\n");
    return 1;
fail:
    printf("nvn_vk_sgsr: pass=%d %s failed: %s (%d)\n", pass, operation,
           result == VK_SUCCESS ? "compiler failure" : vk_result_name(result), (int)result);
    if (result == VK_ERROR_DEVICE_LOST) vk_fatal(operation, result);
    if (fragment) vkDestroyShaderModule(V.device, fragment, NULL);
    if (vertex) vkDestroyShaderModule(V.device, vertex, NULL);
    sgsr_release_setup(); SG.ok = -1;
    return 0;
}

static void sgsr_retire_images(void) {
    for (int i = 0; i < SG_IMAGES; i++) {
        if (SG.image[i].vt) vt_garbage(SG.image[i].vt);
        memset(&SG.image[i], 0, sizeof SG.image[i]);
    }
    SG.w = SG.h = SG.ow = SG.oh = SG.have_history = SG.history = 0;
}

static int sgsr_image_supported(int w, int h, int attachment) {
    if (w <= 0 || h <= 0) return 0;
    VkImageFormatProperties props;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    VkResult result = vkGetPhysicalDeviceImageFormatProperties(V.gpu, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &props);
    return result == VK_SUCCESS && (uint32_t)w <= props.maxExtent.width && (uint32_t)h <= props.maxExtent.height &&
        props.maxExtent.depth >= 1 && props.maxMipLevels >= 1 && props.maxArrayLayers >= 1 && (props.sampleCounts & VK_SAMPLE_COUNT_1_BIT);
}

static int sgsr_image_new(FImg *out, int w, int h, int attachment) {
    VTex *vt = calloc(1, sizeof *vt);
    VView *view = calloc(1, sizeof *view);
    if (!vt || !view) { free(vt); free(view); return 0; }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1;
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    VkResult result = vkCreateImage(V.device, &ci, NULL, &vt->img);
    if (result != VK_SUCCESS) goto fail_image;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(V.device, vt->img, &requirements);
    vt->mem = vmem_alloc(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    result = vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off);
    if (result != VK_SUCCESS) goto fail_memory;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = vt->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    result = vkCreateImageView(V.device, &vi, NULL, &view->view);
    if (result != VK_SUCCESS) goto fail_memory;
    vt->magic = 0x56544558; vt->fmt = ci.format; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    vt->w = w; vt->h = h; vt->d = vt->levels = vt->layers = vt->samples = 1;
    view->vtype = -2; vt->views = view;
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    R.images_live++;
    out->vt = vt; out->all = view->view;
    return 1;
fail_memory:
    vkDestroyImage(V.device, vt->img, NULL);
    vmem_free(vt->mem);
    goto fail;
fail_image:
    if (vt->img) vkDestroyImage(V.device, vt->img, NULL);
fail:
    printf("nvn_vk_sgsr: image %dx%d attachment=%d failed: %s (%d)\n", w, h, attachment, vk_result_name(result), (int)result);
    if (result == VK_ERROR_DEVICE_LOST) vk_fatal("SGSR image allocation", result);
    free(view); free(vt);
    return 0;
}

static int sgsr_resize(int w, int h, int ow, int oh) {
    if (!sgsr_image_supported(w, h, 1) || !sgsr_image_supported(w, h, 0) || !sgsr_image_supported(ow, oh, 1)) return 0;
    FImg next[SG_IMAGES] = {0};
    for (int i = 0; i < SG_IMAGES; i++) {
        int output = i == SG_HISTORY0 || i == SG_HISTORY1;
        if (!sgsr_image_new(&next[i], output ? ow : w, output ? oh : h, i != SG_ZERO)) {
            /* No commands reference the new generation until every allocation succeeds. */
            for (int j = 0; j < SG_IMAGES; j++) if (next[j].vt) vt_destroy(next[j].vt);
            return 0;
        }
    }
    sgsr_retire_images();
    memcpy(SG.image, next, sizeof next);
    SG.w = w; SG.h = h; SG.ow = ow; SG.oh = oh;
    VkClearColorValue zero = {0};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int i = 0; i < SG_IMAGES; i++) {
        VTex *vt = SG.image[i].vt;
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(R.cmd, vt->img, vt->layout, &zero, 1, &range);
        vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return 1;
}

static void sgsr_pass(int pass, FImg *target, const VkDescriptorBufferInfo *ubo, const VkDescriptorImageInfo *inputs) {
    VTex *vt = target->vt;
    vt_layout(vt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkWriteDescriptorSet writes[4]; memset(writes, 0, sizeof writes);
    int count = pass ? 4 : 3;
    for (int i = 0; i < count; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = (uint32_t)i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (i) writes[i].pImageInfo = &inputs[i - 1]; else writes[i].pBufferInfo = ubo;
    }
    VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target->all; color.imageLayout = vt->layout;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent.width = (uint32_t)vt->w; rendering.renderArea.extent.height = (uint32_t)vt->h;
    rendering.layerCount = rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color;
    VkViewport viewport = {0, 0, (float)vt->w, (float)vt->h, 0, 1};
    VkRect2D scissor = {{0, 0}, {(uint32_t)vt->w, (uint32_t)vt->h}};
    vkCmdBeginRendering(R.cmd, &rendering);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, SG.pipe[pass]);
    vkCmdSetViewport(R.cmd, 0, 1, &viewport); vkCmdSetScissor(R.cmd, 0, 1, &scissor);
    vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, SG.pl[pass], 0, (uint32_t)count, writes);
    vkCmdDraw(R.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(R.cmd);
    R.pipe_bound = NULL; R.dyn_dirty = R.dyn3_dirty = 1; dsh_forget();
    vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void sgsr_shutdown(void) {
    /* Images belong to g_vtex_all and have already been destroyed by renderer shutdown. */
    sgsr_release_setup(); memset(&SG, 0, sizeof SG);
}

static int sgsr_want(void) {
    return g_rs_mode >= 6 && g_rs_mode <= 8 && gfx_sgsr_variant() == 0 && SG.ok >= 0;
}

static EView sgsr_scene_view(int format) {
    EView view; memset(&view, 0, sizeof view);
    view.nlvl = view.nlay = 1; view.format = format;
    view.swz[0] = 2; view.swz[1] = 3; view.swz[2] = 4; view.swz[3] = 5;
    return view;
}

static void sgsr_run(Tex *color, Tex *depth, int effect) {
    (void)effect;
    VTex *vc = vt_get(color), *vd = vt_get(depth);
    int w = vc->w, h = vc->h, ow = w, oh = h;
    if (!sgsr_want() || w <= 0 || h <= 0 || vd->w != w || vd->h != h) return;
    if (vc->rs) rs_size(color->b.width, color->b.height, 1, &ow, &oh);
    if (ow < w || oh < h) { ow = w; oh = h; }
    if (!fmt_supported(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !fmt_supported(vc->fmt, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        printf("nvn_vk_sgsr: scene format cannot receive SGSR output; native fallback\n"); SG.ok = -1; return;
    }
    if (!sgsr_setup()) return;
    double jx, jy, vp[16], inverse[16], transform[16];
    upscale_scene_jitter(&U, w, h, &jx, &jy);
    const float *camera = U.c3;
    for (int i = 0; i < 16; i++) vp[i] = camera[32 + i];
    for (int c = 0; c < 4; c++) { vp[c] -= jx * vp[12 + c]; vp[4 + c] -= jy * vp[12 + c]; }
    float position[3] = {camera[51], camera[55], camera[59]};
    float forward[3] = {-camera[8], -camera[9], -camera[10]};
    float distance = 0, dot = 0, fl = 0, pl = 0;
    for (int i = 0; i < 3; i++) {
        float delta = position[i] - SG.previous_position[i]; distance += delta * delta;
        dot += forward[i] * SG.previous_forward[i]; fl += forward[i] * forward[i]; pl += SG.previous_forward[i] * SG.previous_forward[i];
    }
    /* shortcut: camera cuts use the existing FSR distance/angle heuristic until a game signal is recovered. */
    int cut = distance > 25 || (fl > 0 && pl > 0 && dot / sqrtf(fl * pl) < 0.9f);
    int resize = SG.w != w || SG.h != h || SG.ow != ow || SG.oh != oh;
    int incompatible = SG.previous_color != color || SG.previous_depth != depth ||
        SG.previous_color_image != vc || SG.previous_depth_image != vd;
    for (int i = 0; i < 16; i++) if (i != 2 && i != 6 && SG.previous_projection[i] != camera[16 + i]) incompatible = 1;
    int reset = upscale_scene_reset(&U, resize || incompatible, SG.have_history, cut, g_diag_surface_gen);
    SG.frame_reset = reset;
    if (reset) for (int i = 0; i < 16; i++) transform[i] = i % 5 == 0;
    else {
        fsr_inv4(vp, inverse);
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
            double value = 0; for (int k = 0; k < 4; k++) value += SG.previous_vp[r * 4 + k] * inverse[k * 4 + c];
            transform[r * 4 + c] = value;
        }
    }
    Sgsr2FsParams params = {0};
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) params.clip_to_prev_clip[c * 4 + r] = (float)transform[r * 4 + c];
    params.render_size[0] = (float)w; params.render_size[1] = (float)h;
    params.output_size[0] = (float)ow; params.output_size[1] = (float)oh;
    params.render_size_rcp[0] = 1.0f / w; params.render_size_rcp[1] = 1.0f / h;
    params.output_size_rcp[0] = 1.0f / ow; params.output_size_rcp[1] = 1.0f / oh;
    params.jitter_offset[0] = (float)(jx * w * 0.5); params.jitter_offset[1] = (float)(-jy * h * 0.5);
    params.scale_ratio[0] = (float)ow / w;
    double area_ratio = (double)ow * oh / ((double)w * h);
    params.scale_ratio[1] = (float)fmin(20.0, area_ratio * area_ratio * area_ratio);
    params.camera_fov_angle_hor = camera[21] > 0 ? (float)w / (h * camera[21]) : 1.0f;
    params.reset = reset ? 1.0f : 0.0f;
    params.same_camera = !reset && memcmp(vp, SG.previous_vp, sizeof vp) == 0;
    SG.still_frames = params.same_camera ? (SG.still_frames < 6 ? SG.still_frames + 1 : 6) : 0;
    params.min_lerp_contribution = SG.still_frames > 5 ? 0.3f : 0.0f;
    /* The stream ring owns each constant copy through its GPU users, including wrap. */
    uint64_t alignment = V.props.limits.minUniformBufferOffsetAlignment;
    if (alignment < 1) alignment = 1;
    pass_end();
    uint64_t raw = ring_raw(NULL, sizeof params + alignment - 1);
    uint64_t offset = (raw + alignment - 1) / alignment * alignment;
    memcpy((uint8_t *)R.ring_mem.map + offset, &params, sizeof params);
    cmd_begin();
    if (resize && !sgsr_resize(w, h, ow, oh)) { SG.ok = -1; return; }
    vt_layout(vc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vt_layout(vd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EView view = sgsr_scene_view(depth->b.format);
    VkImageView depth_view = vt_view(vd, &view, DIM_2D, 0);
    view.format = color->b.format;
    VkImageView color_view = vt_view(vc, &view, DIM_2D, 0);
    VkDescriptorBufferInfo ubo = {R.ring, offset, sizeof params};
    VkDescriptorImageInfo convert[2] = {
        {SG.nearest, depth_view, vd->layout},
        {SG.nearest, SG.image[SG_ZERO].all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
    };
    sgsr_pass(0, &SG.image[SG_MOTION], &ubo, convert);
    int output = SG.history ^ 1;
    VkDescriptorImageInfo upscale[3] = {
        {SG.linear, SG.image[SG_HISTORY0 + SG.history].all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {SG.linear, SG.image[SG_MOTION].all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {SG.nearest, color_view, vc->layout}
    };
    sgsr_pass(1, &SG.image[SG_HISTORY0 + output], &ubo, upscale);
    VTex *result = SG.image[SG_HISTORY0 + output].vt;
    vt_layout(result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[1].x = ow; blit.srcOffsets[1].y = oh; blit.srcOffsets[1].z = 1;
    blit.dstOffsets[1].x = w; blit.dstOffsets[1].y = h; blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(R.cmd, result->img, result->layout, vc->img, vc->layout, 1, &blit, ow == w && oh == h ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    vt_layout(result, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); vt_layout(vc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    SG.history = output; SG.have_history = 1;
    memcpy(SG.previous_vp, vp, sizeof vp); memcpy(SG.previous_position, position, sizeof position); memcpy(SG.previous_forward, forward, sizeof forward);
    SG.previous_color = color; SG.previous_depth = depth; SG.previous_color_image = vc; SG.previous_depth_image = vd;
    memcpy(SG.previous_projection, camera + 16, sizeof SG.previous_projection);
    U.col = color; U.output_larger = ow != w || oh != h; U.ran = U.frame;
    U.surface_generation = g_diag_surface_gen; U.have_surface = 1;
    game_fsr_w = w; game_fsr_h = h; game_fsr_phases = (int)(8.0 * ow / w * ow / w + 0.5);
}

static void sgsr_present(void) {
    int selected = g_rs_mode >= 6 && g_rs_mode <= 8;
    if (!selected && SG.last_mode < 6) return;
    int variant = gfx_sgsr_variant();
    int state = !selected ? 0 : variant != 0 ? 1 : SG.ok < 0 ? 2 : U.ran != U.frame ? 3 : 4;
    int reset = state == 4 && SG.frame_reset;
    if (SG.observations < 32 && (!SG.observations || state != SG.last_state || g_rs_mode != SG.last_mode ||
        variant != SG.last_variant || reset != SG.last_reset)) {
        const char *names[] = {"off", "variant-unavailable", "backend-unavailable", "no-recorded-scene", "passes-and-scene-copy-recorded"};
        printf("nvn_vk_sgsr: evidence frame=%lld requested=%d variant=%d initialized=%d render=%dx%d output=%dx%d reset=%d result=%s motion=camera-only reactive=unavailable (commands recorded, not GPU completion or consumed-output proof)\n",
               U.frame, g_rs_mode, variant, SG.ok == 1, SG.w, SG.h, SG.ow, SG.oh, reset, names[state]);
        SG.observations++;
    }
    SG.last_state = state; SG.last_mode = g_rs_mode; SG.last_variant = variant; SG.last_reset = reset;
}

static VkImageView sgsr_up_view(int stage, Tex *texture, VTex *image, const View *view) {
    if (stage != 1 || !sgsr_want() || SG.ok != 1 || !SG.have_history || U.ran != U.frame ||
        texture != U.col || !U.output_larger || !R.prog[1] || stage_fx(&R.prog[1]->st[1]) != FX_GRAIN ||
        !R.ncol || !R.col[0].tex) return VK_NULL_HANDLE;
    Tex *target = R.col[0].tex;
    if (!rs_post_has(target) && !(target->vk && ((VTex *)target->vk)->post)) return VK_NULL_HANDLE;
    EView effective; eview_of(texture, image, view, &effective);
    if (effective.format != texture->b.format || effective.lvl0 != 0 || effective.nlvl != 1 ||
        effective.lay0 != 0 || effective.nlay != 1 || effective.swz[0] != 2 || effective.swz[1] != 3 ||
        effective.swz[2] != 4 || effective.swz[3] != 5) return VK_NULL_HANDLE;
    return SG.image[SG_HISTORY0 + SG.history].all;
}
