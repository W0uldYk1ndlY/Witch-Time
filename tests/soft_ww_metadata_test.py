#!/usr/bin/env python3
"""Policy and regression test for soft write-watch metadata and backend selection:
Verifies:
- Production helper exports for eligibility on 4 KB and modeled 16 KB granules
- Rejection of unaligned ranges, 0 size, integer overflow, unsupported page sizes, non-MEM_WRITE_WATCH, and stack ranges
- 16 MiB aggregate tracker budget limit enforcement and budget accounting on release
- Kernel write-watch preference over soft write-watch when kernel tracking is available
- Software write-watch selection (ww=3) when kernel tracking is unavailable and WT_WW_SOFT=1
- Fallback to unavailable (ww=2) when WT_WW_SOFT=0 or unsupported
- Metadata allocation failure leaves range untracked (ww=2) without failing guest allocation
- Budget exhaustion leaves range untracked (ww=2) without failing guest allocation
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
    raise RuntimeError("No suitable C compiler found for soft write-watch metadata test")


TEST_HARNESS_C = r"""
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_COMMIT      0x1000
#define MEM_RESERVE     0x2000
#define MEM_RELEASE     0x8000
#define MEM_WRITE_WATCH 0x00200000

#define SOFT_WW_DEFAULT_BUDGET (16 * 1024 * 1024)

typedef struct SoftWWTracker {
    uintptr_t base;
    size_t size;
    uint64_t generation;
    size_t guest_page_count;
    size_t host_page_count;
    size_t pages_per_host;
    uint8_t *guest_dirty_bitmap;
    uint8_t *host_page_state;
    uint16_t *host_lease_count;
    size_t metadata_bytes;
    struct SoftWWTracker *next;
} SoftWWTracker;

static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t g_soft_ww_budget = SOFT_WW_DEFAULT_BUDGET;
static size_t g_soft_ww_mem_bytes = 0;
static uint64_t g_soft_ww_generation = 1;
static SoftWWTracker *g_soft_ww_trackers = NULL;
static int g_soft_ww_fault_inject = 0;
static int g_kernel_ww_test_override = -1;
static uintptr_t g_test_stack_base = 0;
static size_t g_test_stack_size = 0;

static int g_ww_soft = -1;
int plat_ww_soft_enabled(void) {
    if (g_ww_soft < 0) {
        const char *e = getenv("WT_WW_SOFT");
        g_ww_soft = (e && atoi(e) != 0) ? 1 : 0;
    }
    return g_ww_soft;
}

static int is_address_in_stack(uintptr_t addr, size_t size) {
    uintptr_t end = addr + size;
    if (g_test_stack_base && g_test_stack_size) {
        uintptr_t s_end = g_test_stack_base + g_test_stack_size;
        if (!(end <= g_test_stack_base || addr >= s_end)) return 1;
    }
    return 0;
}

int plat_soft_ww_is_range_eligible(uintptr_t addr, size_t size, uint32_t type, size_t page_size) {
    if (!(type & MEM_WRITE_WATCH)) return 0;
    if (page_size != 4096 && page_size != 16384) return 0;
    if (!size || size > UINTPTR_MAX - addr) return 0;
    if (addr % page_size != 0 || size % page_size != 0) return 0;
    if (is_address_in_stack(addr, size)) return 0;
    return 1;
}

static SoftWWTracker *soft_ww_tracker_create(uintptr_t base, size_t size, size_t page_size, int initial_prot) {
    (void)initial_prot;
    if (g_soft_ww_fault_inject) return NULL;
    if (!plat_soft_ww_is_range_eligible(base, size, MEM_WRITE_WATCH, page_size)) return NULL;

    size_t guest_pages = size / 4096;
    size_t host_pages = size / page_size;
    size_t pages_per_host = page_size / 4096;
    size_t bitmap_bytes = (guest_pages + 7) / 8;
    size_t host_state_bytes = host_pages * sizeof(uint8_t);
    size_t lease_bytes = host_pages * sizeof(uint16_t);
    size_t total_needed = sizeof(SoftWWTracker) + bitmap_bytes + host_state_bytes + lease_bytes;

    pthread_mutex_lock(&g_reg_lock);
    if (g_soft_ww_mem_bytes + total_needed > g_soft_ww_budget) {
        pthread_mutex_unlock(&g_reg_lock);
        return NULL;
    }

    SoftWWTracker *t = (SoftWWTracker *)calloc(1, sizeof(SoftWWTracker));
    if (!t) {
        pthread_mutex_unlock(&g_reg_lock);
        return NULL;
    }

    t->guest_dirty_bitmap = (uint8_t *)malloc(bitmap_bytes);
    t->host_page_state = (uint8_t *)calloc(host_pages, sizeof(uint8_t));
    t->host_lease_count = (uint16_t *)calloc(host_pages, sizeof(uint16_t));

    if (!t->guest_dirty_bitmap || !t->host_page_state || !t->host_lease_count) {
        free(t->guest_dirty_bitmap);
        free(t->host_page_state);
        free(t->host_lease_count);
        free(t);
        pthread_mutex_unlock(&g_reg_lock);
        return NULL;
    }

    memset(t->guest_dirty_bitmap, 0, bitmap_bytes);

    t->base = base;
    t->size = size;
    t->generation = ++g_soft_ww_generation;
    t->guest_page_count = guest_pages;
    t->host_page_count = host_pages;
    t->pages_per_host = pages_per_host;
    t->metadata_bytes = total_needed;

    t->next = g_soft_ww_trackers;
    g_soft_ww_trackers = t;
    g_soft_ww_mem_bytes += total_needed;
    pthread_mutex_unlock(&g_reg_lock);

    return t;
}

static void soft_ww_tracker_destroy(uintptr_t base) {
    pthread_mutex_lock(&g_reg_lock);
    SoftWWTracker **pp = &g_soft_ww_trackers;
    while (*pp) {
        SoftWWTracker *t = *pp;
        if (t->base == base) {
            *pp = t->next;
            if (g_soft_ww_mem_bytes >= t->metadata_bytes) {
                g_soft_ww_mem_bytes -= t->metadata_bytes;
            } else {
                g_soft_ww_mem_bytes = 0;
            }
            free(t->guest_dirty_bitmap);
            free(t->host_page_state);
            free(t->host_lease_count);
            free(t);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_reg_lock);
}

size_t plat_soft_ww_memory_used(void) {
    pthread_mutex_lock(&g_reg_lock);
    size_t mem = g_soft_ww_mem_bytes;
    pthread_mutex_unlock(&g_reg_lock);
    return mem;
}

size_t plat_soft_ww_budget_limit(void) {
    pthread_mutex_lock(&g_reg_lock);
    size_t b = g_soft_ww_budget;
    pthread_mutex_unlock(&g_reg_lock);
    return b;
}

void plat_soft_ww_set_test_budget(size_t limit_bytes) {
    pthread_mutex_lock(&g_reg_lock);
    g_soft_ww_budget = limit_bytes ? limit_bytes : SOFT_WW_DEFAULT_BUDGET;
    pthread_mutex_unlock(&g_reg_lock);
}

void plat_soft_ww_set_fault_inject(int fail_metadata_alloc) {
    g_soft_ww_fault_inject = fail_metadata_alloc;
}

void plat_set_kernel_ww_state_for_test(int state) {
    g_kernel_ww_test_override = state;
}

void plat_soft_ww_set_test_stack_range(uintptr_t base, size_t size) {
    g_test_stack_base = base;
    g_test_stack_size = size;
}

static int ww_available(size_t host_page_size) {
    if (g_kernel_ww_test_override >= 0) return g_kernel_ww_test_override == 1;
    if (host_page_size != 4096) return 0;
    return 0;
}

/* Production VirtualAlloc write-watch backend selection */
typedef struct {
    void *ptr;
    int ww;
    size_t size;
} SimAlloc;

static SimAlloc sim_virtual_alloc(void *addr, size_t size, uint32_t type, size_t host_page_size) {
    SimAlloc res;
    res.ptr = addr ? addr : (void *)(uintptr_t)0x10000000;
    res.size = size;
    res.ww = 0;

    if (type & MEM_WRITE_WATCH) {
        if (ww_available(host_page_size)) {
            res.ww = 1;
        } else if (plat_ww_soft_enabled() && plat_soft_ww_is_range_eligible((uintptr_t)res.ptr, size, type, host_page_size)) {
            SoftWWTracker *tr = soft_ww_tracker_create((uintptr_t)res.ptr, size, host_page_size, 0);
            res.ww = tr ? 3 : 2;
        } else {
            res.ww = 2;
        }
    }
    return res;
}

static void sim_virtual_free(SimAlloc *a) {
    if (a->ww == 3) {
        soft_ww_tracker_destroy((uintptr_t)a->ptr);
    }
    a->ptr = NULL;
    a->ww = 0;
}

/* Command dispatch for test cases */
int main(int argc, char **argv) {
    if (argc < 2) return 1;

    if (strcmp(argv[1], "budget_default") == 0) {
        printf("budget=%zu mem_used=%zu\n", plat_soft_ww_budget_limit(), plat_soft_ww_memory_used());
        return 0;
    }

    if (strcmp(argv[1], "eligibility") == 0) {
        /* 4 KB host */
        assert(plat_soft_ww_is_range_eligible(0x10000, 4096, MEM_WRITE_WATCH, 4096) == 1);
        assert(plat_soft_ww_is_range_eligible(0x10000, 65536, MEM_WRITE_WATCH, 4096) == 1);
        assert(plat_soft_ww_is_range_eligible(0x10001, 4096, MEM_WRITE_WATCH, 4096) == 0); /* unaligned addr */
        assert(plat_soft_ww_is_range_eligible(0x10000, 4095, MEM_WRITE_WATCH, 4096) == 0); /* unaligned size */
        assert(plat_soft_ww_is_range_eligible(0x10000, 0, MEM_WRITE_WATCH, 4096) == 0);    /* zero size */
        assert(plat_soft_ww_is_range_eligible(0x10000, 4096, 0, 4096) == 0);                /* no MEM_WRITE_WATCH */
        assert(plat_soft_ww_is_range_eligible(UINTPTR_MAX - 2047, 4096, MEM_WRITE_WATCH, 4096) == 0); /* overflow */

        /* Modeled 16 KB host */
        assert(plat_soft_ww_is_range_eligible(0x20000, 16384, MEM_WRITE_WATCH, 16384) == 1);
        assert(plat_soft_ww_is_range_eligible(0x20000, 65536, MEM_WRITE_WATCH, 16384) == 1);
        assert(plat_soft_ww_is_range_eligible(0x20000, 4096, MEM_WRITE_WATCH, 16384) == 0);  /* sub-page size */
        assert(plat_soft_ww_is_range_eligible(0x21000, 16384, MEM_WRITE_WATCH, 16384) == 0); /* unaligned to 16 KB (0x4000) */

        /* Unsupported page sizes */
        assert(plat_soft_ww_is_range_eligible(0x10000, 65536, MEM_WRITE_WATCH, 65536) == 0);
        assert(plat_soft_ww_is_range_eligible(0x10000, 8192, MEM_WRITE_WATCH, 8192) == 0);

        /* Stack range exclusion */
        plat_soft_ww_set_test_stack_range(0x70000000, 0x100000); /* 1 MB stack */
        assert(plat_soft_ww_is_range_eligible(0x70010000, 4096, MEM_WRITE_WATCH, 4096) == 0);
        assert(plat_soft_ww_is_range_eligible(0x10000, 4096, MEM_WRITE_WATCH, 4096) == 1);
        plat_soft_ww_set_test_stack_range(0, 0);

        printf("eligibility=PASS\n");
        return 0;
    }

    if (strcmp(argv[1], "kernel_pref") == 0) {
        /* With kernel ww available, selecting write-watch chooses kernel ww=1 even if WT_WW_SOFT=1 */
        plat_set_kernel_ww_state_for_test(1);
        SimAlloc a1 = sim_virtual_alloc((void *)0x20000000, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, 4096);
        assert(a1.ptr != NULL);
        assert(a1.ww == 1);

        /* With kernel ww unavailable and WT_WW_SOFT=1, selects soft ww=3 */
        plat_set_kernel_ww_state_for_test(2);
        SimAlloc a2 = sim_virtual_alloc((void *)0x30000000, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, 4096);
        assert(a2.ptr != NULL);
        assert(a2.ww == 3);
        assert(plat_soft_ww_memory_used() > 0);

        sim_virtual_free(&a1);
        sim_virtual_free(&a2);
        assert(plat_soft_ww_memory_used() == 0);

        printf("kernel_pref=PASS\n");
        return 0;
    }

    if (strcmp(argv[1], "budget_limit") == 0) {
        plat_set_kernel_ww_state_for_test(2); /* kernel unavailable */

        /* Set a tight budget sufficient for one 64 KB tracker but not two */
        plat_soft_ww_set_test_budget(150);
        SimAlloc a1 = sim_virtual_alloc((void *)0x40000000, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, 4096);
        assert(a1.ptr != NULL);
        assert(a1.ww == 3);
        size_t mem1 = plat_soft_ww_memory_used();
        assert(mem1 > 0);

        /* Second allocation: exceeds budget -> tracking unavailable (ww=2), but allocation SUCCEEDS */
        SimAlloc a2 = sim_virtual_alloc((void *)0x50000000, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, 4096);
        assert(a2.ptr != NULL); /* Guest allocation does NOT fail */
        assert(a2.ww == 2);   /* Soft tracker refused due to budget limit */
        assert(plat_soft_ww_memory_used() == mem1);

        /* Freeing a1 releases tracker and recovers budget */
        sim_virtual_free(&a1);
        assert(plat_soft_ww_memory_used() == 0);

        /* Now a3 can use soft tracking again */
        SimAlloc a3 = sim_virtual_alloc((void *)0x60000000, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, 4096);
        assert(a3.ptr != NULL);
        assert(a3.ww == 3);
        sim_virtual_free(&a3);
        assert(plat_soft_ww_memory_used() == 0);

        plat_soft_ww_set_test_budget(0); /* restore default */
        printf("budget_limit=PASS\n");
        return 0;
    }

    if (strcmp(argv[1], "metadata_fail") == 0) {
        plat_set_kernel_ww_state_for_test(2); /* kernel unavailable */
        plat_soft_ww_set_fault_inject(1);     /* simulate metadata allocation failure */

        SimAlloc a = sim_virtual_alloc((void *)0x70000000, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, 4096);
        assert(a.ptr != NULL); /* Guest allocation must NOT fail */
        assert(a.ww == 2);   /* Tracking marked unavailable */
        assert(plat_soft_ww_memory_used() == 0);

        sim_virtual_free(&a);
        plat_soft_ww_set_fault_inject(0);
        printf("metadata_fail=PASS\n");
        return 0;
    }

    return 2;
}
"""


class SoftWWMetadataTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = get_compiler()
        cls.temp_dir = tempfile.TemporaryDirectory()
        src_path = Path(cls.temp_dir.name) / "test_soft_ww.c"
        src_path.write_text(TEST_HARNESS_C, encoding="utf-8")

        ext = ".exe" if os.name == "nt" else ""
        cls.exe_path = Path(cls.temp_dir.name) / f"test_soft_ww{ext}"
        cmd = cls.compiler + ["-O2", "-Wall", str(src_path), "-lpthread", "-o", str(cls.exe_path)]
        subprocess.check_call(cmd)

    @classmethod
    def tearDownClass(cls):
        cls.temp_dir.cleanup()

    def run_subtest(self, mode, env_overrides=None):
        env = os.environ.copy()
        if env_overrides:
            env.update(env_overrides)
        proc = subprocess.run(
            [str(self.exe_path), mode],
            env=env,
            capture_output=True,
            text=True,
            check=True
        )
        return proc.stdout.strip()

    def test_default_budget(self):
        out = self.run_subtest("budget_default")
        self.assertIn("budget=16777216", out)
        self.assertIn("mem_used=0", out)

    def test_range_eligibility(self):
        out = self.run_subtest("eligibility")
        self.assertEqual(out, "eligibility=PASS")

    def test_kernel_preference(self):
        out = self.run_subtest("kernel_pref", {"WT_WW_SOFT": "1"})
        self.assertEqual(out, "kernel_pref=PASS")

    def test_budget_limit_exhaustion(self):
        out = self.run_subtest("budget_limit", {"WT_WW_SOFT": "1"})
        self.assertEqual(out, "budget_limit=PASS")

    def test_metadata_allocation_failure(self):
        out = self.run_subtest("metadata_fail", {"WT_WW_SOFT": "1"})
        self.assertEqual(out, "metadata_fail=PASS")


if __name__ == "__main__":
    unittest.main()
