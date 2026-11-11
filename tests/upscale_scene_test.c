#include "upscale_scene.h"
#include <assert.h>
#include <stdio.h>

struct Tex { int tag; };
int main(void) {
    UpscaleScene s = {0}; struct Tex c1 = {0}, c2 = {0}, d1 = {0}, d2 = {0}; int cv1 = 0, cv2 = 0, dv1 = 0, dv2 = 0;
    float guest[64] = {0}; guest[30] = -1.0f; guest[22] = 0.1f; guest[0] = 7.0f;
    s = (UpscaleScene)UPSCALE_SCENE_INITIALIZER;
    upscale_scene_observe(&s, &c1, &cv1, &d1, &dv1, guest);
    assert(upscale_scene_matches(&s, &c1, &cv1, &d1, &dv1));
    assert(s.ran != s.frame && s.up_frame != s.frame);
    s = (UpscaleScene){0};
    s.frame = 1;
    assert(upscale_scene_reset(&s, 1, 0, 0, 4));
    assert(upscale_scene_reset(&s, 0, 1, 0, 4));
    s.have_surface = 1; s.surface_generation = 4;
    assert(!upscale_scene_reset(&s, 0, 1, 0, 4));
    assert(upscale_scene_reset(&s, 0, 1, 1, 4));
    assert(upscale_scene_reset(&s, 0, 1, 0, 5));
    assert(!upscale_scene_matches(&s, &c1, &cv1, &d1, &dv1));
    upscale_scene_observe(&s, &c1, &cv1, &d1, &dv1, guest);
    assert(upscale_scene_matches(&s, &c1, &cv1, &d1, &dv1));
    { double x, y; upscale_scene_jitter(&s, 1, 1, &x, &y); assert(x == 0.0 && y < -0.09 && y > -0.11); upscale_scene_jitter(&s, 100, 100, &x, &y); assert(x == 0.0 && y == 0.0); }
    guest[0] = 99.0f; assert(s.c3[0] == 7.0f);
    guest[0] = 7.0f;
    guest[22] = 0.0f;
    upscale_scene_observe(&s, &c1, &cv1, &d1, &dv1, guest);
    assert(s.c3[0] == 7.0f); /* A later unjittered camera must not replace the jittered one. */
    guest[22] = 0.2f;
    assert(!upscale_scene_matches(&s, &c2, &cv1, &d1, &dv1));
    assert(!upscale_scene_matches(&s, &c1, &cv2, &d1, &dv1));
    assert(!upscale_scene_matches(&s, &c1, &cv1, &d2, &dv1));
    assert(!upscale_scene_matches(&s, &c1, &cv1, &d1, &dv2));
    upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, NULL);
    assert(!upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2));
    upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, guest);
    assert(upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2));
    /* The first bloom or tonemap can claim the scene; a second consumer cannot. */
    s.ran = s.frame;
    assert(!upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2));
    s.frame++;
    assert(!upscale_scene_reset(&s, 0, 1, 0, 4));
    assert(!upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2));
    upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, NULL);
    assert(!upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2));
    upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, guest);
    assert(upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2));
    guest[0] = 123.0f;
    upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, guest);
    assert(!upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2)); /* ambiguous cameras are not mixed */
    guest[0] = 7.0f;
    upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, guest);
    assert(!upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2));
    s.frame++;
    assert(upscale_scene_reset(&s, 0, 1, 0, 4)); /* prior scene frame was skipped */
    upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, guest);
    guest[22] += 0.1f;
    upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, guest);
    assert(!upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2)); /* mismatched raster jitter */
    s.frame++; upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, guest);
    guest[32] = 1.0f; upscale_scene_observe(&s, &c2, &cv2, &d2, &dv2, guest);
    assert(!upscale_scene_matches(&s, &c2, &cv2, &d2, &dv2)); /* mismatched view-projection */
    puts("temporal scene identity checks passed");
    return 0;
}
