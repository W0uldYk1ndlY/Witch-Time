"""Execute production SGSR resize/retirement and native view decisions with mocks."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "runtime/nvn_vk_sgsr.c").read_text(encoding="utf-8")


def function(name):
    return re.search(r'static [^\n]+[ *]' + name + r'\(.*?\n\}', source, re.S).group(0)


assert "VK_IMAGE_USAGE_STORAGE_BIT" not in function("sgsr_image_new")
assert "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT" not in function("sgsr_unable")
draw = function("sgsr_pass")
assert draw.index("vkCmdEndRendering") < draw.index("R.pipe_bound = NULL") < draw.index("dsh_forget()")
assert "R.dyn_dirty = R.dyn3_dirty = 1" in draw


harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int format, nlvl, nlay; unsigned char swz[4]; } EView;
typedef struct { int w, h, layout, img; } VTex;
typedef struct { VTex *vt; } FImg;
enum { SG_MOTION, SG_HISTORY0, SG_HISTORY1, SG_ZERO, SG_IMAGES };
enum { VK_IMAGE_ASPECT_COLOR_BIT = 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 3 };
typedef struct { float color[4]; } VkClearColorValue;
typedef struct { int aspect, base, levels, layer, layers; } VkImageSubresourceRange;
static struct { int cmd; } R;
static struct { FImg image[SG_IMAGES]; int w, h, ow, oh, have_history, history; } SG;
static int allocated, destroyed, retired, cleared, fail_at, bad_usage;
static int sgsr_image_supported(int w, int h, int attachment) { (void)attachment; return w > 0 && h > 0; }
static int sgsr_image_new(FImg *out, int w, int h, int attachment) {
    int i = allocated++;
    if (i == fail_at) return 0;
    if (attachment != (i != SG_ZERO)) bad_usage++;
    out->vt = calloc(1, sizeof *out->vt); assert(out->vt);
    out->vt->w = w; out->vt->h = h; return 1;
}
static void vt_destroy(VTex *vt) { destroyed++; free(vt); }
static void vt_garbage(VTex *vt) { retired++; free(vt); }
static void vt_layout(VTex *vt, int layout) { vt->layout = layout; }
static void vkCmdClearColorImage(int cmd, int image, int layout, const VkClearColorValue *zero, int n, const VkImageSubresourceRange *range) {
    (void)cmd; (void)image; assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(zero->color[0] == 0 && n == 1 && range->levels == 1); cleared++;
}
''' + function("sgsr_retire_images") + '\n' + function("sgsr_resize") + '\n' + function("sgsr_scene_view") + r'''
int main(void) {
    for (int failure = 0; failure < SG_IMAGES; failure++) {
        memset(&SG, 0, sizeof SG); SG.w = 5; SG.have_history = 1;
        allocated = destroyed = retired = cleared = bad_usage = 0; fail_at = failure;
        assert(!sgsr_resize(17, 13, 25, 19));
        assert(destroyed == failure && retired == 0 && cleared == 0);
        assert(SG.w == 5 && SG.have_history == 1);
    }
    allocated = destroyed = retired = cleared = bad_usage = 0; fail_at = -1;
    assert(sgsr_resize(17, 13, 25, 19));
    assert(cleared == SG_IMAGES && !bad_usage && !SG.have_history);
    assert(SG.image[SG_ZERO].vt->w == 17 && SG.image[SG_ZERO].vt->h == 13);
    assert(SG.image[SG_HISTORY0].vt->w == 25 && SG.image[SG_HISTORY1].vt->h == 19);
    for (int i = 0; i < SG_IMAGES; i++) assert(SG.image[i].vt->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    allocated = 0; assert(sgsr_resize(19, 15, 29, 23)); assert(retired == SG_IMAGES);
    sgsr_retire_images(); assert(retired == SG_IMAGES * 2 && !SG.w && !SG.have_history);
    sgsr_retire_images(); assert(retired == SG_IMAGES * 2);
    EView view = sgsr_scene_view(123);
    assert(view.format == 123 && view.nlvl == 1 && view.nlay == 1);
    for (int i = 0; i < 4; i++) assert(view.swz[i] == i + 2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sgsr-resources-") as directory:
    directory = Path(directory)
    src, exe = directory / "test.c", directory / ("test.exe" if os.name == "nt" else "test")
    src.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("SGSR production resize/partial allocation/zero extent/view checks passed (mock GPU)")
