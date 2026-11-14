/* [witchtime] The PC settings menu on the screen (Vulkan; included by nvn_vk.c after nvn_vk_draw.c). pcmenu.c draws the panel into
 * BGRA pixels on the CPU when its content changes; here they are copied over the presented picture (after the game's frame, the
 * bars and the first-run text: never into the game's textures), at the size of the window.
 *   - The GPU path (pcm_gpu_*): the panel and the first-start screen are composed on the GPU every frame: the ground pcmenu.c
 *     drew (once a version) is copied into a canvas of the panel's size, the moving parts (the wheels' rings and gears, the
 *     hands) are drawn over it as turned textured quads from pcmenu_gpu_scene, with the hubs and the words (layers pcmenu.c
 *     drew once a version) between, and the canvas is copied over the picture. The art is 16 textures uploaded once.
 *     While the panel opens or closes, the canvas starts instead from the game's picture under it (when it can be read:
 *     the swapchain images with TRANSFER_SRC, or the HDR picture) and the whole panel is sprites over it (the ground and a
 *     flat pearl as textures too), so the wedge can slide in and the rows step in. pcmenu.c then never draws a frame for
 *     the turning wheel; WT_PCMENU_CPU=1 keeps the CPU path (the fallback when the pipeline cannot be made; it has no
 *     opening or closing motion).
 *   - nvn_vk_pcmenu_job: a job of the render thread that pcmenu.c posts about 60 times a second while the menu holds the game:
 *     the work the menu asks for (a sampler flush for anisotropic filtering / texture detail, the present mode for vsync, the images and stages of a new resolution scale), the panel's pixels
 *     into a staging buffer when they changed, then the game's last picture presented again with the panel (vk_present2's
 *     extra present, as the held loading screens do).
 *   - pcm_vk_draw: in every present while the menu is open (also the game's own presents before it is held); one flag read
 *     while it is closed.
 *   - WT_PCMENU_SHOT=<folder> (test aid): every panel change saves the next presented picture, panel included, as
 *     pcmenu_<n>.bmp (needs swapchain images with TRANSFER_SRC; the log says when they have none). */
int pcmenu_is_open(void), pcmenu_visible(void), pcmenu_kind(void), pcmenu_settled(void);      /* kind: 1 the menu, 2 the hint, 3 the first-start screen */
int pcmenu_panel_size(int W, int H, int r[4], unsigned *gen);      /* pcmenu.c: 0 = closed; r = x, y, w, h in the window's pixels */
void pcmenu_panel_read(void *dst, int w, int h);
int pcmenu_take_work(void);                                         /* 1 flush the samplers, 2 present mode again, 4 resolution scale */
typedef struct {                                   /* as pcmenu.c */
    int art; float x, y, w, h, deg, alpha, ann0, ann1;
    float clip_top; int clip_right; float clip2_top; int clip2_right; float y0, y1;
} PcmSprite;
void pcmenu_gpu(int on); int pcmenu_gpu_scene(int W, int H, PcmSprite *out, int max, int *over_game);
void *pcmenu_art_pixels(int id, int *w, int *h); int pcmenu_layer_read(int which, void *dst, int w, int h);
#define PCM_NART 17                                /* pcmenu_art_pixels' ids: 16 pieces of art and a flat pearl (16) */
#ifdef __ANDROID__
#include "touch_controls.h"
#define PCM_TOUCH_N (TOUCH_COUNT + 10)
#else
#define PCM_TOUCH_N 0
#endif
#define PCM_NTEX (PCM_NART + 3 + PCM_TOUCH_N)       /* panel layers followed by cached touch shapes */
#define PCM_NSPR 64
typedef struct { VkImage img; VMem mem; VkImageView view; VkDescriptorSet set; int w, h, ready; } PcmTex;      /* ready: it holds pixels the shaders may read */
static struct {
    int tried, on;                                 /* on: the GPU path; decided at the first upload */
    VkSampler smp; VkDescriptorSetLayout dl; VkPipelineLayout pl; VkDescriptorPool dp; VkShaderModule vs, fs; VkPipeline pipe;
    PcmTex tex[PCM_NTEX];                          /* the art (PCM_NART), then the hubs layer, the words layer and the ground, of the canvas's size */
    VkImage canvas; VMem canvas_mem; VkImageView canvas_view; int cw, ch;
#ifdef __ANDROID__
    int available;
    VkSampler nearest;
    VkPipeline replace;
    VkDescriptorSet canvas_set;
    PcmTex scratch, cpu;
    VkFormat menu_format;
    TouchPresentation placement;
    int bounds[4], cpu_w, cpu_h;
#endif
    VkBuffer art_buf; VMem art_mem; uint64_t art_off[PCM_NART]; int art_staged, art_uploaded;      /* the art's staging: filled once, copied by a present, then freed */
    unsigned layers_gen;                           /* the panel version whose layers the layer textures hold */
    int have[2];                                   /* the staging buffer holds a hubs / words layer for this version */
    unsigned ground_gen;                           /* the panel version whose ground the ground texture holds (~0u: none) */
} g_gpu;
static struct { VkBuffer buf; VMem mem; uint64_t size; unsigned gen; int r[4], ready, panel; } g_pcm;      /* panel: whose pixels these are (pcmenu_kind) */
static struct { VkBuffer buf; VMem mem; uint64_t size; int w, h, pending, n, said; } g_pcm_shot;

/* ---- the GPU path ---- */
/* One pipeline draws every sprite: a quad of half size h about the centre c, turned by rot (cos, sin), its texture
 * premultiplied, at the opacity alpha, over the canvas (ONE, ONE_MINUS_SRC_ALPHA, as pcmenu.c's put_pre); the fragment
 * stage keeps the ring ann (inner and outer radius, in pixels; none when the outer is 0), the side of the slanted edge
 * (clip.x + y * tan 22, the same integer edge as art_blit's; clip.y: 0 all, 1 left of it, -1 right of it), the side of a
 * second such edge (clip2), and the band of rows band.x <= y < band.y (none when band.y <= band.x). */
static const char k_pcm_vs[] =
    "#version 460\n"
    "layout(push_constant) uniform PC { vec2 c; vec2 h; vec2 rot; vec2 view; vec2 ann; vec2 clip; vec2 clip2; vec2 band; float alpha; float pad; vec4 region; } pc;\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "void main() {\n"
    "    const vec2 k[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));\n"
    "    vec2 l = k[gl_VertexIndex] * pc.h;\n"
    "    vec2 p = pc.c + vec2(l.x * pc.rot.x - l.y * pc.rot.y, l.x * pc.rot.y + l.y * pc.rot.x);\n"
    "    vec2 uv = k[gl_VertexIndex] * 0.5 + 0.5; vec2 q = p / pc.view;\n"
    "    if (pc.pad == 2.0) q = vec2(1.0-q.y,q.x);\n"
    "    else if (pc.pad == 4.0) q = vec2(1.0-q.x,1.0-q.y);\n"
    "    else if (pc.pad == 8.0) q = vec2(q.y,1.0-q.x);\n"
    "    if (pc.pad < 0.0) { q = p;\n"
    "        if (pc.pad == -2.0) q = vec2(q.y,1.0-q.x);\n"
    "        else if (pc.pad == -4.0) q = vec2(1.0-q.x,1.0-q.y);\n"
    "        else if (pc.pad == -8.0) q = vec2(1.0-q.y,q.x);\n"
    "        q = (q * pc.view - pc.region.xy) / pc.region.zw; }\n"
    "    v_uv = uv; gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char k_pcm_fs[] =
    "#version 460\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_pic;\n"
    "layout(push_constant) uniform PC { vec2 c; vec2 h; vec2 rot; vec2 view; vec2 ann; vec2 clip; vec2 clip2; vec2 band; float alpha; float pad; vec4 region; } pc;\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out vec4 o_col;\n"
    "void main() {\n"
    "    if (pc.pad < 0.0) { o_col = texture(u_pic, v_uv); return; }\n"
    "    vec2 d = gl_FragCoord.xy - pc.c;\n"
    "    if (pc.ann.y > 0.0) { float rr = dot(d, d); if (rr < pc.ann.x * pc.ann.x || rr > pc.ann.y * pc.ann.y) discard; }\n"
    "    float x = floor(gl_FragCoord.x), y = floor(gl_FragCoord.y);\n"
    "    if (pc.band.y > pc.band.x && (y < pc.band.x || y >= pc.band.y)) discard;\n"
    "    if (pc.clip.y != 0.0) { float e = floor(pc.clip.x + y * 0.4040); if (pc.clip.y > 0.0 ? x >= e : x < e) discard; }\n"
    "    if (pc.clip2.y != 0.0) { float e = floor(pc.clip2.x + y * 0.4040); if (pc.clip2.y > 0.0 ? x >= e : x < e) discard; }\n"
    "    o_col = texture(u_pic, v_uv) * pc.alpha;\n"
    "}\n";
typedef struct { float c[2], h[2], rot[2], view[2], ann[2], clip[2], clip2[2], band[2], alpha, pad, reserved[2], region[4]; } PcmPush;
static void pcm_tex_free(PcmTex *t) {
    if (t->view) vkDestroyImageView(V.device, t->view, NULL);
    if (t->img) { vkDestroyImage(V.device, t->img, NULL); vmem_free(t->mem); }
    t->img = NULL; t->view = NULL; t->w = t->h = t->ready = 0;
}
/* a BGRA8 image with its view; the descriptor set of a texture is written to it (the set is not in use: the device was waited for) */
static void pcm_image_new_format(int w, int h, VkFlags usage, VkFormat format, VkImage *img, VMem *mem, VkImageView *view) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    ci.flags = format == VK_FORMAT_B8G8R8A8_SRGB ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format;
    ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(V.device, &ci, NULL, img));
    vkGetImageMemoryRequirements(V.device, *img, &rq);
    *mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, *img, mem->mem, mem->off));
    if (!view) return;                             /* CPU fallback needs only a transfer image */
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(V.device, &vi, NULL, view));
}
static void pcm_image_new(int w, int h, VkFlags usage, VkImage *img, VMem *mem, VkImageView *view) {
    pcm_image_new_format(w, h, usage, VK_FORMAT_B8G8R8A8_UNORM, img, mem, view);
}
static void pcm_descriptor(VkDescriptorSet set, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo ii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = set; wr.dstBinding = 0; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(V.device, 1, &wr, 0, NULL);
}
static void pcm_tex_new(PcmTex *t, int w, int h) {
    pcm_tex_free(t);
    pcm_image_new(w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &t->img, &t->mem, &t->view);
    t->w = w; t->h = h;
    pcm_descriptor(t->set, t->view, g_gpu.smp);
}
static VkPipeline pcm_gpu_pipeline(void) {
    if (g_gpu.pipe) {
#ifdef __ANDROID__
        return g_gpu.replace ? g_gpu.pipe : NULL;
#else
        return g_gpu.pipe;
#endif
    }
    g_gpu.vs = module_from_glsl(0, k_pcm_vs, "PC menu panel, vertex stage", &g_spv_unused);
    g_gpu.fs = module_from_glsl(1, k_pcm_fs, "PC menu panel, fragment stage", &g_spv_unused);
    if (!g_gpu.vs || !g_gpu.fs) return NULL;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VKC(vkCreateSampler(V.device, &si, NULL, &g_gpu.smp));
#ifdef __ANDROID__
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    VKC(vkCreateSampler(V.device, &si, NULL, &g_gpu.nearest));
#endif
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1; li.pBindings = &b;
    VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &g_gpu.dl));
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PcmPush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &g_gpu.dl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VKC(vkCreatePipelineLayout(V.device, &pli, NULL, &g_gpu.pl));
    int sets = PCM_NTEX;
#ifdef __ANDROID__
    sets += 2;
#endif
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)sets};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = (uint32_t)sets; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    VKC(vkCreateDescriptorPool(V.device, &dpi, NULL, &g_gpu.dp));
    for (int i = 0; i < PCM_NTEX; i++) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_gpu.dp; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_gpu.dl;
        VKC(vkAllocateDescriptorSets(V.device, &ai, &g_gpu.tex[i].set));
    }
#ifdef __ANDROID__
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_gpu.dp; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_gpu.dl;
    VKC(vkAllocateDescriptorSets(V.device, &ai, &g_gpu.canvas_set));
    VKC(vkAllocateDescriptorSets(V.device, &ai, &g_gpu.scratch.set));
#endif
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_gpu.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_gpu.fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = VK_TRUE; ba.colorWriteMask = 0xf;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ren = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ren.colorAttachmentCount = 1; ren.pColorAttachmentFormats = &cf;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ren; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vin; gi.pInputAssemblyState = &ia; gi.pViewportState = &vps;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = g_gpu.pl;
    VkResult r = vkCreateGraphicsPipelines(V.device, NULL, 1, &gi, NULL, &g_gpu.pipe);
    if (r != VK_SUCCESS) { printf("pcmenu: THE DRIVER REFUSED the panel's pipeline (%s): the panel is composed on the CPU\n", vk_result_name(r)); fflush(stdout); g_gpu.pipe = NULL; return NULL; }
#ifdef __ANDROID__
    ba.blendEnable = VK_FALSE;                     /* completed canvas replaces, never blends a second time */
    r = vkCreateGraphicsPipelines(V.device, NULL, 1, &gi, NULL, &g_gpu.replace);
    if (r != VK_SUCCESS) { g_gpu.replace = NULL; return NULL; }
#endif
    return g_gpu.pipe;
}
/* the art into a staging buffer and its textures (render thread, once); the next present copies it across */
static void pcm_gpu_art_stage(void) {
    void *px[PCM_NART];
    int w[PCM_NART], h[PCM_NART];
    uint64_t total = 0;
    g_gpu.art_staged = 1;
    for (int i = 0; i < PCM_NART; i++) {
        px[i] = pcmenu_art_pixels(i, &w[i], &h[i]);
        if (!px[i]) { w[i] = h[i] = 0; continue; }
        g_gpu.art_off[i] = total; total += (uint64_t)w[i] * h[i] * 4;
    }
    if (!total) return;
    g_gpu.art_buf = vbuf_new(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_gpu.art_mem);
    for (int i = 0; i < PCM_NART; i++) {
        if (!px[i]) continue;
        memcpy((uint8_t *)g_gpu.art_mem.map + g_gpu.art_off[i], px[i], (size_t)w[i] * h[i] * 4);
        free(px[i]);
        pcm_tex_new(&g_gpu.tex[i], w[i], h[i]);
    }
}
/* the canvas and the layer textures at the panel's size (render thread; 0 x 0: gone) */
static void pcm_gpu_canvas(int w, int h) {
    if (g_gpu.canvas && g_gpu.cw == w && g_gpu.ch == h) return;
    cmd_wait_all();                                /* a present may still draw with them */
    if (g_gpu.canvas) { vkDestroyImageView(V.device, g_gpu.canvas_view, NULL); vkDestroyImage(V.device, g_gpu.canvas, NULL); vmem_free(g_gpu.canvas_mem); g_gpu.canvas = NULL; g_gpu.canvas_view = NULL; }
    for (int k = 0; k < 3; k++) pcm_tex_free(&g_gpu.tex[PCM_NART + k]);
    g_gpu.cw = w; g_gpu.ch = h; g_gpu.layers_gen = g_gpu.ground_gen = ~0u;
    if (!w || !h) return;
    pcm_image_new(w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &g_gpu.canvas, &g_gpu.canvas_mem, &g_gpu.canvas_view);
#ifdef __ANDROID__
    pcm_descriptor(g_gpu.canvas_set, g_gpu.canvas_view, g_gpu.nearest);
#endif
    for (int k = 0; k < 3; k++) pcm_tex_new(&g_gpu.tex[PCM_NART + k], w, h);
}
static void pcm_tex_upload(PcmTex *t, VkBuffer buf, uint64_t off) {      /* the staging buffer's pixels into the texture, in the present's commands */
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)t->w; c.imageExtent.height = (uint32_t)t->h; c.imageExtent.depth = 1;
    image_barrier(t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(R.cmd, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    image_barrier(t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->ready = 1;
}
#ifdef __ANDROID__
/* UNORM views preserve the encoded panel bytes; blits use the target's colour class. */
static int pcm_menu_images(int w, int h) {
    VkFormat format = V.format.format == VK_FORMAT_B8G8R8A8_SRGB || V.format.format == VK_FORMAT_R8G8B8A8_SRGB
        ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    VkFormatProperties source, local, view;
    vkGetPhysicalDeviceFormatProperties(V.gpu, V.format.format, &source);
    vkGetPhysicalDeviceFormatProperties(V.gpu, format, &local);
    vkGetPhysicalDeviceFormatProperties(V.gpu, VK_FORMAT_B8G8R8A8_UNORM, &view);
    if (!(source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !(local.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return 0;
    int renderable = (source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
        (local.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) &&
        (local.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
        (local.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        (view.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
        (view.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        (view.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (g_gpu.on && !renderable) { g_gpu.available = g_gpu.on = 0; g_pcm.ready = 0; pcmenu_gpu(0); }
    int scratch = g_gpu.on && (V.hdr || g_sc_src) &&
        (source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT);
    if (g_gpu.menu_format != format || g_gpu.cpu.w != w || g_gpu.cpu.h != h ||
        (scratch && (g_gpu.scratch.w != (int)V.extent.width || g_gpu.scratch.h != (int)V.extent.height))) {
        cmd_wait_all();
        pcm_tex_free(&g_gpu.cpu); pcm_tex_free(&g_gpu.scratch);
        g_gpu.menu_format = format;
        pcm_image_new_format(w, h, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             format, &g_gpu.cpu.img, &g_gpu.cpu.mem, NULL);
        g_gpu.cpu.w = w; g_gpu.cpu.h = h;
    }
    if (scratch && !g_gpu.scratch.img) {
        pcm_image_new_format((int)V.extent.width, (int)V.extent.height,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            format, &g_gpu.scratch.img, &g_gpu.scratch.mem, &g_gpu.scratch.view);
        g_gpu.scratch.w = (int)V.extent.width; g_gpu.scratch.h = (int)V.extent.height;
        pcm_descriptor(g_gpu.scratch.set, g_gpu.scratch.view, g_gpu.nearest);
    }
    return 1;
}

static void pcm_menu_sample(VkImageView view, int w, int h, const int bounds[4], VkDescriptorSet set, const PcmPush *pc) {
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = (VkExtent2D){(uint32_t)w, (uint32_t)h}; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    if (pc->pad > 0) { vp.x = (float)bounds[0]; vp.y = (float)bounds[1]; vp.width = (float)bounds[2]; vp.height = (float)bounds[3]; }
    VkRect2D sc = {{bounds[0], bounds[1]}, {(uint32_t)bounds[2], (uint32_t)bounds[3]}};
    vkCmdBeginRendering(R.cmd, &ri);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gpu.replace);
    vkCmdSetViewport(R.cmd, 0, 1, &vp); vkCmdSetScissor(R.cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gpu.pl, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(R.cmd, g_gpu.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *pc, pc);
    vkCmdDraw(R.cmd, 6, 1, 0, 0);
    vkCmdEndRendering(R.cmd);
}
#endif
/* the panel composed on the canvas and copied over the picture at r (in the present's commands, tgt in TRANSFER_DST) */
static void pcm_gpu_draw(VkImage tgt, const int r[4]) {
    uint64_t one = (uint64_t)r[2] * (uint64_t)r[3] * 4;
    if (!g_gpu.art_uploaded && g_gpu.art_buf) { for (int i = 0; i < PCM_NART; i++) if (g_gpu.tex[i].img) pcm_tex_upload(&g_gpu.tex[i], g_gpu.art_buf, g_gpu.art_off[i]); g_gpu.art_uploaded = 1; }
    if (g_gpu.layers_gen != g_pcm.gen) {
        for (int k = 0; k < 2; k++) { PcmTex *t = &g_gpu.tex[PCM_NART + k]; t->ready = 0; if (g_gpu.have[k]) pcm_tex_upload(t, g_pcm.buf, one * (1 + k)); }
        g_gpu.layers_gen = g_pcm.gen;
    }
    PcmSprite sp[PCM_NSPR];
    int over = V.hdr || g_sc_src;                  /* the picture under the panel can be read: the opening and the closing play over it */
    int n = pcmenu_gpu_scene(r[2], r[3], sp, PCM_NSPR, &over);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)r[2]; c.imageExtent.height = (uint32_t)r[3]; c.imageExtent.depth = 1;
    VkImageCopy ic;
    memset(&ic, 0, sizeof ic);
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1; ic.dstSubresource = ic.srcSubresource;
    ic.extent = c.imageExtent;
#ifdef __ANDROID__
    TouchPresentation p = g_gpu.placement;
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; blit.srcSubresource.layerCount = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = blit.dstOffsets[1] = (VkOffset3D){p.width, p.height, 1};
    xfer_wait();
    image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(g_gpu.scratch.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdBlitImage(R.cmd, tgt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_gpu.scratch.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
#endif
    image_barrier(g_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (over) {                                    /* the game's picture under the panel, and the ground as a texture */
        if (g_gpu.ground_gen != g_pcm.gen) { pcm_tex_upload(&g_gpu.tex[PCM_NART + 2], g_pcm.buf, 0); g_gpu.ground_gen = g_pcm.gen; }
#ifdef __ANDROID__
        image_barrier(g_gpu.scratch.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image_barrier(g_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        PcmPush pc = {0};
        pc.c[0] = pc.c[1] = pc.h[0] = pc.h[1] = .5f; pc.rot[0] = 1;
        pc.view[0] = (float)p.sdl_width; pc.view[1] = (float)p.sdl_height; pc.pad = (float)-p.transform;
        for (int i = 0; i < 4; i++) pc.region[i] = (float)r[i];
        int local[4] = {0, 0, r[2], r[3]};
        pcm_menu_sample(g_gpu.canvas_view, r[2], r[3], local, g_gpu.scratch.set, &pc);
#else
        xfer_wait();                               /* after the blit of the game's picture */
        image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        ic.srcOffset.x = r[0]; ic.srcOffset.y = r[1];
        vkCmdCopyImage(R.cmd, tgt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_gpu.canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        ic.srcOffset.x = ic.srcOffset.y = 0;
#endif
    } else vkCmdCopyBufferToImage(R.cmd, g_pcm.buf, g_gpu.canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);      /* the ground */
#ifdef __ANDROID__
    image_barrier(g_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, over ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
#else
    image_barrier(g_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
#endif
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = g_gpu.canvas_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent.width = (uint32_t)r[2]; ri.renderArea.extent.height = (uint32_t)r[3]; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    VkViewport vp = {0, 0, (float)r[2], (float)r[3], 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)r[2], (uint32_t)r[3]}};
    vkCmdBeginRendering(R.cmd, &ri);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gpu.pipe);
    vkCmdSetViewport(R.cmd, 0, 1, &vp); vkCmdSetScissor(R.cmd, 0, 1, &sc);
    for (int i = 0; i < n; i++) {
        const PcmSprite *s = &sp[i];
        PcmTex *t = s->art >= 0 && s->art < PCM_NART ? &g_gpu.tex[s->art] : s->art == -1 ? &g_gpu.tex[PCM_NART] : s->art == -2 ? &g_gpu.tex[PCM_NART + 1]
                  : s->art == -3 ? &g_gpu.tex[PCM_NART + 2] : s->art == -4 ? &g_gpu.tex[PCM_NART - 1] : NULL;
        if (!t || !t->ready) continue;
        float a = s->deg * 3.14159265f / 180;
        PcmPush pc = {{s->x + s->w / 2, s->y + s->h / 2}, {s->w / 2, s->h / 2}, {cosf(a), sinf(a)}, {(float)r[2], (float)r[3]},
                      {s->ann1 > 0 ? s->ann0 * s->w / 2 : 0, s->ann1 > 0 ? s->ann1 * s->w / 2 : 0}, {s->clip_top, s->clip_top > -1e8f ? (s->clip_right ? -1.0f : 1.0f) : 0},
                      {s->clip2_top, s->clip2_top > -1e8f ? (s->clip2_right ? -1.0f : 1.0f) : 0}, {s->y0, s->y1}, s->alpha, 0};      /* (NOCLIP: -1e9) */
        vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gpu.pl, 0, 1, &t->set, 0, NULL);
        vkCmdPushConstants(R.cmd, g_gpu.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(R.cmd, 6, 1, 0, 0);
    }
    vkCmdEndRendering(R.cmd);
    R.pipe_bound = NULL; R.dyn_dirty = 1; R.dyn3_dirty = 1;
    dsh_forget();
#ifdef __ANDROID__
    image_barrier(g_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_barrier(g_gpu.scratch.img, VK_IMAGE_ASPECT_COLOR_BIT,
                  over ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    PcmPush final = {0};
    final.c[0] = r[2] / 2.0f; final.c[1] = r[3] / 2.0f;
    final.h[0] = r[2] / 2.0f; final.h[1] = r[3] / 2.0f; final.rot[0] = 1;
    final.view[0] = (float)r[2]; final.view[1] = (float)r[3];
    final.alpha = 1; final.pad = (float)p.transform;
    pcm_menu_sample(g_gpu.scratch.view, p.width, p.height, g_gpu.bounds, g_gpu.canvas_set, &final);
    R.pipe_bound = NULL; R.dyn_dirty = 1; R.dyn3_dirty = 1; dsh_forget();
    image_barrier(g_gpu.scratch.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    xfer_wait();
    vkCmdBlitImage(R.cmd, g_gpu.scratch.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
#else
    image_barrier(g_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ic.dstOffset.x = r[0]; ic.dstOffset.y = r[1];
    xfer_wait();                                   /* over the blit of the game's picture */
    vkCmdCopyImage(R.cmd, g_gpu.canvas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
#endif
}
#ifdef __ANDROID__
#define TOUCH_TEX (PCM_NART + 3)
#define TOUCH_TEX_SIZE 96
static struct {
    VkImage canvas; VMem canvas_mem; VkImageView canvas_view;
    VkBuffer stage; VMem stage_mem;
    int w, h, uploaded, said, failed;
    VkFormat format;
} g_touch_gpu;
static int g_touch_transform;
static int g_touch_sdl_w, g_touch_sdl_h;

static const unsigned char *touch_glyph(char ch) {
    static const struct { char c; unsigned char row[7]; } glyphs[] = {
        {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
        {'D',{30,17,17,17,17,17,30}}, {'E',{31,16,16,30,16,16,31}},
        {'C',{15,16,16,16,16,16,15}}, {'N',{17,25,25,21,19,19,17}},
        {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
        {'V',{17,17,17,17,17,10,4}}, {'K',{17,18,20,24,20,18,17}},
        {'L',{16,16,16,16,16,16,31}}, {'R',{30,17,17,30,20,18,17}},
        {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
        {'X',{17,17,10,4,10,17,17}}, {'Y',{17,17,10,4,4,4,4}},
        {'Z',{31,1,2,4,8,16,31}}, {'3',{30,1,1,14,1,1,30}},
        {'+',{0,4,4,31,4,4,0}}, {'-',{0,0,0,31,0,0,0}}
    };
    for (size_t i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++) if (glyphs[i].c == ch) return glyphs[i].row;
    return NULL;
}

static void touch_shape(uint8_t *pixels, int id) {
    static const char *const labels[TOUCH_COUNT + 10] = {
        "LS", "RS", "", "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-", "L3", "R3", "SET", "",
        "SZ-", "SZ+", "OP-", "OP+", "SAVE", "CANCEL", "RESET", "ERROR"
    };
    const char *label = labels[id];
    int n = (int)strlen(label), scale = n > 5 ? 2 : n > 4 ? 3 : n > 2 ? 4 : 7, left = (TOUCH_TEX_SIZE - n * 6 * scale + scale) / 2;
    for (int y = 0; y < TOUCH_TEX_SIZE; y++) for (int x = 0; x < TOUCH_TEX_SIZE; x++) {
        float dx = x + .5f - TOUCH_TEX_SIZE / 2, dy = y + .5f - TOUCH_TEX_SIZE / 2;
        float d = sqrtf(dx * dx + dy * dy);
        int r = 44, g = 44, b = 52, alpha = d < 44 ? 136 : 0;
        if (d > 39 && d < 42) { r = 148; g = 116; b = 69; alpha = 190; }
        else if (d >= 42 && d < 44) { r = 230; g = 218; b = 190; alpha = 205; }
        if (id == TOUCH_COUNT && d < 40) { r = 93; g = 24; b = 42; alpha = 165; }
        if (id == TOUCH_DPAD && ((fabsf(dx) < 8 && fabsf(dy) < 28) || (fabsf(dy) < 8 && fabsf(dx) < 28))) { r = 95; g = 71; b = 45; alpha = 225; }
        for (int k = 0; k < n; k++) {
            const unsigned char *rows = touch_glyph(label[k]);
            int gx = (x - left - k * 6 * scale) / scale, gy = (y - (TOUCH_TEX_SIZE - 7 * scale) / 2) / scale;
            if (rows && x >= left + k * 6 * scale && y >= (TOUCH_TEX_SIZE - 7 * scale) / 2 && gx >= 0 && gx < 5 && gy >= 0 && gy < 7 && (rows[gy] & (1 << (4 - gx)))) { r = 255; g = 245; b = 220; alpha = 245; }
        }
        uint8_t *p = pixels + ((size_t)y * TOUCH_TEX_SIZE + x) * 4;
        p[0] = (uint8_t)(b * alpha / 255); p[1] = (uint8_t)(g * alpha / 255); p[2] = (uint8_t)(r * alpha / 255); p[3] = (uint8_t)alpha;
    }
}

static int touch_vk_prepare(VkFormat format) {
    if (g_touch_gpu.failed) return 0;
    VkFormatProperties source, canvas, canvas_view;
    VkFormat canvas_format = format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    vkGetPhysicalDeviceFormatProperties(V.gpu, format, &source);
    vkGetPhysicalDeviceFormatProperties(V.gpu, canvas_format, &canvas);
    vkGetPhysicalDeviceFormatProperties(V.gpu, VK_FORMAT_B8G8R8A8_UNORM, &canvas_view);
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((source.optimalTilingFeatures & required) != required || (canvas.optimalTilingFeatures & required) != required ||
        !(canvas.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !(canvas_view.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        if (!g_touch_gpu.said++) fprintf(stderr, "touch: Vulkan format cannot blit to the overlay; touch input disabled\n");
        touch_runtime_set_ready(0); return 0;
    }
    if (!pcm_gpu_pipeline()) {
        if (!g_touch_gpu.said++) fprintf(stderr, "touch: Vulkan overlay pipeline unavailable; touch input disabled\n");
        g_touch_gpu.failed = 1; g_gpu.tried = 1; g_gpu.on = 0; pcmenu_gpu(0);
        touch_runtime_set_ready(0); return 0;
    }
    if (g_touch_gpu.stage && g_touch_gpu.uploaded) {
        cmd_wait_all();
        vkDestroyBuffer(V.device, g_touch_gpu.stage, NULL); vmem_free(g_touch_gpu.stage_mem); g_touch_gpu.stage = NULL;
    }
    if (!g_gpu.tex[TOUCH_TEX].img) {
        uint64_t one = (uint64_t)TOUCH_TEX_SIZE * TOUCH_TEX_SIZE * 4;
        g_touch_gpu.stage = vbuf_new(one * PCM_TOUCH_N, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_touch_gpu.stage_mem);
        for (int i = 0; i < PCM_TOUCH_N; i++) {
            pcm_tex_new(&g_gpu.tex[TOUCH_TEX + i], TOUCH_TEX_SIZE, TOUCH_TEX_SIZE);
            touch_shape((uint8_t *)g_touch_gpu.stage_mem.map + one * i, i);
        }
    }
    if (!g_touch_gpu.canvas || g_touch_gpu.w != (int)V.extent.width || g_touch_gpu.h != (int)V.extent.height || g_touch_gpu.format != canvas_format) {
        cmd_wait_all();
        if (g_touch_gpu.canvas) {
            vkDestroyImageView(V.device, g_touch_gpu.canvas_view, NULL);
            vkDestroyImage(V.device, g_touch_gpu.canvas, NULL); vmem_free(g_touch_gpu.canvas_mem);
        }
        g_touch_gpu.w = (int)V.extent.width; g_touch_gpu.h = (int)V.extent.height;
        g_touch_gpu.format = canvas_format;
        pcm_image_new_format(g_touch_gpu.w, g_touch_gpu.h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             canvas_format, &g_touch_gpu.canvas, &g_touch_gpu.canvas_mem, &g_touch_gpu.canvas_view);
    }
    return 1;
}

static void touch_vk_quad(TouchRect r, float alpha, int texture) {
    PcmTex *t = &g_gpu.tex[TOUCH_TEX + texture];
    PcmPush pc = {{r.x + r.w / 2, r.y + r.h / 2}, {r.w / 2, r.h / 2}, {1, 0},
                  {(float)g_touch_sdl_w, (float)g_touch_sdl_h}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, alpha, (float)g_touch_transform};
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gpu.pl, 0, 1, &t->set, 0, NULL);
    vkCmdPushConstants(R.cmd, g_gpu.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(R.cmd, 6, 1, 0, 0);
}

static void touch_vk_draw(VkImage tgt) {
    TouchSnapshot s = touch_runtime_snapshot();
    if (!s.valid || !s.presentation.valid || s.width != s.presentation.sdl_width || s.height != s.presentation.sdl_height ||
        s.presentation.width != (int)V.extent.width || s.presentation.height != (int)V.extent.height || (pcmenu_is_open() && !s.editing)) {
        if (!pcmenu_is_open()) touch_runtime_set_ready(0);
        return;
    }
    g_touch_transform = s.presentation.transform;
    g_touch_sdl_w = s.width; g_touch_sdl_h = s.height;
    if (!g_touch_gpu.uploaded && g_touch_gpu.stage) {
        uint64_t one = (uint64_t)TOUCH_TEX_SIZE * TOUCH_TEX_SIZE * 4;
        for (int i = 0; i < PCM_TOUCH_N; i++) pcm_tex_upload(&g_gpu.tex[TOUCH_TEX + i], g_touch_gpu.stage, one * i);
        g_touch_gpu.uploaded = 1;
    }
    VkImageBlit c; memset(&c, 0, sizeof c);
    c.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.srcSubresource.layerCount = 1; c.dstSubresource = c.srcSubresource;
    c.srcOffsets[1] = (VkOffset3D){(int32_t)V.extent.width, (int32_t)V.extent.height, 1};
    c.dstOffsets[1] = c.srcOffsets[1];
    xfer_wait();
    image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(g_touch_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdBlitImage(R.cmd, tgt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_touch_gpu.canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c, VK_FILTER_NEAREST);
    image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    image_barrier(g_touch_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = g_touch_gpu.canvas_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = V.extent; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    VkViewport vp = {0, 0, (float)V.extent.width, (float)V.extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, V.extent};
    vkCmdBeginRendering(R.cmd, &ri);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gpu.pipe);
    vkCmdSetViewport(R.cmd, 0, 1, &vp); vkCmdSetScissor(R.cmd, 0, 1, &sc);
    TouchSprite sprites[TOUCH_SCENE_MAX];
    int n = touch_scene_build(&s, pcmenu_is_open(), sprites);
    for (int i = 0; i < n; i++) {
        touch_vk_quad(sprites[i].rect, sprites[i].opacity, sprites[i].texture);
    }
    vkCmdEndRendering(R.cmd);
    R.pipe_bound = NULL; R.dyn_dirty = 1; R.dyn3_dirty = 1; dsh_forget();
    image_barrier(g_touch_gpu.canvas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    xfer_wait();
    vkCmdBlitImage(R.cmd, g_touch_gpu.canvas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c, VK_FILTER_NEAREST);
    touch_runtime_set_ready(1);
}
#endif
static void pcm_shot_write(void) {                 /* the picture copied by the last present (the GPU has finished: cmd_wait_all) */
    if (g_pcm_shot.pending != 1) return;           /* (-1: a picture asked for, not yet copied by a present) */
    g_pcm_shot.pending = 0;
    char path[300];
    snprintf(path, sizeof path, "%s/pcmenu_%03d.bmp", getenv("WT_PCMENU_SHOT"), g_pcm_shot.n++);
    BmpJob *j = malloc(sizeof *j);
    uint8_t *px = malloc((size_t)g_pcm_shot.w * g_pcm_shot.h * 4);
    if (!j || !px) { free(j); free(px); return; }
    memcpy(px, g_pcm_shot.mem.map, (size_t)g_pcm_shot.w * g_pcm_shot.h * 4);
    snprintf(j->path, sizeof j->path, "%s", path); j->px = px; j->w = g_pcm_shot.w; j->h = g_pcm_shot.h; j->bgr = 1;
    InterlockedIncrement(&g_bmp_pending);
    bmp_write_thread(j);
}
void nvn_vk_pcmenu_job(void *arg) {
    (void)arg;
    if (!V.alive) return;
    int work = pcmenu_take_work();
    if (work || g_pcm_shot.pending) cmd_wait_all();
    pcm_shot_write();
    if (work & 1) {      /* anisotropic filtering, texture_lod_bias: new samplers from now on; the old ones may still be named by cached descriptor sets */
        if (g_nsmp_old + g_nsmp <= (int)(sizeof g_smp_old / sizeof g_smp_old[0])) {
            for (int i = 0; i < g_nsmp; i++) g_smp_old[g_nsmp_old++] = g_smp[i].s;
            printf("pcmenu: %d samplers made again from now on (anisotropic_filtering %d, texture_lod_bias %.2f)\n", g_nsmp, gfx_options()->aniso, gfx_options()->texture_lod_bias);
            g_nsmp = 0;
        } else printf("pcmenu: too many samplers kept already: anisotropic_filtering applies to new samplers only (all at the next start)\n");
    }
    if (work & 2) { present_mode_pick(); if (V.swapchain) swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); }
    if (work & 4) rs_apply();
#ifdef __ANDROID__
    if (!touch_runtime_editing())
#endif
        pcm_upload(!work && !g_pcm_shot.pending);
    if (g_pcm_last.win && pcmenu_is_open()) vk_present2(&g_pcm_last, 1);      /* (the hint alone rides on the game's presents) */
}
/* the panel's pixels into the staging buffer when they changed (render thread; also the first-start screen, prep_first_hold) */
static void pcm_upload(int wait) {
    int r[4];
    unsigned gen;
    if (!g_gpu.tried) {                            /* the GPU path, unless it cannot be made */
        g_gpu.tried = 1;
        const char *e = getenv("WT_PCMENU_CPU");
        g_gpu.on = !(e && atoi(e)) && pcm_gpu_pipeline() != NULL;
#ifdef __ANDROID__
        g_gpu.available = g_gpu.on;
#endif
        pcmenu_gpu(g_gpu.on);
        printf("pcmenu: the panel is composed on the %s\n", g_gpu.on ? "GPU" : "CPU"); fflush(stdout);
    }
#ifdef __ANDROID__
    /* Without readable target pixels the scratch pass cannot preserve the game around the panel. */
    int gpu_on = g_gpu.available && (V.hdr || g_sc_src);
    if (g_gpu.on != gpu_on) { g_gpu.on = gpu_on; g_pcm.ready = 0; pcmenu_gpu(gpu_on); }
#endif
    if (g_gpu.art_buf && g_gpu.art_uploaded) {     /* the art reached its textures: the staging goes */
        if (wait) cmd_wait_all();
        vkDestroyBuffer(V.device, g_gpu.art_buf, NULL); vmem_free(g_gpu.art_mem); g_gpu.art_buf = VK_NULL_HANDLE;
    }
    int panel_w = (int)V.extent.width, panel_h = (int)V.extent.height;
#ifdef __ANDROID__
    TouchSnapshot snapshot = touch_runtime_snapshot();
    TouchPresentation p = snapshot.presentation;
    if (!p.valid || p.width != panel_w || p.height != panel_h ||
        snapshot.width != p.sdl_width || snapshot.height != p.sdl_height) { g_pcm.ready = 0; return; }
    if (p.generation != g_gpu.placement.generation) g_pcm.ready = 0;
    panel_w = p.sdl_width; panel_h = p.sdl_height;
#endif
    int panel = pcmenu_panel_size(panel_w, panel_h, r, &gen);
#ifdef __ANDROID__
    int bounds[4];
    if (!panel || !touch_presentation_bounds(&p, r, bounds)) { g_pcm.ready = 0; return; }
    int cpu_w = p.transform == 2 || p.transform == 8 ? r[3] : r[2];
    int cpu_h = p.transform == 2 || p.transform == 8 ? r[2] : r[3];
    int was_gpu = g_gpu.on;
    if (!pcm_menu_images(cpu_w, cpu_h)) { g_pcm.ready = 0; return; }
    if (was_gpu && !g_gpu.on) return;              /* next job redraws the complete CPU panel, not the GPU ground */
#endif
    if (panel && (gen != g_pcm.gen || !g_pcm.ready)) {
        uint64_t one = (uint64_t)r[2] * (uint64_t)r[3] * 4, need = g_gpu.on ? 3 * one : one;
#ifdef __ANDROID__
        if (g_gpu.on) need += one;                 /* local rotated CPU fallback, never a full-screen upload */
#endif
        if (wait) cmd_wait_all();                          /* the staging buffer may be read by a present in flight */
        if (need > g_pcm.size) {
            if (g_pcm.buf) { vkDestroyBuffer(V.device, g_pcm.buf, NULL); vmem_free(g_pcm.mem); }
            g_pcm.buf = vbuf_new(need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_pcm.mem); g_pcm.size = need;
        }
        pcmenu_panel_read(g_pcm.mem.map, r[2], r[3]);
#ifdef __ANDROID__
        void *rotated = malloc((size_t)one);
        if (!rotated) { g_pcm.ready = 0; return; }
        int rotated_ok = touch_bitmap_rotate(g_pcm.mem.map, (size_t)one, rotated, (size_t)one,
                                               r[2], r[3], p.transform, &g_gpu.cpu_w, &g_gpu.cpu_h);
        if (rotated_ok) memcpy((uint8_t *)g_pcm.mem.map + (g_gpu.on ? 3 * one : 0), rotated, (size_t)one);
        free(rotated);
        if (!rotated_ok) { g_pcm.ready = 0; return; }
        g_gpu.cpu.ready = 0;
        g_gpu.placement = p;
        memcpy(g_gpu.bounds, bounds, sizeof bounds);
#endif
        if (g_gpu.on) {                            /* the layers after the ground; the canvas and the layer textures at the panel's size */
            g_gpu.have[0] = pcmenu_layer_read(1, (uint8_t *)g_pcm.mem.map + one, r[2], r[3]);
            g_gpu.have[1] = pcmenu_layer_read(2, (uint8_t *)g_pcm.mem.map + 2 * one, r[2], r[3]);
            if (!g_gpu.art_staged) pcm_gpu_art_stage();
            pcm_gpu_canvas(r[2], r[3]);
        }
        memcpy(g_pcm.r, r, sizeof r); g_pcm.gen = gen; g_pcm.ready = 1; g_pcm.panel = pcmenu_kind();
        if (getenv("WT_PCMENU_SHOT")) g_pcm_shot.pending = -1;       /* the next present */
    }
}
static void pcm_vk_draw(VkImage tgt) {
    if (!g_pcm.ready || g_pcm.panel != pcmenu_kind()) return;      /* (not another screen's pixels before the next job) */
    int W = (int)V.extent.width, H = (int)V.extent.height, *r = g_pcm.r;
#ifdef __ANDROID__
    TouchPresentation p = touch_runtime_snapshot().presentation;
    if (!p.valid || p.generation != g_gpu.placement.generation || p.width != W || p.height != H ||
        !touch_presentation_bounds(&p, r, g_gpu.bounds)) return;
#else
    if (r[0] < 0 || r[1] < 0 || r[0] + r[2] > W || r[1] + r[3] > H) return;      /* made for another window size: the next job makes it again */
#endif
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageExtent.width = (uint32_t)r[2]; c.imageExtent.height = (uint32_t)r[3]; c.imageExtent.depth = 1;
    int gpu = g_gpu.on && g_pcm.panel != 2 && g_gpu.canvas && g_gpu.cw == r[2] && g_gpu.ch == r[3];
#ifdef __ANDROID__
    gpu = gpu && g_gpu.scratch.img && (V.hdr || g_sc_src);
    if (g_gpu.on && g_pcm.panel != 2 && !gpu) return; /* next upload switches modes; GPU ground alone is not a CPU panel */
#endif
    if (gpu) pcm_gpu_draw(tgt, r);                  /* (the hint is copied as drawn) */
    else {
        xfer_wait();                               /* over the blit of the game's picture */
#ifdef __ANDROID__
        if (!g_gpu.cpu.ready) {
            c.bufferOffset = g_gpu.on ? (uint64_t)r[2] * r[3] * 4 * 3 : 0;
            c.imageOffset.x = c.imageOffset.y = 0;
            c.imageExtent = (VkExtent3D){(uint32_t)g_gpu.cpu_w, (uint32_t)g_gpu.cpu_h, 1};
            image_barrier(g_gpu.cpu.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(R.cmd, g_pcm.buf, g_gpu.cpu.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            image_barrier(g_gpu.cpu.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            g_gpu.cpu.ready = 1;
        }
        VkImageBlit blit = {0};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; blit.srcSubresource.layerCount = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.srcOffsets[1] = (VkOffset3D){g_gpu.cpu_w, g_gpu.cpu_h, 1};
        blit.dstOffsets[0] = (VkOffset3D){g_gpu.bounds[0], g_gpu.bounds[1], 0};
        blit.dstOffsets[1] = (VkOffset3D){g_gpu.bounds[0] + g_gpu.bounds[2], g_gpu.bounds[1] + g_gpu.bounds[3], 1};
        vkCmdBlitImage(R.cmd, g_gpu.cpu.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
#else
        vkCmdCopyBufferToImage(R.cmd, g_pcm.buf, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
#endif
    }
    if (g_pcm_shot.pending == -1 && pcmenu_settled()) {      /* (the menu: once it has opened, not its slide's first frame) */
        c.bufferOffset = 0;
        g_pcm_shot.pending = 0;
        if (V.hdr || !g_sc_src) { if (!g_pcm_shot.said++) printf("pcmenu: WT_PCMENU_SHOT: the swapchain images cannot be read (no TRANSFER_SRC, or HDR): no picture\n"); return; }
        uint64_t need = (uint64_t)W * H * 4;
        if (need > g_pcm_shot.size) {
            if (g_pcm_shot.buf) { vkDestroyBuffer(V.device, g_pcm_shot.buf, NULL); vmem_free(g_pcm_shot.mem); }
            g_pcm_shot.buf = vbuf_new(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_pcm_shot.mem); g_pcm_shot.size = need;
        }
        image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        c.imageOffset.x = c.imageOffset.y = 0; c.imageExtent.width = (uint32_t)W; c.imageExtent.height = (uint32_t)H;
        vkCmdCopyImageToBuffer(R.cmd, tgt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_pcm_shot.buf, 1, &c);
        image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        g_pcm_shot.w = W; g_pcm_shot.h = H; g_pcm_shot.pending = 1;      /* written by the next job, after its wait */
    }
}
static void pcm_vk_free(void) {                    /* the shutdown, the device idle */
#ifdef __ANDROID__
    if (g_gpu.nearest) vkDestroySampler(V.device, g_gpu.nearest, NULL);
    g_gpu.nearest = NULL;
    pcm_tex_free(&g_gpu.scratch); pcm_tex_free(&g_gpu.cpu);
    if (g_gpu.replace) vkDestroyPipeline(V.device, g_gpu.replace, NULL);
    g_gpu.replace = NULL;
    touch_runtime_set_ready(0);
    if (g_touch_gpu.stage) { vkDestroyBuffer(V.device, g_touch_gpu.stage, NULL); vmem_free(g_touch_gpu.stage_mem); }
    if (g_touch_gpu.canvas) { vkDestroyImageView(V.device, g_touch_gpu.canvas_view, NULL); vkDestroyImage(V.device, g_touch_gpu.canvas, NULL); vmem_free(g_touch_gpu.canvas_mem); }
    memset(&g_touch_gpu, 0, sizeof g_touch_gpu);
#endif
    if (g_pcm.buf) { vkDestroyBuffer(V.device, g_pcm.buf, NULL); vmem_free(g_pcm.mem); g_pcm.buf = VK_NULL_HANDLE; }
    if (g_pcm_shot.buf) { vkDestroyBuffer(V.device, g_pcm_shot.buf, NULL); vmem_free(g_pcm_shot.mem); g_pcm_shot.buf = VK_NULL_HANDLE; }
    if (g_gpu.art_buf) { vkDestroyBuffer(V.device, g_gpu.art_buf, NULL); vmem_free(g_gpu.art_mem); g_gpu.art_buf = VK_NULL_HANDLE; }
    for (int i = 0; i < PCM_NTEX; i++) pcm_tex_free(&g_gpu.tex[i]);
    pcm_gpu_canvas(0, 0);
    if (g_gpu.pipe) vkDestroyPipeline(V.device, g_gpu.pipe, NULL);
    if (g_gpu.pl) vkDestroyPipelineLayout(V.device, g_gpu.pl, NULL);
    if (g_gpu.dp) vkDestroyDescriptorPool(V.device, g_gpu.dp, NULL);
    if (g_gpu.dl) vkDestroyDescriptorSetLayout(V.device, g_gpu.dl, NULL);
    if (g_gpu.smp) vkDestroySampler(V.device, g_gpu.smp, NULL);
    if (g_gpu.vs) vkDestroyShaderModule(V.device, g_gpu.vs, NULL);
    if (g_gpu.fs) vkDestroyShaderModule(V.device, g_gpu.fs, NULL);
    g_gpu.pipe = NULL; g_gpu.pl = NULL; g_gpu.dp = NULL; g_gpu.dl = NULL; g_gpu.smp = NULL; g_gpu.vs = g_gpu.fs = NULL;
}
