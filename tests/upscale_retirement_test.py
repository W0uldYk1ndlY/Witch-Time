"""Execute production temporal retirement and serial collection with mock fences."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(path, name):
    source = (ROOT / path).read_text(encoding="utf-8")
    return re.search(r"static void " + name + r"\([^\n]*\)[^\n]*\{.*?\n\}", source, re.S).group(0)


harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upscale_scene.h"
typedef long long LONG64;
typedef struct { long long QuadPart; } LARGE_INTEGER;
static LONG64 tk(void) { return 1; }
static void QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1; }
static long long nvn_front_frame_next(void) { return 0; }
typedef struct { uint64_t user_serial; int destroyed; } VTex;
typedef struct { VTex *vt; } FImg;
enum { FI_N = 3 };
enum { SG_MOTION, SG_HISTORY0, SG_HISTORY1, SG_ZERO, SG_IMAGES };
static struct { uint64_t serial, done; } R;
static struct { VTex *vt; uint64_t serial; } *g_garbage;
static int g_ngarbage, g_garbage_cap, destroyed;
static void vt_destroy(VTex *vt) {
    assert(!vt->destroyed && R.done >= vt->user_serial);
    vt->destroyed = 1; destroyed++;
}
static struct { FImg image[SG_IMAGES]; int w, h, ow, oh, have_history, history; } SG;
static struct { FImg img[FI_N]; int w, h, uw, uh, have_prev, t_last, res_index; } F;
static UpscaleScene U = UPSCALE_SCENE_INITIALIZER;
static int game_fsr_w, game_fsr_h;
static long long upscale_diag_run_frame[2];
static int upscale_diag_pending, upscale_diag_bound, upscale_diag_consumer;
static uint32_t upscale_diag_native_pending_w, upscale_diag_native_pending_h, upscale_diag_native_w, upscale_diag_native_h;
static int variant;
static int gfx_sgsr_variant(void) { return variant; }
'''
harness += function("runtime/nvn_vk_draw.c", "garbage_collect") + "\n"
harness += function("runtime/nvn_vk_draw.c", "vt_garbage") + "\n"
harness += function("runtime/nvn_vk_sgsr.c", "sgsr_retire_images") + "\n"
transition = function("runtime/nvn_vk_upscale.c", "upscale_mode_changed")
if "sgsr_cs_retire_images" in transition:
    harness += "static void sgsr_cs_retire_images(void) {}\n"
harness += transition + r'''
int main(void) {
    VTex images[32][FI_N + SG_IMAGES] = {0};
    for (int generation = 0; generation < 32; generation++) {
        R.serial = (uint64_t)generation + 10;
        R.done = generation ? R.serial - 1 : 0;
        for (int i = 0; i < FI_N + SG_IMAGES; i++) {
            images[generation][i].user_serial = R.serial + 1;
            if (i < FI_N) F.img[i].vt = &images[generation][i];
            else SG.image[i - FI_N].vt = &images[generation][i];
        }
        F.w = F.uw = SG.w = SG.ow = game_fsr_w = 17;
        F.have_prev = SG.have_history = 1;
        int before = generation % 9, after = (before + 1) % 9;
        upscale_mode_changed(before, before);
        assert(F.have_prev && SG.have_history);
        upscale_mode_changed(before, after);
        assert(!F.have_prev && !SG.have_history && !F.w && !SG.w && !game_fsr_w);
        int count = destroyed;
        garbage_collect();
        assert(destroyed >= count);
        for (int i = 0; i < FI_N + SG_IMAGES; i++) assert(!images[generation][i].destroyed);
        /* A completed earlier submission cannot release this generation's pending users. */
        R.done = R.serial; garbage_collect();
        for (int i = 0; i < FI_N + SG_IMAGES; i++) assert(!images[generation][i].destroyed);
        R.done = R.serial + 1; garbage_collect();
        for (int i = 0; i < FI_N + SG_IMAGES; i++) assert(images[generation][i].destroyed);
        assert(!g_ngarbage);
    }
    assert(destroyed == 32 * (FI_N + SG_IMAGES));
    VTex pending[4][SG_IMAGES] = {0};
    R.done = 99;
    for (int generation = 0; generation < 4; generation++) {
        R.serial = 100 + (uint64_t)generation;
        for (int i = 0; i < SG_IMAGES; i++) {
            pending[generation][i].user_serial = R.serial + 1;
            SG.image[i].vt = &pending[generation][i];
        }
        sgsr_retire_images(); garbage_collect();
    }
    assert(g_ngarbage == 4 * SG_IMAGES);
    for (R.done = 100; R.done <= 104; R.done++) {
        garbage_collect();
        for (int generation = 0; generation < 4; generation++) for (int i = 0; i < SG_IMAGES; i++)
            assert(pending[generation][i].destroyed == (R.done >= 101 + (uint64_t)generation));
    }
    assert(!g_ngarbage && destroyed == 32 * (FI_N + SG_IMAGES) + 4 * SG_IMAGES);
    upscale_mode_changed(0, 6);
    F.have_prev = SG.have_history = 1;
    variant = 1; upscale_mode_changed(6, 6);
    assert(!F.have_prev && !SG.have_history && !U.have_surface);
    F.have_prev = SG.have_history = 1;
    upscale_mode_changed(6, 6);
    assert(F.have_prev && SG.have_history);
    free(g_garbage);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="upscale-retirement-") as directory:
    directory = Path(directory)
    src, exe = directory / "test.c", directory / ("test.exe" if os.name == "nt" else "test")
    src.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "runtime"), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("temporal retirement: 32 mode transitions and four pending generations preserve serial users (mock fences)")
