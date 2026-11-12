"""Execute production temporal observation/routing helpers with mocked renderer state."""
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime/nvn_vk_upscale.c").read_text(encoding="utf-8")
helpers = source[source.index("static void upscale_diag_dispatched"):]
harness = r'''
#include <assert.h>
#include <string.h>
#include "diag.h"
#include "fsr_evidence.h"
#include "upscale_scene.h"
typedef unsigned long long VkImageView;
#define VK_NULL_HANDLE 0
#define FX_GRAIN 1
typedef struct { int w, h; } VTex;
typedef struct Tex { int dead; VTex *vk; } Tex;
static UpscaleScene U = UPSCALE_SCENE_INITIALIZER;
static long long nvn_front_frame_next(void) { return U.frame; }
static int g_rs_mode, g_diag_surface_gen = 3, variant, unavailable;
static struct { int upscaler; } options;
#define gfx_options() (&options)
static int gfx_sgsr_variant(void) { return variant; }
static const char *sgsr_unable(void) { return unavailable ? "unsupported" : 0; }
static const char *sgsr_cs_unable(void) { return sgsr_unable(); }
static struct { int st[2]; } program;
static struct { int ncol; struct { Tex *tex; } col[1]; typeof(program) *prog[2]; } R;
static int stage_fx(const int *p) { return *p; }
enum { SG_HISTORY0, SG_HISTORY1, SC_OUTPUT };
typedef struct { VkImageView all; } Image;
static struct { int ok, have_history, history, w, h, ow, oh, frame_reset; Image image[3]; } SG, SC;
static struct { FsrEvidence evidence; int w, h, uw, uh, evidence_reset, evidence_mode; } F;
static diag_temporal_observation_t observed;
static long long upscale_diag_run_frame[2] = {-1, -1};
static int upscale_diag_pending, upscale_diag_bound, upscale_diag_consumer;
static uint32_t upscale_diag_native_pending_w, upscale_diag_native_pending_h, upscale_diag_native_w, upscale_diag_native_h;
bool diag_is_enabled(void) { return true; }
void diag_hook_temporal(const diag_temporal_observation_t *o) { observed = *o; }
'''
harness += helpers
harness += r'''
int main(void) {
    assert(!strcmp(fsr_evidence_result(&F.evidence), "off"));
    VTex image = {853, 479}; Tex color = {0, &image};
    U.frame = U.scene_frame = 17; U.scene_color = &color;
    U.camera_generation = U.generation = 1;
    g_rs_mode = options.upscaler = 2;
    F.evidence.selected = F.evidence.supported = 1;
    upscale_diag_present(); assert(observed.fallback_reason == DIAG_TEMPORAL_NO_SCENE);
    F.evidence.eligible = 1; F.evidence.supported = 0;
    upscale_diag_present(); assert(observed.fallback_reason == DIAG_TEMPORAL_UNSUPPORTED);
    F.evidence.supported = 1;
    upscale_diag_present(); assert(observed.fallback_reason == DIAG_TEMPORAL_SETUP_FAILED);
    F.evidence.initialized = F.evidence.dispatched = 1;
    F.evidence_mode = 2;
    F.w = 853; F.h = 479; F.uw = 1280; F.uh = 720; F.evidence_reset = 1;
    upscale_diag_present(); assert(observed.fallback_reason == DIAG_TEMPORAL_NO_CONSUMER);
    F.evidence.output_bound = F.evidence.draw_recorded = 1;
    upscale_diag_present(); assert(observed.effective_mode == 2 && observed.flags == 15);
    assert(observed.scene_width == 853 && observed.output_width == 1280 && observed.reset_reason == 1);
    assert(observed.scene_frame == 17 && observed.surface_generation == 3 && observed.motion_support == 1);

    g_rs_mode = options.upscaler = 7; variant = 0;
    SG.ok = SG.have_history = 1; SG.history = 0;
    SG.w = 853; SG.h = 479; SG.ow = 1280; SG.oh = 720;
    SG.image[SG_HISTORY0].all = 100;
    U.col = &color; U.ran = U.frame; U.output_larger = 1;
    upscale_diag_present(); assert(!(observed.flags & DIAG_TEMPORAL_DISPATCHED)); /* Shared U.ran alone is not backend proof. */
    upscale_diag_dispatched(0);
    program.st[1] = FX_GRAIN; R.prog[1] = &program;
    upscale_diag_present(); /* Passes + scene copy alone are never consumption. */
    assert(observed.fallback_reason == DIAG_TEMPORAL_NO_CONSUMER && !(observed.flags & 8));
    upscale_diag_prepare(); upscale_diag_sample(1, &color, 99); upscale_diag_draw();
    upscale_diag_present(); assert(!(observed.flags & 8)); /* Wrong view. */
    upscale_diag_prepare(); upscale_diag_sample(1, &color, 100);
    upscale_diag_present(); assert(!(observed.flags & 8)); /* No recorded draw. */
    upscale_diag_prepare(); upscale_diag_sample(1, &color, 100); upscale_diag_draw();
    upscale_diag_present(); assert(observed.effective_mode == 7 && (observed.flags & 8));
    upscale_diag_present(); assert(!(observed.flags & 8)); /* Evidence resets per scene. */
    variant = 2;
    upscale_diag_present(); assert(observed.fallback_reason == DIAG_TEMPORAL_UNSUPPORTED);
    g_rs_mode = options.upscaler = 0;
    upscale_diag_present(); assert(observed.effective_mode == 0 && !observed.output_width); /* Never infer native output from scene size. */
    VTex target_image = {1280, 720}; Tex target = {0, &target_image};
    R.ncol = 1; R.col[0].tex = &target;
    upscale_diag_prepare(); upscale_diag_sample(1, &color, 99); upscale_diag_draw();
    upscale_diag_present(); assert(observed.scene_width == 853 && observed.output_width == 1280);
    return 0;
}
'''
compiler = shlex.split(os.environ["HOST_CC"]) if os.environ.get("HOST_CC") else [shutil.which("gcc") or shutil.which("clang") or "cc"]
with tempfile.TemporaryDirectory(prefix="temporal_diag_") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness, encoding="utf-8")
    executable = path / ("test.exe" if os.name == "nt" else "test")
    subprocess.run(compiler + ["-std=gnu11", "-O2", "-Wall", "-I", str(ROOT / "runtime"), str(path / "test.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
draw = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
fsr = (ROOT / "runtime/nvn_vk_fsr.c").read_text(encoding="utf-8")
assert draw.count("fsr_evidence_draw();") == 3
assert "upscale_diag_prepare();" in fsr and "upscale_diag_sample(stage, tex, view);" in fsr
assert fsr.index("upscale_diag_present();") < fsr.index("memset(&F.evidence, 0, sizeof F.evidence);")
print("Temporal renderer diagnostics production-helper tests passed (mocked state, no GPU evidence)")
