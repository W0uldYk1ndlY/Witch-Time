"""Execute production indirect draw/dispatch paths with a recording Vulkan boundary.

The extracted functions choose buffers, prepare descriptors, record commands,
publish writer serials and emit dependencies. Only the driver and unrelated
resource/translation infrastructure are replaced. This does not qualify actual
GPU execution, cache visibility, shader translation or image equivalence.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")


def function(name, optional=False):
    match = re.search(r"static[ \t]+[\w \t*]+\b" + name + r"\([^{};]*\)\s*\{", SOURCE)
    if not match:
        if optional:
            return ""
        raise AssertionError(name)
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end] + "\n"


def headers():
    candidates = [os.environ.get("VULKAN_INCLUDE", ""), "/usr/include"]
    if os.environ.get("VULKAN_SDK"):
        candidates += [str(Path(os.environ["VULKAN_SDK"]) / "include"),
                       str(Path(os.environ["VULKAN_SDK"]) / "Include")]
    for path in candidates:
        if path and (Path(path) / "vulkan/vulkan.h").is_file():
            return path
    raise RuntimeError("Set VULKAN_INCLUDE to a directory containing vulkan/vulkan.h")


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
typedef long long LONG64;
typedef struct { VkDeviceMemory mem; uint64_t off; void *map; int block; } VMem;
typedef struct { uint64_t addr, size; } Range;
typedef struct { uint8_t *code; int code_len; uint32_t ctl[8], local[3]; } Stage;
typedef struct { int serial; Stage st[6]; } Prog;
typedef struct {
    int nsmp, cp_state, cp_recorded; uint32_t cbufs, ssbos, ssbo_w, images;
    struct { uint32_t binding; } smp[32];
    VkDescriptorSetLayout cdl[2]; VkPipelineLayout cl; VkPipeline cp;
} VStage;
typedef struct { uint64_t off, size; int zero; VkBuffer buf; } BufRef;
typedef struct { uint64_t wm_off; uint32_t written; int16_t cb[16]; } CDisp;
typedef struct { uint64_t serial; int pending; } Slot;
#define NSLOT 4
#define CB_N 4
@CB_TYPE@
static CB g_cb[CB_N];
static int g_live_idx[CB_N], g_live_n, g_kept_idx[CB_N], g_kept_n;
static int g_cb_live, g_cb_pending, g_cb_nkept, g_cb_ww, g_ngcp;
static uint32_t g_cb_batch = 1, g_cb_epoch;
static uint64_t g_kept_lo, g_kept_hi, g_gpu_writes = 1, g_gpu_writes_seen;
static int g_gpucount = -1, g_writerwait = -1;
static LONG64 g_gc_calls, g_gc_draws, g_gc_fallback, g_gr_calls, g_gr_noext;
static int g_dp_potential, g_cmd_draws;
static struct {
    VkCommandBuffer cmd; VkBuffer ring, zero; VMem ring_mem;
    uint64_t serial, done, ring_size; int recording, cur, in_pass, pass_stores;
    int host_consumable_writes, cb_waits, dispatches, frame_skipped, frame_draws;
    Prog *prog[6]; uint32_t tex_int[6], tex_sint[6], c2[6][224];
    Range ubo[6][16], ssbo[6][32]; Slot slot[NSLOT];
} R;
static struct { VkDevice device; VkPhysicalDeviceProperties props; } V;
static struct { VkPhysicalDeviceFeatures2 f2; int has_iu8; } D;
static struct { VkImageView view; VkSampler smp; VkImageLayout layout; } g_dtex[64];
static int g_ndtex, g_ncdisp, g_pt_on, g_pipe_miss;
static struct { int last_what; } g_pt_slot[NSLOT];
static CDisp g_cdisp[2048];
static LONG64 g_tm[12];
static Prog program;
static VStage stage;
static uint32_t guest[32], gpu[32], relocated[32];
static uint8_t ring_memory[8192];
static uint64_t ring_pos;
static int waits, submits, cpu_reads, commands, indirect_commands, barriers, prepared;
static int copies, ring_action, ring_calls, cmp_calls, draw_commands, count_commands;
static uint32_t direct_groups[3], cmp_groups[3];
static VkBuffer recorded_buffer;
static uint64_t recorded_offset;
static VkPipelineStageFlags barrier_src[32], barrier_dst[32];
static VkAccessFlags barrier_read[32], barrier_write[32];
static void *P(uint64_t addr) { return (void *)(uintptr_t)addr; }
static LONG64 tk(void) { return 0; }
static int drawlog_on(void) { return 0; }
static long long nvn_front_frame_next(void) { return 0; }
static void nvn_vk_missing(const char *why) { fprintf(stderr, "%s\n", why); abort(); }
static void pass_end(void) { R.in_pass = R.pass_stores = 0; }
static void cmd_begin(void) { R.recording = 1; R.cmd = (VkCommandBuffer)(uintptr_t)7; }
static uint64_t cmd_submit(VkSemaphore a, VkSemaphore b) {
    (void)a; (void)b;
    if (!R.recording) return R.serial;
    submits++; R.recording = 0;
    R.slot[0].serial = ++R.serial; R.slot[0].pending = 1;
    R.host_consumable_writes = 0;
    return R.serial;
}
static void slot_wait(Slot *s) {
    assert(s->pending); waits++; s->pending = 0;
    if (s->serial > R.done) R.done = s->serial;
}
static void cb_sync(void) {
    cpu_reads++;
    for (int i = 0; i < CB_N; i++) if (g_cb[i].state == 1) {
        memcpy(P(g_cb[i].addr), g_cb[i].mem.map, (size_t)g_cb[i].size);
        g_cb[i].state = 0;
    }
    g_cb_live = g_cb_pending = 0; g_cb_epoch++;
}
static void gcp_touch(uint64_t a, uint64_t s) { (void)a; (void)s; abort(); }
static void cb_kept_range(uint64_t a, uint64_t s, int cause) { (void)a; (void)s; (void)cause; abort(); }
static void cb_kept_ready(CB *c) { (void)c; abort(); }
static int cb_guest_wrote(CB *c) { (void)c; abort(); }
static void pipeline_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
    VkPipelineStageFlags dst, VkDependencyFlags dep, uint32_t nm,
    const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
    uint32_t ni, const VkImageMemoryBarrier *i) {
    (void)dep; (void)b; (void)i;
    assert(cmd == R.cmd && !R.in_pass && nm <= 1 && !nb && !ni && barriers < 32);
    barrier_src[barriers] = src; barrier_dst[barriers] = dst;
    barrier_read[barriers] = nm ? m->srcAccessMask : 0;
    barrier_write[barriers++] = nm ? m->dstAccessMask : 0;
}
#define vkCmdPipelineBarrier pipeline_barrier
static void storage_barrier(void) {
    VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    pipeline_barrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0, NULL, 0, NULL);
}
static void dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
    assert(cmd == R.cmd && R.recording && !R.in_pass);
    commands++; direct_groups[0] = x; direct_groups[1] = y; direct_groups[2] = z;
}
static void dispatch_indirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset) {
    assert(cmd == R.cmd && R.recording && !R.in_pass && !(offset & 3));
    commands++; indirect_commands++; recorded_buffer = buffer; recorded_offset = offset;
}
#define vkCmdDispatch dispatch
#define vkCmdDispatchIndirect dispatch_indirect
static uint64_t ring_raw(const void *data, uint64_t size) {
    ring_calls++;
    if (ring_action && ring_calls == 1) {
        if (ring_action == 1) {
            g_cb[1] = g_cb[0]; g_cb[1].buf = (VkBuffer)(uintptr_t)43;
            g_cb[1].mem.map = relocated; g_live_idx[g_live_n++] = 1;
        } else {
            memcpy(guest, gpu, sizeof guest); g_cb_live = 0;
        }
        g_cb[0].state = 0; g_cb_epoch++; R.serial++;
    }
    uint64_t off = ring_pos; ring_pos += size;
    assert(ring_pos <= sizeof ring_memory);
    if (data) memcpy(ring_memory + off, data, (size_t)size);
    return off;
}
static uint64_t ring_put(uint64_t addr, uint64_t size) { copies++; return ring_raw(P(addr), size); }
static int buf_ref(uint64_t addr, uint64_t size, uint64_t align, BufRef *b);
static CB *cb_new(uint64_t addr, uint64_t size) { (void)addr; (void)size; abort(); }
static void resolve_sampler(int s, VStage *v, int k) { (void)s; (void)v; (void)k; abort(); }
static int resolve_image(int s, VStage *v, int k) { (void)s; (void)v; (void)k; return 1; }
static VkDescriptorSet set_alloc(VkDescriptorSetLayout l) { (void)l; return (VkDescriptorSet)(uintptr_t)8; }
static VStage *vstage_get(Prog *p, int s, uint32_t *key, int flag) { (void)p; (void)s; (void)key; (void)flag; return &stage; }
static int compute_pipe_ensure(VStage *v) { v->cp_state = 2; return 0; }
static void nvn_front_ccmp_dispatch(const Range *u, const Range *s, uint32_t w,
    const uint8_t *code, int len, uint32_t x, uint32_t y, uint32_t z) {
    (void)u; (void)s; (void)w; (void)code; (void)len;
    cmp_calls++; cmp_groups[0] = x; cmp_groups[1] = y; cmp_groups[2] = z;
}
#define hold_note(...) ((void)0)
#define piperec_append(...) ((void)0)
#define drawlog_ssbo(...) ((void)0)
#define pt_mark(...) ((void)0)
#define ckpt(...) ((void)0)
#define vkUpdateDescriptorSets(...) ((void)0)
#define vkCmdBindPipeline(...) ((void)0)
#define vkCmdBindDescriptorSets(...) ((void)0)
#define vkd_fail(...) abort()
#define GPUCOUNT_DEFAULT 1
static int draw_prepare(unsigned mode) { (void)mode; prepared++; cmd_begin(); return 1; }
static int index_extent(uint64_t ib, uint64_t *extent) { (void)ib; *extent = 128; return 1; }
static int recs_bounded(uint64_t r, unsigned n, unsigned stride, unsigned sz,
    unsigned elements, uint64_t ib, unsigned t) {
    (void)r; (void)n; (void)stride; (void)sz; (void)elements; (void)ib; (void)t; return 1;
}
static void draw_indirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride) {
    assert(cmd == R.cmd && n <= V.props.limits.maxDrawIndirectCount);
    assert(n <= 1 || (D.f2.features.multiDrawIndirect && !(stride & 3)));
    draw_commands++; recorded_buffer = b; recorded_offset = off;
}
static void draw_count(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize off, VkBuffer count,
    VkDeviceSize count_off, uint32_t n, uint32_t stride) {
    assert(count && !(count_off & 3) && !(stride & 3) && stride >= sizeof(VkDrawIndirectCommand)); count_commands++;
    draw_indirect(cmd, b, off, n, stride);
}
static PFN_vkCmdDrawIndexedIndirectCount vkCmdDrawIndexedIndirectCount_ = draw_count;
static PFN_vkCmdDrawIndirectCount vkCmdDrawIndirectCount_ = draw_count;
static void direct_draw(VkCommandBuffer cmd, uint32_t n, uint32_t instances, uint32_t first, uint32_t base) {
    (void)n; (void)instances; (void)first; (void)base; assert(cmd == R.cmd); draw_commands++;
}
static void indexed_draw(VkCommandBuffer cmd, uint32_t n, uint32_t instances, uint32_t first, int32_t vertex, uint32_t base) {
    (void)vertex; direct_draw(cmd, n, instances, first, base);
}
#define vkCmdDrawIndirect draw_indirect
#define vkCmdDrawIndexedIndirect draw_indirect
#define vkCmdDraw direct_draw
#define vkCmdDrawIndexed indexed_draw
#define vkCmdBindIndexBuffer(...) ((void)0)
#define fsr_evidence_draw(...) ((void)0)
#define nvn_front_count(...) ((void)0)
'''

BODY = r'''
static int buf_ref(uint64_t addr, uint64_t size, uint64_t align, BufRef *b) {
    CB *c = cb_inside(addr, size);
    b->zero = 0; b->size = size;
    if (c && !((addr - c->addr) % align)) { b->buf = c->buf; b->off = addr - c->addr; return 1; }
    b->buf = R.ring; b->off = ring_put(addr, size); return 0;
}
@COMPUTE_RUN@
@COMPUTE@
enum { OP_DISPATCH, OP_DISPATCH_INDIRECT, OP_DRAW_INDIRECT };
static void execute(int op, const uint64_t a[7], const uint8_t *blob) {
    switch (op) {
@DISPATCH_CASES@
@DRAW_CASE@
    default: abort();
    }
}
static void init(void) {
    V.props.limits.minStorageBufferOffsetAlignment = 4;
    V.props.limits.maxDrawIndirectCount = 16;
    D.f2.features.drawIndirectFirstInstance = 1;
    D.f2.features.multiDrawIndirect = 1;
    D.has_iu8 = 1;
    R.serial = 4; R.done = 3; R.recording = 1;
    R.cmd = (VkCommandBuffer)(uintptr_t)7; R.ring = (VkBuffer)(uintptr_t)9;
    R.ring_mem.map = ring_memory; R.ring_size = sizeof ring_memory;
    R.prog[5] = &program; program.st[5].code = (uint8_t *)"code";
    program.st[5].code_len = 4;
    stage.cp_state = 2;
    guest[1] = 1; guest[2] = 1; guest[3] = 1;
    gpu[1] = 6; gpu[2] = 2; gpu[3] = 1;
    relocated[1] = 7; relocated[2] = 3; relocated[3] = 2;
    g_cb[0].addr = (uint64_t)(uintptr_t)guest; g_cb[0].size = sizeof guest;
    g_cb[0].state = 1; g_cb[0].buf = (VkBuffer)(uintptr_t)42;
    g_cb[0].mem.map = gpu; g_cb[0].wserial = 5;
    g_live_idx[0] = 0; g_live_n = g_cb_live = 1;
}
int main(int argc, char **argv) {
    assert(argc == 2); init();
    uint64_t args[7] = {(uint64_t)(uintptr_t)&guest[1]};
    if (!strcmp(argv[1], "gpu") || !strcmp(argv[1], "gpu_without_draw_features")) {
        if (!strcmp(argv[1], "gpu_without_draw_features")) {
            D.f2.features.drawIndirectFirstInstance = 0;
            vkCmdDrawIndexedIndirectCount_ = NULL; vkCmdDrawIndirectCount_ = NULL;
        }
        execute(OP_DISPATCH_INDIRECT, args, NULL);
        assert(indirect_commands == 1 && "GPU-written dispatch arguments must stay on the GPU");
        assert(recorded_buffer == (VkBuffer)(uintptr_t)42 && recorded_offset == 4);
        assert(!waits && !submits && !cpu_reads && !copies);
        assert(barriers == 1 && (barrier_src[0] & VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT));
        assert((barrier_dst[0] & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) && !barrier_read[0] && !barrier_write[0]);
    } else if (!strcmp(argv[1], "relocated")) {
        stage.cbufs = 1u << 2; ring_action = 1;
        execute(OP_DISPATCH_INDIRECT, args, NULL);
        assert(indirect_commands == 1 && recorded_buffer == (VkBuffer)(uintptr_t)43);
        assert(recorded_offset == 4 && ring_calls == 2 && !waits && !submits);
    } else if (!strcmp(argv[1], "indirect_writer")) {
        stage.ssbos = stage.ssbo_w = 1; ring_action = 1;
        R.ssbo[5][0] = (Range){(uint64_t)(uintptr_t)guest, sizeof guest};
        execute(OP_DISPATCH_INDIRECT, args, NULL);
        assert(indirect_commands == 1 && recorded_buffer == (VkBuffer)(uintptr_t)43);
        assert(!waits && !submits && g_ncdisp == 1 && g_cdisp[0].cb[0] == 1);
        assert(g_cb[1].wserial == 6 && R.host_consumable_writes);
        assert(barriers == 1 && (barrier_src[0] & VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT));
        assert(barrier_src[0] & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        assert(barrier_write[0] & VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    } else if (!strcmp(argv[1], "landed") || !strcmp(argv[1], "landed_zero")) {
        stage.cbufs = 1u << 2; ring_action = 2;
        int zero = !strcmp(argv[1], "landed_zero");
        if (zero) gpu[2] = 0;
        execute(OP_DISPATCH_INDIRECT, args, NULL);
        assert(!indirect_commands && commands == !zero);
        if (!zero) assert(direct_groups[0] == 6 && direct_groups[1] == 2 && direct_groups[2] == 1);
        assert(!waits && !submits);
    } else if (!strcmp(argv[1], "unaligned")) {
        uint32_t dims[3] = {9, 4, 2}; memcpy((uint8_t *)gpu + 1, dims, 12);
        args[0] = (uint64_t)(uintptr_t)guest + 1;
        execute(OP_DISPATCH_INDIRECT, args, NULL);
        assert(!indirect_commands && commands == 1 && waits == 1);
        assert(direct_groups[0] == 9 && direct_groups[1] == 4 && direct_groups[2] == 2);
    } else if (!strcmp(argv[1], "disabled") || !strcmp(argv[1], "comparison")) {
        execute(OP_DISPATCH_INDIRECT, args, NULL);
        assert(!indirect_commands && commands == 1 && waits == 1 && submits == 1);
        assert(direct_groups[0] == 6 && direct_groups[1] == 2 && direct_groups[2] == 1);
        assert(cmp_calls == 1 && cmp_groups[0] == 6);
    } else if (!strcmp(argv[1], "direct_zero")) {
        args[0] = 6; args[1] = 0; args[2] = 2;
        execute(OP_DISPATCH, args, NULL);
        assert(!commands && !barriers && !waits && !submits);
    } else if (!strcmp(argv[1], "write_visibility")) {
        stage.ssbos = stage.ssbo_w = 1;
        R.ssbo[5][0] = (Range){(uint64_t)(uintptr_t)guest, sizeof guest};
        args[0] = 6; args[1] = 2; args[2] = 1;
        execute(OP_DISPATCH, args, NULL);
        assert(commands == 1 && barriers == 1 && !waits && !submits);
        assert(barrier_src[0] & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        assert(barrier_write[0] & VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        assert(g_cb[0].wserial == 5 && R.host_consumable_writes && g_ncdisp == 1);
    } else if (!strcmp(argv[1], "records_without_count")) {
        vkCmdDrawIndexedIndirectCount_ = NULL; vkCmdDrawIndirectCount_ = NULL;
        uint64_t draw[7] = {4, 1, 0, (uint64_t)(uintptr_t)&guest[1], 1, 16, 0};
        execute(OP_DRAW_INDIRECT, draw, NULL);
        assert(draw_commands == 1 && recorded_buffer == (VkBuffer)(uintptr_t)42);
        assert(!waits && !submits && !count_commands && !copies);
    } else if (!strcmp(argv[1], "draw_limit")) {
        V.props.limits.maxDrawIndirectCount = 1;
        gpu[1] = gpu[5] = 6; gpu[2] = gpu[6] = 1;
        uint64_t draw[7] = {4, 1, 0, (uint64_t)(uintptr_t)&guest[1], 2, 16, 0};
        execute(OP_DRAW_INDIRECT, draw, NULL);
        assert(draw_commands == 2 && !recorded_buffer && waits == 1);
    } else if (!strcmp(argv[1], "count_without_extension")) {
        vkCmdDrawIndexedIndirectCount_ = NULL; vkCmdDrawIndirectCount_ = NULL;
        gpu[0] = 1;
        uint64_t param = (uint64_t)(uintptr_t)guest;
        uint64_t draw[7] = {4, 1, 0, (uint64_t)(uintptr_t)&guest[1], 1, 16, 2};
        execute(OP_DRAW_INDIRECT, draw, (const uint8_t *)&param);
        assert(draw_commands == 1 && !count_commands && waits == 1);
    } else if (!strcmp(argv[1], "count_single_stride")) {
        gpu[0] = 1;
        uint64_t param = (uint64_t)(uintptr_t)guest;
        uint64_t draw[7] = {4, 1, 0, (uint64_t)(uintptr_t)&guest[1], 1, 13, 2};
        execute(OP_DRAW_INDIRECT, draw, (const uint8_t *)&param);
        assert(draw_commands == 1 && count_commands == 1 && !waits && !submits);
    } else abort();
    puts(argv[1]);
    return 0;
}
'''


def harness():
    cb_type = re.search(r"typedef struct \{[^\n]+\} CB;", SOURCE).group()
    code = PRELUDE.replace("@CB_TYPE@", cb_type)
    for name in ("cb_inside", "cb_overlap", "cpu_read_sync", "writerwait_on", "indirect_src"):
        code += function(name)
    for name in ("gpurecords_on", "gpucount_on", "gpudispatch_on", "gpu_indirect_buffer", "gpu_written"):
        code += function(name, optional=True)
    cases = SOURCE[SOURCE.index("    case OP_DISPATCH:"):SOURCE.index("    case OP_COPY_BUF_BUF:")]
    draw = SOURCE[SOURCE.index("    case OP_DRAW_INDIRECT:"):SOURCE.index("    case OP_COPY_BUF_TEX:")]
    return code + (BODY.replace("@COMPUTE_RUN@", function("compute_dispatch_run"))
                  .replace("@COMPUTE@", function("compute_dispatch"))
                  .replace("@DISPATCH_CASES@", cases).replace("@DRAW_CASE@", draw))


class IndirectCommandsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="vulkan-indirect-")
        cls.addClassCleanup(cls.directory.cleanup)
        path = Path(cls.directory.name)
        c_file, cls.exe = path / "test.c", path / "test"
        c_file.write_text(harness(), encoding="utf-8")
        cc = shlex.split(os.environ.get("HOST_CC", os.environ.get("CC", "cc")))
        if not shutil.which(cc[0]):
            raise RuntimeError("No C compiler available")
        result = subprocess.run(cc + ["-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-Wno-missing-field-initializers", "-I", headers(), str(c_file), "-o", str(cls.exe)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def run_case(self, case, **flags):
        env = dict(os.environ, WT_VK_WRITERWAIT="1", WT_VK_GPUCOUNT="1", WT_VK_GPUDISPATCH="1")
        env.pop("WT_COMPUTE_CMP", None)
        env.update(flags)
        result = subprocess.run([str(self.exe), case], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, f"{case}: {result.stderr}")

    def test_gpu_dispatch_records_without_cpu_wait(self): self.run_case("gpu")
    def test_gpu_dispatch_does_not_need_draw_features(self): self.run_case("gpu_without_draw_features", WT_VK_GPUCOUNT="0")
    def test_source_reselected_after_preparation(self): self.run_case("relocated")
    def test_indirect_writer_publishes_current_buffer_and_serial(self): self.run_case("indirect_writer")
    def test_landed_record_uses_exact_cpu_groups(self): self.run_case("landed")
    def test_landed_zero_dispatch_remains_empty(self): self.run_case("landed_zero")
    def test_unaligned_record_uses_cpu_fallback(self): self.run_case("unaligned")
    def test_disabled_dispatch_uses_cpu_fallback(self): self.run_case("disabled", WT_VK_GPUDISPATCH="0")
    def test_comparison_keeps_actual_group_counts(self): self.run_case("comparison", WT_COMPUTE_CMP="test")
    def test_direct_zero_dispatch_remains_empty(self): self.run_case("direct_zero")
    def test_dispatch_visibility_independent_of_draw_count_gate(self): self.run_case("write_visibility", WT_VK_GPUCOUNT="0")
    def test_gpu_record_draw_does_not_require_count_extension(self): self.run_case("records_without_count")
    def test_draw_count_limit_falls_back_without_dropping_records(self): self.run_case("draw_limit")
    def test_count_without_extension_stays_on_cpu(self): self.run_case("count_without_extension")
    def test_single_count_draw_normalizes_unused_stride(self): self.run_case("count_single_stride")


if __name__ == "__main__":
    unittest.main()
