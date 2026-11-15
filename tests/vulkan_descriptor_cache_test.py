#!/usr/bin/env python3
"""Exercise exact descriptor reuse and its new last-resolved shortcut.

Reuse the existing production-code fixture, including collision, capacity,
generation-wrap, template, push-layout, empty-list and rollback assertions.
"""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("descriptor_fixture", Path(__file__).with_name("vulkan_draw_submission_test.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)

harness.TEST_MAIN = harness.TEST_MAIN.replace("int main(void)", "static int baseline(void)") + r'''
int main(void) {
    assert(baseline() == 0);
    uint32_t bindings[2] = {1, 43};
    VkDescriptorBufferInfo infos[2] = {{buffer(11), 0, 64}, {buffer(12), 256, 128}};
    VkWriteDescriptorSet writes[160];
    Pipe p = {pl(1), {dl(2), dl(3)}, {2, 1}, 0};
    VStage stage = {0};
    g_draw_bset_on = 1; draw_buffer_sets_forget(); clear_calls();
    VkDescriptorSet first = draw_buffer_descriptors(&p, 0, &stage, &stage, 2, bindings, infos, writes);
    assert(g_draw_bset_last < DRAW_BSET_SLOTS);
    unsigned saved = g_draw_bset_last, slot;
    uint64_t key;
    assert(draw_buffer_set_find(p.dl[0], 2, bindings, infos, &slot, &key) == first);
    assert(slot == DRAW_BSET_SLOTS && key == 0); /* Returned before hashing. */
    infos[0].offset++;
    VkDescriptorSet other = draw_buffer_descriptors(&p, 0, &stage, &stage, 2, bindings, infos, writes);
    assert(other != first);
    infos[0].offset--;
    assert(draw_buffer_set_find(p.dl[0], 2, bindings, infos, &slot, &key) == first);
    assert(key != 0 && g_draw_bset_last == saved); /* Table hits promote too. */
    assert(draw_buffer_set_find(p.dl[0], 2, bindings, infos, &slot, &key) == first && key == 0);
    draw_buffer_sets_forget();
    assert(g_draw_bset_last == DRAW_BSET_SLOTS);
    assert(draw_buffer_descriptors(&p, 0, &stage, &stage, 2, bindings, infos, writes) != first);
    g_draw_bset_epoch = UINT32_MAX; draw_buffer_sets_forget();
    assert(g_draw_bset_last == DRAW_BSET_SLOTS && g_draw_bset_epoch == 1);
    assert(draw_buffer_set_find(p.dl[0], 2, bindings, infos, &slot, &key) == VK_NULL_HANDLE);
    puts("PASS: last-resolved shortcut, exact mismatch, table promotion and generation invalidation");
    return 0;
}
'''


class DescriptorCacheTests(harness.DrawSubmissionTests):
    pass


if __name__ == "__main__":
    unittest.main()
