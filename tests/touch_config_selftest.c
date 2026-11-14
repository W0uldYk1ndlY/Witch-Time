#include "../runtime/touch_controls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
typedef struct { HANDLE go, done; TouchRect a, settings; } Race;
static DWORD WINAPI blocked_finger(void *arg) {
    Race *r = arg;
    assert(WaitForSingleObject(r->go, 5000) == WAIT_OBJECT_0);
    touch_runtime_event(0, 7, 8, r->a.x + r->a.w / 2, r->a.y + r->a.h / 2);
    touch_runtime_event(0, 7, 9, r->settings.x + 1, r->settings.y + 1);
    assert(SetEvent(r->done));
    return 0;
}
#endif

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fputs(text, f) >= 0); assert(fclose(f) == 0);
}
static void expect_text(const char *path, const char *text) {
    char buf[4096]; FILE *f = fopen(path, "rb"); assert(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; assert(fclose(f) == 0);
    assert(!strcmp(buf, text));
}
int main(int argc, char **argv) {
    assert(argc == 3);
    char path[1024], other[1024], computed[1024], impossible[1024];
    assert(snprintf(path, sizeof path, "%s/touch_controls.ini", argv[1]) > 0);
    assert(snprintf(other, sizeof other, "%s/controllers.ini", argv[1]) > 0);
    assert(touch_config_path(other, computed, sizeof computed) && !strcmp(path, computed));
    assert(!touch_config_path(other, computed, 4));
    TouchConfig cfg, out;
    touch_runtime_focus(1, 1);
    assert(touch_config_load(argv[2], &out) && out.enabled == 1 && out.layout.control[TOUCH_A].x == .91f);
    assert(touch_config_load(path, &cfg) && cfg.enabled == 1);
    cfg.enabled = 0; cfg.layout.control[TOUCH_A].x = .7f;
    write_text(other, "unrelated setting\n");
    assert(touch_config_save(path, &cfg));
    assert(touch_config_load(path, &out) && !out.enabled && out.layout.control[TOUCH_A].x == cfg.layout.control[TOUCH_A].x);
    expect_text(other, "unrelated setting\n");
    const char *unknown = "version = 9\nenabled = 0\nunknown = data\n";
    write_text(path, unknown);
    assert(touch_config_load(path, &out) && out.enabled == 1);
    expect_text(path, unknown);
    touch_runtime_init(other);
    touch_runtime_geometry((TouchRect){0, 0, 1920, 1080}, 1, 1920, 1080);
    touch_runtime_set_ready(1);
    touch_runtime_menu(1);
    assert(touch_runtime_editor_begin());
    touch_runtime_editor_cancel();
    touch_runtime_menu(0);
    expect_text(path, unknown);
    assert(touch_runtime_save(&cfg));
    assert(touch_config_load(path, &out) && !out.enabled);
    write_text(path, "version = 1\nenabled = 1\nleft_stick = 0.13, 0.70, 1, 0.65\na = 0.91, 0.58, 1, 0.65\n");
    assert(touch_config_load(path, &out) && out.enabled == 1 && out.layout.control[TOUCH_A].x == .91f);
    assert(touch_config_save(path, &cfg));
    assert(touch_config_load(path, &out) && out.enabled == 0);
    write_text(path, "version = 1\nenabled = 0\na = nan, .5, 1, .5\n");
    assert(touch_config_load(path, &out) && out.enabled == 0 && out.layout.control[TOUCH_A].x != cfg.layout.control[TOUCH_A].x);
    write_text(path, "version = 1\nenabled = 0\na = .5, .5, 1, .5\na = .6, .5, 1, .5\n");
    assert(touch_config_load(path, &out) && out.enabled == 0 && out.layout.control[TOUCH_A].x != .5f);
    write_text(path, "version = 1\nenabled = 0\nenabled = 1\n");
    assert(touch_config_load(path, &out) && out.enabled == 1);
    assert(snprintf(impossible, sizeof impossible, "%s/missing/touch_controls.ini", argv[1]) > 0);
    assert(!touch_config_save(impossible, &cfg));
    assert(!touch_config_save(argv[1], &cfg)); /* replacing a directory must fail */
    expect_text(other, "unrelated setting\n");
    assert(touch_config_save(path, &cfg));
    touch_runtime_init(other);
    touch_runtime_geometry((TouchRect){0, 0, 1920, 1080}, 1, 1920, 1080);
    assert(!touch_runtime_snapshot().ready);
    touch_runtime_set_ready(1);
    TouchSnapshot snapshot = touch_runtime_snapshot();
    assert(snapshot.ready && !snapshot.config.enabled);
    TouchSprite scene[TOUCH_SCENE_MAX];
    assert(touch_scene_build(&snapshot, 0, scene) == 0);
    assert(touch_scene_build(&snapshot, 1, scene) == 0);
    touch_runtime_event(0, 1, 1, snapshot.settings.x + 1, snapshot.settings.y + 1);
    assert(!touch_runtime_take_settings());
    cfg.enabled = 1;
    assert(touch_runtime_save(&cfg));
    snapshot = touch_runtime_snapshot();
    int scene_count = touch_scene_build(&snapshot, 0, scene);
    assert(scene_count == TOUCH_COUNT + 3 && scene[scene_count - 1].texture == TOUCH_COUNT);
    touch_runtime_event(0, 1, 10, snapshot.settings.x + 1, snapshot.settings.y + 1);
    assert(touch_runtime_take_settings());
    snapshot = touch_runtime_snapshot();
    for (int i = 0; i < scene_count; i++) if (scene[i].texture == TOUCH_A) {
        TouchRect r = scene[i].rect;
        assert(touch_geometry_hit(&snapshot.geometry, r.x + r.w / 2, r.y + r.h / 2) == TOUCH_A);
    }
#ifdef _WIN32
    HANDLE held = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(held != INVALID_HANDLE_VALUE);
    TouchConfig failed = snapshot.config; failed.enabled = 0;
    assert(!touch_runtime_save(&failed));
    assert(touch_runtime_snapshot().config.enabled == 1);
    assert(touch_config_load(path, &out) && out.enabled == 1);
    assert(CloseHandle(held));
#endif
    TouchRect a = snapshot.geometry.control[TOUCH_A];
    touch_runtime_event(0, 1, 2, a.x + a.w / 2, a.y + a.h / 2);
    assert(touch_runtime_snapshot().pad.buttons & 1);
    cfg = touch_runtime_snapshot().config; cfg.enabled = 0;
    assert(touch_runtime_save(&cfg) && !touch_runtime_snapshot().pad.buttons);
    snapshot = touch_runtime_snapshot();
    assert(touch_scene_build(&snapshot, 0, scene) == 0);
    touch_runtime_event(2, 1, 2, a.x + a.w / 2, a.y + a.h / 2);
    touch_runtime_event(0, 1, 11, snapshot.settings.x + 1, snapshot.settings.y + 1);
    assert(!touch_runtime_take_settings());
    cfg.enabled = 1;
    assert(touch_runtime_save(&cfg));
    snapshot = touch_runtime_snapshot(); a = snapshot.geometry.control[TOUCH_A];
    touch_runtime_event(0, 1, 12, snapshot.settings.x + 1, snapshot.settings.y + 1);
    cfg.enabled = 0;
    assert(touch_runtime_save(&cfg));
    assert(!touch_runtime_take_settings());
    cfg.enabled = 1;
    assert(touch_runtime_save(&cfg));
    unsigned stale_generation = touch_runtime_generation();
    touch_runtime_foreground(0);
#ifdef _WIN32
    Race race = {CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL), a, snapshot.settings};
    assert(race.go && race.done);
    HANDLE worker = CreateThread(NULL, 0, blocked_finger, &race, 0, NULL); assert(worker);
    assert(SetEvent(race.go) && WaitForSingleObject(race.done, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
    CloseHandle(worker); CloseHandle(race.go); CloseHandle(race.done);
#endif
    touch_runtime_event(0, 1, 8, a.x + a.w / 2, a.y + a.h / 2);
    touch_runtime_foreground(1);
    touch_runtime_event_at(stale_generation, 0, 1, 99, a.x + a.w / 2, a.y + a.h / 2);
    assert(!touch_runtime_take_settings());
    touch_runtime_event(1, 1, 8, a.x + a.w / 2, a.y + a.h / 2);
    assert(!touch_runtime_snapshot().pad.buttons);
    touch_runtime_focus(0, 0);
    touch_runtime_event(0, 1, 9, snapshot.settings.x + 1, snapshot.settings.y + 1);
    touch_runtime_focus(1, 0);
    assert(!touch_runtime_take_settings());
    touch_runtime_event(0, 1, 2, a.x + a.w / 2, a.y + a.h / 2);
    assert(touch_runtime_snapshot().pad.buttons & 1);
    touch_runtime_set_replay(1);
    assert(!touch_runtime_snapshot().pad.buttons);
    touch_runtime_event(0, 1, 3, snapshot.settings.x + 1, snapshot.settings.y + 1);
    assert(!touch_runtime_take_settings());
    touch_runtime_set_replay(0);
    assert(!touch_runtime_snapshot().pad.buttons);
    touch_runtime_event(0, 1, 2, a.x + a.w / 2, a.y + a.h / 2);
    assert(touch_runtime_snapshot().pad.buttons & 1);
    touch_runtime_invalidate();
    assert(!touch_runtime_snapshot().pad.buttons);
    touch_runtime_event(1, 1, 2, a.x + a.w / 2, a.y + a.h / 2);
    assert(!touch_runtime_snapshot().pad.buttons);
    touch_runtime_menu(1);
    touch_runtime_event(0, 1, 4, 200, 300);
    touch_runtime_event(2, 1, 4, 200, 300);
    TouchPointer pointer;
    assert(touch_runtime_pointer(&pointer) && pointer.type == 0);
    touch_runtime_invalidate();
    assert(!touch_runtime_pointer(&pointer));
    touch_runtime_event(1, 1, 4, 200, 300);
    assert(!touch_runtime_pointer(&pointer));
    assert(touch_runtime_editor_begin());
    snapshot = touch_runtime_snapshot();
    a = snapshot.geometry.control[TOUCH_A];
    scene_count = touch_scene_build(&snapshot, 1, scene);
    assert(scene_count == TOUCH_COUNT + 10 && scene[scene_count - 1].texture == TOUCH_COUNT + 8);
    float original_x = snapshot.config.layout.control[TOUCH_A].x;
    touch_runtime_editor_event((TouchPointer){0, a.x + a.w / 2, a.y + a.h / 2});
    touch_runtime_editor_event((TouchPointer){1, a.x + a.w / 2 - 20, a.y + a.h / 2});
    touch_runtime_editor_event((TouchPointer){2, a.x + a.w / 2 - 20, a.y + a.h / 2});
    TouchRect size_plus = snapshot.editor_buttons[1], opacity_minus = snapshot.editor_buttons[2];
    touch_runtime_editor_event((TouchPointer){0, size_plus.x + 1, size_plus.y + 1});
    touch_runtime_editor_event((TouchPointer){2, size_plus.x + 1, size_plus.y + 1});
    touch_runtime_editor_event((TouchPointer){0, opacity_minus.x + 1, opacity_minus.y + 1});
    touch_runtime_editor_event((TouchPointer){2, opacity_minus.x + 1, opacity_minus.y + 1});
    snapshot = touch_runtime_snapshot();
    assert(snapshot.editing && snapshot.config.layout.control[TOUCH_A].x == original_x && snapshot.layout.control[TOUCH_A].x != original_x);
    assert(snapshot.layout.control[TOUCH_A].scale > 1 && snapshot.layout.control[TOUCH_A].opacity < .65f);
    TouchRect before_resize = snapshot.geometry.control[TOUCH_A];
    touch_runtime_geometry((TouchRect){0, 0, 900, 900}, 1, 900, 900);
    assert(touch_runtime_snapshot().editing);
    touch_runtime_editor_event((TouchPointer){1, 300, 300});
    touch_runtime_geometry((TouchRect){0, 0, 1920, 1080}, 1, 1920, 1080);
    assert(touch_runtime_snapshot().geometry.control[TOUCH_A].x == before_resize.x);
    TouchRect current_a = touch_runtime_snapshot().geometry.control[TOUCH_A];
    touch_runtime_editor_event((TouchPointer){0, current_a.x + current_a.w / 2, current_a.y + current_a.h / 2});
    touch_runtime_editor_event((TouchPointer){1, 900, 0});
    assert(touch_runtime_snapshot().geometry.control[TOUCH_A].y >= 48);
    touch_runtime_editor_event((TouchPointer){3, 0, 0});
    /* Return to the valid draft for the save test. */
    touch_runtime_editor_event((TouchPointer){0, 900, 48 + current_a.h / 2});
    touch_runtime_editor_event((TouchPointer){1, current_a.x + current_a.w / 2, current_a.y + current_a.h / 2});
    touch_runtime_editor_event((TouchPointer){2, current_a.x + current_a.w / 2, current_a.y + current_a.h / 2});
    assert(touch_runtime_editor_save());
    snapshot = touch_runtime_snapshot();
    assert(!snapshot.editing && snapshot.config.layout.control[TOUCH_A].x != original_x);
    float saved_x = snapshot.config.layout.control[TOUCH_A].x;
    assert(touch_runtime_editor_begin());
    snapshot = touch_runtime_snapshot();
    TouchRect reset = snapshot.editor_buttons[6];
    touch_runtime_editor_event((TouchPointer){0, reset.x + 1, reset.y + 1});
    touch_runtime_editor_event((TouchPointer){2, reset.x + 1, reset.y + 1});
    touch_runtime_editor_cancel();
    assert(touch_runtime_snapshot().config.layout.control[TOUCH_A].x == saved_x);
    cfg = touch_runtime_snapshot().config; cfg.enabled = 0;
    assert(touch_runtime_save(&cfg));
    assert(touch_runtime_editor_begin());
    touch_runtime_editor_event((TouchPointer){0, reset.x + 1, reset.y + 1});
    touch_runtime_editor_event((TouchPointer){2, reset.x + 1, reset.y + 1});
    assert(touch_runtime_editor_save());
    assert(!touch_runtime_snapshot().config.enabled && touch_runtime_snapshot().config.layout.control[TOUCH_A].x == .91f);
    expect_text(other, "unrelated setting\n");
    touch_runtime_menu(0);
    touch_runtime_geometry((TouchRect){0, 0, 100, 100}, 1, 100, 100);
    snapshot = touch_runtime_snapshot();
    assert(snapshot.ready && !snapshot.gamepad_valid);
    touch_runtime_event(0, 1, 5, snapshot.settings.x + 1, snapshot.settings.y + 1);
    assert(!touch_runtime_take_settings());
    touch_runtime_init(impossible);
    touch_runtime_geometry((TouchRect){0, 0, 1920, 1080}, 1, 1920, 1080);
    touch_runtime_menu(1);
    touch_runtime_event(0, 1, 20, 300, 300);
    assert(touch_runtime_pointer(&pointer) && pointer.type == 0); /* CPU panel remains touchable without overlay */
    touch_runtime_event(3, 1, 20, 300, 300);
    assert(!touch_runtime_editor_begin());
    touch_runtime_set_ready(1);
    assert(touch_runtime_editor_begin());
    touch_runtime_set_ready(0);
    assert(!touch_runtime_snapshot().editing && touch_runtime_snapshot().edit_error);
    touch_runtime_event(0, 1, 21, 300, 300);
    int got_down = 0;
    while (touch_runtime_pointer(&pointer)) if (pointer.type == 0) got_down = 1;
    assert(got_down);
    touch_runtime_event(3, 1, 21, 300, 300);
    touch_runtime_set_ready(1);
    assert(touch_runtime_editor_begin());
    snapshot = touch_runtime_snapshot();
    opacity_minus = snapshot.editor_buttons[2];
    touch_runtime_editor_event((TouchPointer){0, opacity_minus.x + 1, opacity_minus.y + 1});
    touch_runtime_editor_event((TouchPointer){2, opacity_minus.x + 1, opacity_minus.y + 1});
    assert(!touch_runtime_editor_save() && touch_runtime_snapshot().editing && touch_runtime_snapshot().edit_error);
    assert(touch_runtime_snapshot().layout.control[TOUCH_LEFT_STICK].opacity < .65f);
    touch_runtime_editor_cancel();
    touch_runtime_menu(0);
    touch_runtime_init(NULL);
    TouchLayout defaults; touch_layout_default(&defaults);
    touch_runtime_geometry((TouchRect){0, 0, 734, 144}, 1, 734, 144);
    snapshot = touch_runtime_snapshot();
    assert(snapshot.gamepad_valid && memcmp(&snapshot.layout, &defaults, sizeof defaults));
    touch_runtime_set_ready(1);
    touch_runtime_menu(1);
    assert(touch_runtime_editor_begin());
    TouchSnapshot editor = touch_runtime_snapshot();
    for (int i = 0; i < TOUCH_COUNT; i++) {
        assert(editor.geometry.control[i].x == snapshot.geometry.control[i].x);
        assert(editor.geometry.control[i].y == snapshot.geometry.control[i].y);
        assert(editor.geometry.control[i].w == snapshot.geometry.control[i].w);
    }
    touch_runtime_editor_cancel(); touch_runtime_menu(0);
    touch_runtime_set_ready(0);
    puts("touch config self-test passed");
    return 0;
}
