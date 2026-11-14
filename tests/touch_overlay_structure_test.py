#!/usr/bin/env python3
"""Check production overlay placement, staging and lifecycle contracts."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRAW = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
PANEL = (ROOT / "runtime/nvn_vk_pcmenu.c").read_text(encoding="utf-8")


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin + len(start))]


class TouchOverlayStructureTest(unittest.TestCase):
    def test_presentation_transform_keeps_asymmetric_label_uv_attached(self):
        shader = section(PANEL, "static const char k_pcm_vs[] =", "static const char k_pcm_fs[] =")
        self.assertIn('"    vec2 uv = k[gl_VertexIndex] * 0.5 + 0.5; vec2 q = p / pc.view;\\n"', shader)
        self.assertIn('"    else if (pc.pad == 8.0) q = vec2(q.y,1.0-q.x);\\n"', shader)
        self.assertIn('"    v_uv = uv; gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);\\n"', shader)
        transforms = shader[shader.index('if (pc.pad == 2.0)'):shader.index('v_uv = uv;')]
        self.assertNotIn("uv =", transforms)

    def test_overlay_is_after_game_and_before_menu_and_hdr(self):
        present = DRAW[DRAW.index("static void vk_present2(void *arg, int progress) {"):]
        self.assertLess(present.index("overlay_draw(tgt)"), present.index("touch_vk_draw(tgt)"))
        self.assertLess(present.index("touch_vk_draw(tgt)"), present.index("pcm_vk_draw(tgt)"))
        self.assertLess(present.index("pcm_vk_draw(tgt)"), present.index("hdr_pass(index, tgt)"))
        self.assertIn("touch_indirect", present)

    def test_only_cached_small_textures_are_uploaded(self):
        prepare = section(PANEL, "static int touch_vk_prepare(", "static void touch_vk_quad(")
        draw = section(PANEL, "static void touch_vk_draw(", "#endif\nstatic void pcm_shot_write")
        self.assertIn("#define TOUCH_TEX_SIZE 96", PANEL)
        self.assertIn("if (!g_gpu.tex[TOUCH_TEX].img)", prepare)
        self.assertIn("if (!g_touch_gpu.uploaded && g_touch_gpu.stage)", draw)
        self.assertNotIn("vkCmdCopyBufferToImage", draw)
        self.assertIn("touch_scene_build", draw)
        job = section(PANEL, "void nvn_vk_pcmenu_job(", "static void pcm_upload(")
        self.assertIn("if (!touch_runtime_editing())", job)

    def test_resize_and_shutdown_wait_and_release_resources(self):
        prepare = section(PANEL, "static int touch_vk_prepare(", "static void touch_vk_quad(")
        free = PANEL[PANEL.index("static void pcm_vk_free(void)"):]
        self.assertIn("g_touch_gpu.w != (int)V.extent.width", prepare)
        self.assertIn("g_touch_gpu.format != canvas_format", prepare)
        self.assertIn("cmd_wait_all()", prepare)
        self.assertIn("vkDestroyImageView(V.device, g_touch_gpu.canvas_view", prepare)
        self.assertIn("vkDestroyBuffer(V.device, g_touch_gpu.stage", free)
        self.assertIn("vkDestroyImage(V.device, g_touch_gpu.canvas", free)
        self.assertIn("touch_runtime_set_ready(0)", free)


if __name__ == "__main__":
    unittest.main()
