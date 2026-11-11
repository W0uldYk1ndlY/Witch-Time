"""Verify lifecycle hooks use existing present and deferred-retirement boundaries."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
draw = (root / "runtime" / "nvn_vk_draw.c").read_text(encoding="utf-8")
shared = (root / "runtime" / "nvn_vk_upscale.c").read_text(encoding="utf-8")
fsr = (root / "runtime" / "nvn_vk_fsr.c").read_text(encoding="utf-8")
sgsr = (root / "runtime" / "nvn_vk_sgsr.c").read_text(encoding="utf-8")
present = draw.split("static void vk_present2(void *arg, int progress) {", 1)[1]
assert present.index("if (progress) goto swap;") < present.index("fsr_present();")
assert present.index("if (progress) goto swap;") < present.index("sgsr_present();")
flush = draw.split("static void present_flush(", 1)[1].split("static void present_progress(", 1)[0]
assert "fsr_present(" not in flush, "a repeated held present must not advance temporal history"
apply = draw.split("static void rs_apply(void) {", 1)[1].split("/* Session 8", 1)[0]
assert apply.index("upscale_mode_changed(") < apply.index("if (rs_scale() == was"), "mode changes must reset even at the same size"
transition = shared.split("static void upscale_mode_changed(", 1)[1].split("static int upscale_reads(", 1)[0]
assert "vt_garbage(" in transition and "cmd_wait_all(" not in transition
assert "g_garbage[g_ngarbage++].serial = R.serial + 1" in draw
assert "g_garbage[i].serial <= R.done" in draw
assert "upscale_scene_reset(&U, F.fresh, F.have_prev, cut, g_diag_surface_gen)" in fsr
assert "sgsr_retire_images();" in transition
assert "upscale_scene_reset(&U, resize || incompatible, SG.have_history, cut, g_diag_surface_gen)" in sgsr
output = sgsr.split("static VkImageView sgsr_up_view(", 1)[1]
assert "U.ran != U.frame" in output and "!SG.have_history" in output
assert "effective.format != texture->b.format" in output and "effective.swz[0] != 2" in output
assert "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL" in draw.split("if (sgsr_output && dim == DIM_2D)", 1)[1].split("\n", 1)[0]
print("temporal lifecycle wiring checks passed (source-level; GPU lifetime not executed)")
