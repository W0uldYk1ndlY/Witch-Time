#!/usr/bin/env python3
"""Execute the Android lifecycle filter and mouse-origin predicate with host mocks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "runtime/display.c").read_text(encoding="utf-8")
def function(start_text):
    start = source.index(start_text)
    return source[start:source.index("\n}", start) + 2]
cutoff = function("static void android_touch_cutoff(void)")
lifecycle = function("static bool SDLCALL android_lifecycle(")
start = source.index("if ((e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which == SDL_TOUCH_MOUSEID)")
end = source.index("\n            continue;", start)
predicate = source[start + 4:end].strip()[:-1]
assert "touch_runtime_event_at(touch_generation" in source
code = r'''
#include "touch_controls.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t Uint64; typedef long LONG;
typedef struct { int type; struct { int which; } motion, button; } SDL_Event;
#define SDLCALL
#define SDL_EVENT_WILL_ENTER_BACKGROUND 1
#define SDL_EVENT_DID_ENTER_FOREGROUND 2
#define SDL_EVENT_TERMINATING 3
#define SDL_EVENT_WINDOW_FOCUS_LOST 4
#define SDL_EVENT_WINDOW_FOCUS_GAINED 5
#define SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED 6
#define SDL_EVENT_WINDOW_SAFE_AREA_CHANGED 7
#define SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED 8
#define SDL_EVENT_MOUSE_MOTION 9
#define SDL_EVENT_MOUSE_BUTTON_DOWN 10
#define SDL_EVENT_MOUSE_BUTTON_UP 11
#define SDL_TOUCH_MOUSEID (-1)
static int g_android_background, g_android_generation;
static int g_android_terminating, g_android_lifecycle_lock, g_android_lifecycle_cv;
#define AcquireSRWLockExclusive(p) ((void)(p))
#define ReleaseSRWLockExclusive(p) ((void)(p))
#define WakeAllConditionVariable(p) ((void)(p))
static void android_power_snapshot(void) { }
static Uint64 g_android_touch_cutoff, ticks;
volatile LONG g_display_close;
static Uint64 SDL_GetTicksNS(void) { return ++ticks; }
static LONG InterlockedExchange(volatile LONG *v, LONG x) { LONG old=*v; *v=x; return old; }
''' + cutoff + '\n' + lifecycle + '\n' + 'static int synthetic(SDL_Event e) { return (' + predicate + '); }\n' + r'''
int main(void) {
    SDL_Event e={0};
    e.type=SDL_EVENT_MOUSE_MOTION; e.motion.which=42; assert(!synthetic(e));
    e.motion.which=SDL_TOUCH_MOUSEID; assert(synthetic(e));
    e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.which=42; assert(!synthetic(e));
    e.button.which=SDL_TOUCH_MOUSEID; assert(synthetic(e));
    e.type=SDL_EVENT_MOUSE_BUTTON_UP; assert(synthetic(e));
    touch_runtime_init(NULL); touch_runtime_focus(1,1);
    touch_runtime_geometry((TouchRect){0,0,1920,1080},1,1920,1080); touch_runtime_set_ready(1);
    TouchSnapshot s=touch_runtime_snapshot(); TouchRect a=s.geometry.control[TOUCH_A];
    touch_runtime_event(0,1,1,a.x+a.w/2,a.y+a.h/2); assert(touch_runtime_snapshot().pad.buttons & 1);
    unsigned old=touch_runtime_generation();
    e.type=SDL_EVENT_WILL_ENTER_BACKGROUND; assert(android_lifecycle(NULL,&e));
    assert(g_android_background && !touch_runtime_snapshot().pad.buttons);
    touch_runtime_event(0,1,2,a.x+a.w/2,a.y+a.h/2);
    e.type=SDL_EVENT_DID_ENTER_FOREGROUND; assert(android_lifecycle(NULL,&e));
    touch_runtime_event_at(old,0,1,3,a.x+a.w/2,a.y+a.h/2);
    touch_runtime_event(1,1,2,a.x+a.w/2,a.y+a.h/2);
    assert(!g_android_background && g_android_generation==1 && !touch_runtime_snapshot().pad.buttons);
    e.type=SDL_EVENT_WINDOW_FOCUS_LOST; android_lifecycle(NULL,&e);
    touch_runtime_event(0,1,4,s.settings.x+1,s.settings.y+1);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED; android_lifecycle(NULL,&e);
    assert(!touch_runtime_take_settings());
    e.type=SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED; android_lifecycle(NULL,&e);
    assert(g_android_touch_cutoff >= 3);
    puts("touch SDL filter and physical-mouse predicate passed (host mocks)");
    return 0;
}
'''
cc = os.environ.get("HOST_CC") or shutil.which("gcc") or shutil.which("cc")
if not cc: raise RuntimeError("C compiler required")
with tempfile.TemporaryDirectory(prefix="touch-routing-") as temp:
    temp = Path(temp); (temp / "test.c").write_text(code, encoding="utf-8")
    exe = temp / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([cc, "-std=gnu11", "-I", str(root / "runtime"), str(temp / "test.c"),
                    str(root / "runtime/touch_controls.c"), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
