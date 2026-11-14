#ifndef WT_TOUCH_CONTROLS_H
#define WT_TOUCH_CONTROLS_H
#include <stdint.h>
#include <stddef.h>

typedef enum {
    TOUCH_LEFT_STICK, TOUCH_RIGHT_STICK, TOUCH_DPAD,
    TOUCH_A, TOUCH_B, TOUCH_X, TOUCH_Y,
    TOUCH_L, TOUCH_R, TOUCH_ZL, TOUCH_ZR,
    TOUCH_PLUS, TOUCH_MINUS, TOUCH_L3, TOUCH_R3,
    TOUCH_COUNT
} TouchControlId;

typedef struct { float x, y, scale, opacity; } TouchPlacement;
typedef struct { TouchPlacement control[TOUCH_COUNT]; } TouchLayout;
typedef struct { int enabled; TouchLayout layout; } TouchConfig;
typedef struct { float x, y, w, h; } TouchRect;
typedef struct { int transform, width, height, sdl_width, sdl_height, valid; unsigned generation; } TouchPresentation;
int touch_presentation_map(const TouchPresentation *p, float x, float y, float *out_x, float *out_y);
/* Local four-byte pixels only: quarter turns swap dimensions, never scale. Buffers must not overlap. */
int touch_bitmap_rotate(const void *src, size_t src_size, void *dst, size_t dst_size,
                        int w, int h, int transform, int *out_w, int *out_h);
/* Unlike pointer mapping, rectangle corners include the window's right/bottom edges. */
int touch_presentation_bounds(const TouchPresentation *p, const int rect[4], int out[4]);
typedef struct { TouchRect control[TOUCH_COUNT]; } TouchGeometry;
typedef struct {
    uint64_t buttons;
    int lx, ly, rx, ry, left_owned, right_owned;
    unsigned activity;
} TouchPad;
typedef struct {
    struct { int active; int64_t touch, finger; float x, y; } owner[TOUCH_COUNT];
    TouchPad pad;
    unsigned invalidation, accepted_generation, activity;
} TouchInput;

void touch_layout_default(TouchLayout *layout);
int touch_layout_valid(const TouchLayout *layout);
int touch_geometry_make(const TouchLayout *layout, TouchRect safe, float pixels_per_dp,
                        TouchGeometry *geometry);
int touch_geometry_accessible(const TouchGeometry *geometry);
int touch_geometry_hit(const TouchGeometry *geometry, float x, float y);
int touch_safe_pixels(TouchRect window_safe, int window_w, int window_h, int pixel_w, int pixel_h, TouchRect *pixel_safe);
void touch_input_reset(TouchInput *input);
void touch_input_invalidate(TouchInput *input);
int touch_input_reset_expected(TouchInput *input, unsigned generation);
TouchPad touch_input_snapshot(const TouchInput *input);
void touch_input_down(TouchInput *input, const TouchGeometry *geometry, int64_t touch, int64_t finger, float x, float y);
void touch_input_motion(TouchInput *input, const TouchGeometry *geometry, int64_t touch, int64_t finger, float x, float y);
void touch_input_up(TouchInput *input, const TouchGeometry *geometry, int64_t touch, int64_t finger);
void touch_pad_merge(TouchPad pad, uint64_t *buttons, int *lx, int *ly, int *rx, int *ry);
int touch_config_path(const char *controller_path, char *out, unsigned size);
int touch_config_load(const char *path, TouchConfig *config);
int touch_config_save(const char *path, const TouchConfig *config);

typedef struct {
    TouchConfig config;
    TouchLayout layout;
    TouchGeometry geometry;
    TouchRect safe, settings;
    TouchPad pad;
    int valid, gamepad_valid, ready, width, height;
    int editing, selected, edit_error;
    TouchRect editor_buttons[7];
    TouchPresentation presentation;
} TouchSnapshot;
typedef struct { int type; float x, y; } TouchPointer;
typedef struct { TouchRect rect; float opacity; int texture; } TouchSprite;
#define TOUCH_SCENE_MAX (TOUCH_COUNT + 12)
int touch_scene_build(const TouchSnapshot *snapshot, int menu_open, TouchSprite out[TOUCH_SCENE_MAX]);
void touch_runtime_init(const char *controller_path);
void touch_runtime_geometry(TouchRect safe, float pixels_per_dp, int width, int height);
void touch_runtime_invalidate(void);
void touch_runtime_event(int type, int64_t touch, int64_t finger, float x, float y);
unsigned touch_runtime_generation(void);
void touch_runtime_event_at(unsigned generation, int type, int64_t touch, int64_t finger, float x, float y);
TouchSnapshot touch_runtime_snapshot(void);
int touch_runtime_save(const TouchConfig *config);
void touch_runtime_set_ready(int ready);
void touch_runtime_presentation(TouchPresentation presentation);
int touch_runtime_take_settings(void);
void touch_runtime_set_replay(int replay);
void touch_runtime_foreground(int foreground);
void touch_runtime_focus(int focused, int initial);
void touch_runtime_menu(int open);
int touch_runtime_pointer(TouchPointer *event);
int touch_runtime_editor_begin(void);
int touch_runtime_editor_event(TouchPointer event);
int touch_runtime_editor_save(void);
void touch_runtime_editor_cancel(void);
int touch_runtime_editing(void);

#endif
