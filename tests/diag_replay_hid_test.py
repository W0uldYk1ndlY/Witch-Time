"""Execute the production synthetic HID branch with mocked exit and guest memory."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime/nnhid.c").read_text(encoding="utf-8")
start = source.index("    if (diag_input_is_synthetic_player1_enabled() && id == 0) {")
end = source.index("    } else {", start)
branch = source[start:end] + "    }\n"
write_buttons = "    wr64(out + 8, buttons);\n"
assert write_buttons in source[end:]
assert start < end < source.index("touch_pad_merge(") < source.index(write_buttons, end)
harness = r'''
#include "diag_replay.h"
#include "touch_controls.h"
#include "diag.h"
#include <assert.h>
#include <setjmp.h>
#include <string.h>
bool diag_is_enabled(void) { return false; }
static long long g_rec_poll, g_anchor_poll;
static int g_anchor = -1, completed;
static jmp_buf exit_jump;
static void diag_writer_write_outcome(const char *status, const char *reason, const char *details) {
    assert(!strcmp(status, "completed") && !strcmp(reason, "REPLAY_FINISHED"));
    (void)details; completed++;
}
static void report_and_exit(int code) { assert(code == 0); longjmp(exit_jump, 1); }
static void wr64(uint64_t address, uint64_t value) { memcpy((void *)(uintptr_t)address, &value, 8); }
static void sample(uint64_t out) {
    uint32_t id = 0;
    uint64_t buttons = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0, wired = 1, prompts_used = -1;
'''
harness += branch + write_buttons
harness += r'''
    (void)lx; (void)ly; (void)rx; (void)ry; (void)wired; (void)prompts_used;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    touch_runtime_init(argv[1]);
    touch_runtime_focus(1, 1);
    touch_runtime_geometry((TouchRect){0, 0, 1920, 1080}, 1, 1920, 1080);
    touch_runtime_set_ready(1);
    TouchSnapshot before = touch_runtime_snapshot();
    TouchRect a = before.geometry.control[TOUCH_A];
    touch_runtime_event(0, 1, 10, a.x + a.w / 2, a.y + a.h / 2);
    assert(touch_runtime_snapshot().pad.buttons & 1);
    diag_replay_record_t r = {.poll = 1, .anchor = -1, .buttons = DIAG_REPLAY_BTN_A,
                              .attributes = DIAG_REPLAY_ATTR_CONNECTED};
    diag_replay_session_t s = {0};
    s.is_loaded = true; s.records = &r; s.record_count = 1;
    s.header.total_polls = 3;
    diag_input_set_active_replay(&s);
    assert(s.next_global_poll == 0);
    uint64_t output[5] = {0};
    sample((uint64_t)(uintptr_t)output);
    assert(output[1] == 0 && s.next_global_poll == 1 && !completed);
    assert(!touch_runtime_snapshot().pad.buttons);
    touch_runtime_event(0, 1, 11, before.settings.x + 1, before.settings.y + 1);
    assert(!touch_runtime_take_settings());
    g_anchor = 9; g_anchor_poll = g_rec_poll; /* no replay-global reset */
    sample((uint64_t)(uintptr_t)output);
    assert(output[1] == DIAG_REPLAY_BTN_A && s.next_global_poll == 2 && !completed);
    diag_input_set_active_replay(NULL);
    diag_input_set_active_replay(&s);
    assert(s.next_global_poll == 2); /* Rebinding is not a reload/restart. */
    output[1] = 0;
    sample((uint64_t)(uintptr_t)output);
    assert(output[1] == DIAG_REPLAY_BTN_A && s.next_global_poll == 3 && !completed);
    if (!setjmp(exit_jump)) { sample((uint64_t)(uintptr_t)output); assert(0); }
    assert(completed == 1 && s.is_eof && s.next_global_poll == 3);
    /* At the largest duration the last sample advances to MAX, never wraps to zero. */
    s.is_eof = false; s.header.total_polls = UINT64_MAX;
    s.next_global_poll = UINT64_MAX - 1;
    sample((uint64_t)(uintptr_t)output);
    assert(output[1] == DIAG_REPLAY_BTN_A && s.next_global_poll == UINT64_MAX);
    if (!setjmp(exit_jump)) { sample((uint64_t)(uintptr_t)output); assert(0); }
    assert(completed == 2 && s.next_global_poll == UINT64_MAX);
    diag_input_set_active_replay(NULL);
    diag_input_enable_synthetic_player1(false);
    touch_runtime_event(1, 1, 10, a.x + a.w / 2, a.y + a.h / 2);
    assert(!touch_runtime_snapshot().pad.buttons && !touch_runtime_take_settings());
    touch_runtime_event(0, 1, 10, a.x + a.w / 2, a.y + a.h / 2);
    assert(touch_runtime_snapshot().pad.buttons & 1);
    return 0;
}
'''
cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
assert cc, "C compiler required"
with tempfile.TemporaryDirectory(prefix="diag-replay-hid-") as temp:
    path = Path(temp)
    (path / "probe.c").write_text(harness, encoding="utf-8")
    exe = path / ("probe.exe" if os.name == "nt" else "probe")
    subprocess.run([cc, "-std=gnu11", "-D__ANDROID__", "-Wall", "-Wextra", "-I", str(ROOT / "runtime"),
                    str(path / "probe.c"), str(ROOT / "runtime/diag_replay.c"), str(ROOT / "runtime/touch_controls.c"), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(path / "controllers.ini")], check=True)
print("Production synthetic HID final-sample/EOF ordering passed (mocked guest memory and exit)")
