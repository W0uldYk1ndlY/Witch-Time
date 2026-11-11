/* Execute the production route with fake renderer state; no Vulkan device is used. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "upscale_scene.h"

enum { FX_GRAIN = 1, FX_DOWN = 2, VK_IMAGE_ASPECT_DEPTH_BIT = 1 };
typedef struct { int width, height; } Builder;
typedef struct Tex { Builder b; int dead; void *vk; } Tex;
typedef struct { int aspect, samples, post, hdr; } VTex;
typedef struct { uint64_t addr, size; } Range;
typedef struct { int unused; } View, SamplerParams;
typedef struct { int fx; } Stage;
typedef struct { Stage st[2]; } Prog;
typedef struct { int nsmp; struct { unsigned unit; } smp[4]; } VStage;
static struct {
    int ncol; struct { Tex *tex; } col[1], depth;
    Range ubo[1][1]; Prog *prog[2]; uint64_t nvn_tex[2][32]; int tpool, spool;
} R;
static UpscaleScene U = UPSCALE_SCENE_INITIALIZER;
enum { FI_N = 3 };
static struct { struct { VTex *vt; } img[FI_N]; int w, h, uw, uh, have_prev, t_last, res_index; } F;
static int game_fsr_w, game_fsr_h, garbage_count;
static VTex *retired[FI_N];
static void vt_garbage(VTex *v) { retired[garbage_count++] = v; }
static int runs, last_fx, promoted;
static int sgsr_selected, sgsr_runs, sgsr_retirements;
static float rs_scale(void) { return 1.0f; }
static int sgsr_want(void) { return sgsr_selected; }
static void sgsr_retire_images(void) { sgsr_retirements++; }
#define P(a) ((void *)(uintptr_t)(a))
static int fsr_want(void) { return 1; }
static int fsr_hdr(const VTex *v) { return v->hdr; }
static int wintex_is(Tex *t) { (void)t; return 0; }
static int stage_fx(Stage *s) { return s->fx; }
static int nvn_front_handle(int tpool, int spool, uint64_t h, Tex **t, View *v, SamplerParams *s) {
    (void)tpool; (void)spool; (void)v; (void)s; *t = (Tex *)(uintptr_t)h; return *t != NULL;
}
static void fsr_run(Tex *t, Tex *d, int fx) { assert(d == U.depth); U.col = t; U.ran = U.frame; runs++; last_fx = fx; }
static void sgsr_run(Tex *t, Tex *d, int fx) { assert(d == U.depth); (void)fx; U.col = t; U.ran = U.frame; sgsr_runs++; }
static void rs_promote(Tex *t) { (void)t; promoted++; }
#include "../runtime/nvn_vk_upscale.c"

int main(void) {
    VTex cv = {0, 1, 0, 1}, cv2 = {0, 1, 0, 1}, dv = {1, 1, 0, 0};
    Tex color = {{1280, 720}, 0, &cv}, other = {{1280, 720}, 0, &cv2}, target = {{1280, 720}, 0, &cv2}, depth = {{1280, 720}, 0, &dv};
    Prog prog = {0}; VStage fs = {1, {{0}}}; float camera[64] = {0}; camera[30] = -1.0f; camera[0] = 7.0f;
    R.ncol = 1; R.col[0].tex = &color; R.depth.tex = &depth;
    R.ubo[0][0].addr = (uintptr_t)camera; R.ubo[0][0].size = sizeof camera; R.prog[1] = &prog;
    upscale_draw(&fs); camera[0] = 99.0f; assert(U.c3[0] == 7.0f);
    R.col[0].tex = &other; R.depth.tex = NULL; R.nvn_tex[1][0] = (uintptr_t)&color; prog.st[1].fx = FX_DOWN;
    upscale_draw(&fs); assert(runs == 1 && last_fx == FX_DOWN);
    prog.st[1].fx = FX_GRAIN; upscale_draw(&fs); assert(runs == 1);
    U.frame++; R.col[0].tex = &color; R.depth.tex = &depth; upscale_draw(&fs);
    R.col[0].tex = &other; R.depth.tex = NULL; upscale_draw(&fs); assert(runs == 2 && last_fx == FX_GRAIN);
    U.frame++; R.col[0].tex = &color; R.depth.tex = &depth; R.ubo[0][0].addr = 0; upscale_draw(&fs);
    R.col[0].tex = &other; R.depth.tex = NULL; upscale_draw(&fs); assert(runs == 2);
    R.col[0].tex = &color; R.depth.tex = &depth; R.ubo[0][0].addr = (uintptr_t)camera; upscale_draw(&fs);
    R.col[0].tex = &target; R.depth.tex = NULL; R.nvn_tex[1][0] = (uintptr_t)&other; upscale_draw(&fs); assert(runs == 2);
    R.nvn_tex[1][0] = (uintptr_t)&color; upscale_draw(&fs); assert(runs == 3);
    assert(!promoted);
    U.output_larger = 1; prog.st[1].fx = FX_GRAIN;
    upscale_draw(&fs); assert(runs == 3 && promoted == 1);
    F.img[0].vt = &cv; F.img[1].vt = &cv2; F.have_prev = 1; F.t_last = 9; F.res_index = 3; game_fsr_w = game_fsr_h = 100;
    upscale_mode_changed(1, 1); assert(garbage_count == 0 && F.have_prev && U.col);
    upscale_mode_changed(1, 6);
    assert(garbage_count == 2 && retired[0] == &cv && retired[1] == &cv2);
    assert(!F.img[0].vt && !F.img[1].vt && !F.have_prev && !F.t_last && !F.res_index && !U.col && !game_fsr_w && !game_fsr_h);
    assert(cv.samples == 1 && cv2.samples == 1); /* retirement queues resources rather than destroying them */
    assert(sgsr_retirements == 1);
    U.frame++; sgsr_selected = 1; R.col[0].tex = &color; R.depth.tex = &depth;
    upscale_draw(&fs); R.col[0].tex = &target; R.depth.tex = NULL;
    upscale_draw(&fs); assert(sgsr_runs == 1 && runs == 3);
    puts("temporal production route checks passed (mock renderer)");
    return 0;
}
