#!/usr/bin/env python3
"""Compile and run the production draw submission helpers with Vulkan call mocks.

The cache/state implementation is included directly. The production descriptor
submission function is extracted from nvn_vk_draw.c, so allocation/update/bind
counts exercise the renderer's function rather than a test-side copy.
Set VULKAN_INCLUDE_DIR if Vulkan headers are outside the usual include paths.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def extract_function(source, name):
    match = re.search(r"^static [^\n]*\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source, re.M)
    if match is None:
        raise AssertionError(f"production function {name} is missing")
    start = match.start()
    level = 1
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(source, match.end()):
        if token.group() == "{":
            level += 1
        elif token.group() == "}":
            level -= 1
            if level == 0:
                return source[start:token.end()]
    raise AssertionError(f"unclosed production function {name}")


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
typedef int64_t LONG64;
static struct { VkCommandBuffer cmd; LONG64 sets_reused; } R;
static struct { VkDevice device; } V;
static struct { int has_vids; } D;
static unsigned allocs, updates, template_updates, desc_binds, push_buffers, vi_calls, vb_calls, pc_calls;
static unsigned last_first_set, last_set_count, last_buffer_count, index_calls;
static VkDescriptorSet last_sets[2];
static VkWriteDescriptorSet last_writes[160];
static VkDescriptorBufferInfo last_infos[160];
static uint32_t last_pc[5];
static uint64_t handle_seq = 100;
static VkDescriptorUpdateTemplate mock_template;
static VkDescriptorSet set_alloc(VkDescriptorSetLayout layout) {
    assert(layout != VK_NULL_HANDLE); ++allocs;
    return (VkDescriptorSet)(uintptr_t)++handle_seq;
}
static void vkUpdateDescriptorSets(VkDevice device, uint32_t n, const VkWriteDescriptorSet *writes, uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)device; (void)copy; assert(copies == 0); assert(n <= 160); ++updates;
    last_buffer_count = n;
    for (uint32_t i = 0; i < n; ++i) {
        assert(writes[i].sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        assert(writes[i].dstSet != VK_NULL_HANDLE);
        assert(writes[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        assert(writes[i].descriptorCount == 1 && writes[i].dstArrayElement == 0);
        assert(writes[i].pNext == NULL && writes[i].pImageInfo == NULL && writes[i].pTexelBufferView == NULL);
        last_writes[i] = writes[i]; last_infos[i] = *writes[i].pBufferInfo;
    }
}
static void vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate update_template, const void *data) {
    (void)device; assert(set && update_template && data); ++template_updates;
}
static void vkCmdBindDescriptorSets(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout,
                                   uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t offsets, const uint32_t *dynamic) {
    (void)command; assert(point == VK_PIPELINE_BIND_POINT_GRAPHICS && layout);
    assert(first + count <= 2 && offsets == 0 && dynamic == NULL); ++desc_binds;
    last_first_set = first; last_set_count = count;
    for (uint32_t i = 0; i < count; ++i) last_sets[first + i] = sets[i];
}
static void vkCmdPushDescriptorSetKHR(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout,
                                    uint32_t set, uint32_t n, const VkWriteDescriptorSet *writes) {
    (void)command; assert(point == VK_PIPELINE_BIND_POINT_GRAPHICS && layout && set == 0 && n <= 160);
    ++push_buffers; last_buffer_count = n;
    for (uint32_t i = 0; i < n; ++i) {
        assert(writes[i].dstSet == VK_NULL_HANDLE && writes[i].descriptorCount == 1);
        last_writes[i] = writes[i]; last_infos[i] = *writes[i].pBufferInfo;
    }
}
static void vkCmdSetVertexInputEXT(VkCommandBuffer command, uint32_t nb, const VkVertexInputBindingDescription2EXT *bd,
                                 uint32_t na, const VkVertexInputAttributeDescription2EXT *ad) {
    (void)command; assert(nb <= 17 && na <= 16); assert((!nb || bd) && (!na || ad)); ++vi_calls;
}
static void vkCmdBindVertexBuffers(VkCommandBuffer command, uint32_t first, uint32_t n, const VkBuffer *buffers, const VkDeviceSize *offsets) {
    (void)command; assert(first + n <= 17 && buffers && offsets); ++vb_calls;
}
static void vkCmdBindIndexBuffer(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    (void)command; (void)buffer; (void)offset; (void)type; ++index_calls;
}
static void vkCmdPushConstants(VkCommandBuffer command, VkPipelineLayout layout, VkShaderStageFlags stages,
                               uint32_t offset, uint32_t size, const void *values) {
    (void)command; assert(layout && stages == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT));
    assert(offset == 0 && size == sizeof last_pc); memcpy(last_pc, values, size); ++pc_calls;
}
typedef struct { int unused; } VStage;
typedef struct { VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; } Pipe;
static VkDescriptorUpdateTemplate set0_template(VkDescriptorSetLayout layout, const VStage *vs, const VStage *fs) {
    (void)layout; (void)vs; (void)fs; return mock_template;
}
static int g_ptm_on;
static LONG64 g_ptm[12], g_ptm_t, g_set0_upd;
static LONG64 tk(void) { static LONG64 ticks; return ++ticks; }
#include "nvn_vk_draw_submit.h"
'''

TEST_MAIN = r'''
static VkDescriptorSetLayout dl(uintptr_t value) { return (VkDescriptorSetLayout)value; }
static VkPipelineLayout pl(uintptr_t value) { return (VkPipelineLayout)value; }
static VkBuffer buffer(uintptr_t value) { return (VkBuffer)value; }
static void clear_calls(void) {
    allocs = updates = template_updates = desc_binds = push_buffers = vi_calls = vb_calls = pc_calls = 0;
}
int main(void) {
    uint32_t bindings[3] = { 1, 3, 43 };
    VkDescriptorBufferInfo infos[3] = {{buffer(11), 0, 64}, {buffer(12), 256, 128}, {buffer(13), 512, 256}};
    VkWriteDescriptorSet writes[160];
    Pipe p = {pl(1), {dl(2), dl(3)}, {3, 1}, 0};
    VStage stage = {0};
    draw_buffer_sets_forget(); draw_state_forget();
    g_draw_shadow_on = 1; g_draw_bset_on = 1; D.has_vids = 1;

    /* Reusing a set must never update descriptors already recorded by earlier draws. */
    VkDescriptorSet first = draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes);
    assert(first && allocs == 1 && updates == 1 && template_updates == 0);
    for (int i = 0; i < 3; ++i) {
        assert(last_writes[i].dstBinding == bindings[i] && last_writes[i].dstSet == first);
        assert(last_infos[i].buffer == infos[i].buffer && last_infos[i].offset == infos[i].offset && last_infos[i].range == infos[i].range);
    }
    for (int i = 0; i < 1000; ++i)
        assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) == first);
    assert(allocs == 1 && updates == 1 && R.sets_reused == 1000);

    /* Every descriptor identity component, the set layout, and count are keys. */
    infos[0].offset++;
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) != first);
    infos[0].offset--; infos[1].range++;
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) != first);
    infos[1].range--; infos[1].buffer = buffer(99);
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) != first);
    infos[1].buffer = buffer(12); bindings[1]++;
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) != first);
    bindings[1]--; p.dl[0] = dl(9);
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) != first);
    p.dl[0] = dl(2);
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 2, bindings, infos, writes) != first);
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) == first);

    /* A command-buffer/pool reset or destroyed buffer invalidates all earlier handles. */
    draw_buffer_sets_forget();
    VkDescriptorSet next = draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes);
    assert(next != first); first = next;
    draw_buffer_sets_forget();
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) != first);

    /* Exact data comparison defeats a deliberately injected 64-bit hash collision. */
    draw_buffer_sets_forget();
    unsigned slot; uint64_t key;
    assert(draw_buffer_set_find(p.dl[0], 3, bindings, infos, &slot, &key) == VK_NULL_HANDLE);
    draw_buffer_set_save(slot, key, p.dl[0], (VkDescriptorSet)(uintptr_t)1000, 3, bindings, infos);
    g_draw_bset_values[g_draw_bsets[slot].off].offset++;
    assert(draw_buffer_set_find(p.dl[0], 3, bindings, infos, &slot, &key) == VK_NULL_HANDLE);

    /* Bounded hash probes and a full value arena fall back to independent new sets. */
    draw_buffer_sets_forget();
    for (unsigned i = 0; i < DRAW_BSET_SLOTS; ++i) {
        g_draw_bsets[i].epoch = g_draw_bset_epoch;
        g_draw_bsets[i].key = UINT64_MAX;
    }
    assert(draw_buffer_set_find(p.dl[0], 3, bindings, infos, &slot, &key) == VK_NULL_HANDLE);
    assert(slot == DRAW_BSET_SLOTS);
    draw_buffer_sets_forget();
    g_draw_bset_values_used = DRAW_BSET_VALUES;
    first = draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes);
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) != first);

    /* Epoch rollover cannot resurrect stale descriptors. Empty sets can be reused. */
    g_draw_bset_epoch = UINT32_MAX; draw_buffer_sets_forget();
    assert(g_draw_bset_epoch != 0);
    first = draw_buffer_descriptors(&p, 0, &stage, &stage, 0, bindings, infos, writes);
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 0, bindings, infos, writes) == first);

    /* Template updates stay on their production path, and cache disable is exact. */
    draw_buffer_sets_forget(); clear_calls(); mock_template = (VkDescriptorUpdateTemplate)(uintptr_t)7;
    first = draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes);
    assert(template_updates == 1 && updates == 0);
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) == first);
    assert(template_updates == 1);
    g_draw_bset_on = 0;
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) != first);
    assert(template_updates == 2); g_draw_bset_on = 1; mock_template = VK_NULL_HANDLE;

    /* Same layout/values skip push descriptors; changing the binding or layout replays them. */
    draw_state_forget(); clear_calls(); p.push0 = 1;
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes) == VK_NULL_HANDLE);
    for (int i = 0; i < 1000; ++i) draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes);
    assert(push_buffers == 1 && allocs == 0 && updates == 0);
    bindings[1]++; draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes); assert(push_buffers == 2); bindings[1]--;
    p.layout = pl(8); draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes); assert(push_buffers == 3);
    draw_state_forget(); draw_buffer_descriptors(&p, 0, &stage, &stage, 3, bindings, infos, writes); assert(push_buffers == 4);
    p.layout = pl(1); p.push0 = 0;

    /* Two changed sets are one Vulkan bind; only the changed set is rebound later. */
    draw_state_forget(); clear_calls();
    VkDescriptorSet sets[2] = {(VkDescriptorSet)(uintptr_t)21, (VkDescriptorSet)(uintptr_t)22};
    draw_bind_sets(p.layout, sets); assert(desc_binds == 1 && last_first_set == 0 && last_set_count == 2);
    draw_bind_sets(p.layout, sets); assert(desc_binds == 1);
    sets[0] = (VkDescriptorSet)(uintptr_t)23;
    draw_bind_sets(p.layout, sets); assert(desc_binds == 2 && last_first_set == 0 && last_set_count == 1);
    sets[1] = (VkDescriptorSet)(uintptr_t)24;
    draw_bind_sets(p.layout, sets); assert(desc_binds == 3 && last_first_set == 1 && last_set_count == 1);
    draw_bind_sets(pl(4), sets); assert(desc_binds == 4 && last_set_count == 2);
    /* A pipeline with no sets must still invalidate incompatible layout state. */
    VkDescriptorSet empty[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    draw_bind_sets(pl(5), empty);
    draw_bind_sets(pl(4), sets); assert(desc_binds == 5 && last_set_count == 2);

    /* Vertex state compares meaningful fields rather than padding in Vulkan structs. */
    draw_state_forget(); clear_calls();
    VkVertexInputBindingDescription2EXT bd[2]; VkVertexInputAttributeDescription2EXT ad[2];
    memset(bd, 0, sizeof bd); memset(ad, 0, sizeof ad);
    for (unsigned i = 0; i < 2; ++i) {
        bd[i].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
        bd[i].binding = i; bd[i].stride = 16; bd[i].divisor = 1;
        ad[i].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
        ad[i].location = i; ad[i].binding = i; ad[i].format = VK_FORMAT_R32G32_SFLOAT;
    }
    VkBuffer vbuf[2] = {buffer(31), buffer(32)}; VkDeviceSize voff[2] = {0, 256};
    draw_vertex_state(2, bd, 2, ad, vbuf, voff);
    for (int i = 0; i < 1000; ++i) draw_vertex_state(2, bd, 2, ad, vbuf, voff);
    assert(vi_calls == 1 && vb_calls == 1);
    VkVertexInputBindingDescription2EXT bd_padding[2]; VkVertexInputAttributeDescription2EXT ad_padding[2];
    memset(bd_padding, 0xa5, sizeof bd_padding); memset(ad_padding, 0xa5, sizeof ad_padding);
    for (int i = 0; i < 2; ++i) {
        bd_padding[i].sType = bd[i].sType; bd_padding[i].pNext = bd[i].pNext; bd_padding[i].binding = bd[i].binding;
        bd_padding[i].stride = bd[i].stride; bd_padding[i].inputRate = bd[i].inputRate; bd_padding[i].divisor = bd[i].divisor;
        ad_padding[i].sType = ad[i].sType; ad_padding[i].pNext = ad[i].pNext; ad_padding[i].location = ad[i].location;
        ad_padding[i].binding = ad[i].binding; ad_padding[i].format = ad[i].format; ad_padding[i].offset = ad[i].offset;
    }
    draw_vertex_state(2, bd_padding, 2, ad_padding, vbuf, voff);
    assert(vi_calls == 1 && vb_calls == 1);
    bd[0].stride++; draw_vertex_state(2, bd, 2, ad, vbuf, voff); assert(vi_calls == 2 && vb_calls == 1);
    ad[1].offset++; draw_vertex_state(2, bd, 2, ad, vbuf, voff); assert(vi_calls == 3 && vb_calls == 1);
    voff[1]++; draw_vertex_state(2, bd, 2, ad, vbuf, voff); assert(vi_calls == 3 && vb_calls == 2);
    vbuf[1] = buffer(40); draw_vertex_state(2, bd, 2, ad, vbuf, voff); assert(vb_calls == 3);
    draw_vertex_state(1, bd, 1, ad, vbuf, voff); assert(vi_calls == 4 && vb_calls == 3);
    draw_state_forget(); draw_vertex_state(1, bd, 1, ad, vbuf, voff); assert(vi_calls == 5 && vb_calls == 4);
    D.has_vids = 0; draw_state_forget(); draw_vertex_state(1, bd, 1, ad, vbuf, voff); assert(vi_calls == 5 && vb_calls == 5); D.has_vids = 1;
    draw_index_state(buffer(41), 0, VK_INDEX_TYPE_UINT16);
    draw_index_state(buffer(41), 0, VK_INDEX_TYPE_UINT16); assert(index_calls == 1);
    draw_state_forget(); draw_index_state(buffer(41), 0, VK_INDEX_TYPE_UINT16); assert(index_calls == 2);

    /* Push constants match raw bit patterns, layout, and invalidation boundaries. */
    draw_state_forget(); clear_calls(); uint32_t pc[5] = {1, 0x7fc00001, 0x80000000, 4, 5};
    draw_push_constants(pl(1), pc);
    for (int i = 0; i < 1000; ++i) draw_push_constants(pl(1), pc);
    assert(pc_calls == 1 && memcmp(last_pc, pc, sizeof pc) == 0);
    pc[1]++; draw_push_constants(pl(1), pc); assert(pc_calls == 2);
    draw_push_constants(pl(2), pc); assert(pc_calls == 3);
    draw_state_forget(); draw_push_constants(pl(2), pc); assert(pc_calls == 4);

    /* Disabling command shadowing always records the requested commands. */
    g_draw_shadow_on = 0; clear_calls();
    for (int i = 0; i < 3; ++i) {
        draw_vertex_state(2, bd, 2, ad, vbuf, voff);
        draw_bind_sets(pl(1), sets); draw_push_constants(pl(1), pc);
    }
    assert(vi_calls == 3 && vb_calls == 3 && desc_binds == 6 && pc_calls == 3);
    puts("PASS: production descriptor reuse and draw state (identity, collisions, limits, lifetime, templates, push/bind batching, invalidation)");
    puts("Synthetic 1001 identical preparations: allocated/updated sets 1001 -> 1; each unchanged vertex/push state command 1001 -> 1; draw count unchanged");
    return 0;
}
'''


class DrawSubmissionTests(unittest.TestCase):
    def test_production_helpers(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not shutil.which(cc[0]):
            self.skipTest("host C compiler is unavailable")
        source = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
        descriptor_function = extract_function(source, "draw_buffer_descriptors")
        includes = ["-I", str(ROOT / "runtime")]
        candidates = [Path(os.environ["VULKAN_INCLUDE_DIR"])] if os.environ.get("VULKAN_INCLUDE_DIR") else []
        candidates += [ROOT / "deps/include", ROOT / "deps/android-arm64/include", Path("/usr/include"), Path("/usr/local/include")]
        vulkan = next((candidate for candidate in candidates if (candidate / "vulkan/vulkan.h").is_file()), None)
        if vulkan is None:
            self.skipTest("Vulkan headers unavailable; set VULKAN_INCLUDE_DIR")
        includes += ["-I", str(vulkan)]
        with tempfile.TemporaryDirectory(prefix="wt_draw_submit_") as temp:
            path = Path(temp)
            src = path / "test.c"
            exe = path / "test"
            src.write_text(PRELUDE + "\n" + descriptor_function + "\n" + TEST_MAIN, encoding="utf-8")
            extra_flags = shlex.split(os.environ.get("CFLAGS", ""))
            subprocess.run([*cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *extra_flags, *includes,
                            str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_invalidation_and_production_wiring(self):
        source = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
        self.assertIn("draw_state_forget();", extract_function(source, "dsh_forget"))
        self.assertIn("draw_buffer_sets_forget();", extract_function(source, "cmd_begin"))
        self.assertIn("draw_buffer_sets_forget();", extract_function(source, "vkd_init"))
        replacement = extract_function(source, "cb_new")
        self.assertLess(replacement.index("draw_buffer_sets_forget();"), replacement.index("vkDestroyBuffer(V.device, c->buf"))
        prepare = extract_function(source, "draw_prepare")
        for helper in ["draw_vertex_state(", "draw_buffer_descriptors(", "draw_bind_sets(", "draw_push_constants("]:
            self.assertIn(helper, prepare)
        self.assertNotIn("vkCmdPushConstants(", prepare)
        self.assertNotIn("vkCmdBindVertexBuffers(", prepare)
        self.assertNotIn("vkCmdSetVertexInputEXT(", prepare)


if __name__ == "__main__":
    unittest.main()
