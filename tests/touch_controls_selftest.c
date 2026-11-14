#include "../runtime/touch_controls.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    touch_runtime_init(NULL);
    touch_runtime_focus(1, 1);
    touch_runtime_presentation((TouchPresentation){8, 800, 600, 0, 0, 1, 0}); /* swapchain first; SDL dimensions arrive later */
    touch_runtime_geometry((TouchRect){0, 0, 800, 600}, 1, 800, 600);
    assert(touch_runtime_snapshot().presentation.valid);
    touch_runtime_set_ready(1);
    TouchSnapshot live = touch_runtime_snapshot();
    TouchRect live_a = live.geometry.control[TOUCH_A];
    touch_runtime_event(0, 1, 1, live_a.x + live_a.w / 2, live_a.y + live_a.h / 2);
    assert(touch_runtime_snapshot().pad.buttons & 1);
    touch_runtime_presentation((TouchPresentation){0, 0, 0, 800, 600, 0, 0});
    assert(!touch_runtime_snapshot().ready && !touch_runtime_snapshot().pad.buttons);
    touch_runtime_presentation((TouchPresentation){8, 800, 600, 800, 600, 1, 0});
    touch_runtime_set_ready(1); /* redraw must not restore the held contact */
    assert(!touch_runtime_snapshot().pad.buttons);
    touch_runtime_event(0, 1, 2, live_a.x + live_a.w / 2, live_a.y + live_a.h / 2);
    assert(touch_runtime_snapshot().pad.buttons & 1);
    touch_runtime_set_ready(0);
    touch_runtime_geometry((TouchRect){0, 0, 800, 600}, 1, 800, 600); /* geometry first; recreate publishes extent/transform */
    touch_runtime_presentation((TouchPresentation){8, 800, 600, 0, 1, 1, 0}); /* stale caller dimensions cannot override runtime geometry */
    assert(touch_runtime_snapshot().presentation.valid && touch_runtime_snapshot().presentation.sdl_width == 800 &&
           touch_runtime_snapshot().presentation.sdl_height == 600);

    TouchPresentation presentation = {8, 2248, 2480, 2248, 2480, 1, 1};
    float mx, my;
    assert(touch_presentation_map(&presentation, 1124, 224, &mx, &my));
    assert(fabsf(mx - 203.05f) < .02f && fabsf(my - 1240) < .01f);
    assert(!touch_presentation_map(&(TouchPresentation){16,2248,2480,2248,2480,1,1}, 1, 1, &mx, &my));
    for (int i = 0; i < 4; i++) {
        static const int transforms[] = {1,2,4,8};
        presentation.transform = transforms[i];
        assert(touch_presentation_map(&presentation, 0, 0, &mx, &my));
        assert(touch_presentation_map(&presentation, 2247, 2479, &mx, &my));
    }
    TouchLayout layout;
    TouchGeometry original, resized, restored;
    TouchRect safe = { 10, 20, 1920, 1080 };
    TouchRect pixels;
    assert(touch_safe_pixels((TouchRect){10, 20, 300, 200}, 400, 300, 800, 900, &pixels));
    assert(pixels.x == 20 && pixels.y == 60 && pixels.w == 600 && pixels.h == 600);
    assert(!touch_safe_pixels((TouchRect){10, 20, 500, 200}, 400, 300, 800, 900, &pixels));
    touch_layout_default(&layout);
    assert(touch_layout_valid(&layout));
    assert(touch_geometry_make(&layout, safe, 1, &original));
    assert(touch_geometry_accessible(&original));
    for (int i = 0; i < TOUCH_COUNT; i++) {
        TouchRect r = original.control[i];
        assert(r.w >= 48 && r.h >= 48);
        assert(r.x >= safe.x && r.y >= safe.y && r.x + r.w <= safe.x + safe.w && r.y + r.h <= safe.y + safe.h);
        assert(touch_geometry_hit(&original, r.x + r.w / 2, r.y + r.h / 2) == i);
    }
    assert(touch_geometry_hit(&original, 0, 0) == -1);
    TouchGeometry edge = {0};
    edge.control[TOUCH_A] = (TouchRect){100, 200, 48, 48};
    assert(touch_geometry_hit(&edge, 100, 200) == TOUCH_A);
    assert(touch_geometry_hit(&edge, 148, 200) == -1);
    assert(touch_geometry_hit(&edge, 100, 248) == -1);
    assert(touch_geometry_make(&layout, safe, 2, &resized));
    for (int i = 0; i < TOUCH_COUNT; i++) assert(resized.control[i].w >= 96 && resized.control[i].h >= 96);
    assert(touch_geometry_accessible(&resized));
    assert(touch_geometry_make(&layout, safe, 3, &resized));
    assert(touch_geometry_accessible(&resized));
    assert(touch_geometry_make(&layout, (TouchRect){0, 0, 900, 900}, 1, &resized));
    assert(touch_geometry_make(&layout, safe, 1, &restored));
    for (int i = 0; i < TOUCH_COUNT; i++) {
        assert(original.control[i].x == restored.control[i].x);
        assert(original.control[i].y == restored.control[i].y);
    }
    layout.control[TOUCH_A].x = NAN;
    assert(!touch_layout_valid(&layout));
    touch_layout_default(&layout);
    layout.control[TOUCH_A].scale = .49f;
    assert(!touch_layout_valid(&layout));
    layout.control[TOUCH_A].scale = 2.01f;
    assert(!touch_layout_valid(&layout));
    touch_layout_default(&layout);
    layout.control[TOUCH_A].opacity = .19f;
    assert(!touch_layout_valid(&layout));
    layout.control[TOUCH_A].opacity = INFINITY;
    assert(!touch_layout_valid(&layout));
    touch_layout_default(&layout);
    layout.control[TOUCH_A].x = 1.01f;
    assert(!touch_layout_valid(&layout));
    touch_layout_default(&layout);
    layout.control[TOUCH_A] = layout.control[TOUCH_B];
    assert(touch_geometry_make(&layout, safe, 1, &resized));
    assert(!touch_geometry_accessible(&resized));
    touch_layout_default(&layout);
    assert(touch_geometry_make(&layout, safe, 1, &resized));
    resized.control[TOUCH_A] = original.control[TOUCH_B];
    resized.control[TOUCH_A].x += resized.control[TOUCH_A].w / 2;
    assert(touch_geometry_accessible(&resized));
    assert(!touch_geometry_make(&layout, safe, 100, &resized));
    TouchInput input = {0};
    TouchRect left = original.control[TOUCH_LEFT_STICK], right = original.control[TOUCH_RIGHT_STICK], a = original.control[TOUCH_A];
    touch_input_down(&input, &original, 1, 10, left.x + left.w / 2, left.y + left.h / 2);
    touch_input_down(&input, &original, 1, 11, right.x + right.w / 2, right.y + right.h / 2);
    touch_input_down(&input, &original, 1, 12, a.x + a.w / 2, a.y + a.h / 2);
    assert(input.pad.left_owned && input.pad.right_owned && (input.pad.buttons & 1));
    uint64_t buttons = UINT64_C(1) << 9;
    int lx = 20, ly = 30, rx = 40, ry = 50;
    touch_pad_merge(input.pad, &buttons, &lx, &ly, &rx, &ry);
    assert(buttons == ((UINT64_C(1) << 9) | 1) && lx == 0 && ly == 0 && rx == 0 && ry == 0);
    touch_input_down(&input, &original, 1, 13, a.x + a.w / 2, a.y + a.h / 2);
    touch_input_up(&input, &original, 1, 12);
    assert(input.pad.left_owned && input.pad.right_owned && !(input.pad.buttons & 1));
    touch_input_motion(&input, &original, 1, 10, left.x - left.w * 20, left.y + left.h / 2);
    assert(input.pad.lx == -32767);
    touch_input_invalidate(&input);
    assert(!touch_input_snapshot(&input).left_owned);
    unsigned before_interrupt = input.invalidation;
    touch_input_invalidate(&input);
    assert(!touch_input_reset_expected(&input, before_interrupt));
    touch_input_motion(&input, &original, 1, 10, left.x, left.y);
    assert(!touch_input_snapshot(&input).left_owned);
    touch_input_reset(&input);
    touch_input_motion(&input, &original, 1, 10, left.x, left.y);
    assert(!input.pad.left_owned && !input.pad.buttons);
    touch_input_down(&input, &original, 1, 10, a.x + a.w / 2, a.y + a.h / 2);
    assert(input.pad.buttons & 1);
    touch_input_motion(&input, &original, 1, 10, -100, -100);
    assert(input.pad.buttons & 1);
    touch_input_up(&input, &original, 1, 10);
    TouchRect d = original.control[TOUCH_DPAD];
    touch_input_down(&input, &original, 1, 10, d.x + d.w * .1f, d.y + d.h * .1f);
    assert((input.pad.buttons & (UINT64_C(1) << 12 | UINT64_C(1) << 13)) == (UINT64_C(1) << 12 | UINT64_C(1) << 13));
    touch_input_reset(&input);
    const int bits[] = { 0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 4, 5 };
    for (int i = TOUCH_A; i < TOUCH_COUNT; i++) {
        TouchRect r = original.control[i];
        touch_input_down(&input, &original, 9, i, r.x + r.w / 2, r.y + r.h / 2);
        assert(input.pad.buttons == (UINT64_C(1) << bits[i - TOUCH_A]));
        touch_input_up(&input, &original, 9, i);
        assert(!input.pad.buttons);
    }
    puts("touch geometry self-test passed");
    return 0;
}
