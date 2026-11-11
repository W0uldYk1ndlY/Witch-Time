#ifndef WT_UPSCALE_SCENE_H
#define WT_UPSCALE_SCENE_H

#include <stdint.h>
#include <string.h>
#include <float.h>

struct Tex;
typedef struct {
    long long frame, ran, depth_frame, cam_frame, up_frame, scene_frame, surface_generation;
    struct Tex *depth, *col, *scene_color;
    const void *color_image, *depth_image;
    uint64_t generation, depth_generation, camera_generation;
    float c3[64]; int cam_jit, camera_conflict, output_larger, have_surface;
} UpscaleScene;

#define UPSCALE_SCENE_INITIALIZER { .ran = -1, .up_frame = -1 }

static inline int upscale_camera_compatible(const float *a, const float *b) {
    for (int i = 0; i < 64; i++) {
        if (i == 18 || i == 22 || (i >= 32 && i < 48)) continue;
        if (a[i] != b[i]) return 0;
    }
    double ajx = a[30] != 0.0f ? (double)a[18] / a[30] : 0.0, ajy = a[30] != 0.0f ? (double)a[22] / a[30] : 0.0;
    double bjx = b[30] != 0.0f ? (double)b[18] / b[30] : 0.0, bjy = b[30] != 0.0f ? (double)b[22] / b[30] : 0.0;
    for (int i = 0; i < 16; i++) {
        int row = i / 4, column = i % 4;
        double av = a[32 + i], bv = b[32 + i];
        if (row < 2) { av -= (row ? ajy : ajx) * a[44 + column]; bv -= (row ? bjy : bjx) * b[44 + column]; }
        double diff = av - bv, magnitude = (av < 0 ? -av : av) + (bv < 0 ? -bv : bv);
        if (!((diff < 0 ? -diff : diff) <= 0.00001 * (1.0 + magnitude))) return 0;
    }
    return 1;
}

static inline void upscale_scene_observe(UpscaleScene *s, struct Tex *color, const void *color_image,
                                  struct Tex *depth, const void *depth_image, const float *camera) {
    if (!s->generation || s->scene_frame != s->frame || s->scene_color != color || s->depth != depth ||
        s->color_image != color_image || s->depth_image != depth_image) {
        if (++s->generation == 0) ++s->generation;
        s->scene_frame = s->frame; s->scene_color = color; s->depth = depth;
        s->color_image = color_image; s->depth_image = depth_image;
        s->camera_generation = 0; s->cam_jit = 0; s->camera_conflict = 0;
    }
    s->depth_frame = s->frame; s->depth_generation = s->generation;
    if (camera) for (int i = 0; i < 64; i++) if (!(camera[i] <= FLT_MAX && camera[i] >= -FLT_MAX)) {
        s->camera_generation = 0; s->camera_conflict = 1; return;
    }
    if (camera && camera[28] == 0.0f && camera[29] == 0.0f && camera[30] < -0.5f && camera[31] == 0.0f) {
        int jittered = camera[22] != 0.0f;
        if (s->camera_generation == s->generation && (!upscale_camera_compatible(s->c3, camera) ||
            (jittered && s->cam_jit && (s->c3[18] != camera[18] || s->c3[22] != camera[22])))) {
            s->camera_generation = 0; s->camera_conflict = 1;
        }
        if (s->camera_conflict) return;
        if (s->camera_generation != s->generation || jittered || !s->cam_jit) {
            memcpy(s->c3, camera, sizeof s->c3);
            s->cam_frame = s->frame; s->cam_jit = jittered; s->camera_generation = s->generation;
        }
    }
}

static inline int upscale_scene_matches(const UpscaleScene *s, const struct Tex *color, const void *color_image,
                                 const struct Tex *depth, const void *depth_image) {
    return s->generation && s->ran != s->frame && s->scene_frame == s->frame &&
           s->depth_frame == s->frame && s->cam_frame == s->frame &&
           s->depth_generation == s->generation && s->camera_generation == s->generation &&
           s->scene_color == color && s->color_image == color_image && s->depth == depth && s->depth_image == depth_image;
}

static inline int upscale_scene_reset(const UpscaleScene *s, int fresh, int have_history, int camera_cut, long long surface_generation) {
    return fresh || !have_history || s->ran != s->frame - 1 || camera_cut || !s->have_surface || s->surface_generation != surface_generation;
}

static inline int upscale_scene_mode_changed(UpscaleScene *s, int before, int after) {
    if (before == after) return 0;
    s->generation = s->depth_generation = s->camera_generation = 0;
    s->scene_color = s->depth = s->col = NULL;
    s->color_image = s->depth_image = NULL;
    s->camera_conflict = s->cam_jit = s->output_larger = 0;
    s->have_surface = 0;
    return 1;
}

static inline void upscale_scene_jitter(const UpscaleScene *s, int width, int height, double *x, double *y) {
    *x = s->c3[30] != 0.0f ? (double)s->c3[18] / s->c3[30] : 0.0;
    *y = s->c3[30] != 0.0f ? (double)s->c3[22] / s->c3[30] : 0.0;
    double ax = *x < 0.0 ? -*x : *x, ay = *y < 0.0 ? -*y : *y;
    /* shortcut: off-centre projections are guessed from size, use an explicit jitter signal when available. */
    if (ax * width > 1.15 || ay * height > 1.15) *x = *y = 0.0;
}

#endif
