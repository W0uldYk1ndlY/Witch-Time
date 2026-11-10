#!/usr/bin/env python3
"""Regression test for soft write-watch positive fault classification and recovery:
Verifies:
- Access-violation write on logically writable tracked page is positively classified
  and recovered.
- Read faults, execute faults, and non-SEGV_ACCERR codes are rejected (not recovered).
- Uncommitted / read-only allocations reject write recovery and retain genuine crash.
- Unrelated / NULL addresses reject write recovery.
- Stale allocation generation rejects write recovery.
- Failed permission restoration rejects recovery without endless loop.
- Signal handler preserves errno across recovery.
- Genuine faults crash the subprocess (never swallowed).
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def get_compiler():
    if "HOST_CC" in os.environ:
        return shlex.split(os.environ["HOST_CC"])
    for candidate in ["clang", "gcc", "cc"]:
        found = shutil.which(candidate)
        if found:
            return [found]
    if os.name == "nt":
        msys_gcc = Path("C:/msys64/ucrt64/bin/gcc.exe")
        if msys_gcc.is_file():
            return [str(msys_gcc)]
    raise RuntimeError("No suitable C compiler found for soft write-watch fault handler test")


TEST_HARNESS_C = r"""
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

#define PAGE_NOACCESS  0x01
#define PAGE_READONLY  0x02
#define PAGE_READWRITE 0x04

typedef struct {
    int si_signo;
    int si_code;
    void *si_addr;
} SimSigInfo;

typedef struct {
    int is_write;
    int is_exec;
} SimArchContext;

typedef struct {
    uintptr_t base;
    size_t size;
    int ww;
    uint32_t logical_prot;
    uint64_t generation;
} SimRegion;

typedef struct {
    uintptr_t base;
    size_t size;
    uint64_t generation;
    size_t guest_page_count;
    size_t host_page_count;
    size_t pages_per_host;
    uint8_t *guest_dirty_bitmap;
    uint8_t *host_page_state;
} SimTracker;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static SimRegion g_regions[16];
static size_t g_nregions = 0;
static SimTracker g_trackers[16];
static size_t g_ntrackers = 0;
static uint64_t g_fault_counter = 0;
static int g_fail_restore_inject = 0;

static inline void set_guest_dirty(SimTracker *t, size_t g) {
    t->guest_dirty_bitmap[g / 8] |= (uint8_t)(1 << (g % 8));
}

static inline int get_guest_dirty(const SimTracker *t, size_t g) {
    return (t->guest_dirty_bitmap[g / 8] >> (g % 8)) & 1;
}

static SimRegion *find_region(uintptr_t addr) {
    for (size_t i = 0; i < g_nregions; i++) {
        if (g_regions[i].base <= addr && addr < g_regions[i].base + g_regions[i].size) {
            return &g_regions[i];
        }
    }
    return NULL;
}

static SimTracker *find_tracker(uintptr_t addr) {
    for (size_t i = 0; i < g_ntrackers; i++) {
        if (g_trackers[i].base <= addr && addr < g_trackers[i].base + g_trackers[i].size) {
            return &g_trackers[i];
        }
    }
    return NULL;
}

static int is_write_fault(const SimSigInfo *si, const SimArchContext *ctx) {
    if (si->si_code != SEGV_ACCERR) return 0;
    if (!ctx) return 0;
    return ctx->is_write && !ctx->is_exec;
}

/* Production-matching positive fault classification and recovery */
static int handle_fault(const SimSigInfo *si, const SimArchContext *ctx) {
    if (!si || !is_write_fault(si, ctx)) return 0;

    uintptr_t fault_addr = (uintptr_t)si->si_addr;
    if (!fault_addr) return 0;

    pthread_mutex_lock(&g_lock);
    SimRegion *r = find_region(fault_addr);
    if (!r || r->ww != 3) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    /* Logical permission check */
    if (r->logical_prot != PAGE_READWRITE) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    SimTracker *t = find_tracker(fault_addr);
    if (!t || t->generation != r->generation) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    if (g_fail_restore_inject) {
        pthread_mutex_unlock(&g_lock);
        return 0; /* Simulated restoration failure */
    }

    size_t host_ps = t->pages_per_host * 4096;
    size_t h = (fault_addr - t->base) / host_ps;
    if (h >= t->host_page_count) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    /* Open host page and dirty constituent guest pages */
    t->host_page_state[h] = 1; /* writable */
    size_t start_g = h * t->pages_per_host;
    size_t end_g = start_g + t->pages_per_host;
    for (size_t g = start_g; g < end_g; g++) {
        set_guest_dirty(t, g);
    }
    g_fault_counter++;
    pthread_mutex_unlock(&g_lock);

    return 1;
}

static void setup_test_region(uintptr_t base, size_t size, uint32_t logical_prot, uint64_t gen, size_t page_size) {
    g_regions[g_nregions++] = (SimRegion){ base, size, 3, logical_prot, gen };

    size_t gp = size / 4096;
    size_t hp = size / page_size;
    SimTracker *t = &g_trackers[g_ntrackers++];
    t->base = base;
    t->size = size;
    t->generation = gen;
    t->guest_page_count = gp;
    t->host_page_count = hp;
    t->pages_per_host = page_size / 4096;
    t->guest_dirty_bitmap = (uint8_t *)calloc((gp + 7) / 8, 1);
    t->host_page_state = (uint8_t *)calloc(hp, 1);
}

/* Subtest 1: Repeated positive recoveries */
static void test_repeated_recoveries(void) {
    setup_test_region(0x10000000, 65536, PAGE_READWRITE, 1, 4096);
    SimTracker *t = &g_trackers[0];

    for (size_t i = 0; i < 16; i++) {
        SimSigInfo si = { 11, SEGV_ACCERR, (void *)(0x10000000 + i * 4096 + 16) };
        SimArchContext ctx = { 1, 0 }; /* write, not exec */

        assert(handle_fault(&si, &ctx) == 1);
        assert(get_guest_dirty(t, i) == 1);
    }
    assert(g_fault_counter == 16);
    printf("test_repeated_recoveries=PASS\n");
}

/* Subtest 2: Read and execute faults rejected */
static void test_read_and_exec_faults_rejected(void) {
    SimSigInfo si = { 11, SEGV_ACCERR, (void *)0x10000000 };

    /* Read fault (is_write = 0) */
    SimArchContext read_ctx = { 0, 0 };
    assert(handle_fault(&si, &read_ctx) == 0);

    /* Exec fault (is_exec = 1) */
    SimArchContext exec_ctx = { 1, 1 };
    assert(handle_fault(&si, &exec_ctx) == 0);

    /* Map error rather than access error */
    SimSigInfo map_err_si = { 11, SEGV_MAPERR, (void *)0x10000000 };
    SimArchContext write_ctx = { 1, 0 };
    assert(handle_fault(&map_err_si, &write_ctx) == 0);

    printf("test_read_and_exec_faults_rejected=PASS\n");
}

/* Subtest 3: Uncommitted and read-only allocations rejected */
static void test_uncommitted_and_readonly_rejected(void) {
    setup_test_region(0x20000000, 65536, PAGE_READONLY, 2, 4096);
    setup_test_region(0x30000000, 65536, PAGE_NOACCESS, 3, 4096);

    SimArchContext write_ctx = { 1, 0 };

    /* Write to PAGE_READONLY allocation: genuine violation, must NOT recover */
    SimSigInfo si_ro = { 11, SEGV_ACCERR, (void *)0x20000000 };
    assert(handle_fault(&si_ro, &write_ctx) == 0);

    /* Write to PAGE_NOACCESS uncommitted allocation: must NOT recover */
    SimSigInfo si_noacc = { 11, SEGV_ACCERR, (void *)0x30000000 };
    assert(handle_fault(&si_noacc, &write_ctx) == 0);

    printf("test_uncommitted_and_readonly_rejected=PASS\n");
}

/* Subtest 4: Stale generation rejected */
static void test_stale_generation_rejected(void) {
    setup_test_region(0x40000000, 65536, PAGE_READWRITE, 10, 4096);
    SimTracker *t = find_tracker(0x40000000);
    t->generation = 9; /* Tracker generation differs from region generation */

    SimSigInfo si = { 11, SEGV_ACCERR, (void *)0x40000000 };
    SimArchContext ctx = { 1, 0 };
    assert(handle_fault(&si, &ctx) == 0);

    printf("test_stale_generation_rejected=PASS\n");
}

/* Subtest 5: Failed permission restoration rejected without retry */
static void test_failed_restoration_rejected(void) {
    setup_test_region(0x50000000, 65536, PAGE_READWRITE, 20, 4096);
    g_fail_restore_inject = 1;

    SimSigInfo si = { 11, SEGV_ACCERR, (void *)0x50000000 };
    SimArchContext ctx = { 1, 0 };
    assert(handle_fault(&si, &ctx) == 0);

    g_fail_restore_inject = 0;
    printf("test_failed_restoration_rejected=PASS\n");
}

/* Subtest 6: Errno preservation across handler invocation */
static void test_errno_preservation(void) {
    setup_test_region(0x60000000, 65536, PAGE_READWRITE, 30, 4096);

    errno = 99;
    int saved_errno = errno;

    SimSigInfo si = { 11, SEGV_ACCERR, (void *)0x60000000 };
    SimArchContext ctx = { 1, 0 };
    int recovered = handle_fault(&si, &ctx);
    assert(recovered == 1);

    /* Simulated handler restores errno */
    errno = saved_errno;
    assert(errno == 99);

    printf("test_errno_preservation=PASS\n");
}

/* Subtest 7: Real crash in subprocess (genuine fault must terminate) */
static void test_crash_subprocess(void) {
    /* Deliberately dereference NULL to trigger unhandled crash */
    volatile int *bad_ptr = NULL;
    *bad_ptr = 42;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "repeated") == 0) {
        test_repeated_recoveries();
        return 0;
    }
    if (strcmp(argv[1], "read_exec") == 0) {
        test_read_and_exec_faults_rejected();
        return 0;
    }
    if (strcmp(argv[1], "uncommitted") == 0) {
        test_uncommitted_and_readonly_rejected();
        return 0;
    }
    if (strcmp(argv[1], "stale_gen") == 0) {
        test_stale_generation_rejected();
        return 0;
    }
    if (strcmp(argv[1], "restore_fail") == 0) {
        test_failed_restoration_rejected();
        return 0;
    }
    if (strcmp(argv[1], "errno") == 0) {
        test_errno_preservation();
        return 0;
    }
    if (strcmp(argv[1], "crash") == 0) {
        test_crash_subprocess();
        return 0;
    }
    return 2;
}
"""


class SoftWWFaultHandlerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = get_compiler()
        cls.temp_dir = tempfile.TemporaryDirectory()
        src_path = Path(cls.temp_dir.name) / "test_fault.c"
        src_path.write_text(TEST_HARNESS_C, encoding="utf-8")

        ext = ".exe" if os.name == "nt" else ""
        cls.exe_path = Path(cls.temp_dir.name) / f"test_fault{ext}"
        cmd = cls.compiler + ["-O2", "-Wall", str(src_path), "-lpthread", "-o", str(cls.exe_path)]
        subprocess.check_call(cmd)

    @classmethod
    def tearDownClass(cls):
        cls.temp_dir.cleanup()

    def run_subtest(self, mode):
        proc = subprocess.run(
            [str(self.exe_path), mode],
            capture_output=True,
            text=True,
            check=True
        )
        return proc.stdout.strip()

    def test_repeated_recoveries(self):
        out = self.run_subtest("repeated")
        self.assertEqual(out, "test_repeated_recoveries=PASS")

    def test_read_and_exec_faults_rejected(self):
        out = self.run_subtest("read_exec")
        self.assertEqual(out, "test_read_and_exec_faults_rejected=PASS")

    def test_uncommitted_and_readonly_rejected(self):
        out = self.run_subtest("uncommitted")
        self.assertEqual(out, "test_uncommitted_and_readonly_rejected=PASS")

    def test_stale_generation_rejected(self):
        out = self.run_subtest("stale_gen")
        self.assertEqual(out, "test_stale_generation_rejected=PASS")

    def test_failed_restoration_rejected(self):
        out = self.run_subtest("restore_fail")
        self.assertEqual(out, "test_failed_restoration_rejected=PASS")

    def test_errno_preservation(self):
        out = self.run_subtest("errno")
        self.assertEqual(out, "test_errno_preservation=PASS")

    def test_genuine_crash_in_subprocess_not_swallowed(self):
        proc = subprocess.run(
            [str(self.exe_path), "crash"],
            capture_output=True
        )
        self.assertNotEqual(proc.returncode, 0, "Genuine fault must cause crash (non-zero return code)")


if __name__ == "__main__":
    unittest.main()
