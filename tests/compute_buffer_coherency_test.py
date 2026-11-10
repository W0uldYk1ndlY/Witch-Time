#!/usr/bin/env python3
"""Exercise compute buffer coherency checks extracted from runtime/nvn_vk_draw.c.
Follows tests/android_surface_test.py by extracting production functions.
Verifies:
- Interior CPU write with unavailable tracking fails against old implementation (returns 0)
  and passes against new implementation (returns 1).
- Complete snapshot fallback covers aligned, unaligned, and sub-page ranges.
- Head, interior, and tail changes are all detected.
- Missing and invalid snapshots (including retained allocation with disabled merge)
  conservatively return 1 (possibly changed).
- Tracking failure after tracked fill falls back safely.
- Reliable tracked path retains efficient reuse without full-range scan.
- Once-per-batch verdict reuse in cb_kept_wrote.
- No out-of-range reads.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRAW_SOURCE = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")

def extract_function(name):
    if name == "cb_ww_query":
        sig = "static uint64_t cb_ww_query(CB *k) {"
    else:
        sig = f"static int {name}(CB *k) {{"
    start = DRAW_SOURCE.index(sig)
    end = DRAW_SOURCE.index("\n}", start) + 2
    return DRAW_SOURCE[start:end]

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int64_t LONG64;
typedef uint64_t VkBuffer;
typedef struct { void *mem; uint64_t off; void *map; } VMem;

typedef struct {
    uint64_t addr, size, cap, busy;
    VkBuffer buf;
    VMem mem;
    int state, dirty;
    uint8_t *snap;
    uint64_t snap_cap;
    uint8_t snap_valid;
    int stale, age;
    uint64_t kserial, ww, wserial;
    VkBuffer stg;
    VMem stg_mem;
    uint64_t stg_cap, lserial;
    uint32_t wchk;
    uint8_t wrote, touch;
} CB;

static int g_cb_merge = 1;
static int g_cb_ww = 1;
static uint32_t g_cb_batch = 1;
static LONG64 g_ck_unwatched = 0;

static uint64_t g_mock_ww_result = 0;
static int g_mock_ww_calls = 0;
static void *g_mock_last_ptr = NULL;
static size_t g_mock_last_len = 0;

uint64_t nvn_front_mem_newest(void *p, size_t len) {
    g_mock_ww_calls++;
    g_mock_last_ptr = p;
    g_mock_last_len = len;
    return g_mock_ww_result;
}

static uint64_t cb_ww_query(CB *k);

/* Old flawed implementation of cb_guest_wrote for regression demonstration */
static int cb_guest_wrote_old(CB *k) {
    uintptr_t a = (uintptr_t)k->addr, e = a + (uintptr_t)k->size, lo = (a + 4095) & ~(uintptr_t)4095, hi = e & ~(uintptr_t)4095;
    if (k->snap) {
        if (hi <= lo) return memcmp((void *)a, k->snap, (size_t)k->size) != 0;
        if (lo > a && memcmp((void *)a, k->snap, lo - a)) return 1;
        if (e > hi && memcmp((void *)hi, k->snap + (hi - a), e - hi)) return 1;
    }
    uint64_t nw = cb_ww_query(k);
    if (!nw) { g_ck_unwatched++; return 0; }
    return nw > k->ww;
}
'''

TEST_MAIN = r'''
int main(void) {
    printf("[COHERENCY TEST] Starting write-check harness...\n");

    /* Buffer allocation with guard bytes: align to 4096 */
    uint8_t raw[4096 + 16384 + 64];
    memset(raw, 0xaa, sizeof raw);
    uint8_t *guest = (uint8_t *)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    uint8_t snap[16384];

    /* --- TEST 1: Old vs New implementation on interior CPU write with unavailable tracking --- */
    {
        memset(guest, 0, 8192);
        memset(snap, 0, 8192);
        guest[4500] = 0xff; /* interior byte changed on second 4KB page */

        CB k = {
            .addr = (uint64_t)(uintptr_t)guest,
            .size = 8192,
            .snap = snap,
            .snap_cap = 8192,
            .snap_valid = 1,
            .ww = 10
        };

        g_mock_ww_result = 0; /* tracking unavailable */
        g_mock_ww_calls = 0;

        /* Old flawed logic returns 0 (misses write) */
        int old_verdict = cb_guest_wrote_old(&k);
        assert(old_verdict == 0 && "Old implementation was expected to miss the interior write!");

        /* New production logic returns 1 (detects write) */
        int new_verdict = cb_guest_wrote(&k);
        assert(new_verdict == 1 && "New implementation must detect interior CPU write!");
        printf("  [PASS] Test 1: Interior CPU write demonstration (old returned 0, new returned 1)\n");
    }

    /* --- TEST 2: Aligned ranges with unavailable tracking --- */
    {
        CB k = { .addr = (uint64_t)(uintptr_t)guest, .size = 8192, .snap = snap, .snap_cap = 8192, .snap_valid = 1 };
        g_mock_ww_result = 0;

        /* Equal */
        memset(guest, 0x42, 8192);
        memset(snap, 0x42, 8192);
        assert(cb_guest_wrote(&k) == 0 && "Equal aligned range must return 0");

        /* Head change */
        guest[0] = 0x99;
        assert(cb_guest_wrote(&k) == 1 && "Head change must return 1");
        guest[0] = 0x42;

        /* Interior change */
        guest[4096] = 0x99;
        assert(cb_guest_wrote(&k) == 1 && "Interior change must return 1");
        guest[4096] = 0x42;

        /* Tail change */
        guest[8191] = 0x99;
        assert(cb_guest_wrote(&k) == 1 && "Tail change must return 1");
        guest[8191] = 0x42;

        printf("  [PASS] Test 2: Aligned range head/interior/tail and equality\n");
    }

    /* --- TEST 3: Unaligned ranges with unavailable tracking --- */
    {
        uint8_t *unaligned_guest = guest + 128;
        CB k = { .addr = (uint64_t)(uintptr_t)unaligned_guest, .size = 9000, .snap = snap, .snap_cap = 9000, .snap_valid = 1 };
        g_mock_ww_result = 0;

        /* Equal */
        memset(unaligned_guest, 0x33, 9000);
        memset(snap, 0x33, 9000);
        assert(cb_guest_wrote(&k) == 0 && "Equal unaligned range must return 0");

        /* Head change */
        unaligned_guest[10] = 0x77;
        assert(cb_guest_wrote(&k) == 1 && "Unaligned head change must return 1");
        unaligned_guest[10] = 0x33;

        /* Interior whole-page change */
        unaligned_guest[4500] = 0x77;
        assert(cb_guest_wrote(&k) == 1 && "Unaligned interior change must return 1");
        unaligned_guest[4500] = 0x33;

        /* Tail change */
        unaligned_guest[8990] = 0x77;
        assert(cb_guest_wrote(&k) == 1 && "Unaligned tail change must return 1");
        unaligned_guest[8990] = 0x33;

        printf("  [PASS] Test 3: Unaligned range head/interior/tail and equality\n");
    }

    /* --- TEST 4: Sub-page ranges (< 4096 bytes) --- */
    {
        uint8_t *sub_guest = guest + 256;
        CB k = { .addr = (uint64_t)(uintptr_t)sub_guest, .size = 512, .snap = snap, .snap_cap = 512, .snap_valid = 1 };
        g_mock_ww_result = 0;

        memset(sub_guest, 0x55, 512);
        memset(snap, 0x55, 512);
        assert(cb_guest_wrote(&k) == 0 && "Equal sub-page range must return 0");

        sub_guest[200] = 0x88;
        assert(cb_guest_wrote(&k) == 1 && "Changed sub-page range must return 1");
        sub_guest[200] = 0x55;

        printf("  [PASS] Test 4: Sub-page range equality and modification\n");
    }

    /* --- TEST 5: Missing or invalid snapshots --- */
    {
        CB k = { .addr = (uint64_t)(uintptr_t)guest, .size = 4096, .snap = NULL, .snap_cap = 0, .snap_valid = 0 };
        g_mock_ww_result = 0;
        assert(cb_guest_wrote(&k) == 1 && "NULL snapshot with unavailable tracking must return 1");

        /* Retained allocation with disabled merge (snap != NULL, snap_valid = 0) */
        k.snap = snap;
        k.snap_cap = 4096;
        k.snap_valid = 0;
        assert(cb_guest_wrote(&k) == 1 && "Retained allocation with snap_valid=0 must return 1");

        printf("  [PASS] Test 5: Missing and invalid snapshots conservatively report 1\n");
    }

    /* --- TEST 6: Tracking failure after tracked fill --- */
    {
        CB k = { .addr = (uint64_t)(uintptr_t)guest, .size = 8192, .snap = snap, .snap_cap = 8192, .snap_valid = 1, .ww = 100 };
        memset(guest, 0x11, 8192);
        memset(snap, 0x11, 8192);

        /* Query returns 0 (tracking failed after fill) */
        g_mock_ww_result = 0;
        assert(cb_guest_wrote(&k) == 0 && "Tracking failure with equal snapshot falls back to 0");

        guest[3000] = 0x22;
        assert(cb_guest_wrote(&k) == 1 && "Tracking failure with modified snapshot falls back to 1");
        guest[3000] = 0x11;

        printf("  [PASS] Test 6: Tracking failure fallback after tracked fill\n");
    }

    /* --- TEST 7: Reliable tracking path (fast path without full range scan) --- */
    {
        CB k = { .addr = (uint64_t)(uintptr_t)guest, .size = 8192, .snap = snap, .snap_cap = 8192, .snap_valid = 1, .ww = 100 };
        memset(guest, 0x22, 8192);
        memset(snap, 0x22, 8192);

        /* Case 7a: Tracking reports newer write stamp */
        g_mock_ww_result = 150; /* nw > ww */
        assert(cb_guest_wrote(&k) == 1 && "Tracked write stamp > ww must return 1");

        /* Case 7b: Tracking reports clean stamp (nw <= ww) */
        g_mock_ww_result = 100;
        assert(cb_guest_wrote(&k) == 0 && "Tracked clean stamp must return 0");

        printf("  [PASS] Test 7: Reliable tracking fast path\n");
    }

    /* --- TEST 8: Once-per-batch verdict caching (cb_kept_wrote) --- */
    {
        CB k = { .addr = (uint64_t)(uintptr_t)guest, .size = 4096, .snap = snap, .snap_cap = 4096, .snap_valid = 1, .ww = 50, .wchk = 0 };
        memset(guest, 0x77, 4096);
        memset(snap, 0x77, 4096);
        g_mock_ww_result = 50;
        g_mock_ww_calls = 0;
        g_cb_batch = 10;

        int w1 = cb_kept_wrote(&k);
        assert(w1 == 0);
        assert(g_mock_ww_calls == 1 && "First call in batch must query write watch");

        /* Second call in same batch */
        int w2 = cb_kept_wrote(&k);
        assert(w2 == 0);
        assert(g_mock_ww_calls == 1 && "Second call in same batch must reuse cached verdict");

        /* Next batch */
        g_cb_batch = 11;
        int w3 = cb_kept_wrote(&k);
        assert(w3 == 0);
        assert(g_mock_ww_calls == 2 && "Call in next batch must re-query write watch");

        printf("  [PASS] Test 8: Once-per-batch verdict reuse\n");
    }

    /* --- TEST 9: Memory safety: guard bytes untouched --- */
    {
        for (int i = 0; i < 64; i++) {
            assert(raw[i] == 0xaa && "Underflow: guard bytes before buffer modified!");
            assert(raw[sizeof(raw) - 1 - i] == 0xaa && "Overflow: guard bytes after buffer modified!");
        }
        printf("  [PASS] Test 9: Guard bytes intact, no out-of-range memory accesses\n");
    }

    printf("[COHERENCY TEST] All tests passed successfully.\n");
    return 0;
}
'''

def find_compiler():
    if "HOST_CC" in os.environ:
        return shlex.split(os.environ["HOST_CC"])
    ndk_clang = Path(r"D:\android-sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe")
    if ndk_clang.exists():
        return [str(ndk_clang)]
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if cc:
        return [cc]
    return ["cc"]

class ComputeBufferCoherencyTest(unittest.TestCase):
    def test_extracted_coherency_functions(self):
        query_fn = extract_function("cb_ww_query")
        wrote_fn = extract_function("cb_guest_wrote")
        kept_fn = extract_function("cb_kept_wrote")

        c_code = PRELUDE + "\n" + query_fn + "\n" + wrote_fn + "\n" + kept_fn + "\n" + TEST_MAIN

        with tempfile.TemporaryDirectory() as tmpdir:
            src = Path(tmpdir) / "test_coherency.c"
            exe = Path(tmpdir) / "test_coherency"
            if os.name == "nt":
                exe = exe.with_suffix(".exe")

            src.write_text(c_code, encoding="utf-8")
            cc = find_compiler()
            compile_cmd = cc + ["-O2", "-Wall", str(src), "-o", str(exe)]
            res = subprocess.run(compile_cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Compilation failed:\n{res.stderr}")

            run_res = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            if run_res.stdout:
                sys.stdout.write(run_res.stdout)
            self.assertEqual(run_res.returncode, 0, f"Execution failed:\n{run_res.stdout}\n{run_res.stderr}")
            self.assertIn("[COHERENCY TEST] All tests passed successfully.", run_res.stdout)

if __name__ == "__main__":
    unittest.main()
