#!/usr/bin/env python3
"""Exercise the production pipeline lookup without Vulkan or game data.

The four-entry fast path must avoid the lookup lock for alternating complete
keys while preserving eviction, compilation, waiting and first-use accounting.
The shader/compiler and OS wait boundaries are stubbed; pipe_get and its real
key, table and pair bookkeeping functions are compiled directly from source.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "runtime/nvn_vk_m4.c").read_text(encoding="utf-8")


def function(name):
    match = re.search(r"static [^\n;{}]*\b" + name + r"\([^;{}]*\)\s*\{", SOURCE)
    if not match:
        raise AssertionError(f"Production function not found: {name}")
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end] + "\n"


def typedef(name):
    match = re.search(r"^typedef struct\b.*?\}\s*" + name + r";", SOURCE, re.MULTILINE | re.DOTALL)
    if not match:
        raise AssertionError(f"Production type not found: {name}")
    # Start at the last typedef before this declaration's closing name.
    start = SOURCE.rfind("typedef struct", match.start(), match.end())
    return SOURCE[start:match.end()] + "\n"


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long LONG;
typedef long long LONG64;
typedef uintptr_t VkPipeline;
typedef uintptr_t VkPipelineLayout;
typedef uintptr_t VkDescriptorSetLayout;
typedef uintptr_t VkShaderModule;
typedef struct { LONG64 QuadPart; } LARGE_INTEGER;
'''

GLOBALS = r'''
static Pipe *g_pipes[1024];
static Sig g_sig[64];
static int g_nsig, g_predict;
static PairRec *g_pair_tab[1024], *g_pair_all;
static struct { int has_gpl, debug; } D;
static int g_sh_lock, g_sh_cv, g_render_tid, g_in_fight;
static LONG64 g_tm[12], g_pipe_miss, g_pred_used;
static LONG64 g_fu_fr_n, g_fu_fr_linked, g_fu_fr_made, g_fu_fr_tk, g_fu_fr_newsig;
enum { H_STAGE_DRAW, H_STAGE_WAIT, H_PIPE_DRAW, H_PIPE_WAIT, H_N };
static LONG64 g_hold_n[H_N], g_hold_tk[H_N], g_hold_max;
static unsigned locks, sleeps, builds, records, learns;
static int skip_mode, build_fails, separate_libraries;
static Pipe *wait_for;
static Job *queued;
static VStage vs = {.id = 1, .hb = 11}, fs = {.id = 2, .hb = 22};

/* Count the real lookup's lock entry; no rendering or worker thread is needed. */
static void EnterCriticalSection(int *lock) { assert(lock == &g_sh_lock); locks++; }
static void LeaveCriticalSection(int *lock) { assert(lock == &g_sh_lock); }
static LONG64 tk(void) { static LONG64 now; return ++now; }
static int GetCurrentThreadId(void) { return 1; }
static void QueryPerformanceFrequency(LARGE_INTEGER *q) { q->QuadPart = 1000; }
static LONG64 nvn_front_frame_next(void) { return 1; }
static int skip_active(void) { return skip_mode; }
static int gpl_sep(void) { return separate_libraries; }
static int holdlog_on(void) { return 0; }
static void skip_note(int what, uint64_t vh, uint64_t fh) {
    assert(what == 1 && vh == vs.hb && fh == fs.hb);
}
static void cont_learn(PairRec *p) { assert(p->drawn); learns++; }
static void gpl_shared_vs(VStage *v) { assert(v == &vs); }
static void predict_pair(PairRec *p, int signature) { (void)p; (void)signature; abort(); }
static void job_push_front(Job *job) {
    assert(job->kind == 1 && job->pipe->state == 0 && !queued);
    queued = job;
}
static void wait_worker(int *cv, int *lock, unsigned timeout) {
    assert(cv == &g_sh_cv && lock == &g_sh_lock && timeout == UINT32_MAX);
    assert(wait_for && wait_for->state == 1);
    sleeps++;
    wait_for->p = (VkPipeline)(uintptr_t)wait_for;
    wait_for->state = 2;
}
#define INFINITE UINT32_MAX
#define WD_SLEEP(cv, lock, timeout) wait_worker(cv, lock, timeout)
/* Shader compilation and disk persistence are outside this lookup contract. */
static double pipe_build(Pipe *p) {
    assert(p->state == 1);
    builds++;
    p->p = build_fails ? 0 : (VkPipeline)(uintptr_t)p;
    p->state = 2;
    return 0.5;
}
static void piperec_append(const PKey *k, VStage *v, VStage *f, int compute) {
    assert(!compute && v == &vs && f == &fs && k->vs == vs.id && k->fs == fs.id);
    records++;
}
'''

CASES = r'''
static PKey key(unsigned color) {
    PKey k = {0};
    k.vs = vs.id; k.fs = fs.id; k.ncolor = 1; k.cfmt[0] = (int32_t)color;
    k.samples = 1; k.topo_class = 2;
    return k;
}
static Pipe *ready(PKey k, int used) {
    Pipe *p = pipe_new(&k, &vs, &fs, fnv(UINT64_C(0xcbf29ce484222325), &k, sizeof k), 2);
    p->p = (VkPipeline)(uintptr_t)p; p->used = (uint8_t)used;
    return p;
}
static Pipe *get(Pipe *p) {
    PKey copied_key = p->k; /* Equal bytes at another address must also hit. */
    return pipe_get(&copied_key, &vs, &fs);
}
static void alternating(void) {
    Pipe *a = ready(key(37), 1), *b = ready(key(44), 1);
    assert(get(a) == a && get(b) == b && locks == 2);
    for (int i = 0; i < 20; i++) assert(get(i & 1 ? b : a) == (i & 1 ? b : a));
    assert(locks == 2 && "alternating ready keys must not re-enter the lookup lock");
    assert(!builds && !records && !g_fu_fr_n);
}
static void promotion_and_eviction(void) {
    Pipe *a = ready(key(1), 1), *b = ready(key(2), 1), *c = ready(key(3), 1);
    Pipe *d = ready(key(4), 1), *e = ready(key(5), 1);
    assert(get(a) == a && get(b) == b && get(c) == c && get(d) == d && locks == 4);
    assert(get(a) == a && locks == 4); /* Promote the oldest entry. */
    assert(get(e) == e && locks == 5); /* Evict B, leaving A, C and D. */
    assert(get(a) == a && get(c) == c && get(d) == d && locks == 5);
    assert(get(b) == b && locks == 6); /* Still in the full table, outside the MRU. */
    assert(!builds && !records);
}
static void complete_key(void) {
    Pipe *base = ready(key(37), 1);
    assert(get(base) == base && locks == 1);
    for (size_t byte = 0; byte < sizeof(PKey); byte++) {
        PKey changed = base->k;
        ((uint8_t *)&changed)[byte] ^= 0x80;
        Pipe *other = ready(changed, 1);
        unsigned before = locks;
        assert(get(other) == other && locks == before + 1);
        assert(get(base) == base && locks == before + 1);
        assert(get(other) == other && locks == before + 1);
    }
}
static void first_use(void) {
    Pipe *a = ready(key(37), 0), *b = ready(key(44), 1);
    a->predicted = 1;
    assert(get(a) == a && a->used && a->recorded && locks == 1);
    assert(g_fu_fr_n == 1 && g_pred_used == 1 && records == 1 && learns == 1);
    assert(g_fu_fr_newsig == 1 && g_pair_all && g_pair_all->drawn && g_pair_all->used == 1);
    assert(get(b) == b && get(a) == a && locks == 2);
    assert(g_fu_fr_n == 1 && g_pred_used == 1 && records == 1 && learns == 1);
    /* A cached entry that still needs first-use bookkeeping cannot bypass it. */
    a->used = 0;
    assert(get(a) == a && locks == 3 && a->used);
    assert(g_fu_fr_n == 2 && records == 1 && learns == 2);
}
static void readiness_and_wait(void) {
    Pipe *a = ready(key(37), 1), *b = ready(key(44), 1);
    assert(get(a) == a && get(b) == b && locks == 2);
    skip_mode = 1; a->state = 0;
    assert(!get(a) && locks == 3 && !builds && !sleeps);
    a->state = 1;
    assert(!get(a) && locks == 4 && !builds && !sleeps);
    skip_mode = 0; wait_for = a;
    assert(get(a) == a && locks == 5 && sleeps == 1 && !builds);
    assert(g_hold_n[H_PIPE_WAIT] == 1 && !g_fu_fr_n);
    assert(get(b) == b && get(a) == a && locks == 5);
    a->state = 0;
    assert(get(a) == a && locks == 6 && builds == 1 && a->state == 2);
}
static void queued_and_failed(void) {
    PKey k = key(37);
    skip_mode = 1;
    assert(!pipe_get(&k, &vs, &fs) && locks == 1 && queued && !builds);
    Pipe *p = queued->pipe;
    assert(p->state == 0 && !p->used && !p->recorded);
    skip_mode = 0; build_fails = 1;
    assert(get(p) == p && locks == 2 && builds == 1 && !p->p && p->state == 2);
    assert(p->used && p->recorded && records == 1 && g_fu_fr_n == 1 && g_fu_fr_made == 1);
    Pipe *other = ready(key(44), 1);
    assert(get(other) == other && get(p) == p && locks == 3 && !p->p && builds == 1);
    assert(records == 1 && g_fu_fr_n == 1);
}
static void ready_libraries(void) {
    PKey k = key(37);
    skip_mode = D.has_gpl = separate_libraries = 1;
    vs.lib_state = fs.lib_state = 2;
    Pipe *p = pipe_get(&k, &vs, &fs);
    assert(p && p->p && builds == 1 && !queued && locks == 1);
    assert(g_fu_fr_linked == 1 && !g_fu_fr_made && !g_hold_n[H_PIPE_DRAW]);
    assert(get(p) == p && locks == 1 && records == 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "alternating")) alternating();
    else if (!strcmp(argv[1], "promotion_and_eviction")) promotion_and_eviction();
    else if (!strcmp(argv[1], "complete_key")) complete_key();
    else if (!strcmp(argv[1], "first_use")) first_use();
    else if (!strcmp(argv[1], "readiness_and_wait")) readiness_and_wait();
    else if (!strcmp(argv[1], "queued_and_failed")) queued_and_failed();
    else if (!strcmp(argv[1], "ready_libraries")) ready_libraries();
    else abort();
    puts("production pipeline lookup passed");
    return 0;
}
'''


class PipelineMruTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        source, cls.exe = directory / "pipeline_mru.c", directory / "pipeline_mru"
        types = "".join(typedef(name) for name in ("VSmp", "VStage", "SSrc", "PKey", "Pipe", "Sig", "PairRec", "Job"))
        production = "".join(function(name) for name in (
            "fnv", "pipe_find", "pipe_new", "pkey_vlay_set", "pair_get", "sig_note", "hold_note", "pipe_get"))
        source.write_text(PRELUDE + types + GLOBALS + production + CASES, encoding="utf-8")
        compiler = shlex.split(os.environ.get("HOST_CC", os.environ.get("CC", "cc")))
        compiled = subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.exe)],
                                  capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True, cwd=self.temp.name)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_alternating_ready_keys_avoid_lookup_lock(self):
        self.run_case("alternating")

    def test_four_entries_promote_hits_and_evict_least_recent(self):
        self.run_case("promotion_and_eviction")

    def test_every_key_byte_participates_in_equality(self):
        self.run_case("complete_key")

    def test_first_use_accounting_is_preserved(self):
        self.run_case("first_use")

    def test_not_ready_entries_use_skip_build_and_wait_paths(self):
        self.run_case("readiness_and_wait")

    def test_queued_compilation_preserves_failed_pipeline_handle(self):
        self.run_case("queued_and_failed")

    def test_ready_libraries_can_still_link_in_skip_mode(self):
        self.run_case("ready_libraries")


if __name__ == "__main__":
    unittest.main()
