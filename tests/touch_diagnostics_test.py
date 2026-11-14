#!/usr/bin/env python3
"""Host-mocked Android diagnostic gate/caps and unchanged SET acceptance."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "runtime/display.c").read_text(encoding="utf-8")
start = source.index("        /* Independent lifetime caps")
end = source.index("        if ((e.type == SDL_EVENT_MOUSE_MOTION", start)
dequeue = source[start:end]
assert source.index("touch-diag dequeue finger") < source.index("if (e.tfinger.timestamp <=")
assert source.index("touch-diag menu-open call") < source.index("                    pcmenu_open_touch();")
code = r'''
#include "touch_controls.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
static bool enabled;
bool diag_is_enabled(void) { return enabled; }
enum { SDL_EVENT_FINGER_DOWN=1, SDL_EVENT_FINGER_MOTION, SDL_EVENT_FINGER_UP,
       SDL_EVENT_FINGER_CANCELED, SDL_EVENT_MOUSE_MOTION,
       SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP };
#define SDL_TOUCH_MOUSEID (-1)
typedef unsigned long long Uint64;
typedef struct {
    unsigned type;
    struct { unsigned long long timestamp; } common;
    struct { unsigned long long timestamp; long long touchID, fingerID; float x,y; } tfinger;
    struct { int which; float x,y; } motion, button;
} SDL_Event;
static unsigned long long g_android_touch_cutoff;
static void observe(SDL_Event e, int pw, int ph) {
''' + dequeue + r'''
}
int main(int argc, char **argv) {
    enabled = argc > 1 && atoi(argv[1]);
    touch_runtime_init(NULL);
    touch_runtime_focus(1,1);
    touch_runtime_event(0,1,0,0,0);
    touch_runtime_geometry((TouchRect){0,0,1920,1080},1,1920,1080);
    touch_runtime_set_ready(1);
    TouchRect r=touch_runtime_snapshot().settings;
    unsigned old=touch_runtime_generation();
    touch_runtime_event_at(old,0,1,1,r.x+1,r.y+1);
    assert(touch_runtime_take_settings());
    assert(!touch_runtime_take_settings());
    touch_runtime_event_at(old,0,1,2,r.x+1,r.y+1);
    assert(!touch_runtime_take_settings());
    touch_runtime_set_replay(1);
    touch_runtime_event(0,1,3,r.x+1,r.y+1);
    assert(!touch_runtime_take_settings());
    touch_runtime_set_replay(0);
    touch_runtime_focus(0,0);
    touch_runtime_event(0,1,3,r.x+1,r.y+1);
    touch_runtime_focus(1,0);
    touch_runtime_event(1,1,3,0,0);
    touch_runtime_menu(1);
    touch_runtime_event(0,1,3,r.x+1,r.y+1);
    touch_runtime_menu(0);
    touch_runtime_set_ready(0);
    touch_runtime_event(0,1,4,r.x+1,r.y+1);
    assert(!touch_runtime_take_settings());
    SDL_Event e={0}; e.tfinger.x=.5f; e.tfinger.y=.25f;
    e.type=SDL_EVENT_FINGER_DOWN; observe(e,1920,1080);
    e.tfinger.timestamp=1; observe(e,1920,1080);
#ifdef _WIN32
    _putenv_s("WT_PLAY", "test");
#else
    setenv("WT_PLAY", "test", 1);
#endif
    observe(e,1920,1080);
#ifdef _WIN32
    _putenv_s("WT_PLAY", "");
#else
    unsetenv("WT_PLAY");
#endif
    for (int i=0; i<200; i++) {
        e.type=SDL_EVENT_FINGER_DOWN; observe(e,1920,1080);
        e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.which=SDL_TOUCH_MOUSEID; observe(e,1920,1080);
        touch_runtime_event(0,1,i,r.x+1,r.y+1);
    }
    return 0;
}
'''
cc = os.environ.get("HOST_CC") or shutil.which("gcc") or shutil.which("clang")
if not cc:
    raise RuntimeError("C compiler required")
with tempfile.TemporaryDirectory(prefix="touch-diagnostics-") as temp:
    temp = Path(temp)
    cfile = temp / "test.c"
    cfile.write_text(code, encoding="utf-8")
    exe = temp / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-D__ANDROID__",
                    "-I", str(root / "runtime"), str(cfile), str(root / "runtime/touch_controls.c"),
                    "-lm", "-o", str(exe)], check=True)
    off = subprocess.run([str(exe), "0"], capture_output=True, text=True, check=True)
    assert not off.stderr, off.stderr
    on = subprocess.run([str(exe), "1"], capture_output=True, text=True, check=True)
    assert on.stderr.count("touch-diag dequeue finger") == 64
    assert on.stderr.count("touch-diag dequeue touch-mouse") == 32
    assert on.stderr.count("touch-diag runtime") == 127  # consume shares the 128-entry budget
    assert "request raised" in on.stderr and "request consumed request=1 accepted=1" in on.stderr
    assert "generation_match=0" in on.stderr and "replay=1" in on.stderr and "visible=0" in on.stderr
    for disposition in ("stale", "replay", "forward", "stale-generation", "invalid-geometry",
                        "not-visible", "lifecycle", "menu", "SET-hit", "gamepad-path"):
        assert f"disposition={disposition} snapshot=advisory" in on.stderr, disposition
print("touch diagnostic gate, lifetime caps, dequeue compile, and SET acceptance passed (host mocks)")
