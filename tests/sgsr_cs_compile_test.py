"""Syntax-check the complete CS runtime unit against real Vulkan headers and host seams.

This is not a full renderer/Android link check; source embeddings are checked separately.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime/nvn_vk_sgsr_cs.c").read_text(encoding="utf-8")
source = re.sub(r'^#embed .*$', '0', source, flags=re.M)
prelude = r'''
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "upscale_scene.h"
typedef struct { int width, height, format; } Builder;
typedef struct Tex { Builder b; void *vk; } Tex;
typedef struct { int format, lvl0, nlvl, lay0, nlay; unsigned char swz[4]; } EView;
typedef struct { int unused; } View;
typedef struct { int unused; } Stage;
typedef struct { Stage st[2]; } Prog;
typedef struct { int block; uint64_t off, size; void *map; VkDeviceMemory mem; } VMem;
typedef struct VView { int vtype; VkImageView view; struct VView *next; } VView;
typedef struct VTex {
    unsigned magic; VkImage img; VMem mem; VkFormat fmt; VkImageAspectFlags aspect;
    int w, h, d, levels, layers, samples, rs, post; VkImageLayout layout;
    VView *views; struct VTex *next, *prev;
} VTex;
typedef struct { VTex *vt; VkImageView all, lvl[6]; } FImg;
static struct { VkDevice device; VkPhysicalDevice gpu; VkPhysicalDeviceProperties props; } V;
static struct { int has_push, fsr_push_descriptors; } D;
static struct { VkCommandBuffer cmd; void *pipe_bound; int images_live; VMem ring_mem; VkBuffer ring;
    Prog *prog[2]; int ncol; struct { Tex *tex; } col[1]; } R;
static UpscaleScene U = UPSCALE_SCENE_INITIALIZER;
static int g_rs_mode, g_diag_surface_gen, game_fsr_w, game_fsr_h, game_fsr_phases;
enum { DIM_2D, FX_GRAIN };
enum { PT_SGSR_CS_BASE = 30, PT_COPY_CONVERT = 24 };
static VTex *g_vtex_all;
static VkPipelineCache g_pcache;
extern int fmt_supported(VkFormat format, VkFormatFeatureFlags features);
extern VkShaderModule module_from_glsl(int stage, const char *source, const char *what, uint64_t *hash);
extern const char *vk_result_name(VkResult result);
extern void vk_fatal(const char *operation, VkResult result);
extern void vt_garbage(VTex *image);
extern void vt_destroy(VTex *image);
extern void vt_layout(VTex *image, VkImageLayout layout);
extern VMem vmem_alloc(VkMemoryRequirements requirements, VkFlags need, VkFlags prefer);
extern void vmem_free(VMem memory);
extern void pass_end(void);
extern void cmd_begin(void);
extern uint64_t ring_raw(const void *data, uint64_t size);
extern void dsh_forget(void);
extern void pt_begin(int pass, int w, int h);
extern void pt_end(void);
extern void upscale_diag_dispatched(int compute);
extern int gfx_sgsr_variant(void);
extern VTex *vt_get(Tex *texture);
extern void rs_size(int width, int height, int output, int *w, int *h);
extern void fsr_inv4(const double *matrix, double *inverse);
extern EView sgsr_scene_view(int format);
extern VkImageView vt_view(VTex *image, const EView *view, int dim, int attachment);
extern int stage_fx(Stage *stage);
extern int rs_post_has(const Tex *texture);
extern void eview_of(Tex *texture, VTex *image, const View *view, EView *effective);
'''
with tempfile.TemporaryDirectory(prefix="sgsr-cs-compile-") as directory:
    path = Path(directory) / "unit.c"
    path.write_text(prelude + source, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-variable", "-Wno-missing-field-initializers", "-fsyntax-only",
                    "-I", str(ROOT / "runtime"), "-I", str(ROOT / "deps/android-arm64/include"), str(path)], check=True)
print("Complete SGSR CS runtime unit syntax-checked against Vulkan headers (mock host seams; no renderer link)")
