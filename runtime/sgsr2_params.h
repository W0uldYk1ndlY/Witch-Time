#ifndef WT_SGSR2_PARAMS_H
#define WT_SGSR2_PARAMS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    float clip_to_prev_clip[16];
    float render_size[2], output_size[2], render_size_rcp[2], output_size_rcp[2];
    float jitter_offset[2], scale_ratio[2];
    float camera_fov_angle_hor, min_lerp_contribution, reset;
    uint32_t same_camera;
} Sgsr2FsParams;

typedef struct {
    uint32_t render_size[2], display_size[2];
    float render_size_rcp[2], display_size_rcp[2], jitter_offset[2], padding[2];
    float clip_to_prev_clip[16];
    float pre_exposure, camera_fov_angle_hor, camera_near, min_lerp_contribution;
    uint32_t same_camera, reset;
    uint32_t tail_padding[2];
} Sgsr2CsParams;

_Static_assert(offsetof(Sgsr2FsParams, clip_to_prev_clip) == 0, "SGSR clip matrix offset");
_Static_assert(offsetof(Sgsr2FsParams, render_size) == 64, "SGSR render size offset");
_Static_assert(offsetof(Sgsr2FsParams, output_size) == 72, "SGSR output size offset");
_Static_assert(offsetof(Sgsr2FsParams, render_size_rcp) == 80, "SGSR reciprocal render size offset");
_Static_assert(offsetof(Sgsr2FsParams, output_size_rcp) == 88, "SGSR reciprocal output size offset");
_Static_assert(offsetof(Sgsr2FsParams, jitter_offset) == 96, "SGSR jitter offset");
_Static_assert(offsetof(Sgsr2FsParams, scale_ratio) == 104, "SGSR scale offset");
_Static_assert(offsetof(Sgsr2FsParams, camera_fov_angle_hor) == 112, "SGSR FOV offset");
_Static_assert(offsetof(Sgsr2FsParams, min_lerp_contribution) == 116, "SGSR minimum contribution offset");
_Static_assert(offsetof(Sgsr2FsParams, reset) == 120, "SGSR reset offset");
_Static_assert(offsetof(Sgsr2FsParams, same_camera) == 124, "SGSR same-camera offset");
_Static_assert(sizeof(Sgsr2FsParams) == 128, "SGSR FS UBO size");
_Static_assert(offsetof(Sgsr2CsParams, render_size) == 0, "SGSR CS render size offset");
_Static_assert(offsetof(Sgsr2CsParams, display_size) == 8, "SGSR CS display size offset");
_Static_assert(offsetof(Sgsr2CsParams, render_size_rcp) == 16, "SGSR CS reciprocal render size offset");
_Static_assert(offsetof(Sgsr2CsParams, display_size_rcp) == 24, "SGSR CS reciprocal display size offset");
_Static_assert(offsetof(Sgsr2CsParams, jitter_offset) == 32, "SGSR CS jitter offset");
_Static_assert(offsetof(Sgsr2CsParams, clip_to_prev_clip) == 48, "SGSR CS clip matrix offset");
_Static_assert(offsetof(Sgsr2CsParams, pre_exposure) == 112, "SGSR CS exposure offset");
_Static_assert(offsetof(Sgsr2CsParams, camera_fov_angle_hor) == 116, "SGSR CS FOV offset");
_Static_assert(offsetof(Sgsr2CsParams, camera_near) == 120, "SGSR CS near plane offset");
_Static_assert(offsetof(Sgsr2CsParams, min_lerp_contribution) == 124, "SGSR CS minimum contribution offset");
_Static_assert(offsetof(Sgsr2CsParams, same_camera) == 128, "SGSR CS same-camera offset");
_Static_assert(offsetof(Sgsr2CsParams, reset) == 132, "SGSR CS reset offset");
_Static_assert(sizeof(Sgsr2CsParams) == 144, "SGSR CS std140 block size");

#endif
