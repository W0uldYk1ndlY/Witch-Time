#ifndef WT_NVN_VK_DRAW_SUBMIT_H
#define WT_NVN_VK_DRAW_SUBMIT_H

/* Render-thread helpers included by nvn_vk_draw.c. Cached descriptor sets are
 * immutable and belong to the current command buffer's pools. Full descriptor
 * identities confirm every hash hit; a crowded cache simply allocates a new set.
 * No guest draw, buffer preparation, resource barrier or resource lifetime is
 * skipped by these helpers. Set 0 uses STORAGE_BUFFER, never dynamic offsets. */
#define DRAW_BSET_SLOTS 2048u
#define DRAW_BSET_VALUES 32768u
static struct {
    uint64_t key;
    VkDescriptorSetLayout layout;
    VkDescriptorSet set;
    uint32_t epoch, count, off;
} g_draw_bsets[DRAW_BSET_SLOTS];
static VkDescriptorBufferInfo g_draw_bset_values[DRAW_BSET_VALUES];
static uint32_t g_draw_bset_bindings[DRAW_BSET_VALUES];
static uint32_t g_draw_bset_epoch, g_draw_bset_values_used;
static unsigned g_draw_bset_last = DRAW_BSET_SLOTS;
static int g_draw_bset_on = -1;
static LONG64 g_draw_bset_hits, g_draw_bset_misses, g_draw_bset_uncached;

static int draw_buffer_sets_on(void) {
    if (g_draw_bset_on < 0) g_draw_bset_on = !(getenv("WT_VK_BUFFERSETS") && !atoi(getenv("WT_VK_BUFFERSETS")));
    return g_draw_bset_on;
}
static void draw_buffer_sets_forget(void) {
    if (++g_draw_bset_epoch == 0) {
        memset(g_draw_bsets, 0, sizeof g_draw_bsets);
        g_draw_bset_epoch = 1;
    }
    g_draw_bset_values_used = 0;
    g_draw_bset_last = DRAW_BSET_SLOTS;
}
static int draw_buffer_infos_equal(uint32_t count, const uint32_t *ab, const VkDescriptorBufferInfo *a,
                                   const uint32_t *bb, const VkDescriptorBufferInfo *b) {
    for (uint32_t i = 0; i < count; i++)
        if (ab[i] != bb[i] || a[i].buffer != b[i].buffer || a[i].offset != b[i].offset || a[i].range != b[i].range) return 0;
    return 1;
}
static VkDescriptorSet draw_buffer_set_find(VkDescriptorSetLayout layout, uint32_t count, const uint32_t *bindings,
                                           const VkDescriptorBufferInfo *values, unsigned *slot, uint64_t *key) {
    *slot = DRAW_BSET_SLOTS;
    *key = 0;
    if (!draw_buffer_sets_on()) return VK_NULL_HANDLE;
    unsigned last = g_draw_bset_last;
    if (last < DRAW_BSET_SLOTS && g_draw_bsets[last].epoch == g_draw_bset_epoch &&
        g_draw_bsets[last].layout == layout && g_draw_bsets[last].count == count &&
        draw_buffer_infos_equal(count, bindings, values, &g_draw_bset_bindings[g_draw_bsets[last].off],
                                &g_draw_bset_values[g_draw_bsets[last].off])) {
        g_draw_bset_hits++;
        return g_draw_bsets[last].set;
    }
    uint64_t h = (UINT64_C(0xcbf29ce484222325) ^ (uint64_t)(uintptr_t)layout) * UINT64_C(0x100000001b3);
    h = (h ^ count) * UINT64_C(0x100000001b3);
    for (uint32_t i = 0; i < count; i++) {
        h = (h ^ bindings[i]) * UINT64_C(0x100000001b3);
        h = (h ^ (uint64_t)(uintptr_t)values[i].buffer) * UINT64_C(0x100000001b3);
        h = (h ^ values[i].offset) * UINT64_C(0x100000001b3);
        h = (h ^ values[i].range) * UINT64_C(0x100000001b3);
    }
    /* Buffer handles and offsets are aligned: mix their high bits into the slot. */
    h ^= h >> 33; h *= UINT64_C(0xff51afd7ed558ccd); h ^= h >> 33;
    *key = h;
    unsigned pos = (unsigned)h & (DRAW_BSET_SLOTS - 1);
    for (unsigned probe = 0; probe < 32; probe++, pos = (pos + 1) & (DRAW_BSET_SLOTS - 1)) {
        if (g_draw_bsets[pos].epoch != g_draw_bset_epoch) { *slot = pos; break; }
        if (g_draw_bsets[pos].key == h && g_draw_bsets[pos].layout == layout && g_draw_bsets[pos].count == count &&
            draw_buffer_infos_equal(count, bindings, values, &g_draw_bset_bindings[g_draw_bsets[pos].off], &g_draw_bset_values[g_draw_bsets[pos].off])) {
            g_draw_bset_hits++;
            g_draw_bset_last = pos;
            return g_draw_bsets[pos].set;
        }
    }
    g_draw_bset_misses++;
    return VK_NULL_HANDLE;
}
static void draw_buffer_set_save(unsigned slot, uint64_t key, VkDescriptorSetLayout layout, VkDescriptorSet set,
                                 uint32_t count, const uint32_t *bindings, const VkDescriptorBufferInfo *values) {
    if (slot >= DRAW_BSET_SLOTS || count > DRAW_BSET_VALUES - g_draw_bset_values_used) { g_draw_bset_uncached++; return; }
    uint32_t off = g_draw_bset_values_used;
    g_draw_bsets[slot].key = key; g_draw_bsets[slot].layout = layout; g_draw_bsets[slot].set = set;
    g_draw_bsets[slot].epoch = g_draw_bset_epoch; g_draw_bsets[slot].count = count; g_draw_bsets[slot].off = off;
    if (count) {
        memcpy(&g_draw_bset_bindings[off], bindings, count * sizeof bindings[0]);
        memcpy(&g_draw_bset_values[off], values, count * sizeof values[0]);
    }
    g_draw_bset_values_used += count;
    g_draw_bset_last = slot;
}
static void draw_buffer_writes(VkDescriptorSet set, uint32_t count, const uint32_t *bindings,
                               const VkDescriptorBufferInfo *values, VkWriteDescriptorSet *writes) {
    for (uint32_t i = 0; i < count; i++)
        writes[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, bindings[i], 0, 1,
                                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &values[i], NULL};
}

/* Graphics state is local to one command buffer. The existing dsh_forget sites
 * also cover the renderer's own stencil, motion-vector, menu and upscale passes.
 * Layout identity is deliberately stricter than Vulkan layout compatibility. */
static struct {
    int vertex_ok, push_ok, pc_ok;
    uint32_t nb, na, push_count;
    uint32_t buffer_valid;
    int index_ok;
    VkBuffer index_buffer;
    VkDeviceSize index_offset;
    VkIndexType index_type;
    VkVertexInputBindingDescription2EXT bd[17];
    VkVertexInputAttributeDescription2EXT ad[16];
    VkBuffer buffers[17];
    VkDeviceSize offsets[17];
    VkPipelineLayout descriptor_layout, pc_layout;
    VkDescriptorSet sets[2];
    VkDescriptorBufferInfo push_values[160];
    uint32_t push_bindings[160], pc[5];
} g_draw_shadow;
static int g_draw_shadow_on = -1;
static LONG64 g_draw_shadow_kept[5];            /* vertex input, vertex buffers, set binds, pushed buffers, constants */
static int draw_shadow_on(void) {
    if (g_draw_shadow_on < 0) g_draw_shadow_on = !(getenv("WT_VK_DRAWSHADOW") && !atoi(getenv("WT_VK_DRAWSHADOW")));
    return g_draw_shadow_on;
}
static void draw_state_forget(void) {
    g_draw_shadow.vertex_ok = g_draw_shadow.push_ok = g_draw_shadow.pc_ok = 0;
    g_draw_shadow.buffer_valid = 0; g_draw_shadow.index_ok = 0;
    g_draw_shadow.descriptor_layout = VK_NULL_HANDLE;
    g_draw_shadow.sets[0] = g_draw_shadow.sets[1] = VK_NULL_HANDLE;
}
static int draw_vertex_equal(uint32_t nb, const VkVertexInputBindingDescription2EXT *bd,
                             uint32_t na, const VkVertexInputAttributeDescription2EXT *ad) {
    if (!g_draw_shadow.vertex_ok || g_draw_shadow.nb != nb || g_draw_shadow.na != na) return 0;
    for (uint32_t i = 0; i < nb; i++) {
        const VkVertexInputBindingDescription2EXT *a = &g_draw_shadow.bd[i], *b = &bd[i];
        if (a->sType != b->sType || a->pNext != b->pNext || a->binding != b->binding || a->stride != b->stride ||
            a->inputRate != b->inputRate || a->divisor != b->divisor || b->pNext) return 0;
    }
    for (uint32_t i = 0; i < na; i++) {
        const VkVertexInputAttributeDescription2EXT *a = &g_draw_shadow.ad[i], *b = &ad[i];
        if (a->sType != b->sType || a->pNext != b->pNext || a->location != b->location || a->binding != b->binding ||
            a->format != b->format || a->offset != b->offset || b->pNext) return 0;
    }
    return 1;
}
static void draw_vertex_state(uint32_t nb, const VkVertexInputBindingDescription2EXT *bd,
                              uint32_t na, const VkVertexInputAttributeDescription2EXT *ad,
                              const VkBuffer *buffers, const VkDeviceSize *offsets) {
    int shadow = draw_shadow_on();
    if (D.has_vids) {
        if (!shadow || !draw_vertex_equal(nb, bd, na, ad)) {
            vkCmdSetVertexInputEXT(R.cmd, nb, bd, na, ad);
            if (nb) memcpy(g_draw_shadow.bd, bd, nb * sizeof bd[0]);
            if (na) memcpy(g_draw_shadow.ad, ad, na * sizeof ad[0]);
            g_draw_shadow.nb = nb; g_draw_shadow.na = na; g_draw_shadow.vertex_ok = 1;
        } else g_draw_shadow_kept[0]++;
    }
    if (nb) {
        uint32_t dirty = 0;
        for (uint32_t i = 0; i < nb; i++)
            if (!shadow || !(g_draw_shadow.buffer_valid >> i & 1u) ||
                g_draw_shadow.buffers[i] != buffers[i] || g_draw_shadow.offsets[i] != offsets[i]) dirty |= 1u << i;
        if (!dirty) g_draw_shadow_kept[1]++;
        for (uint32_t first = 0; first < nb;) {
            if (!(dirty >> first & 1u)) { first++; continue; }
            uint32_t end = first + 1;
            while (end < nb && (dirty >> end & 1u)) end++;
            vkCmdBindVertexBuffers(R.cmd, first, end - first, buffers + first, offsets + first);
            memcpy(g_draw_shadow.buffers + first, buffers + first, (end - first) * sizeof buffers[0]);
            memcpy(g_draw_shadow.offsets + first, offsets + first, (end - first) * sizeof offsets[0]);
            first = end;
        }
        g_draw_shadow.buffer_valid |= (1u << nb) - 1u;
    }
}
static void draw_index_state(VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    if (draw_shadow_on() && g_draw_shadow.index_ok && g_draw_shadow.index_buffer == buffer &&
        g_draw_shadow.index_offset == offset && g_draw_shadow.index_type == type) return;
    vkCmdBindIndexBuffer(R.cmd, buffer, offset, type);
    g_draw_shadow.index_buffer = buffer; g_draw_shadow.index_offset = offset;
    g_draw_shadow.index_type = type; g_draw_shadow.index_ok = 1;
}
static void draw_descriptor_layout(VkPipelineLayout layout) {
    if (g_draw_shadow.descriptor_layout != layout) {
        g_draw_shadow.descriptor_layout = layout;
        g_draw_shadow.sets[0] = g_draw_shadow.sets[1] = VK_NULL_HANDLE;
        g_draw_shadow.push_ok = 0;
    }
}
static void draw_push_buffers(VkPipelineLayout layout, uint32_t count, const uint32_t *bindings,
                              const VkDescriptorBufferInfo *values, VkWriteDescriptorSet *writes) {
    draw_descriptor_layout(layout);
    if (!count) return;
    if (draw_shadow_on() && g_draw_shadow.push_ok && g_draw_shadow.push_count == count &&
        draw_buffer_infos_equal(count, bindings, values, g_draw_shadow.push_bindings, g_draw_shadow.push_values)) {
        g_draw_shadow_kept[3]++;
        return;
    }
    draw_buffer_writes(VK_NULL_HANDLE, count, bindings, values, writes);
    vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, count, writes);
    memcpy(g_draw_shadow.push_bindings, bindings, count * sizeof bindings[0]);
    memcpy(g_draw_shadow.push_values, values, count * sizeof values[0]);
    g_draw_shadow.push_count = count; g_draw_shadow.push_ok = 1;
}
static void draw_bind_sets(VkPipelineLayout layout, const VkDescriptorSet sets[2]) {
    draw_descriptor_layout(layout);
    int shadow = draw_shadow_on();
    if (!shadow) {                               /* exact pre-shadow command sequence for A/B measurements */
        for (uint32_t i = 0; i < 2; i++) if (sets[i]) {
            vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, i, 1, &sets[i], 0, NULL);
            g_draw_shadow.sets[i] = sets[i];
        }
        return;
    }
    int b0 = sets[0] && sets[0] != g_draw_shadow.sets[0];
    int b1 = sets[1] && sets[1] != g_draw_shadow.sets[1];
    if (b0 || b1) {
        uint32_t first = b0 ? 0 : 1, count = b0 && b1 ? 2 : 1;
        vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, count, &sets[first], 0, NULL);
        if (b0) g_draw_shadow.sets[0] = sets[0];
        if (b1) g_draw_shadow.sets[1] = sets[1];
        if (b0 && b1) g_draw_shadow_kept[2]++;     /* two consecutive set binds represented by one Vulkan command */
    }
    if (sets[0] && !b0) g_draw_shadow_kept[2]++;
    if (sets[1] && !b1) g_draw_shadow_kept[2]++;
}
static void draw_push_constants(VkPipelineLayout layout, const uint32_t pc[5]) {
    if (draw_shadow_on() && g_draw_shadow.pc_ok && g_draw_shadow.pc_layout == layout && !memcmp(g_draw_shadow.pc, pc, sizeof g_draw_shadow.pc)) {
        g_draw_shadow_kept[4]++;
        return;
    }
    vkCmdPushConstants(R.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof g_draw_shadow.pc, pc);
    memcpy(g_draw_shadow.pc, pc, sizeof g_draw_shadow.pc);
    g_draw_shadow.pc_layout = layout; g_draw_shadow.pc_ok = 1;
}

#endif
