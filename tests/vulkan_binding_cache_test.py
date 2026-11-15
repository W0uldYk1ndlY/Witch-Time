#!/usr/bin/env python3
"""Exercise sparse vertex and index bindings in the existing production helpers."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("binding_fixture", Path(__file__).with_name("vulkan_draw_submission_test.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)

harness.PRELUDE = harness.PRELUDE.replace(
    "static unsigned last_first_set",
    "static VkBuffer actual_vb[17], actual_ib;\n"
    "static VkDeviceSize actual_vo[17], actual_io;\n"
    "static VkIndexType actual_it;\n"
    "static unsigned first_vb, count_vb;\n"
    "static unsigned last_first_set",
).replace(
    "++vb_calls;",
    "++vb_calls; first_vb = first; count_vb = n;\n"
    "for (uint32_t i = 0; i < n; i++) { actual_vb[first+i] = buffers[i]; actual_vo[first+i] = offsets[i]; }",
).replace(
    "++index_calls;",
    "++index_calls; actual_ib = buffer; actual_io = offset; actual_it = type;",
)
harness.TEST_MAIN = harness.TEST_MAIN.replace("int main(void)", "static int baseline(void)") + r'''
int main(void) {
    assert(baseline() == 0);
    VkVertexInputBindingDescription2EXT bd[17] = {{0}};
    VkVertexInputAttributeDescription2EXT ad[16] = {{0}};
    VkBuffer b[17]; VkDeviceSize o[17];
    for (unsigned i = 0; i < 17; i++) { b[i] = buffer(100+i); o[i] = i*16; }
    g_draw_shadow_on = 1; D.has_vids = 1;
    draw_state_forget(); clear_calls();
    draw_vertex_state(17, bd, 16, ad, b, o);
    assert(vb_calls == 1 && first_vb == 0 && count_vb == 17);
    b[2] = buffer(999); o[3]++;
    draw_vertex_state(17, bd, 16, ad, b, o);
    assert(vb_calls == 2 && first_vb == 2 && count_vb == 2);
    o[1]++; o[15]++;
    draw_vertex_state(17, bd, 16, ad, b, o);
    assert(vb_calls == 4 && first_vb == 15 && count_vb == 1);
    draw_vertex_state(2, bd, 1, ad, b, o);
    draw_vertex_state(17, bd, 16, ad, b, o);
    assert(vb_calls == 4); /* Vulkan retains slots outside the smaller list. */
    unsigned before = index_calls;
    draw_index_state(buffer(7), 8, VK_INDEX_TYPE_UINT16);
    draw_index_state(buffer(7), 8, VK_INDEX_TYPE_UINT16);
    assert(index_calls == before+1);
    draw_index_state(buffer(7), 9, VK_INDEX_TYPE_UINT16);
    draw_index_state(buffer(7), 9, VK_INDEX_TYPE_UINT32);
    draw_index_state(buffer(8), 9, VK_INDEX_TYPE_UINT32);
    assert(index_calls == before+4);
    for (unsigned i = 0; i < 2000; i++) {
        uint32_t n = i%17+1, slot = i%n;
        b[slot] = buffer(100+i); o[slot] = i*32;
        if (!(i%19)) {
            draw_state_forget();
            memset(actual_vb, 0, sizeof actual_vb);
            memset(actual_vo, 0, sizeof actual_vo);
        }
        draw_vertex_state(n, bd, i%17, ad, b, o);
        assert(!memcmp(actual_vb, b, n*sizeof b[0]));
        assert(!memcmp(actual_vo, o, n*sizeof o[0]));
        VkIndexType type = i%2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        draw_index_state(buffer(20+i%3), i%13, type);
        assert(actual_ib == buffer(20+i%3) && actual_io == i%13 && actual_it == type);
    }
    g_draw_shadow_on = 0; before = vb_calls;
    for (int i = 0; i < 3; i++) draw_vertex_state(17, bd, 16, ad, b, o);
    assert(vb_calls == before+3 && first_vb == 0 && count_vb == 17);
    before = index_calls;
    for (int i = 0; i < 3; i++) draw_index_state(buffer(7), 0, VK_INDEX_TYPE_UINT16);
    assert(index_calls == before+3);
    puts("PASS: sparse vertex groups, retained slots, exact index state, invalidation, rollback, 2000 varied draws");
    return 0;
}
'''


class BindingCacheTests(harness.DrawSubmissionTests):
    def test_index_production_wiring(self):
        source = (harness.ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
        execution = harness.extract_function(source, "exec")
        self.assertEqual(execution.count("draw_index_state("), 3)
        self.assertNotIn("vkCmdBindIndexBuffer(", execution)


if __name__ == "__main__":
    unittest.main()
