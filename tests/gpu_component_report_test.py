"""Run the production marked-time report block: absent is not measured zero."""
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
start = source.index("    if (g_pt_on > 0 && g_pt_frames) {")
end = source.index("\n    }\n}", start) + len("\n    }")
enum_start = source.index("enum { PT_GAP")
enum_end = source.index(";", enum_start) + 1
kind_start = source.index("typedef struct { int w, h, ncol, cf, depth, win, what; } PtKind;")
kind_end = source.index("} PtKind;", kind_start) + len("} PtKind;")
harness = "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
harness += source[enum_start:enum_end] + "\n" + source[kind_start:kind_end] + "\n"
harness += r'''
static int g_pt_on = 1, g_pt_frames = 10, g_pt_nkind = 1;
static double g_pt_ns[1];
static uint64_t g_pt_cnt[1], g_pt_draws[1];
static PtKind g_pt_kind[1];
static void report(void) {
'''
harness += source[start:end]
harness += r'''
}
int main(int argc, char **argv) {
    g_pt_kind[0].what = PT_FSR_BASE;
    g_pt_cnt[0] = argc > 1 ? strtoull(argv[1], 0, 10) : 0;
    report();
    return 0;
}
'''
compiler = shlex.split(os.environ["HOST_CC"]) if os.environ.get("HOST_CC") else [shutil.which("gcc") or shutil.which("clang") or "cc"]
with tempfile.TemporaryDirectory(prefix="gpu_component_report_") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness, encoding="utf-8")
    exe = path / ("test.exe" if os.name == "nt" else "test")
    subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", str(path / "test.c"), "-o", str(exe)], check=True)
    absent = subprocess.check_output([str(exe), "0"], text=True)
    measured_zero = subprocess.check_output([str(exe), "1"], text=True)
    assert "unavailable: no_valid_marked_samples" in absent and "0.00 ms" not in absent
    assert "0.00 ms" in measured_zero and "unavailable" not in measured_zero
print("Production marked GPU report distinguishes unavailable from valid zero")
