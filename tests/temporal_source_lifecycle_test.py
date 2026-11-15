"""Compile actual lifecycle callsites with disposable mocks; no game/device/dependencies."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, start):
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


native = (ROOT / "android/app/src/main/cpp/main.c").read_text()
native_calls = native[native.index("    int (*capture_finish)(void)"):native.rindex("    return result;") + len("    return result;")]
native_test = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "temporal_source_capture.h"
#define SDL_LOG_CATEGORY_APPLICATION 0
static int requested, missing_start, missing_finish, missing_bind, init_error, finish_error, run_code;
static int starts, finishes, runs, lookups, rejected;
static int diag_bootstrap_temporal_source_requested(void) { return requested; }
static int finish_mock(void) { finishes++; return finish_error; }
static int bind_mock(uint32_t v, uint32_t s, uint64_t epoch, const uint32_t *a, WtTscEmit e) {
    (void)v; (void)s; (void)epoch; (void)a; (void)e; return 0;
}
static int start_mock(const char *dir, int on, WtTscBind bind) {
    starts++; assert(on == 1 && !strcmp(dir, "diagnostics/validated_run"));
    return !bind || init_error ? -1 : 0;
}
static void *dlsym(void *runtime, const char *symbol) {
    assert(runtime == (void *)123); lookups++;
    if (!strcmp(symbol, "wt_tsc_start")) return missing_start ? NULL : (void *)start_mock;
    if (!strcmp(symbol, "wt_tsc_finish")) return missing_finish ? NULL : (void *)finish_mock;
    assert(!strcmp(symbol, "game_temporal_source_bind")); return missing_bind ? NULL : (void *)bind_mock;
}
static int run_mock(int argc, char **argv) { assert(argc == 2 && !strcmp(argv[1], "targets/exefs/main.elf")); runs++; return run_code; }
static int fail(const char *s) { assert(s); return 1; }
static void SDL_LogError(int category, const char *s) { (void)category; assert(s); }
static int diag_bootstrap_write_outcome_rejection(const char *d, const char *r, const char *reason, const char *detail) {
    assert(!strcmp(d, "diagnostics/validated_run") && !strcmp(r, "validated_run"));
    assert(!strcmp(reason, "TEMPORAL_SOURCE_CAPTURE_UNAVAILABLE") && detail); rejected++; return 0;
}
static int launch(int diag_mode) {
    void *runtime = (void *)123; const char *run_dir = "diagnostics/validated_run", *run_id = "validated_run";
    int (*run)(int, char **) = run_mock;
''' + native_calls + r'''
}
static void reset(void) { requested = missing_start = missing_finish = missing_bind = init_error = finish_error = run_code = 0; starts = finishes = runs = lookups = rejected = 0; }
int main(void) {
    reset(); assert(!launch(0) && runs == 1 && !lookups && !starts);
    reset(); requested = 1; assert(!launch(0) && runs == 1 && !lookups);
    reset(); assert(!launch(1) && runs == 1 && !lookups);
    reset(); requested = 1; assert(!launch(1) && starts == 1 && finishes == 1 && runs == 1 && !rejected);
    reset(); requested = missing_start = 1; assert(launch(1) == 1 && !starts && !runs && rejected == 1);
    reset(); requested = missing_finish = 1; assert(launch(1) == 1 && !starts && !runs && !finishes && rejected == 1);
    reset(); requested = missing_bind = 1; assert(launch(1) == 1 && starts == 1 && finishes == 1 && !runs && rejected == 1);
    reset(); requested = init_error = 1; assert(launch(1) == 1 && !runs && rejected == 1);
    reset(); requested = finish_error = 1; assert(launch(1) == 1 && runs == 1);
    reset(); requested = finish_error = 1; run_code = 7; assert(launch(1) == 7 && runs == 1);
    return 0;
}
'''

boot = (ROOT / "runtime/boot.c").read_text()
boot_calls = boot[boot.index("    int capture_failed = 0;"):boot.index("    return capture_failed ? 1 : 0;") + len("    return capture_failed ? 1 : 0;")]
host = (ROOT / "runtime/hostrt.c").read_text()
host_calls = block(host, host.index("    if (diag_is_enabled()) {", host.index("void report_and_exit(int code)")))
teardown_test = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
static int enabled, requested, sealed, finish_error, seal_calls, finish_calls, shutdowns, stopped, writer_closed;
static uint64_t drops, losses;
static const char *outcome, *reason;
static int diag_is_enabled(void) { return enabled; }
static bool diag_seal_producers(void) { seal_calls++; return sealed != 0; }
static uint64_t diag_get_total_dropped(void) { assert(seal_calls); return drops; }
static int wt_tsc_requested(void) { return requested; }
static void wt_tsc_core_loss(uint64_t n) { assert(seal_calls); losses += n; }
static int wt_tsc_finish(void) { assert(seal_calls); finish_calls++; return finish_error; }
static void diag_writer_write_outcome(const char *o, const char *r, const char *checkpoint) {
    assert(seal_calls && (!requested || finish_calls) && !checkpoint); outcome = o; reason = r;
}
static void diag_writer_stop(void) { stopped++; }
static void diag_writer_shutdown(void) { writer_closed++; }
static void diag_shutdown(void) { assert(sealed); shutdowns++; }
static int boot_end(void) {
''' + boot_calls + r'''
}
static int host_end(int code) {
''' + host_calls + r'''
return code;
}
static void reset(void) {
    enabled = requested = sealed = 1; finish_error = seal_calls = finish_calls = shutdowns = stopped = writer_closed = 0;
    drops = losses = 0; outcome = reason = NULL;
}
int main(void) {
    reset(); assert(!boot_end() && finish_calls == 1 && shutdowns == 1 && !strcmp(outcome, "completed"));
    reset(); finish_error = 1; assert(boot_end() == 1 && !strcmp(reason, "TEMPORAL_SOURCE_CAPTURE_INCOMPLETE"));
    reset(); sealed = 0; finish_error = 1; drops = 4; assert(boot_end() == 1 && losses == 5 && !shutdowns);
    assert(!strcmp(reason, "DIAG_PRODUCERS_NOT_QUIESCENT") && !strcmp(outcome, "failed"));
    reset(); finish_error = 1; assert(host_end(7) == 7 && !strcmp(reason, "EXIT_NON_ZERO"));
    reset(); finish_error = 1; assert(host_end(0) == 1 && !strcmp(reason, "TEMPORAL_SOURCE_CAPTURE_INCOMPLETE"));
    reset(); sealed = 0; finish_error = 1; assert(host_end(0) == 1 && losses == 1 && !shutdowns);
    assert(!strcmp(reason, "DIAG_PRODUCERS_NOT_QUIESCENT"));
    reset(); requested = 0; assert(!host_end(0) && !finish_calls && !losses);
    reset(); enabled = 0; assert(host_end(7) == 7 && !seal_calls && !finish_calls);
    return 0;
}
'''

draw = (ROOT / "runtime/nvn_vk_draw.c").read_text()
scene_calls = block(draw, draw.index("    if (wt_tsc_requested()) {\n        int scene_eligible"))
advance = next(line for line in draw.splitlines() if "presents never arm" in line.lower())
scene_test = r'''
#include <assert.h>
#include <stdint.h>
#include "upscale_scene.h"
typedef struct Tex { void *vk; } Tex;
static UpscaleScene U;
static struct { int ncol; struct { Tex *tex; } col[1], depth; } R;
static int requested = 1, g_dp_potential, calls, eligible;
static uint64_t frame;
static int wt_tsc_requested(void) { return requested; }
static void wt_tsc_frame(uint64_t f, int e) { calls++; frame = f; eligible = e; }
static void scene(void) {
''' + scene_calls + r'''
}
static void present(void) {
''' + advance + r'''
}
int main(void) {
    Tex c = {(void *)1}, d = {(void *)2}; R.ncol = 1; R.col[0].tex = &c; R.depth.tex = &d;
    U.frame = U.scene_frame = U.depth_frame = U.cam_frame = 10; U.ran = -1;
    U.generation = U.depth_generation = U.camera_generation = 1;
    U.scene_color = &c; U.depth = &d; U.color_image = c.vk; U.depth_image = d.vk;
    scene(); assert(calls == 1 && frame == 10 && eligible);
    g_dp_potential = 1; scene(); assert(!eligible); g_dp_potential = 0;
    U.camera_conflict = 1; scene(); assert(!eligible); U.camera_conflict = 0;
    U.camera_generation = 0; scene(); assert(!eligible); U.camera_generation = 1;
    R.ncol = 0; scene(); assert(!eligible); R.ncol = 1;
    U.scene_frame = 9; scene(); assert(!eligible); U.scene_frame = 10;
    U.frame = 11; present(); assert(frame == 11 && !eligible);
    requested = 0; int before = calls; scene(); present(); assert(calls == before);
    return 0;
}
'''

cc = shutil.which("gcc") or shutil.which("clang") or "cc"
temp = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode" if os.name == "nt" else None
with tempfile.TemporaryDirectory(prefix="temporal_lifecycle_", dir=temp) as directory:
    tmp = Path(directory)
    for name, source in [("native", native_test), ("teardown", teardown_test), ("scene", scene_test)]:
        c = tmp / (name + ".c"); c.write_text(source)
        exe = tmp / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-I", str(ROOT / "runtime"), str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("Actual native/scene/sealed-teardown callsite regressions passed (mocked dependencies)")
