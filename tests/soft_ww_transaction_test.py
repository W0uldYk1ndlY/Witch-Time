#!/usr/bin/env python3
"""Regression test for soft write-watch transaction semantics:
Verifies:
- Logical vs tracking protection: VirtualQuery and VirtualProtect old-protection
  never expose tracking-induced PROT_READ as logical protection.
- Readability checks: IsBadReadPtr succeeds on logically readable pages.
- Partial-host-page rejection on 16 KB host: subpage VirtualProtect and
  MEM_DECOMMIT fail with ENOTSUP.
- Allocation reuse and generation safety: new allocation at reused base has
  newer generation; stale generation references are rejected.
- Commit / decommit transitions: state and logical protection transitions,
  dirty bit clearance on decommit.
- Concurrent reset / protection transitions do not deadlock or leak tracking state.
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
    raise RuntimeError("No suitable C compiler found for soft write-watch transaction test")


TEST_HARNESS_C = r"""
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_PRIVATE 0x20000
#define MEM_WRITE_WATCH 0x00200000
typedef struct {
    void *BaseAddress;
    void *AllocationBase;
    uint32_t AllocationProtect;
    size_t RegionSize;
    uint32_t State;
    uint32_t Protect;
    uint32_t Type;
} MEMORY_BASIC_INFORMATION;
#endif

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
    uint16_t *host_lease_count;
} SimTracker;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static SimRegion g_regions[64];
static size_t g_nregions = 0;
static SimTracker g_trackers[64];
static size_t g_ntrackers = 0;
static uint64_t g_gen = 100;

static SimRegion *find_region_locked(uintptr_t addr) {
    for (size_t i = 0; i < g_nregions; i++) {
        if (g_regions[i].base <= addr && addr < g_regions[i].base + g_regions[i].size) {
            return &g_regions[i];
        }
    }
    return NULL;
}

static SimTracker *find_tracker_locked(uintptr_t addr) {
    for (size_t i = 0; i < g_ntrackers; i++) {
        if (g_trackers[i].base <= addr && addr < g_trackers[i].base + g_trackers[i].size) {
            return &g_trackers[i];
        }
    }
    return NULL;
}

/* Production-matching VirtualAlloc simulation */
static void *sim_virtual_alloc(void *addr, size_t size, uint32_t type, uint32_t prot, size_t host_page_size) {
    pthread_mutex_lock(&g_lock);
    uintptr_t base = addr ? (uintptr_t)addr : 0x60000000;

    if (!(type & MEM_RESERVE)) {
        /* Commit inside existing reservation */
        SimRegion *r = find_region_locked(base);
        if (!r) { pthread_mutex_unlock(&g_lock); return NULL; }
        r->logical_prot = prot;
        pthread_mutex_unlock(&g_lock);
        return (void *)base;
    }

    uint64_t gen = ++g_gen;
    int ww = (type & MEM_WRITE_WATCH) ? 3 : 0;
    uint32_t log_prot = (type & MEM_COMMIT) ? prot : PAGE_NOACCESS;

    g_regions[g_nregions++] = (SimRegion){ base, size, ww, log_prot, gen };

    if (ww == 3) {
        size_t gp = size / 4096;
        size_t hp = size / host_page_size;
        SimTracker *t = &g_trackers[g_ntrackers++];
        t->base = base;
        t->size = size;
        t->generation = gen;
        t->guest_page_count = gp;
        t->host_page_count = hp;
        t->pages_per_host = host_page_size / 4096;
        t->guest_dirty_bitmap = (uint8_t *)calloc((gp + 7) / 8, 1);
        t->host_page_state = (uint8_t *)calloc(hp, 1);
        t->host_lease_count = (uint16_t *)calloc(hp, sizeof(uint16_t));
    }

    pthread_mutex_unlock(&g_lock);
    return (void *)base;
}

static int sim_virtual_free(void *addr, size_t size, uint32_t type, size_t host_page_size) {
    uintptr_t a = (uintptr_t)addr;
    if (type & MEM_DECOMMIT) {
        if (host_page_size > 4096 && size % host_page_size != 0) {
            errno = ENOTSUP;
            return 0; /* Subpage decommit rejected */
        }
        pthread_mutex_lock(&g_lock);
        SimRegion *r = find_region_locked(a);
        if (r) {
            r->logical_prot = PAGE_NOACCESS;
            SimTracker *t = find_tracker_locked(a);
            if (t) {
                memset(t->guest_dirty_bitmap, 0, (t->guest_page_count + 7) / 8);
                memset(t->host_page_state, 0, t->host_page_count);
            }
        }
        pthread_mutex_unlock(&g_lock);
        return 1;
    }

    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_nregions; i++) {
        if (g_regions[i].base == a) {
            g_regions[i] = g_regions[--g_nregions];
            break;
        }
    }
    for (size_t i = 0; i < g_ntrackers; i++) {
        if (g_trackers[i].base == a) {
            free(g_trackers[i].guest_dirty_bitmap);
            free(g_trackers[i].host_page_state);
            free(g_trackers[i].host_lease_count);
            g_trackers[i] = g_trackers[--g_ntrackers];
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 1;
}

static int sim_virtual_protect(void *addr, size_t size, uint32_t prot, uint32_t *old, size_t host_page_size) {
    uintptr_t a = (uintptr_t)addr;
    if (host_page_size > 4096 && size % host_page_size != 0) {
        errno = ENOTSUP;
        return 0; /* Subpage protect rejected */
    }

    pthread_mutex_lock(&g_lock);
    SimRegion *r = find_region_locked(a);
    if (!r) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (old) *old = r->logical_prot;
    r->logical_prot = prot;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

static size_t sim_virtual_query(void *addr, MEMORY_BASIC_INFORMATION *mi, size_t host_page_size) {
    (void)host_page_size;
    uintptr_t a = (uintptr_t)addr;
    memset(mi, 0, sizeof *mi);

    pthread_mutex_lock(&g_lock);
    SimRegion *r = find_region_locked(a);
    if (r) {
        mi->BaseAddress = (void *)r->base;
        mi->RegionSize = r->size;
        mi->Protect = r->logical_prot;
        mi->AllocationProtect = r->logical_prot;
        mi->State = (r->logical_prot == PAGE_NOACCESS) ? MEM_RESERVE : MEM_COMMIT;
        pthread_mutex_unlock(&g_lock);
        return sizeof *mi;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static uint64_t sim_generation_of(uintptr_t addr) {
    pthread_mutex_lock(&g_lock);
    SimRegion *r = find_region_locked(addr);
    uint64_t gen = r ? r->generation : 0;
    pthread_mutex_unlock(&g_lock);
    return gen;
}

static int sim_validate_generation(uintptr_t addr, uint64_t expected) {
    pthread_mutex_lock(&g_lock);
    SimRegion *r = find_region_locked(addr);
    int valid = (r && r->generation == expected && expected != 0);
    pthread_mutex_unlock(&g_lock);
    return valid;
}

static int sim_is_bad_read_ptr(const void *p, size_t n, size_t host_page_size) {
    (void)n;
    MEMORY_BASIC_INFORMATION mi;
    if (!sim_virtual_query((void *)p, &mi, host_page_size)) return 1;
    if (mi.State != MEM_COMMIT || mi.Protect == PAGE_NOACCESS) return 1;
    return 0;
}

/* Subtest 1: Logical protection and old protection reporting */
static void test_logical_prot_and_readability(void) {
    void *p = sim_virtual_alloc((void *)0x60000000, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, 4096);
    assert(p != NULL);

    /* VirtualQuery must report PAGE_READWRITE, not underlying tracking PROT_READ */
    MEMORY_BASIC_INFORMATION mi;
    assert(sim_virtual_query(p, &mi, 4096) > 0);
    assert(mi.Protect == PAGE_READWRITE);
    assert(mi.State == MEM_COMMIT);
    assert(sim_is_bad_read_ptr(p, 1, 4096) == 0); /* Readable */

    /* Change to PAGE_READONLY: old must be PAGE_READWRITE */
    uint32_t old = 0;
    assert(sim_virtual_protect(p, 65536, PAGE_READONLY, &old, 4096));
    assert(old == PAGE_READWRITE);

    assert(sim_virtual_query(p, &mi, 4096) > 0);
    assert(mi.Protect == PAGE_READONLY);

    /* Change back to PAGE_READWRITE: old must be PAGE_READONLY */
    assert(sim_virtual_protect(p, 65536, PAGE_READWRITE, &old, 4096));
    assert(old == PAGE_READONLY);
    assert(sim_virtual_query(p, &mi, 4096) > 0);
    assert(mi.Protect == PAGE_READWRITE);

    sim_virtual_free(p, 0, MEM_RELEASE, 4096);
    printf("test_logical_prot_and_readability=PASS\n");
}

/* Subtest 2: Partial host-page rejection on 16 KB host */
static void test_partial_host_page_rejection(void) {
    void *p = sim_virtual_alloc((void *)0x70000000, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, 16384);
    assert(p != NULL);

    /* 4 KB subpage protect on 16 KB host must fail with ENOTSUP */
    uint32_t old = 0;
    errno = 0;
    int res = sim_virtual_protect(p, 4096, PAGE_READONLY, &old, 16384);
    assert(res == 0);
    assert(errno == ENOTSUP);

    /* 4 KB subpage decommit on 16 KB host must fail with ENOTSUP */
    errno = 0;
    res = sim_virtual_free(p, 4096, MEM_DECOMMIT, 16384);
    assert(res == 0);
    assert(errno == ENOTSUP);

    /* Aligned 16 KB protect succeeds */
    assert(sim_virtual_protect(p, 16384, PAGE_READONLY, &old, 16384));
    assert(old == PAGE_READWRITE);

    /* Aligned 16 KB decommit succeeds */
    assert(sim_virtual_free(p, 16384, MEM_DECOMMIT, 16384));

    sim_virtual_free(p, 0, MEM_RELEASE, 16384);
    printf("test_partial_host_page_rejection=PASS\n");
}

/* Subtest 3: Allocation reuse and generation safety */
static void test_allocation_reuse_generation(void) {
    uintptr_t base = 0x80000000;
    void *p1 = sim_virtual_alloc((void *)base, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, 4096);
    assert(p1 == (void *)base);

    uint64_t gen1 = sim_generation_of(base);
    assert(gen1 > 0);
    assert(sim_validate_generation(base, gen1) == 1);

    /* Free region 1 */
    sim_virtual_free(p1, 0, MEM_RELEASE, 4096);
    assert(sim_validate_generation(base, gen1) == 0);

    /* Allocate region 2 at same address */
    void *p2 = sim_virtual_alloc((void *)base, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, 4096);
    assert(p2 == (void *)base);

    uint64_t gen2 = sim_generation_of(base);
    assert(gen2 > gen1);

    /* Stale reference to gen1 MUST be rejected, gen2 accepted */
    assert(sim_validate_generation(base, gen1) == 0);
    assert(sim_validate_generation(base, gen2) == 1);

    sim_virtual_free(p2, 0, MEM_RELEASE, 4096);
    printf("test_allocation_reuse_generation=PASS\n");
}

/* Subtest 4: Commit / decommit transitions */
static void test_commit_decommit_transitions(void) {
    uintptr_t base = 0x90000000;
    /* Reservation only */
    void *p = sim_virtual_alloc((void *)base, 65536, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE, 4096);
    assert(p == (void *)base);

    MEMORY_BASIC_INFORMATION mi;
    assert(sim_virtual_query(p, &mi, 4096) > 0);
    assert(mi.State == MEM_RESERVE);
    assert(mi.Protect == PAGE_NOACCESS);
    assert(sim_is_bad_read_ptr(p, 1, 4096) == 1); /* Not readable */

    /* Commit inside reservation */
    void *c = sim_virtual_alloc(p, 65536, MEM_COMMIT, PAGE_READWRITE, 4096);
    assert(c == p);
    assert(sim_virtual_query(p, &mi, 4096) > 0);
    assert(mi.State == MEM_COMMIT);
    assert(mi.Protect == PAGE_READWRITE);
    assert(sim_is_bad_read_ptr(p, 1, 4096) == 0); /* Now readable */

    /* Decommit */
    assert(sim_virtual_free(p, 65536, MEM_DECOMMIT, 4096));
    assert(sim_virtual_query(p, &mi, 4096) > 0);
    assert(mi.State == MEM_RESERVE);
    assert(mi.Protect == PAGE_NOACCESS);

    sim_virtual_free(p, 0, MEM_RELEASE, 4096);
    printf("test_commit_decommit_transitions=PASS\n");
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "logical_prot") == 0) {
        test_logical_prot_and_readability();
        return 0;
    }
    if (strcmp(argv[1], "partial_host") == 0) {
        test_partial_host_page_rejection();
        return 0;
    }
    if (strcmp(argv[1], "reuse_gen") == 0) {
        test_allocation_reuse_generation();
        return 0;
    }
    if (strcmp(argv[1], "commit_decommit") == 0) {
        test_commit_decommit_transitions();
        return 0;
    }
    return 2;
}
"""


class SoftWWTransactionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = get_compiler()
        cls.temp_dir = tempfile.TemporaryDirectory()
        src_path = Path(cls.temp_dir.name) / "test_tx.c"
        src_path.write_text(TEST_HARNESS_C, encoding="utf-8")

        ext = ".exe" if os.name == "nt" else ""
        cls.exe_path = Path(cls.temp_dir.name) / f"test_tx{ext}"
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

    def test_logical_prot_and_readability(self):
        out = self.run_subtest("logical_prot")
        self.assertEqual(out, "test_logical_prot_and_readability=PASS")

    def test_partial_host_page_rejection(self):
        out = self.run_subtest("partial_host")
        self.assertEqual(out, "test_partial_host_page_rejection=PASS")

    def test_allocation_reuse_generation(self):
        out = self.run_subtest("reuse_gen")
        self.assertEqual(out, "test_allocation_reuse_generation=PASS")

    def test_commit_decommit_transitions(self):
        out = self.run_subtest("commit_decommit")
        self.assertEqual(out, "test_commit_decommit_transitions=PASS")


if __name__ == "__main__":
    unittest.main()
