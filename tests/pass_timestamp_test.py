"""Compile production PT intervals/reduction; full submission lifecycle is NOT tested."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import os
import re

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn_vk_draw.c').read_text()
declarations = source[source.index('#define PT_MAX'):source.index('/* Round 16, the owner')]
recording = source[source.index('static void pt_mark('):source.index('/* Ends the command buffer')]
reduction = source[source.index('            if (g_pt_slot[si].fight &&'):source.index('            g_pt_slot[si].n = 0;')]
fsr = (root / 'runtime/nvn_vk_fsr.c').read_text()
sgsr = (root / 'runtime/nvn_vk_sgsr.c').read_text()
assert 'pt_begin(p == P_MV ? PT_CAMERA_VELOCITY : PT_FSR_BASE + p' in fsr
assert fsr.count('pt_begin(PT_COPY_CONVERT, w, h);') == 3
assert 'pt_begin(PT_SGSR_FS_BASE + pass' in sgsr
assert 'pt_begin(PT_COPY_CONVERT, w, h);' in sgsr
assert source.count('pt_begin(PT_FULL_STORE, R.pass_w, R.pass_h);') == 2
full_store = re.findall(r'pt_begin\(PT_FULL_STORE, R.pass_w, R.pass_h\);.*?g_pt_internal_draws\+\+;', source)
assert len(full_store) == 2
fs_pass = sgsr[sgsr.index('    pt_begin(PT_SGSR_FS_BASE + pass'):sgsr.index('    pt_end();') + len('    pt_end();')]
draw_regression = '\n'.join('    ' + interval + '\n    pt_end();' for interval in full_store)
draw_regression += '\n    for (int pass = 0; pass < 2; pass++) {\n' + fs_pass + '\n    }\n'
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvn_vk_timestamp.h"
typedef long long LONG64;
typedef int VkQueryPool;
#define NSLOT 2
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT 1
#define VK_SUCCESS 0
static struct { int recording, cur, cmd, pass_w, pass_h, frame_draws; } R;
static struct { struct { struct { float timestampPeriod; } limits; } props; } V;
static unsigned writes, last_query;
static int g_in_fight = 1;
static unsigned g_ts_bits = 64;
static void vkCmdWriteTimestamp(int cmd, int stage, int pool, unsigned query) {
    (void)cmd; (void)stage; (void)pool; writes++; last_query = query;
}
''' + declarations + recording + r'''
static void collect(int si, int pr, int qr, VkTimestampResult *pt, VkTimestampResult *ts) {
    int n = g_pt_slot[si].n;
    uint64_t ticks;
''' + reduction + r'''
}
/* Vulkan command stubs discard arguments, but count every actual production draw. */
static int internal_draws;
#define vkCmdDraw(...) (++internal_draws)
#define vkCmdDrawIndexed(...) (++internal_draws)
#define vkCmdBeginRendering(...) ((void)0)
#define vkCmdEndRendering(...) ((void)0)
#define vkCmdBindPipeline(...) ((void)0)
#define vkCmdSetViewport(...) ((void)0)
#define vkCmdSetScissor(...) ((void)0)
#define vkCmdPushDescriptorSetKHR(...) ((void)0)
static void check_internal_draws(void) {
    struct TestImage { int w, h; } image = {100, 50};
    struct { struct TestImage *vt; } target_value = {&image}, *target = &target_value;
    R.pass_w = 100; R.pass_h = 50;
    int game_draws = R.frame_draws;
    LONG64 before = g_pt_internal_draws, game_before = g_cmd_draws;
''' + draw_regression + r'''
    assert(internal_draws == 4 && g_pt_internal_draws == before + 4);
    assert(g_cmd_draws == game_before); /* WT_VK_SPLIT policy is unchanged. */
    assert(R.frame_draws == game_draws);
    int replay = -1, fs0 = -1, fs1 = -1;
    for (int k = 0; k < g_pt_nkind; k++) {
        if (g_pt_kind[k].what == PT_FULL_STORE) replay = k;
        if (g_pt_kind[k].what == PT_SGSR_FS_BASE) fs0 = k;
        if (g_pt_kind[k].what == PT_SGSR_FS_BASE + 1) fs1 = k;
    }
    assert(replay >= 0 && fs0 >= 0 && fs1 >= 0);
    assert(g_pt_draws[replay] == 2 && g_pt_draws[fs0] == 1 && g_pt_draws[fs1] == 1);
}
int main(void) {
    V.props.limits.timestampPeriod = 1;
    R.recording = 1; g_pt_on = 0;
    pt_begin(PT_CAMERA_VELOCITY, 100, 50); assert(!writes);
    g_pt_on = 1; R.recording = 0;
    pt_begin(PT_CAMERA_VELOCITY, 100, 50); assert(!writes);
    R.recording = 1;
    pt_begin(PT_CAMERA_VELOCITY, 100, 50); pt_end();
    assert(writes == 2 && last_query == 1);
    VkTimestampResult pt[PT_MAX] = {{10,1},{20,1}};
    VkTimestampResult ts[2] = {{0,1},{1000,1}};
    collect(0, 0, 0, pt, ts);
    assert(g_pt_ns[0] == 10 && g_pt_cnt[0] == 1); /* gap is not charged */
    collect(0, 1, 0, pt, ts); assert(g_pt_cnt[0] == 1 && g_pt_state[1] == 1);
    collect(0, -4, 0, pt, ts); assert(g_pt_cnt[0] == 1 && g_pt_state[3] == 1);
    pt[1].available = 0; collect(0, 0, 0, pt, ts);
    assert(g_pt_cnt[0] == 1 && g_pt_state[1] == 2); pt[1].available = 1;
    g_ts_bits = 0; collect(0, 0, 0, pt, ts); assert(g_pt_state[2] == 1);
    g_ts_bits = 64;
    R.cur = 1; pt_begin(PT_CAMERA_VELOCITY, 100, 50); pt_end();
    assert(last_query == PT_MAX + 1);
    pt[0].value = 30; pt[1].value = 45;
    collect(1, 0, 0, pt, ts); assert(g_pt_ns[0] == 25 && g_pt_cnt[0] == 2);
    while (g_pt_slot[1].n < PT_MAX) pt_end();
    unsigned before = writes;
    pt_begin(PT_COPY_CONVERT, 100, 50); pt_end();
    assert(writes == before && g_pt_capacity == 1 && g_pt_slot[1].overflow);
    collect(1, 0, 0, pt, ts); assert(g_pt_cnt[0] == 2);
    memset(g_pt_slot, 0, sizeof g_pt_slot); R.cur = 0;
    check_internal_draws();
    memset(g_pt_slot, 0, sizeof g_pt_slot);
    before = writes;
    g_pt_nkind = PT_KINDS;
    pt_begin(PT_FULL_STORE, 12345, 6789);
    assert(g_pt_slot[0].overflow && writes == before && g_pt_capacity == 2);
    puts("production PT recording/reduction: boundaries, disabled, pending, failure, unsupported, slots, capacities passed");
    puts("internal graphics intervals: 2 full-store + 2 FS draws correctly attributed; game counter unchanged");
    puts("interval-only harness; command/ring lifecycle is covered separately by pass_timestamp_lifecycle_test.py");
}
'''
compiler = shutil.which('gcc')
assert compiler, 'gcc required (no dependencies installed by this test)'
temp_root = Path(os.environ['LOCALAPPDATA']) / 'Temp/opencode' if os.name == 'nt' else None
with tempfile.TemporaryDirectory(prefix='wt-pass-timestamp-', dir=temp_root) as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness)
    subprocess.run([compiler, '-std=c11', '-I', str(root / 'runtime'), str(path / 'test.c'), '-o', str(path / 'test.exe')], check=True)
    subprocess.run([str(path / 'test.exe')], check=True)
