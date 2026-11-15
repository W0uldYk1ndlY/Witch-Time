"""Execute production direct/indexed exec paths with mocked Vulkan/preparation.

No GPU qualification: raster stores and ring allocation are hostile mocks. The
production case bodies, replay helpers, stream-only buf_ref path, active-stage
list and all-stage side-effect loop are extracted afresh, not modeled orchestration.

Remaining gates: actual draw_prepare/pass/resolve_image/storage barriers; real
ring allocator and GPU completion/lifetime; GPU indirect/count variants; real
conditional query evaluation; real shader side effects; integrated safe-scale
rejection and game visual/device acceptance. Do not remove the clamp on this
test's evidence. Synthetic grid coverage is a mock oracle, not GPU coverage.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")


def function(name):
    match = re.search(r"static (?:int|void) " + name + r"\([^\n]*\) \{.*?\n\}", SOURCE, re.S)
    assert match, name
    return match.group()


CASES = SOURCE.split("    case OP_DRAW_ARRAYS:", 1)[1].split("    case OP_COPY_BUF_TEX:", 1)[0]
CASES = "case OP_DRAW_ARRAYS:" + CASES
CASES += re.search(r"    case OP_RENDER_ENABLE:.*?\n", SOURCE).group()
# Exclude the intervening viewport/logging setup; execute the original topology
# validation branch itself, independently of mocked pass setup.
TOPOLOGY = re.search(r"    static const int8_t topo\[7\].*?\n", SOURCE).group()
TOPOLOGY += "int tess = R.prog[3] || R.prog[4];" + SOURCE.split("    int tess = R.prog[3] || R.prog[4];", 1)[1].split("    VkPrimitiveTopology vtopo", 1)[0]
LOW, DONE = function("rs_store_low"), function("rs_store_done")
# Only the production stream fast path: all CB lookup/coherency is out of scope.
BUF = function("buf_ref").split("    if (g_cb_bound < 0)", 1)[0] + "    abort();\n}\n"
PREDICATE = re.search(r"g_dp_fs_stores = fs && fs->images && !buffer_writes;", SOURCE)
assert PREDICATE
ACTIVE_STAGES = "VStage *sv[4];" + SOURCE.split("    VStage *sv[4];", 1)[1].split("    PT(0);", 1)[0]
IMAGE_LOOP = "int stores = 0, buffer_writes = 0;" + SOURCE.split("    int stores = 0, buffer_writes = 0;", 1)[1].split("    PT(1);", 1)[0]

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef long long LONG64;
typedef int VkBuffer;
typedef struct { int texture, view; } TargetRef;
typedef struct { uint64_t off, size; int zero; VkBuffer buf; } BufRef;
typedef struct { int op; uint64_t a[7]; } Cmd;
typedef struct { int images, ssbo_w, ssbos; } VStage;
typedef struct { int serial; } Prog;
typedef struct { int state, buf; uint64_t addr, size; } CB;
static CB g_cb[1];
static int g_live_n, g_kept_n, g_live_idx[1], g_kept_idx[1];
static struct {
    int in_pass, pass_rs, pass_w, pass_h, ncol, have_targets, dyn_dirty;
    int cmd, ring, frame_skipped, frame_draws, pass_stores, render_off;
    uint32_t patch;
    Prog *prog[6];
    struct { void *map; } ring_mem;
    float pass_sx, pass_sy;
    TargetRef col[8], depth;
} R;
static struct { int has_iu8; struct { struct { int multiDrawIndirect; } features; } f2; } D;
static struct { struct { struct { unsigned maxTessellationPatchSize, maxDrawIndirectCount; } limits; } props; } V;
static TargetRef g_sf_col[8], g_sf_dep;
static int g_sf_ncol, g_sf_have, g_sf_oq, g_sf_on = -1;
static LONG64 g_sf_draws, g_tm[9];
static int g_dp_fs_stores, g_rs_on, g_oq_on, g_cmd_draws, g_pt_internal_draws;
static int prepares, draws, refs, ends, markers, query_samples, buffer_writes;
static int fail_prepare, wrap, bound_buf, bad_target, bad_ring, missing;
static int g_gr_noext, g_dp_potential, g_cb_live, g_gr_calls, g_gc_calls, g_gc_draws, g_gc_fallback;
static uint32_t indirect_record[5] = {6, 2, 3, 5, 0};
static VStage stage, other_stages[3];
static int grid[11][13];
#define RS_ON g_rs_on
#define OP_DRAW_ARRAYS 1
#define OP_DRAW_ELEMENTS 2
#define OP_DRAW_INDIRECT 3
#define OP_RENDER_ENABLE 4
#define VK_PRIMITIVE_TOPOLOGY_POINT_LIST 0
#define VK_PRIMITIVE_TOPOLOGY_LINE_LIST 1
#define VK_PRIMITIVE_TOPOLOGY_LINE_STRIP 2
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST 3
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP 4
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN 5
#define VK_INDEX_TYPE_UINT8_EXT 0
#define VK_INDEX_TYPE_UINT16 1
#define VK_INDEX_TYPE_UINT32 2
#define PT_FULL_STORE 3
static LONG64 tk(void) { return 0; }
static int nvn_front_draw_tag(void) { return 0; }
#define nvn_front_draw_tag_next() 0
#define dt_vk(...) ((void)0)
#define shlog_draw(...) ((void)0)
#define nvn_front_count(...) ((void)0)
#define ckpt(...) ((void)0)
#define primlog_add(...) ((void)0)
static void fsr_evidence_draw(void) {}
/* Object-motion replay is outside this raster-store harness. */
#define fsr_mv_commit(...) ((void)0)
static void nvn_vk_missing(const char *s) { (void)s; missing++; }
static int topology_valid(unsigned prim) {
    @TOPOLOGY@
    return 1;
}
#define drawlog_on() 0
#define nvn_front_frame_next() 0LL
static int gpu_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
static int gpucount_on(void) { return 0; }
static int index_extent(uint64_t addr, uint64_t *extent) { (void)addr; *extent = 0; return 0; }
static int cb_overlap(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
static int recs_bounded(uint64_t addr, uint32_t n, uint32_t stride, uint32_t sz, int elements, uint64_t idx, unsigned type) {
    (void)addr; (void)n; (void)stride; (void)sz; (void)elements; (void)idx; (void)type;
    abort(); /* GPU-count paths are deliberately not exercised. */
}
static CB *cb_inside(uint64_t addr, uint64_t size) { (void)addr; (void)size; return NULL; }
static const void *indirect_src(uint64_t addr, uint64_t size) { (void)addr; assert(size <= sizeof indirect_record); return indirect_record; }
static uint64_t ring_raw(const void *p, uint64_t size) { (void)p; (void)size; abort(); }
#define vkCmdDrawIndexedIndirectCount_(...) ((void)rb, (void)ro, (void)rs, abort())
#define vkCmdDrawIndirectCount_(...) ((void)rb, (void)ro, (void)rs, abort())
#define vkCmdDrawIndexedIndirect(...) ((void)rb, (void)ro, (void)rs, abort())
#define vkCmdDrawIndirect(...) ((void)rb, (void)ro, (void)rs, abort())
static void pass_end(void) { ends++; R.in_pass = 0; }
static void pt_end(void) { markers--; }
static void pt_begin(int kind, int w, int h) {
    assert(kind == PT_FULL_STORE && w == 13 && h == 11);
    markers++;
}
static int cb_pf_any(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
static int resolve_image(int s, VStage *v, int k) { assert(s == 0 || s == 1 || s == 3 || s == 4); assert(v->images & (1 << k)); return 1; }
static uint64_t ring_put(uint64_t addr, uint64_t size) {
    assert(addr == 4096 && size == 12);
    refs++;
    if (wrap) { R.cmd++; R.ring++; pass_end(); }
    return (uint64_t)refs * 16;
}
''' + BUF + LOW + DONE + r'''
static int draw_prepare(unsigned prim) {
    prepares++;
    g_dp_fs_stores = 0;
    if (R.render_off || prepares == fail_prepare || !topology_valid(prim)) return 0;
    VStage *fs = &stage, *vs = &other_stages[0], *tcs = &other_stages[1], *tes = &other_stages[2];
    int tess = R.prog[3] || R.prog[4];
    @ACTIVE_STAGES@
    @IMAGE_LOOP@
    (void)stores;
    @PREDICATE@
    R.in_pass = R.pass_rs = 1;
    R.pass_w = R.have_targets ? 10 : 13;
    R.pass_h = R.have_targets ? 8 : 11;
    R.pass_sx = R.pass_sy = R.have_targets ? 0.75f : 1.0f;
    // Hostile wrap after initial index upload: replay must reacquire its reference.
    if (wrap && prepares == 1) { R.cmd++; R.ring++; }
    return 1;
}
static void vkCmdBindIndexBuffer(int cmd, int buf, uint64_t off, int type) {
    assert(cmd == R.cmd && off == (uint64_t)refs * 16 && type == 1);
    bound_buf = buf;
}
static void store_draw(int indexed, uint32_t count, uint32_t instances) {
    draws++;
    if (!count || !instances) return;
    if (indexed && prepares == 2 && bound_buf != R.ring) bad_ring++;
    if (prepares == 2 && (R.have_targets || R.ncol || R.col[0].texture || R.depth.texture)) bad_target++;
    if (g_oq_on) query_samples++;
    if (stage.ssbo_w || other_stages[0].ssbo_w || other_stages[1].ssbo_w || other_stages[2].ssbo_w) buffer_writes++;
    if (stage.images) for (int y = 0; y < R.pass_h; y++) for (int x = 0; x < R.pass_w; x++) {
        int gx = x * 13 / R.pass_w, gy = y * 11 / R.pass_h;
        grid[gy][gx] = 1000 + gy * 13 + gx;
    }
}
static void vkCmdDraw(int cmd, uint32_t count, uint32_t inst, uint32_t first, uint32_t base) {
    assert(cmd == R.cmd && count == 6 && inst == 2 && first == 3 && base == 5);
    store_draw(0, count, inst);
}
static void vkCmdDrawIndexed(int cmd, uint32_t count, uint32_t inst, uint32_t first, int32_t vertex, uint32_t base) {
    assert(cmd == R.cmd && count == 6 && inst == 2 && first == 0 && vertex == -3 && base == 5);
    store_draw(1, count, inst);
}
static void execute(const Cmd *m) {
    const uint64_t *a = m->a;
    const uint8_t *blob = (const uint8_t *)indirect_record;
    switch (m->op) {
    @CASES@
    default: abort();
    }
}
static int holes(void) {
    int n = 0;
    for (int y = 0; y < 11; y++) for (int x = 0; x < 13; x++) n += grid[y][x] != 1000 + y * 13 + x;
    return n;
}
static void reset(void) {
    memset(&R, 0, sizeof R); memset(grid, 0, sizeof grid);
    stage = (VStage){1, 0, 0}; memset(other_stages, 0, sizeof other_stages);
    R.col[0] = (TargetRef){11, 12}; R.col[7] = (TargetRef){71, 72}; R.depth = (TargetRef){13, 14};
    R.ncol = 8; R.have_targets = 1; R.ring = 10; R.cmd = 20;
    g_rs_on = g_oq_on = D.has_iu8 = 1;
    g_sf_on = -1; g_sf_draws = g_pt_internal_draws = g_cmd_draws = 0;
    prepares = draws = refs = ends = markers = query_samples = buffer_writes = 0;
    fail_prepare = wrap = bound_buf = bad_target = bad_ring = missing = 0;
    V.props.limits.maxTessellationPatchSize = 32;
    V.props.limits.maxDrawIndirectCount = 16;
}
static void restored(void) {
    assert(R.ncol == 8 && R.have_targets && R.col[0].texture == 11 && R.col[0].view == 12);
    assert(R.col[7].texture == 71 && R.col[7].view == 72 && R.depth.texture == 13 && R.depth.view == 14);
    assert(g_oq_on == 1 && R.dyn_dirty && !markers && !bad_target && !bad_ring);
}
int main(int argc, char **argv) {
    int disabled = argc > 1 && !strcmp(argv[1], "disabled");
    Cmd arrays = {OP_DRAW_ARRAYS, {4, 3, 6, 5, 2, 0, 0}};
    Cmd indexed = {OP_DRAW_ELEMENTS, {4, 1, 6, 4096, (uint64_t)-3, 5, 2}};
    Cmd indirect = {OP_DRAW_INDIRECT, {4, 1, 0, 8192, 1, 16, 0}};
    for (int i = 0; i < 2; i++) {
        reset(); wrap = i; execute(i ? &indexed : &arrays);
        if (disabled) {
            assert(draws == 1 && prepares == 1 && !g_sf_draws && holes() > 0);
            assert(R.col[0].texture == 11 && g_oq_on == 1);
            continue;
        }
        assert(prepares == 2 && draws == 2 && g_sf_draws == 1 && g_pt_internal_draws == 1);
        assert(R.frame_draws == 1 && g_cmd_draws == 1 && query_samples == 1 && !buffer_writes);
        assert(holes() == 0 && (!i || refs == 2)); restored();
        reset(); fail_prepare = 2; execute(i ? &indexed : &arrays);
        assert(draws == 1 && !g_sf_draws && holes() > 0); restored();
        reset(); stage.ssbo_w = 1; execute(i ? &indexed : &arrays);
        assert(draws == 1 && prepares == 1 && buffer_writes == 1 && !g_sf_draws && holes() > 0);
        reset(); stage.images = 0; execute(i ? &indexed : &arrays);
        assert(draws == 1 && prepares == 1 && !g_sf_draws);
        reset(); R.render_off = 1; execute(i ? &indexed : &arrays);
        assert(!draws && prepares == 1 && !g_sf_draws);
        reset(); fail_prepare = 1; execute(i ? &indexed : &arrays);
        assert(!draws && !g_sf_draws && R.frame_skipped == 1);
    }
    // A demonstrated integration limit, not approval to lower the safe scale:
    // production CPU-record indirect executes once with sparse game-grid stores.
    reset(); execute(&indirect);
    assert(prepares == 1 && draws == 1 && !g_sf_draws && holes() > 0);
    reset(); R.render_off = 1; execute(&indirect);
    assert(prepares == 1 && !draws && !g_sf_draws);
    Cmd enable = {OP_RENDER_ENABLE, {0, 0}};
    reset(); execute(&enable); assert(R.render_off); execute(&arrays); assert(!draws);
    enable.a[0] = 1; execute(&enable); assert(!R.render_off);
    enable.a[1] = 1; execute(&enable); assert(missing == 1 && !R.render_off);
    // Conditional requests are unsupported, not a qualified sparse-store route.
    Prog control = {3}, evaluation = {4};
    reset(); R.prog[3] = &control; R.prog[4] = &evaluation; R.patch = 3;
    arrays.a[0] = 14; execute(&arrays);
    assert(draws == (disabled ? 1 : 2) && !missing);
    reset(); R.prog[3] = &control; R.patch = 3; execute(&arrays);
    assert(!draws && missing == 1);
    reset(); R.prog[3] = &control; R.prog[4] = &evaluation; R.patch = 33; execute(&arrays);
    assert(!draws && missing == 1);
    for (int indexed_draw = 0; indexed_draw < 2; indexed_draw++) {
        Cmd draw = indexed_draw ? indexed : arrays;
        for (int writer = 0; writer < 3; writer++) {
            reset(); draw.a[0] = writer ? 14 : 4;
            if (writer) { R.prog[3] = &control; R.prog[4] = &evaluation; R.patch = 3; }
            other_stages[writer].ssbo_w = 1 << writer;
            // No CB exists: shader metadata must reject replay independently.
            assert(!g_cb_live); execute(&draw);
            assert(draws == 1 && prepares == 1 && buffer_writes == 1 && !g_sf_draws && holes() > 0);
        }
        reset(); draw.a[0] = 14;
        R.prog[3] = &control; R.prog[4] = &evaluation; R.patch = 3;
        for (int s = 0; s < 3; s++) other_stages[s].images = 1 << s;
        execute(&draw); // All four active stages image-only: replay remains safe.
        assert(draws == (disabled ? 1 : 2) && !buffer_writes);
        assert(disabled ? holes() > 0 : holes() == 0);
        reset(); R.prog[3] = &control; R.prog[4] = &evaluation; R.patch = 3;
        stage.ssbos = 1;
        for (int s = 0; s < 3; s++) other_stages[s].ssbos = 1 << s;
        execute(&draw); // Read-only SSBOs must not block replay.
        assert(draws == (disabled ? 1 : 2) && !buffer_writes);
    }
    return 0;
}
'''
HARNESS = HARNESS.replace("@TOPOLOGY@", TOPOLOGY).replace("@PREDICATE@", PREDICATE.group()).replace("@CASES@", CASES)
HARNESS = HARNESS.replace("@ACTIVE_STAGES@", ACTIVE_STAGES).replace("@IMAGE_LOOP@", IMAGE_LOOP)


def run():
    temp_root = Path(os.environ.get("LOCALAPPDATA", "")) / "Temp/opencode" if os.name == "nt" else None
    with tempfile.TemporaryDirectory(prefix="rs-store-production-", dir=temp_root) as directory:
        directory = Path(directory)
        c_file = directory / "test.c"
        exe = directory / ("test.exe" if os.name == "nt" else "test")
        env = dict(os.environ, WT_VK_CBPF="1", WT_VK_STOREFULL="1")
        for name, text in [
            ("production", HARNESS),
            ("missing-replay", HARNESS.replace("if (rs_store_low())", "if (0 && rs_store_low())")),
            ("stale-index", HARNESS.replace(
                "buf_ref(a[3], (uint64_t)count << t, (uint64_t)1 << t, &ib);\n            int ok",
                "/* negative control: reuse stale index reference */\n            int ok")),
            ("wrong-target", HARNESS.replace("R.ncol = 0; R.have_targets = 0;", "R.ncol = 8; R.have_targets = 1;")),
            ("query-leak", HARNESS.replace("g_sf_oq = g_oq_on; g_oq_on = 0;", "g_sf_oq = g_oq_on; g_oq_on = 1;")),
            ("duplicate-buffer-write", HARNESS.replace("fs && fs->images && !buffer_writes", "fs && fs->images")),
            ("fs-only-guard", HARNESS.replace("fs && fs->images && !buffer_writes", "fs && fs->images && !fs->ssbo_w")),
        ]:
            assert name == "production" or text != HARNESS, name
            c_file.write_text(text, encoding="utf-8")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", str(c_file), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], env=env, capture_output=True)
            if name == "production":
                assert result.returncode == 0, result.stderr.decode(errors="replace")
                subprocess.run([str(exe), "disabled"], env=dict(env, WT_VK_STOREFULL="0"), check=True)
            else:
                assert result.returncode != 0, f"negative control survived: {name}"
            print(f"{name}: {'passed' if name == 'production' else 'rejected as expected'}")
    print("mock orchestration only; GPU/barrier/eligibility integration remain unqualified")


if __name__ == "__main__":
    run()
