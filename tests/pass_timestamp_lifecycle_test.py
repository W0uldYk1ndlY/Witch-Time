"""Dump-free production command/ring lifecycle, with Vulkan and unrelated hooks mocked."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'runtime/nvn_vk_draw.c').read_text()


def function(name):
    match = re.search(r'static (?:void|uint64_t) ' + name + r'\([^{}]*?\)\s*\{', source)
    assert match, name
    start = match.start()
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


harness = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "nvn_vk_timestamp.h"
typedef long long LONG64;
typedef long LONG;
#define NSLOT 2
#define FRSTAT_N 8
#define RING_ALIGN 256
typedef struct {
    VkCommandBuffer cmd, up; VkFence fence; VkDescriptorPool pool[1];
    int pending, npool, ipool, up_rec, up_ovl, ts_frame; uint64_t serial;
} Slot;
static struct {
    Slot slot[NSLOT]; int recording, cur, in_pass, dyn_dirty, vi_dirty, host_consumable_writes;
    VkCommandBuffer cmd; void *pipe_bound; uint64_t serial, done, flushes;
    uint64_t ring_pos, ring_size, gen, ring_wraps; struct { void *map; } ring_mem;
} R;
static struct { VkDevice device; VkPhysicalDevice gpu; VkQueue queue;
    uint32_t family; VkPhysicalDeviceProperties props; } V;
static int g_ts_on = 1, g_cur_frame, g_tset_used, g_tset_list_used, g_cb_lazy_bar;
static unsigned g_ts_bits = 64, g_tset_gen;
static VkQueryPool g_ts_pool = (VkQueryPool)(uintptr_t)1;
static double g_ts_ns;
static LONG64 g_ts_n, g_tm[1], g_tl_t0, g_tl_first, g_tl_last;
static float g_fr_gpu[FRSTAT_N];
static LONG64 tk(void) { static LONG64 t; return ++t; }
static void noop(void) {}
#define dsh_forget noop
#define garbage_collect noop
#define gcp_settle_done noop
#define nvn_front_done_moved noop
#define fault_dump noop
#define ckpt_dump noop
#define cb_ring_wrap noop
#define oq_land(...) ((void)0)
#define lr_land(...) ((void)0)
#define oq_slot_reset(...) ((void)0)
#define pass_end noop
#define diag_stream_gpu_sample(...) ((void)0)
#define diag_hook_gpu_complete(...) ((void)0)
#define diag_hook_gpu_submit(...) ((void)0)
#define vk_result_name(...) "mock failure"
#define vkd_fail(...) abort()
#define VKC(call) assert((call) == VK_SUCCESS)
#define vkResetDescriptorPool(...) VK_SUCCESS
#define vkCmdPipelineBarrier(...) ((void)0)
#define vkGetPhysicalDeviceQueueFamilyProperties(...) abort()
#define vkCreateQueryPool(...) (abort(), VK_ERROR_UNKNOWN)
static VkTimestampResult values[2][NSLOT * 96];
static unsigned reads[2][NSLOT], resets[2][NSLOT], submits, waits;
static VkResult pt_result = VK_SUCCESS;
static VkResult whole_result = VK_SUCCESS;
static uint64_t clock_tick;
static struct { int64_t frame, serial, ns; uint32_t pass, state; } events[1024];
static unsigned nevents;
static void diag_stream_gpu_component(int64_t frame, uint32_t pass, int64_t serial, int64_t ns, uint32_t state) {
    assert(nevents < 1024);
    assert((state == 0 || state == 4) ? ns >= 0 : ns == -1);
    events[nevents].frame = frame; events[nevents].pass = pass; events[nevents].serial = serial;
    events[nevents].ns = ns; events[nevents++].state = state;
}
static int slot_of(VkCommandBuffer cmd) {
    for (int i = 0; i < NSLOT; i++) if (R.slot[i].cmd == cmd) return i;
    abort();
}
static int pool_of(VkQueryPool pool) { return pool == g_ts_pool ? 0 : 1; }
static void reset_queries(VkCommandBuffer cmd, VkQueryPool pool, uint32_t first, uint32_t count) {
    int p = pool_of(pool), si = slot_of(cmd), stride = p ? 96 : 2;
    assert(first == (unsigned)(si * stride) && count == (unsigned)stride);
    assert(!R.slot[si].pending); /* collection must precede destructive reset */
    memset(values[p] + first, 0, count * sizeof(VkTimestampResult)); resets[p][si]++;
}
static void timestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
    (void)stage;
    int p = pool_of(pool), stride = p ? 96 : 2;
    assert(query / stride == (unsigned)slot_of(cmd));
    values[p][query] = (VkTimestampResult){clock_tick += 10, 1};
}
static VkResult results(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count,
                       size_t size, void *out, VkDeviceSize stride, VkQueryResultFlags flags) {
    (void)device;
    int p = pool_of(pool), width = p ? 96 : 2, si = first / width;
    assert(first == (unsigned)(si * width) && count <= (unsigned)width);
    assert(R.slot[si].pending && stride == sizeof(VkTimestampResult));
    assert(size == count * stride && flags == (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
    memcpy(out, values[p] + first, size); reads[p][si]++;
    return p ? pt_result : whole_result;
}
static VkResult wait_fences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    (void)d; (void)f; assert(n == 1 && all && timeout); waits++; return VK_SUCCESS;
}
static VkResult reset_fences(VkDevice d, uint32_t n, const VkFence *f) {
    (void)d; (void)n; (void)f; return VK_SUCCESS;
}
static VkResult reset_cmd(VkCommandBuffer cmd, VkCommandBufferResetFlags flags) {
    (void)flags; assert(!R.slot[slot_of(cmd)].pending); return VK_SUCCESS;
}
static VkResult begin_cmd(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info) {
    (void)cmd; assert(info->flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT); return VK_SUCCESS;
}
static VkResult end_cmd(VkCommandBuffer cmd) { (void)cmd; return VK_SUCCESS; }
static VkResult submit(VkQueue queue, uint32_t n, const VkSubmitInfo *info, VkFence fence) {
    (void)queue; assert(n == 1 && info->commandBufferCount == 1);
    int si = slot_of(info->pCommandBuffers[0]);
    assert(fence == R.slot[si].fence && !R.slot[si].pending); submits++; return VK_SUCCESS;
}
#define vkCmdResetQueryPool reset_queries
#define vkCmdWriteTimestamp timestamp
#define vkGetQueryPoolResults results
#define vkWaitForFences wait_fences
#define vkResetFences reset_fences
#define vkResetCommandBuffer reset_cmd
#define vkBeginCommandBuffer begin_cmd
#define vkEndCommandBuffer end_cmd
#define vkQueueSubmit submit
'''
harness += source[source.index('#define PT_MAX'):source.index('/* Round 16, the owner')]
harness += 'static volatile LONG g_in_fight = 1;\n'
for name in ('pt_components', 'slot_wait', 'cmd_begin', 'pt_mark', 'pt_begin', 'pt_end', 'cmd_submit', 'cmd_wait_all', 'ring_raw'):
    harness += function(name)
harness += r'''
static int kind(int what) {
    for (int k = 0; k < g_pt_nkind; k++) if (g_pt_kind[k].what == what) return k;
    abort();
}
static void interval(void) {
    LONG64 game_before = g_cmd_draws;
    pt_begin(PT_CAMERA_VELOCITY, 100, 50); g_pt_internal_draws++; pt_end();
    assert(g_cmd_draws == game_before);
}
int main(void) {
    unsigned char ring[1024] = {0};
    R.cur = -1; R.ring_size = sizeof ring; R.ring_mem.map = ring;
    V.props.limits.timestampPeriod = 1;
    g_pt_on = 1; g_pt_pool = (VkQueryPool)(uintptr_t)2;
    for (int i = 0; i < NSLOT; i++) {
        R.slot[i].cmd = (VkCommandBuffer)(uintptr_t)(i + 1);
        R.slot[i].fence = (VkFence)(uintptr_t)(i + 1);
    }
    cmd_begin(); assert(R.cur == 0 && !g_pt_internal_draws); interval();
    int k = kind(PT_CAMERA_VELOCITY);
    assert(cmd_submit(NULL, NULL) == 1 && g_pt_cnt[k] == 0);
    assert(nevents == 1 && events[0].pass == PT_CAMERA_VELOCITY && events[0].state == 1 && events[0].serial == 1);
    cmd_begin(); assert(R.cur == 1 && !g_pt_internal_draws); interval(); cmd_submit(NULL, NULL);
    /* Slot reuse collects its old submission before clearing query ownership. */
    cmd_begin(); assert(R.cur == 0 && g_pt_cnt[k] == 1 && g_pt_ns[k] == 10 && !g_pt_internal_draws);
    assert(reads[1][0] == 1 && resets[1][0] == 2 && !g_pt_slot[0].n);
    assert(events[2].serial == 1 && events[2].state == 0 && events[2].ns == 10);
    interval(); cmd_submit(NULL, NULL);
    slot_wait(&R.slot[0]); /* production recursively collects older slot 1 first */
    assert(R.done == 3 && g_pt_cnt[k] == 3 && g_pt_ns[k] == 30 && g_pt_draws[k] == 3);
    unsigned before = waits;
    slot_wait(&R.slot[0]); cmd_wait_all();
    assert(waits == before && reads[1][0] == 2 && reads[1][1] == 1);
    /* ring_raw really submits and drains before the next temporal marker. */
    cmd_begin(); pt_begin(PT_FULL_STORE, 100, 50);
    int store = kind(PT_FULL_STORE); R.ring_pos = R.ring_size;
    uint32_t payload = 0x12345678; uint64_t serial = R.serial;
    assert(ring_raw(&payload, sizeof payload) == 0);
    assert(!R.recording && R.serial == serial + 1 && R.done == R.serial);
    assert(R.ring_wraps == 1 && R.gen == 1 && !memcmp(ring, &payload, sizeof payload));
    assert(g_pt_cnt[store] == 1 && g_pt_ns[store] == 10);
    unsigned old_slot = R.cur;
    cmd_begin(); assert((unsigned)R.cur != old_slot && !g_pt_internal_draws);
    interval(); cmd_wait_all();
    assert(g_pt_cnt[k] == 4 && g_pt_ns[k] == 40); /* no interval across submissions */
    /* A final, unclosed marker uses this submission's end timestamp. */
    cmd_begin(); pt_begin(PT_CAMERA_VELOCITY, 100, 50); cmd_wait_all();
    assert(g_pt_cnt[k] == 5 && g_pt_ns[k] == 50);
    for (int scenario = 0; scenario < 4; scenario++) {
        cmd_begin(); interval(); int si = R.cur;
        if (scenario == 0) values[1][si * PT_MAX + 1].available = 0;
        if (scenario == 1) pt_result = VK_NOT_READY;
        if (scenario == 2) pt_result = VK_ERROR_DEVICE_LOST;
        if (scenario == 3) g_ts_bits = 0;
        cmd_wait_all();
        assert(!g_pt_slot[si].n && !R.slot[si].pending && g_pt_cnt[k] == 5);
        before = reads[1][si]; slot_wait(&R.slot[si]); assert(reads[1][si] == before);
        pt_result = VK_SUCCESS; g_ts_bits = 64;
    }
    assert(g_pt_state[1] == 2 && g_pt_state[3] == 1 && g_pt_state[2] == 1);
    cmd_begin();
    for (int i = 0; i < PT_MAX / 2; i++) interval();
    pt_begin(PT_COPY_CONVERT, 100, 50);
    assert(g_pt_slot[R.cur].overflow && g_pt_capacity == 1);
    cmd_wait_all(); assert(g_pt_cnt[k] == 5); /* discard the entire lossy buffer */
    cmd_begin(); assert(!g_pt_slot[R.cur].overflow); interval(); cmd_wait_all();
    assert(g_pt_cnt[k] == 6 && g_pt_ns[k] == 60);
    cmd_begin(); g_pt_nkind = PT_KINDS;
    pt_begin(PT_COPY_CONVERT, 777, 999);
    assert(g_pt_slot[R.cur].overflow && g_pt_capacity == 2);
    cmd_wait_all(); assert(g_pt_cnt[k] == 6);
    assert(submits == R.serial && waits == submits);
    /* Aggregate repeated stable components, including different dimension kinds; preserve measured zero. */
    g_pt_nkind = 0;
    unsigned start = nevents;
    g_cur_frame = 3; g_in_fight = 0; /* Structured components are not battle-only console statistics. */
    cmd_begin();
    pt_begin(PT_FULL_STORE, 100, 50); pt_end();
    pt_begin(PT_FULL_STORE, 200, 100); pt_end();
    pt_begin(PT_COPY_CONVERT, 100, 50); pt_end();
    int si = R.cur;
    values[1][si * PT_MAX + 5].value = values[1][si * PT_MAX + 4].value;
    cmd_submit(NULL, NULL);
    assert(nevents == start + 2 && events[start].state == 1 && events[start + 1].state == 1);
    slot_wait(&R.slot[si]);
    assert(nevents == start + 4);
    assert(events[start].frame == 3 && events[start + 2].frame == 3);
    assert(events[start].serial == events[start + 2].serial && events[start + 1].serial == events[start + 3].serial);
    assert(events[start + 2].pass == PT_FULL_STORE && events[start + 2].state == 0 && events[start + 2].ns == 20);
    assert(events[start + 3].pass == PT_COPY_CONVERT && events[start + 3].state == 0 && events[start + 3].ns == 0);
    slot_wait(&R.slot[si]); assert(nevents == start + 4);
    g_in_fight = 1;
    /* One bad interval invalidates the complete component, not just that interval. */
    for (int scenario = 0; scenario < 5; scenario++) {
        start = nevents; cmd_begin();
        pt_begin(PT_FULL_STORE, 100, 50); pt_end();
        pt_begin(PT_FULL_STORE, 100, 50); pt_end(); si = R.cur;
        unsigned base = si * PT_MAX;
        if (scenario == 0) values[1][base + 3].available = 0;
        if (scenario == 1) pt_result = VK_ERROR_DEVICE_LOST;
        if (scenario == 2) g_ts_bits = 0;
        if (scenario == 3) {
            g_ts_bits = 8; values[1][base + 2].value = 255; values[1][base + 3].value = 2;
        }
        if (scenario == 4) {
            while (g_pt_slot[si].n < PT_MAX) pt_end();
            pt_begin(PT_COPY_CONVERT, 100, 50); /* attempted kind must also receive overflow */
        }
        cmd_wait_all();
        unsigned count = scenario == 4 ? 2 : 1;
        assert(nevents == start + count * 2);
        unsigned state = scenario == 0 ? 1 : scenario == 1 ? 3 : scenario == 2 ? 2 : scenario == 3 ? 4 : 5;
        assert(events[start + count].pass == PT_FULL_STORE && events[start + count].state == state);
        assert(events[start + count].ns == (scenario == 3 ? 13 : -1));
        if (scenario == 4) assert(events[start + 3].pass == PT_COPY_CONVERT && events[start + 3].state == 5 && events[start + 3].ns == -1);
        pt_result = VK_SUCCESS; g_ts_bits = 64;
    }
    /* Kind capacity can fail before the first query; still publish both pending and overflow. */
    start = nevents; cmd_begin(); g_pt_nkind = PT_KINDS;
    pt_begin(PT_COPY_CONVERT, 12345, 6789);
    assert(!g_pt_slot[R.cur].n && g_pt_slot[R.cur].overflow);
    cmd_wait_all();
    assert(nevents == start + 2 && events[start].state == 1 && events[start + 1].state == 5);
    assert(events[start + 1].ns == -1);
    /* An unclosed component requires both PT and submission-end query results. */
    g_pt_nkind = 0;
    start = nevents; cmd_begin(); pt_begin(PT_FULL_STORE, 100, 50);
    pt_result = VK_NOT_READY; whole_result = VK_ERROR_DEVICE_LOST;
    cmd_wait_all();
    assert(nevents == start + 2 && events[start].state == 1);
    assert(events[start + 1].state == 3 && events[start + 1].ns == -1);
    pt_result = VK_SUCCESS; whole_result = VK_SUCCESS;
    puts("production command/ring lifecycle: slots, reuse, collect-once, ring-wrap, boundaries, states, capacity passed");
    puts("component stream: unique pending -> aggregate, zero, invalidation, wrap, overflow, collect-once passed");
}
'''

compiler = shutil.which(os.environ.get('CC', 'gcc'))
assert compiler, 'CC/gcc required; no dependencies installed'
temp_root = Path('C:/Users/HomePC/AppData/Local/Temp/opencode') if os.name == 'nt' else None
with tempfile.TemporaryDirectory(prefix='wt-pass-lifecycle-', dir=temp_root) as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness)
    subprocess.run([compiler, '-std=c11', '-I', str(ROOT / 'runtime'),
                    '-I', str(ROOT / 'deps/android-arm64/include'),
                    str(path / 'test.c'), '-o', str(path / 'test.exe')], check=True)
    subprocess.run([str(path / 'test.exe')], check=True)
