"""Exercise the production full-grid replay state transitions with mocked renderer state."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "runtime" / "nvn_vk_draw.c").read_text(encoding="utf-8")
low = re.search(r'static int rs_store_low\(void\) \{.*?\n\}', source, re.S)
done = re.search(r'static void rs_store_done\(int drawn\) \{.*?\n\}', source, re.S)
assert low and done
assert "buffer_writes |= v->ssbo_w" in source
assert "g_dp_fs_stores = fs && fs->images && !buffer_writes" in source
arrays = source.split("case OP_DRAW_ARRAYS:", 1)[1].split("case OP_DRAW_ELEMENTS:", 1)[0]
indexed = source.split("case OP_DRAW_ELEMENTS:", 1)[1].split("case OP_DRAW_INDIRECT:", 1)[0]
assert arrays.index("vkCmdDraw(") < arrays.index("rs_store_low()") < arrays.index("rs_store_done(ok)")
assert indexed.index("vkCmdDrawIndexed(") < indexed.index("rs_store_low()") < indexed.index("rs_store_done(ok)")
assert indexed.index("buf_ref(", indexed.index("rs_store_low()")) < indexed.index("draw_prepare(", indexed.index("rs_store_low()"))
harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef long long LONG64;
typedef struct { int texture, view; } TargetRef;
static struct { int in_pass, pass_rs; float pass_sx, pass_sy; TargetRef col[8], depth; int ncol, have_targets, dyn_dirty; } R;
static int g_dp_fs_stores, g_rs_on, g_oq_on, pass_ends, marker_ends;
#define RS_ON g_rs_on
static TargetRef g_sf_col[8], g_sf_dep;
static int g_sf_ncol, g_sf_have, g_sf_oq, g_sf_on = -1;
static LONG64 g_sf_draws;
static void pass_end(void) { pass_ends++; R.in_pass = 0; }
static void pt_end(void) { marker_ends++; }
''' + low.group(0) + '\n' + done.group(0) + r'''
int main(void) {
    g_rs_on = 1; R.in_pass = R.pass_rs = 1; R.pass_sx = R.pass_sy = 0.75f;
    R.col[0].texture = 11; R.col[0].view = 12; R.depth.texture = 13;
    R.ncol = 1; R.have_targets = 1; g_oq_on = 1; g_sf_on = 1;
    g_dp_fs_stores = 0; assert(!rs_store_low() && !pass_ends);
    g_dp_fs_stores = 1; g_sf_on = 0; assert(!rs_store_low() && !pass_ends);
    g_sf_on = 1; R.pass_sx = R.pass_sy = 1.0f; assert(!rs_store_low() && !pass_ends);
    R.pass_sx = 0.75f; assert(rs_store_low());
    assert(pass_ends == 1 && !R.ncol && !R.have_targets && !R.col[0].texture && !R.depth.texture && !g_oq_on);
    rs_store_done(1);
    assert(pass_ends == 2 && R.ncol == 1 && R.have_targets && R.col[0].texture == 11 && R.col[0].view == 12 && R.depth.texture == 13);
    assert(g_oq_on == 1 && g_sf_draws == 1 && R.dyn_dirty && marker_ends == 1);
    R.in_pass = 1; assert(rs_store_low()); rs_store_done(0);
    assert(g_sf_draws == 1 && g_oq_on == 1 && R.col[0].texture == 11 && R.depth.texture == 13 && marker_ends == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="rs-store-") as temp:
    temp = Path(temp)
    c_file, exe = temp / "test.c", temp / ("test.exe" if os.name == "nt" else "test")
    c_file.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("full-grid store transition checks passed (mock renderer; no GPU coverage claim)")
