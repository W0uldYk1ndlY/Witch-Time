#include "touch_controls.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __ANDROID__
#include "diag.h"
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
#define touch_open _open
#define touch_fdopen _fdopen
#define touch_close _close
#define touch_pid _getpid
#else
#include <unistd.h>
#define touch_open open
#define touch_fdopen fdopen
#define touch_close close
#define touch_pid getpid
#endif

static const TouchPlacement defaults[TOUCH_COUNT] = {
    { .13f, .70f, 1, .65f }, { .74f, .76f, 1, .65f }, { .28f, .68f, 1, .65f },
    { .91f, .58f, 1, .65f }, { .86f, .69f, 1, .65f },
    { .86f, .47f, 1, .65f }, { .81f, .58f, 1, .65f },
    { .08f, .25f, 1, .65f }, { .92f, .25f, 1, .65f },
    { .18f, .25f, 1, .65f }, { .82f, .25f, 1, .65f },
    { .60f, .25f, 1, .65f }, { .40f, .25f, 1, .65f },
    { .24f, .88f, 1, .65f }, { .65f, .88f, 1, .65f }
};

void touch_layout_default(TouchLayout *layout) {
    for (int i = 0; i < TOUCH_COUNT; i++) layout->control[i] = defaults[i];
}

int touch_presentation_map(const TouchPresentation *p, float x, float y, float *out_x, float *out_y) {
    if (!p || !p->valid || p->width <= 0 || p->height <= 0 || p->sdl_width <= 0 || p->sdl_height <= 0 ||
        !isfinite(x) || !isfinite(y) || x < 0 || y < 0 || x >= p->sdl_width || y >= p->sdl_height) return 0;
    float u = x / p->sdl_width, v = y / p->sdl_height, a, b;
    switch (p->transform) {
    case 2: a = 1-v; b = u; break; case 4: a = 1-u; b = 1-v; break;
    case 8: a = v; b = 1-u; break; case 1: a = u; b = v; break;
    default: return 0;
    }
    *out_x = a * p->width; *out_y = b * p->height;
    return 1;
}

int touch_bitmap_rotate(const void *src, size_t src_size, void *dst, size_t dst_size,
                        int w, int h, int transform, int *out_w, int *out_h) {
    if (!src || !dst || !out_w || !out_h || w <= 0 || h <= 0 ||
        (transform != 1 && transform != 2 && transform != 4 && transform != 8) ||
        (size_t)w > SIZE_MAX / 4 / (size_t)h) return 0;
    size_t bytes = (size_t)w * (size_t)h * 4;
    uintptr_t a = (uintptr_t)src, b = (uintptr_t)dst;
    if (src_size < bytes || dst_size < bytes || (a <= b ? b - a < bytes : a - b < bytes)) return 0;
    int ow = transform == 2 || transform == 8 ? h : w;
    int oh = transform == 2 || transform == 8 ? w : h;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int dx = x, dy = y;
        if (transform == 2) { dx = h - 1 - y; dy = x; }
        else if (transform == 4) { dx = w - 1 - x; dy = h - 1 - y; }
        else if (transform == 8) { dx = y; dy = w - 1 - x; }
        memcpy((uint8_t *)dst + ((size_t)dy * ow + dx) * 4,
               (const uint8_t *)src + ((size_t)y * w + x) * 4, 4);
    }
    *out_w = ow; *out_h = oh;
    return 1;
}

int touch_presentation_bounds(const TouchPresentation *p, const int rect[4], int out[4]) {
    if (!p || !rect || !out || !p->valid || p->width <= 0 || p->height <= 0 ||
        p->sdl_width <= 0 || p->sdl_height <= 0 || rect[0] < 0 || rect[1] < 0 ||
        rect[2] <= 0 || rect[3] <= 0 || rect[0] > p->sdl_width || rect[1] > p->sdl_height ||
        rect[2] > p->sdl_width - rect[0] || rect[3] > p->sdl_height - rect[1]) return 0;
    double min_x = p->width, min_y = p->height, max_x = 0, max_y = 0;
    for (int i = 0; i < 4; i++) {
        double u = (double)(rect[0] + (i & 1 ? rect[2] : 0)) / p->sdl_width;
        double v = (double)(rect[1] + (i & 2 ? rect[3] : 0)) / p->sdl_height;
        double x, y;
        switch (p->transform) {
        case 1: x = u; y = v; break;
        case 2: x = 1-v; y = u; break;
        case 4: x = 1-u; y = 1-v; break;
        case 8: x = v; y = 1-u; break;
        default: return 0;
        }
        x *= p->width; y *= p->height;
        min_x = fmin(min_x, x); min_y = fmin(min_y, y);
        max_x = fmax(max_x, x); max_y = fmax(max_y, y);
    }
    int x0 = (int)floor(min_x), y0 = (int)floor(min_y);
    out[0] = x0; out[1] = y0; out[2] = (int)ceil(max_x) - x0; out[3] = (int)ceil(max_y) - y0;
    return out[2] > 0 && out[3] > 0;
}


int touch_layout_valid(const TouchLayout *layout) {
    if (!layout) return 0;
    for (int i = 0; i < TOUCH_COUNT; i++) {
        TouchPlacement p = layout->control[i];
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.scale) || !isfinite(p.opacity) ||
            p.x < 0 || p.x > 1 || p.y < 0 || p.y > 1 || p.scale < .5f || p.scale > 2 ||
            p.opacity < .2f || p.opacity > 1) return 0;
    }
    return 1;
}

int touch_geometry_make(const TouchLayout *layout, TouchRect safe, float pixels_per_dp,
                        TouchGeometry *geometry) {
    if (!geometry || !touch_layout_valid(layout) || !isfinite(safe.x) || !isfinite(safe.y) ||
        !isfinite(safe.w) || !isfinite(safe.h) || !isfinite(pixels_per_dp) ||
        !isfinite(safe.x + safe.w) || !isfinite(safe.y + safe.h) ||
        safe.w <= 0 || safe.h <= 0 || pixels_per_dp <= 0) return 0;
    float minimum = 48 * pixels_per_dp;
    if (!isfinite(minimum) || minimum > safe.w || 3 * minimum > safe.h) return 0;
    float side = fminf(safe.w, safe.h);
    TouchGeometry result;
    for (int i = 0; i < TOUCH_COUNT; i++) {
        TouchPlacement p = layout->control[i];
        float size = fmaxf(minimum, side * (i <= TOUCH_DPAD ? .16f : .08f) * p.scale);
        size = fminf(size, fminf(safe.w, safe.h - 2 * minimum));
        float x = safe.x + p.x * safe.w - size / 2;
        float y = safe.y + p.y * safe.h - size / 2;
        x = fmaxf(safe.x, fminf(x, safe.x + safe.w - size));
        y = fmaxf(safe.y + minimum, fminf(y, safe.y + safe.h - minimum - size));
        result.control[i] = (TouchRect){ x, y, size, size };
    }
    *geometry = result;
    return 1;
}

int touch_geometry_hit(const TouchGeometry *geometry, float x, float y) {
    if (!geometry || !isfinite(x) || !isfinite(y)) return -1;
    for (int i = TOUCH_COUNT - 1; i >= 0; i--) {
        TouchRect r = geometry->control[i];
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) return i;
    }
    return -1;
}

int touch_safe_pixels(TouchRect safe, int ww, int wh, int pw, int ph, TouchRect *out) {
    if (!out || ww <= 0 || wh <= 0 || pw <= 0 || ph <= 0 || !isfinite(safe.x) || !isfinite(safe.y) ||
        !isfinite(safe.w) || !isfinite(safe.h) || safe.x < 0 || safe.y < 0 || safe.w <= 0 || safe.h <= 0 ||
        safe.x + safe.w > ww || safe.y + safe.h > wh) return 0;
    float x = (float)pw / ww, y = (float)ph / wh;
    *out = (TouchRect){safe.x * x, safe.y * y, safe.w * x, safe.h * y};
    return 1;
}

int touch_geometry_accessible(const TouchGeometry *geometry) {
    if (!geometry) return 0;
    for (int i = 0; i < TOUCH_COUNT; i++) {
        TouchRect r = geometry->control[i];
        if (!isfinite(r.x) || !isfinite(r.y) || !isfinite(r.w) || !isfinite(r.h) || r.w <= 0 || r.h <= 0)
            return 0;
        /* Rectangle edges partition the target into cells with constant hit order. */
        float xs[2 + 2 * TOUCH_COUNT], ys[2 + 2 * TOUCH_COUNT];
        int nx = 0, ny = 0, found = 0;
        xs[nx++] = r.x; xs[nx++] = r.x + r.w;
        ys[ny++] = r.y; ys[ny++] = r.y + r.h;
        for (int j = i + 1; j < TOUCH_COUNT; j++) {
            TouchRect q = geometry->control[j];
            if (q.x > r.x && q.x < r.x + r.w) xs[nx++] = q.x;
            if (q.x + q.w > r.x && q.x + q.w < r.x + r.w) xs[nx++] = q.x + q.w;
            if (q.y > r.y && q.y < r.y + r.h) ys[ny++] = q.y;
            if (q.y + q.h > r.y && q.y + q.h < r.y + r.h) ys[ny++] = q.y + q.h;
        }
        for (int a = 1; a < nx; a++) for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) {
            float t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t;
        }
        for (int a = 1; a < ny; a++) for (int b = a; b > 0 && ys[b] < ys[b - 1]; b--) {
            float t = ys[b]; ys[b] = ys[b - 1]; ys[b - 1] = t;
        }
        for (int a = 0; a + 1 < nx && !found; a++) {
            if (xs[a] >= xs[a + 1]) continue;
            for (int c = 0; c + 1 < ny && !found; c++) {
                if (ys[c] < ys[c + 1] && touch_geometry_hit(geometry, (xs[a] + xs[a + 1]) / 2, (ys[c] + ys[c + 1]) / 2) == i)
                    found = 1;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

static void input_rebuild(TouchInput *input, const TouchGeometry *geometry) {
    static const int bits[TOUCH_COUNT] = { -1, -1, -1, 0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 4, 5 };
    TouchPad pad = {0};
    for (int i = 0; i < TOUCH_COUNT; i++) {
        if (!input->owner[i].active) continue;
        TouchRect r = geometry->control[i];
        double x = ((double)input->owner[i].x - ((double)r.x + r.w / 2)) / (r.w / 2);
        double y = (((double)r.y + r.h / 2) - input->owner[i].y) / (r.h / 2);
        if (i <= TOUCH_RIGHT_STICK) {
            double length = hypot(x, y);
            if (length < .15f) x = y = 0;
            else if (length > 1) { x /= length; y /= length; }
            if (i == TOUCH_LEFT_STICK) { pad.left_owned = 1; pad.lx = (int)(x * 32767); pad.ly = (int)(y * 32767); }
            else { pad.right_owned = 1; pad.rx = (int)(x * 32767); pad.ry = (int)(y * 32767); }
        } else if (i == TOUCH_DPAD) {
            if (x < -.25f) pad.buttons |= UINT64_C(1) << 12;
            if (y > .25f) pad.buttons |= UINT64_C(1) << 13;
            if (x > .25f) pad.buttons |= UINT64_C(1) << 14;
            if (y < -.25f) pad.buttons |= UINT64_C(1) << 15;
        } else pad.buttons |= UINT64_C(1) << bits[i];
    }
    pad.activity = input->activity;
    input->pad = pad;
}

void touch_input_invalidate(TouchInput *input) {
    __atomic_add_fetch(&input->invalidation, 1, __ATOMIC_ACQ_REL);
}

static void input_sync(TouchInput *input) {
    unsigned generation = __atomic_load_n(&input->invalidation, __ATOMIC_ACQUIRE);
    if (generation == input->accepted_generation) return;
    memset(input->owner, 0, sizeof input->owner);
    memset(&input->pad, 0, sizeof input->pad);
    input->accepted_generation = generation;
}

void touch_input_reset(TouchInput *input) { touch_input_invalidate(input); input_sync(input); }

int touch_input_reset_expected(TouchInput *input, unsigned generation) {
    unsigned expected = generation;
    if (!__atomic_compare_exchange_n(&input->invalidation, &expected, generation + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    input_sync(input);
    return 1;
}

TouchPad touch_input_snapshot(const TouchInput *input) {
    TouchPad neutral = {0};
    unsigned generation = __atomic_load_n(&input->invalidation, __ATOMIC_ACQUIRE);
    if (generation != input->accepted_generation) return neutral;
    TouchPad pad = input->pad;
    return generation == __atomic_load_n(&input->invalidation, __ATOMIC_ACQUIRE) ? pad : neutral;
}

void touch_input_down(TouchInput *input, const TouchGeometry *geometry, int64_t touch, int64_t finger, float x, float y) {
    input_sync(input);
    for (int i = 0; i < TOUCH_COUNT; i++)
        if (input->owner[i].active && input->owner[i].touch == touch && input->owner[i].finger == finger) return;
    int i = touch_geometry_hit(geometry, x, y);
    if (i < 0 || input->owner[i].active) return;
    input->owner[i].active = 1; input->owner[i].touch = touch; input->owner[i].finger = finger;
    input->owner[i].x = x; input->owner[i].y = y;
    input->activity++;
    input_rebuild(input, geometry);
}

void touch_input_motion(TouchInput *input, const TouchGeometry *geometry, int64_t touch, int64_t finger, float x, float y) {
    input_sync(input);
    if (!isfinite(x) || !isfinite(y)) return;
    for (int i = 0; i < TOUCH_COUNT; i++) {
        if (!input->owner[i].active || input->owner[i].touch != touch || input->owner[i].finger != finger) continue;
        input->owner[i].x = x; input->owner[i].y = y;
        input->activity++;
        input_rebuild(input, geometry);
        return;
    }
}

void touch_input_up(TouchInput *input, const TouchGeometry *geometry, int64_t touch, int64_t finger) {
    input_sync(input);
    for (int i = 0; i < TOUCH_COUNT; i++) {
        if (!input->owner[i].active || input->owner[i].touch != touch || input->owner[i].finger != finger) continue;
        input->owner[i].active = 0;
        input->activity++;
        input_rebuild(input, geometry);
        return;
    }
}

void touch_pad_merge(TouchPad pad, uint64_t *buttons, int *lx, int *ly, int *rx, int *ry) {
    *buttons |= pad.buttons;
    if (pad.left_owned) { *lx = pad.lx; *ly = pad.ly; }
    if (pad.right_owned) { *rx = pad.rx; *ry = pad.ry; }
}

static const char *const control_names[TOUCH_COUNT] = {
    "left_stick", "right_stick", "dpad", "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "l3", "r3"
};

int touch_config_path(const char *controller_path, char *out, unsigned size) {
    if (!controller_path || !*controller_path || !out || !size) return 0;
    const char *end = controller_path;
    for (const char *p = controller_path; *p; p++) if (*p == '/' || *p == '\\') end = p + 1;
    size_t prefix = (size_t)(end - controller_path);
    const char name[] = "touch_controls.ini";
    if (prefix + sizeof name > size) return 0;
    memcpy(out, controller_path, prefix);
    memcpy(out + prefix, name, sizeof name);
    return 1;
}

int touch_config_load(const char *path, TouchConfig *config) {
    if (!config || !path) return 0;
    config->enabled = 1; touch_layout_default(&config->layout);
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT;
    TouchLayout layout = config->layout;
    unsigned seen = 0;
    int version = -1, enabled = 1, seen_version = 0, seen_enabled = 0, bad_layout = 0, bad_header = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) { bad_header = 1; break; }
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r' || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) { bad_layout = 1; continue; }
        *eq++ = 0;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!strcmp(p, "version") || !strcmp(p, "enabled")) {
            char *tail; errno = 0; long n = strtol(eq, &tail, 10);
            while (*tail == ' ' || *tail == '\t' || *tail == '\r' || *tail == '\n') tail++;
            int valid = tail != eq && !*tail && errno != ERANGE;
            if (!strcmp(p, "version")) { if (seen_version++ || !valid || n != 1) bad_header = 1; else version = 1; }
            else { if (seen_enabled++ || !valid || (n != 0 && n != 1)) bad_header = 1; else enabled = (int)n; }
            continue;
        }
        for (int i = 0; i < TOUCH_COUNT; i++) if (!strcmp(p, control_names[i])) {
            int used = 0; TouchPlacement v;
            if ((seen & (1u << i)) || sscanf(eq, " %f , %f , %f , %f %n", &v.x, &v.y, &v.scale, &v.opacity, &used) != 4 || !used || eq[used]) bad_layout = 1;
            else layout.control[i] = v;
            seen |= 1u << i;
            break;
        }
    }
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) return 0;
    if (bad_header || version != 1 || !seen_enabled) return 1;
    config->enabled = enabled;
    if (!bad_layout && touch_layout_valid(&layout)) config->layout = layout;
    return 1;
}

int touch_config_save(const char *path, const TouchConfig *config) {
    if (!path || !config || (config->enabled != 0 && config->enabled != 1) || !touch_layout_valid(&config->layout)) return 0;
    size_t length = strlen(path);
    char *tmp = malloc(length + 64);
    if (!tmp) return 0;
    static unsigned serial;
    int fd = -1;
    for (int n = 0; n < 32; n++) {
        unsigned id = __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED);
        snprintf(tmp, length + 64, "%s.tmp.%ld.%u", path, (long)touch_pid(), id);
        fd = touch_open(tmp, O_CREAT | O_EXCL | O_WRONLY
#ifdef _WIN32
                        | O_BINARY
#endif
                        , 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { free(tmp); return 0; }
    FILE *f = touch_fdopen(fd, "wb");
    int ok = f != NULL;
    if (!f) touch_close(fd);
    if (f) {
        if (fprintf(f, "version = 1\nenabled = %d\n", config->enabled) < 0) ok = 0;
        for (int i = 0; i < TOUCH_COUNT; i++) {
            TouchPlacement p = config->layout.control[i];
            if (fprintf(f, "%s = %.9g, %.9g, %.9g, %.9g\n", control_names[i], p.x, p.y, p.scale, p.opacity) < 0) ok = 0;
        }
        if (fflush(f) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
    }
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) remove(tmp);
    free(tmp);
    return ok;
}

static struct {
    int lock, write_lock, initialized, valid, gamepad_valid, visible, settings_request, replay, menu, width, height;
    TouchPresentation presentation;
    unsigned settings_generation;
    int pointer_active, pointer_read, pointer_write;
    unsigned pointer_generation;
    int64_t pointer_touch, pointer_finger;
    TouchPointer pointer[32];
    int editing, editor_valid, selected, edit_error, edit_down, drag;
    float drag_x, drag_y;
    TouchRect editor_buttons[7];
    TouchConfig draft;
    TouchGeometry editor_geometry;
    char path[2048];
    float pixels_per_dp;
    TouchRect safe, settings;
    TouchConfig config;
    TouchLayout effective_layout;
    TouchGeometry geometry;
    TouchInput input;
} runtime;
/* Foreground starts true, focus is unknown until SDL reports it. */
static int lifecycle = 1;

/* One process-lifetime budget; never log normal play or an unbounded event stream. */
static int touch_diag_allowed(void) {
#ifdef __ANDROID__
    static unsigned count;
    if (diag_is_enabled()) {
        unsigned n = __atomic_load_n(&count, __ATOMIC_RELAXED);
        while (n < 128) {
            if (__atomic_compare_exchange_n(&count, &n, n + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 1;
        }
    }
#endif
    return 0;
}


static void runtime_lock(int *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) { }
}
static void runtime_unlock(int *lock) { __atomic_store_n(lock, 0, __ATOMIC_RELEASE); }

static int presentation_transform_valid(int transform) {
    return transform == 1 || transform == 2 || transform == 4 || transform == 8;
}

void touch_runtime_presentation(TouchPresentation p) {
    touch_runtime_invalidate();
    runtime_lock(&runtime.lock);
    p.sdl_width = runtime.width;
    p.sdl_height = runtime.height;
    p.valid = p.valid && presentation_transform_valid(p.transform) && p.width > 0 && p.height > 0 &&
              p.sdl_width > 0 && p.sdl_height > 0;
    p.generation = runtime.presentation.generation + 1;
    runtime.presentation = p;
    runtime.pointer_read = runtime.pointer_write = runtime.pointer_active = 0;
    runtime_unlock(&runtime.lock);
    if (!p.valid) touch_runtime_set_ready(0);
}

static int compact_layout(TouchRect safe, float dp, TouchLayout *layout, TouchGeometry *geometry) {
    float unit = 48 * dp;
    if (!isfinite(unit) || unit <= 0 || safe.h < 3 * unit) return 0;
    int cols = (int)fminf((float)TOUCH_COUNT, floorf(safe.w / unit));
    int rows = (int)fminf((float)TOUCH_COUNT, floorf((safe.h - 2 * unit) / unit));
    if (cols <= 0 || rows <= 0 || cols * rows < TOUCH_COUNT) return 0;
    rows = (TOUCH_COUNT + cols - 1) / cols;
    float cw = safe.w / cols, ch = (safe.h - 2 * unit) / rows;
    float side = fminf(cw, ch), short_side = fminf(safe.w, safe.h);
    touch_layout_default(layout);
    for (int i = 0; i < TOUCH_COUNT; i++) {
        TouchPlacement *p = &layout->control[i];
        p->x = ((i % cols) * cw + cw / 2) / safe.w;
        p->y = (unit + (i / cols) * ch + ch / 2) / safe.h;
        p->scale = fmaxf(.5f, fminf(2, side / (short_side * (i <= TOUCH_DPAD ? .16f : .08f))));
    }
    return touch_geometry_make(layout, safe, dp, geometry) && touch_geometry_accessible(geometry);
}

static int layout_usable(const TouchLayout *layout, TouchRect safe, float dp) {
    TouchGeometry geometry;
    if (touch_geometry_make(layout, safe, dp, &geometry) && touch_geometry_accessible(&geometry)) return 1;
    TouchLayout standard, compact;
    touch_layout_default(&standard);
    return !memcmp(layout, &standard, sizeof standard) && compact_layout(safe, dp, &compact, &geometry);
}

static void runtime_layout(void) {
    runtime.effective_layout = runtime.config.layout;
    runtime.gamepad_valid = touch_geometry_make(&runtime.effective_layout, runtime.safe, runtime.pixels_per_dp, &runtime.geometry) &&
                            touch_geometry_accessible(&runtime.geometry);
    if (runtime.gamepad_valid) return;
    touch_layout_default(&runtime.effective_layout);
    runtime.gamepad_valid = touch_geometry_make(&runtime.effective_layout, runtime.safe, runtime.pixels_per_dp, &runtime.geometry) &&
                            touch_geometry_accessible(&runtime.geometry);
    if (runtime.gamepad_valid) return;
    runtime.gamepad_valid = compact_layout(runtime.safe, runtime.pixels_per_dp, &runtime.effective_layout, &runtime.geometry);
}

static void editor_refresh(void) {
    float unit = 48 * runtime.pixels_per_dp;
    memset(runtime.editor_buttons, 0, sizeof runtime.editor_buttons);
    runtime.drag = runtime.edit_down = -1;
    runtime.editor_valid = runtime.valid && isfinite(unit) && unit > 0 && runtime.safe.w >= 5 * unit && runtime.safe.h >= 3 * unit &&
                           touch_geometry_make(&runtime.draft.layout, runtime.safe, runtime.pixels_per_dp, &runtime.editor_geometry);
    if (!runtime.editor_valid) {
        runtime.edit_error = 1;
        if (runtime.valid) runtime.editor_buttons[5] = (TouchRect){runtime.safe.x, runtime.safe.y + runtime.safe.h - unit, unit, unit};
        return;
    }
    for (int i = 0; i < 4; i++) {
        float x = i < 2 ? runtime.safe.x + i * unit : runtime.safe.x + runtime.safe.w - (4 - i) * unit;
        runtime.editor_buttons[i] = (TouchRect){x, runtime.safe.y, unit, unit};
    }
    float width = runtime.safe.w / 3;
    for (int i = 0; i < 3; i++) runtime.editor_buttons[4 + i] = (TouchRect){runtime.safe.x + i * width, runtime.safe.y + runtime.safe.h - unit, width, unit};
}

void touch_runtime_init(const char *controller_path) {
    TouchConfig config = {0};
    config.enabled = 1; touch_layout_default(&config.layout);
    char path[sizeof runtime.path] = {0};
    if (touch_config_path(controller_path, path, sizeof path) && !touch_config_load(path, &config))
        fprintf(stderr, "touch: cannot read settings %s\n", path);
    touch_runtime_invalidate();
    runtime_lock(&runtime.lock);
    runtime.config = config;
    memcpy(runtime.path, path, sizeof path);
    runtime.initialized = 1; runtime.visible = 0; runtime.menu = 0; runtime.editing = 0;
    runtime_unlock(&runtime.lock);
}

void touch_runtime_invalidate(void) {
    touch_input_invalidate(&runtime.input);
    __atomic_store_n(&runtime.settings_request, 0, __ATOMIC_RELEASE);
}

void touch_runtime_foreground(int foreground) {
    if (foreground) __atomic_fetch_or(&lifecycle, 1, __ATOMIC_ACQ_REL);
    else { __atomic_fetch_and(&lifecycle, ~1, __ATOMIC_ACQ_REL); touch_runtime_invalidate(); }
}

void touch_runtime_focus(int focused, int initial) {
    int old, next;
    do {
        old = __atomic_load_n(&lifecycle, __ATOMIC_ACQUIRE);
        if (initial && (old & 4)) return;
        next = (old | 4) & ~2;
        if (focused) next |= 2;
    } while (!__atomic_compare_exchange_n(&lifecycle, &old, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    if (!focused) touch_runtime_invalidate();
}

static int lifecycle_ready(void) { return (__atomic_load_n(&lifecycle, __ATOMIC_ACQUIRE) & 3) == 3; }

void touch_runtime_set_replay(int replay) {
    replay = !!replay;
    if (__atomic_exchange_n(&runtime.replay, replay, __ATOMIC_ACQ_REL) != replay)
        touch_runtime_invalidate();
}

void touch_runtime_geometry(TouchRect safe, float pixels_per_dp, int width, int height) {
    touch_runtime_invalidate();
    runtime_lock(&runtime.lock);
    runtime.safe = safe;
    runtime.width = width; runtime.height = height;
    if (runtime.presentation.sdl_width != width || runtime.presentation.sdl_height != height) {
        runtime.presentation.sdl_width = width;
        runtime.presentation.sdl_height = height;
        runtime.presentation.generation++;
        runtime.presentation.valid = presentation_transform_valid(runtime.presentation.transform) &&
            runtime.presentation.width > 0 && runtime.presentation.height > 0 && width > 0 && height > 0;
    }
    runtime.pixels_per_dp = pixels_per_dp;
    float size = 48 * pixels_per_dp;
    runtime.valid = width > 0 && height > 0 && runtime.initialized && isfinite(safe.x) && isfinite(safe.y) &&
                    isfinite(safe.w) && isfinite(safe.h) && isfinite(size) && size > 0 && safe.w >= size && safe.h >= size &&
                    safe.x >= 0 && safe.y >= 0 && safe.x + safe.w <= width && safe.y + safe.h <= height;
    if (runtime.valid) {
        runtime.settings = (TouchRect){ safe.x + (safe.w - size) / 2, safe.y, size, size };
        runtime_layout();
    } else {
        runtime.gamepad_valid = 0;
    }
    if (runtime.editing) editor_refresh();
    runtime_unlock(&runtime.lock);
}

void touch_runtime_set_ready(int ready) {
    if (!ready) touch_runtime_invalidate();
    runtime_lock(&runtime.lock);
    runtime.visible = !!ready;
    if (!ready && runtime.editing) { runtime.editing = 0; runtime.edit_error = 1; runtime.drag = -1; }
    runtime_unlock(&runtime.lock);
}

unsigned touch_runtime_generation(void) { return __atomic_load_n(&runtime.input.invalidation, __ATOMIC_ACQUIRE); }

void touch_runtime_event(int type, int64_t touch, int64_t finger, float x, float y) {
    touch_runtime_event_at(touch_runtime_generation(), type, touch, finger, x, y);
}

void touch_runtime_event_at(unsigned expected, int type, int64_t touch, int64_t finger, float x, float y) {
    runtime_lock(&runtime.lock);
    unsigned generation = __atomic_load_n(&runtime.input.invalidation, __ATOMIC_ACQUIRE);
    int diagnostic = touch_diag_allowed();
    if (diagnostic) {
        TouchRect r = runtime.settings;
        int state = __atomic_load_n(&lifecycle, __ATOMIC_ACQUIRE);
        int replay = __atomic_load_n(&runtime.replay, __ATOMIC_ACQUIRE);
        int hit = isfinite(x) && isfinite(y) && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
        const char *disposition = generation != expected ? "stale-generation" : !runtime.valid ? "invalid-geometry" :
                !(runtime.visible || runtime.menu) ? "not-visible" : (state & 3) != 3 ? "lifecycle" : replay ? "replay" :
                runtime.menu ? "menu" : runtime.config.enabled && type == 0 && hit ? "SET-hit" :
                runtime.config.enabled && runtime.gamepad_valid ? "gamepad-path" : "gamepad-disabled";
        fprintf(stderr, "touch-diag runtime type=%d touch=%lld finger=%lld px=%.3f py=%.3f expected=%u generation=%u generation_match=%d valid=%d visible=%d lifecycle=%d replay=%d menu=%d SET=%.3f,%.3f,%.3f,%.3f hit=%d disposition=%s snapshot=advisory\n",
                type, (long long)touch, (long long)finger, x, y, expected, generation, generation == expected,
                runtime.valid, runtime.visible, state, replay, runtime.menu, r.x, r.y, r.w, r.h, hit, disposition);
    }
    if (generation != expected) { runtime_unlock(&runtime.lock); return; }
    if (runtime.valid && (runtime.visible || runtime.menu) && lifecycle_ready() && !__atomic_load_n(&runtime.replay, __ATOMIC_ACQUIRE)) {
        TouchRect r = runtime.settings;
        if (runtime.menu) {
            unsigned generation = __atomic_load_n(&runtime.input.invalidation, __ATOMIC_ACQUIRE);
            if (generation != runtime.pointer_generation) {
                runtime.pointer_read = runtime.pointer_write = runtime.pointer_active = 0;
                runtime.pointer_generation = generation;
            }
            if (type == 0 && !runtime.pointer_active) {
                runtime.pointer_active = 1; runtime.pointer_touch = touch; runtime.pointer_finger = finger;
            }
            if (runtime.pointer_active && runtime.pointer_touch == touch && runtime.pointer_finger == finger) {
                int next = (runtime.pointer_write + 1) % 32;
                if (next != runtime.pointer_read) {
                    runtime.pointer[runtime.pointer_write] = (TouchPointer){type, x, y};
                    runtime.pointer_write = next;
                } else { runtime.pointer_read = runtime.pointer_write; runtime.pointer_active = 0; }
                if (type == 2 || type == 3) runtime.pointer_active = 0;
            }
        } else if (runtime.config.enabled && type == 0 && isfinite(x) && isfinite(y) && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) {
            if (touch_input_reset_expected(&runtime.input, generation)) {
                generation++;
                __atomic_store_n(&runtime.settings_generation, generation, __ATOMIC_RELEASE);
                __atomic_store_n(&runtime.settings_request, 1, __ATOMIC_RELEASE);
                if (diagnostic) fprintf(stderr, "touch-diag request raised generation=%u\n", generation);
            }
        } else if (runtime.config.enabled && runtime.gamepad_valid) {
            if (type == 0) touch_input_down(&runtime.input, &runtime.geometry, touch, finger, x, y);
            else if (type == 1) touch_input_motion(&runtime.input, &runtime.geometry, touch, finger, x, y);
            else if (type == 2 || type == 3) touch_input_up(&runtime.input, &runtime.geometry, touch, finger);
        }
    }
    if (generation != __atomic_load_n(&runtime.input.invalidation, __ATOMIC_ACQUIRE) || !lifecycle_ready()) {
        touch_input_reset(&runtime.input);
        runtime.pointer_read = runtime.pointer_write = runtime.pointer_active = 0;
        __atomic_store_n(&runtime.settings_request, 0, __ATOMIC_RELEASE);
        if (diagnostic) fprintf(stderr, "touch-diag request cleared lifecycle-or-generation generation=%u current=%u\n",
                                generation, touch_runtime_generation());
    }
    runtime_unlock(&runtime.lock);
}

void touch_runtime_menu(int open) {
    touch_runtime_invalidate();
    runtime_lock(&runtime.lock);
    runtime.menu = !!open;
    if (!open) runtime.editing = 0;
    runtime.pointer_read = runtime.pointer_write = runtime.pointer_active = 0;
    runtime.pointer_generation = __atomic_load_n(&runtime.input.invalidation, __ATOMIC_ACQUIRE);
    runtime_unlock(&runtime.lock);
}

int touch_runtime_pointer(TouchPointer *event) {
    int result = 0;
    runtime_lock(&runtime.lock);
    if (!lifecycle_ready() || runtime.pointer_generation != __atomic_load_n(&runtime.input.invalidation, __ATOMIC_ACQUIRE)) {
        runtime.pointer_read = runtime.pointer_write = runtime.pointer_active = 0;
    } else if (runtime.pointer_read != runtime.pointer_write) {
        *event = runtime.pointer[runtime.pointer_read];
        runtime.pointer_read = (runtime.pointer_read + 1) % 32;
        result = 1;
    }
    runtime_unlock(&runtime.lock);
    return result;
}

TouchSnapshot touch_runtime_snapshot(void) {
    TouchSnapshot snapshot = {0};
    runtime_lock(&runtime.lock);
    snapshot.config = runtime.config; snapshot.layout = runtime.effective_layout; snapshot.geometry = runtime.geometry;
    snapshot.editing = runtime.editing; snapshot.selected = runtime.selected; snapshot.edit_error = runtime.edit_error;
    memcpy(snapshot.editor_buttons, runtime.editor_buttons, sizeof snapshot.editor_buttons);
    if (runtime.editing) { snapshot.layout = runtime.draft.layout; snapshot.geometry = runtime.editor_geometry; snapshot.gamepad_valid = runtime.editor_valid; }
    snapshot.safe = runtime.safe; snapshot.settings = runtime.settings; snapshot.width = runtime.width; snapshot.height = runtime.height;
    snapshot.valid = runtime.valid; if (!runtime.editing) snapshot.gamepad_valid = runtime.gamepad_valid; snapshot.ready = runtime.valid && runtime.visible;
    snapshot.presentation = runtime.presentation;
    if (snapshot.ready && runtime.gamepad_valid && runtime.config.enabled && lifecycle_ready() && !__atomic_load_n(&runtime.replay, __ATOMIC_ACQUIRE)) snapshot.pad = touch_input_snapshot(&runtime.input);
    runtime_unlock(&runtime.lock);
    return snapshot;
}

int touch_scene_build(const TouchSnapshot *s, int menu_open, TouchSprite out[TOUCH_SCENE_MAX]) {
    if (!s || !out || !s->valid || (menu_open && !s->editing)) return 0;
    int n = 0;
    if ((s->config.enabled || s->editing) && s->gamepad_valid) for (int i = 0; i < TOUCH_COUNT; i++) {
        TouchRect r = s->geometry.control[i];
        out[n++] = (TouchSprite){r, s->editing && i == s->selected ? 1 : s->layout.control[i].opacity, i};
        if (i <= TOUCH_RIGHT_STICK) {
            int owned = i == TOUCH_LEFT_STICK ? s->pad.left_owned : s->pad.right_owned;
            float x = (i == TOUCH_LEFT_STICK ? s->pad.lx : s->pad.rx) / 32767.0f;
            float y = (i == TOUCH_LEFT_STICK ? s->pad.ly : s->pad.ry) / 32767.0f;
            TouchRect knob = {r.x + r.w * (.325f + .25f * x), r.y + r.h * (.325f - .25f * y), r.w * .35f, r.h * .35f};
            out[n++] = (TouchSprite){knob, owned ? .9f : .55f, TOUCH_COUNT + 1};
        }
    }
    if (s->config.enabled) out[n++] = (TouchSprite){s->settings, .9f, TOUCH_COUNT};
    if (s->editing) {
        for (int i = 0; i < 7; i++) if (s->editor_buttons[i].w > 0 && s->editor_buttons[i].h > 0)
            out[n++] = (TouchSprite){s->editor_buttons[i], .95f, TOUCH_COUNT + 2 + i};
        if (s->edit_error) {
            float u = s->settings.h;
            TouchRect error = {s->safe.x + (s->safe.w - 2 * u) / 2, s->safe.y + s->safe.h - 2 * u, 2 * u, u};
            out[n++] = (TouchSprite){error, 1, TOUCH_COUNT + 9};
        }
    }
    return n;
}

int touch_runtime_editing(void) {
    runtime_lock(&runtime.lock); int editing = runtime.editing; runtime_unlock(&runtime.lock); return editing;
}

int touch_runtime_editor_begin(void) {
    runtime_lock(&runtime.lock);
    float unit = 48 * runtime.pixels_per_dp;
    int ok = runtime.menu && runtime.visible && runtime.valid && runtime.gamepad_valid && runtime.safe.w >= 5 * unit && runtime.safe.h >= 3 * unit;
    if (ok) {
        runtime.draft = runtime.config;
        runtime.draft.layout = runtime.effective_layout;
        runtime.selected = TOUCH_LEFT_STICK; runtime.edit_error = 0; runtime.drag = -1; runtime.edit_down = -1;
        runtime.editing = 1;
        editor_refresh();
    }
    runtime_unlock(&runtime.lock);
    return ok;
}

static int point_in(TouchRect r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

int touch_runtime_editor_event(TouchPointer event) {
    int action = 0;
    runtime_lock(&runtime.lock);
    if (!runtime.editing) { runtime_unlock(&runtime.lock); return 0; }
    if (event.type != 3 && (!isfinite(event.x) || !isfinite(event.y))) { runtime_unlock(&runtime.lock); return 0; }
    if (event.type == 0) {
        runtime.edit_down = -1; runtime.drag = -1;
        for (int i = 0; i < 7; i++) if (point_in(runtime.editor_buttons[i], event.x, event.y)) { runtime.edit_down = i; break; }
        if (runtime.edit_down < 0 && runtime.editor_valid) {
            int i = touch_geometry_hit(&runtime.editor_geometry, event.x, event.y);
            if (i >= 0) {
                TouchRect r = runtime.editor_geometry.control[i];
                runtime.selected = runtime.drag = i;
                runtime.drag_x = event.x - (r.x + r.w / 2); runtime.drag_y = event.y - (r.y + r.h / 2);
            }
        }
    } else if (event.type == 1 && runtime.drag >= 0) {
        TouchPlacement *p = &runtime.draft.layout.control[runtime.drag];
        p->x = fmaxf(0, fminf(1, (event.x - runtime.drag_x - runtime.safe.x) / runtime.safe.w));
        p->y = fmaxf(0, fminf(1, (event.y - runtime.drag_y - runtime.safe.y) / runtime.safe.h));
        touch_geometry_make(&runtime.draft.layout, runtime.safe, runtime.pixels_per_dp, &runtime.editor_geometry);
        runtime.edit_error = 0;
    } else if (event.type == 2) {
        int b = runtime.edit_down;
        if (b >= 0 && point_in(runtime.editor_buttons[b], event.x, event.y)) {
            TouchPlacement *p = &runtime.draft.layout.control[runtime.selected];
            if (b == 0) p->scale = fmaxf(.5f, p->scale - .1f);
            if (b == 1) p->scale = fminf(2, p->scale + .1f);
            if (b == 2) p->opacity = fmaxf(.2f, p->opacity - .1f);
            if (b == 3) p->opacity = fminf(1, p->opacity + .1f);
            if (b == 4) action = 1;
            if (b == 5) action = 2;
            if (b == 6) touch_layout_default(&runtime.draft.layout);
            touch_geometry_make(&runtime.draft.layout, runtime.safe, runtime.pixels_per_dp, &runtime.editor_geometry);
            runtime.edit_error = 0;
        }
        runtime.edit_down = -1; runtime.drag = -1;
    } else if (event.type == 3) { runtime.edit_down = -1; runtime.drag = -1; }
    runtime_unlock(&runtime.lock);
    return action;
}

int touch_runtime_editor_save(void) {
    runtime_lock(&runtime.lock);
    if (!runtime.editing) { runtime_unlock(&runtime.lock); return 0; }
    TouchConfig config = runtime.draft;
    int usable = layout_usable(&config.layout, runtime.safe, runtime.pixels_per_dp);
    runtime_unlock(&runtime.lock);
    int ok = usable && touch_runtime_save(&config);
    runtime_lock(&runtime.lock);
    if (ok) runtime.editing = 0; else runtime.edit_error = 1;
    runtime_unlock(&runtime.lock);
    return ok;
}

void touch_runtime_editor_cancel(void) {
    runtime_lock(&runtime.lock); runtime.editing = 0; runtime.drag = -1; runtime_unlock(&runtime.lock);
}

int touch_runtime_take_settings(void) {
    int request = __atomic_exchange_n(&runtime.settings_request, 0, __ATOMIC_ACQ_REL);
    unsigned generation = __atomic_load_n(&runtime.settings_generation, __ATOMIC_ACQUIRE);
    int accepted = request && lifecycle_ready() && !__atomic_load_n(&runtime.replay, __ATOMIC_ACQUIRE) &&
            generation == __atomic_load_n(&runtime.input.invalidation, __ATOMIC_ACQUIRE);
    if (request && touch_diag_allowed())
        fprintf(stderr, "touch-diag request consumed request=%d accepted=%d generation=%u current=%u lifecycle=%d replay=%d\n",
                request, accepted, generation, touch_runtime_generation(), __atomic_load_n(&lifecycle, __ATOMIC_ACQUIRE),
                __atomic_load_n(&runtime.replay, __ATOMIC_ACQUIRE));
    return accepted;
}

int touch_runtime_save(const TouchConfig *config) {
    if (!config) return 0;
    runtime_lock(&runtime.write_lock);
    char path[sizeof runtime.path];
    runtime_lock(&runtime.lock);
    memcpy(path, runtime.path, sizeof path);
    int layout_changed = memcmp(&runtime.config.layout, &config->layout, sizeof config->layout) != 0;
    int layout_ok = !layout_changed || (runtime.valid && layout_usable(&config->layout, runtime.safe, runtime.pixels_per_dp));
    runtime_unlock(&runtime.lock);
    int ok = path[0] && layout_ok && touch_config_save(path, config);
    if (ok) {
        touch_runtime_invalidate();
        runtime_lock(&runtime.lock);
        runtime.config = *config;
        if (runtime.valid) runtime_layout();
        runtime_unlock(&runtime.lock);
    }
    runtime_unlock(&runtime.write_lock);
    return ok;
}
