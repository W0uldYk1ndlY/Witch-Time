"""Exercise the production latched scale policy without a Vulkan device."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
match = re.search(r'static float rs_scale\(void\) \{.*?\n\}', source, re.S)
size = re.search(r'static void rs_size\(.*?\n\}', source, re.S)
assert match and size
harness = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define RS_MAX 3.0f
#define MemoryBarrier() ((void)0)
static float g_rs = -1.0f, g_rs_r = -1.0f;
static int g_rs_mode;
static struct { float scale; int upscaler; } options;
static const void *fake_getenv(const char *name) { (void)name; return NULL; }
#define getenv fake_getenv
static const void *unused;
static const __typeof__(options) *gfx_options(void) { (void)unused; return &options; }
static int gfx_sgsr_variant(void) { return 0; }
static float fsr_ratio(int mode) {
    static const float ratios[] = {1, 1, 1.5f, 1.7f, 2, 3};
    return mode >= 0 && mode <= 5 ? ratios[mode] : 1;
}
''' + match.group(0) + r'''
static float rs_render_scale(void) { rs_scale(); return g_rs_r; }
static int rs_extra_class(int w, int h) { (void)w; (void)h; return 0; }
static float rs_extra_scale(float scale) { return scale; }
static int rs_169_class(int w, int h) { (void)w; (void)h; return 0; }
static float rs_xstretch(void) { return 1.0f; }
''' + size.group(0) + r'''
int main(void) {
    for (int mode = 0; mode <= 5; ++mode) {
        options.upscaler = mode;
        options.scale = 1.0f; g_rs = -1; assert(rs_scale() == 1.0f && g_rs_r == 1.0f);
        options.scale = 0.6f; g_rs = -1; assert(rs_scale() == 0.6f && g_rs_r == 0.6f);
        options.scale = 2.0f; g_rs = -1;
        float ratio = fsr_ratio(mode), requested = 2.0f / ratio;
        assert(rs_scale() == 2.0f && fabsf(g_rs_r - (requested >= 1 ? requested : 1)) < 0.00001f);
    }
    options.upscaler = 0; options.scale = 0.01f; g_rs = -1;
    assert(rs_scale() == 0.25f && g_rs_r == 0.25f);
    options.scale = 5.0f; g_rs = -1; assert(rs_scale() == 3.0f && g_rs_r == 3.0f);
    options.scale = 0.6f; options.upscaler = 2; g_rs = -1;
    int w, h; rs_size(1279, 719, 0, &w, &h); assert(w == 767 && h == 431);
    rs_size(1279, 719, 1, &w, &h); assert(w == 767 && h == 431);
    rs_size(1, 1, 0, &w, &h); assert(w == 1 && h == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="upscale-scale-") as temp:
    temp = Path(temp)
    src = temp / "test.c"
    exe = temp / ("test.exe" if os.name == "nt" else "test")
    src.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("production scale policy checks passed (mock options; no GPU coverage)")
