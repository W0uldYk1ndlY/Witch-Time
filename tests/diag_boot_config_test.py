"""Boot config plus real recorder/writer under modeled drains, not device scheduling."""
import os
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
android_main = (ROOT / "android/app/src/main/cpp/main.c").read_text(encoding="utf-8")
assert android_main.index("capture_start(run_dir, 1, bind)") < android_main.index("run(2, game_argv)")
source = (ROOT / "runtime/boot.c").read_text(encoding="utf-8")
start = source.index('        const char *diag_run = getenv("WT_DIAG_RUN_ID");')
end = source.index("            diag_writer_start();", start) + len("            diag_writer_start();")
snippet = source[start:end] + "\n        }\n"
harness = r'''
#include <assert.h>
#include <string.h>
#include "diag_writer.h"
#include "temporal_source_capture.h"
uint32_t wt_tsc_requested_run;
static const char *run, *dir, *pass_time;
static int initialized, started;
static diag_writer_config_t captured;
static char *test_getenv(const char *name) {
    if (!strcmp(name, "WT_DIAG_RUN_ID")) return (char *)run;
    if (!strcmp(name, "WT_DIAG_DIR")) return (char *)dir;
    assert(!strcmp(name, "WT_VK_PASSTIME"));
    return (char *)pass_time;
}
#define getenv test_getenv
int diag_init(const char *id) { assert(!strcmp(id, run)); return 0; }
int diag_writer_init(const diag_writer_config_t *cfg) {
    captured = *cfg; initialized++; return 0;
}
int diag_writer_start(void) { started++; return 0; }
static void configure(void) {
'''
harness += snippet + r'''
}
int main(void) {
    /* No diagnostic session must remain disabled, even with pass timing enabled. */
    pass_time = "1"; wt_tsc_requested_run = 1;
    configure(); assert(!initialized && !started);
    run = "run_test";
    configure(); assert(!initialized && !started);
    dir = "";
    configure(); assert(!initialized && !started);
    dir = "test_output"; run = "";
    configure(); assert(!initialized && !started);
    run = "run_test";
    const char *values[] = {NULL, "", "0", "1", "2", "01"};
    for (unsigned state = 0; state <= 2; state++)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        wt_tsc_requested_run = state; /* 0 unset, 1 explicitly requested, 2 terminal/finishing */
        pass_time = values[i]; initialized = started = 0;
        configure();
        assert(initialized == 1 && started == 1);
        assert(captured.flush_interval_ms == (state == 1 || i == 3 ? 50 : 500));
        assert(captured.chunk_record_limit == 1000);
        assert(captured.storage_budget_bytes == 128 * 1024 * 1024);
        assert(captured.incident_reserve_bytes == 16 * 1024 * 1024);
        assert(!strcmp(captured.run_id, run) && !strcmp(captured.output_dir, dir));
    }
    return 0;
}
'''
# Controlled timing model based on observed aggregate counts, not recorded arrival times.
# No drains occur in the legacy interval; 50 ms scheduled drains partition this uniform workload.
times = [i * 500 // 1075 for i in range(1075)]
assert len(times) > 1023
assert max(sum(t // 50 == bucket for t in times) for bucket in range(10)) <= 108
recorder = r'''
#include "diag.h"
#include "diag_writer.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    assert(argc == 3 && DIAG_RING_CAPACITY - 1 == 1023);
    int interval = atoi(argv[2]);
    assert(interval == 50 || interval == 500);
    assert(diag_init("scheduled-source-capture") == 0);
    diag_writer_config_t cfg = {0};
    strcpy(cfg.run_id, "scheduled-source-capture");
    assert(strlen(argv[1]) < sizeof(cfg.output_dir));
    strcpy(cfg.output_dir, argv[1]);
    cfg.chunk_record_limit = 1000;
    cfg.storage_budget_bytes = 128 * 1024 * 1024;
    cfg.incident_reserve_bytes = 16 * 1024 * 1024;
    cfg.disable_unbuffered_logging = true;
    /* No writer thread: drive the real flush at known modeled deadlines, without sleeping. */
    assert(diag_writer_init(&cfg) == 0);
    int producer = diag_register_producer("render-burst");
    assert(producer >= 0);
    int next_drain = interval;
    for (int i = 0; i < 1075; i++) {
        int modeled_ms = i * 500 / 1075;
        if (modeled_ms >= next_drain) {
            assert(diag_writer_flush_chunks() == 0);
            next_drain += interval;
        }
        diag_emit(producer, i < 638 ? DIAG_EV_OPT_CLEAR_FOLD : DIAG_EV_CUSTOM,
                  i, 1075, 638, 0);
    }
    diag_writer_stats_t before, after;
    diag_writer_get_stats(&before);
    assert(before.total_records_written < 1075); /* Terminal tail is still buffered. */
    diag_writer_stop(); /* Production seal and final flush, without a final scheduled drain. */
    diag_writer_get_stats(&after);
    assert(after.total_records_written > before.total_records_written);
    assert(diag_get_total_dropped() == (interval == 50 ? 0 : 52));
    assert(diag_get_total_emitted() == (interval == 50 ? 1075 : 1023));
    assert(after.total_records_written == diag_get_total_emitted());
    diag_writer_shutdown();
    diag_shutdown();
    return 0;
}
'''
compiler = shlex.split(os.environ["HOST_CC"]) if os.environ.get("HOST_CC") else [shutil.which("gcc") or shutil.which("clang") or "cc"]
with tempfile.TemporaryDirectory(prefix="diag_boot_config_", dir=os.environ.get("TEMP")) as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness, encoding="utf-8")
    executable = path / ("test.exe" if os.name == "nt" else "test")
    subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-I", str(ROOT / "runtime"), str(path / "test.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    (path / "recorder.c").write_text(recorder, encoding="utf-8")
    recorder_exe = path / ("recorder.exe" if os.name == "nt" else "recorder")
    subprocess.run(compiler + ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-pthread", "-I", str(ROOT / "runtime"),
                              str(path / "recorder.c"), str(ROOT / "runtime/diag.c"),
                              str(ROOT / "runtime/diag_writer.c"), "-o", str(recorder_exe)], check=True)
    for interval, count in ((500, 1023), (50, 1075)):
        output = path / f"scheduled_{interval}"
        subprocess.run([str(recorder_exe), str(output), str(interval)], check=True)
        records = []
        for chunk in sorted(output.glob("events-*.jsonl")):
            lines = [json.loads(line) for line in chunk.read_text().splitlines()]
            assert lines[0]["count"] == len(lines) - 1
            records.extend(lines[1:])
        assert len(records) == count
        assert [r["seq"] for r in records] == list(range(1, count + 1))
        assert [r["p0"] for r in records] == list(range(count))
        assert sum(r["type"] == 24 for r in records) == 638
        assert all(r["type"] == (24 if r["p0"] < 638 else 99) and
                   r["p1"] == 1075 and r["p2"] == 638 and r["p3"] == 0 for r in records)
print("Boot cadence matrix passed; real recorder/writer preserved 1075 records including terminal tail with modeled 50 ms drains")
