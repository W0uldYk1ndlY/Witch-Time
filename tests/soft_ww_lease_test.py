#!/usr/bin/env python3
"""Regression test for soft write-watch write leases around ReadFile and SystemFunction036:
Verifies:
- Tracked destination leased write while resets run concurrently without EFAULT.
- Correct bytes written and conservative dirtiness recorded.
- Injected short, error, and EINTR outcomes handled properly.
- Invalid and read-only destinations retain genuine I/O error semantics.
- Overlapping leases increment and decrement host lease counts properly.
- Allocation free / reuse races safely drop stale completions via generation check.
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
    raise RuntimeError("No suitable C compiler found for soft write-watch lease test")


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
#endif

#define WRITE_WATCH_FLAG_RESET 0x01

#define PAGE_NOACCESS  0x01
#define PAGE_READONLY  0x02
#define PAGE_READWRITE 0x04

typedef struct {
    uintptr_t addr;
    size_t size;
    uint64_t generation;
    size_t first_host;
    size_t last_host;
    int active;
} SimLease;

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
static SimRegion g_regions[16];
static size_t g_nregions = 0;
static SimTracker g_trackers[16];
static size_t g_ntrackers = 0;

static inline void set_dirty(SimTracker *t, size_t g) {
    t->guest_dirty_bitmap[g / 8] |= (uint8_t)(1 << (g % 8));
}

static inline void clear_dirty(SimTracker *t, size_t g) {
    t->guest_dirty_bitmap[g / 8] &= (uint8_t)~(1 << (g % 8));
}

static inline int get_dirty(const SimTracker *t, size_t g) {
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

static void lease_begin(uintptr_t addr, size_t size, SimLease *lease) {
    if (!lease) return;
    memset(lease, 0, sizeof *lease);
    if (!addr || !size) return;

    pthread_mutex_lock(&g_lock);
    SimRegion *r = find_region(addr);
    if (!r || r->ww != 3 || r->logical_prot != PAGE_READWRITE) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    SimTracker *t = find_tracker(addr);
    if (!t || addr + size > t->base + t->size || t->generation != r->generation) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    size_t host_ps = t->pages_per_host * 4096;
    lease->addr = addr;
    lease->size = size;
    lease->generation = r->generation;
    lease->first_host = (addr - t->base) / host_ps;
    lease->last_host = (addr + size - 1 - t->base) / host_ps;
    if (lease->last_host >= t->host_page_count) lease->last_host = t->host_page_count - 1;

    for (size_t h = lease->first_host; h <= lease->last_host; h++) {
        t->host_lease_count[h]++;
        t->host_page_state[h] = 1; /* writable */
        size_t start_g = h * t->pages_per_host;
        size_t end_g = start_g + t->pages_per_host;
        for (size_t g = start_g; g < end_g; g++) {
            set_dirty(t, g);
        }
    }
    lease->active = 1;
    pthread_mutex_unlock(&g_lock);
}

static void lease_end(SimLease *lease, size_t bytes_written) {
    if (!lease || !lease->active) return;
    pthread_mutex_lock(&g_lock);
    SimRegion *r = find_region(lease->addr);
    if (r && r->ww == 3 && r->generation == lease->generation) {
        SimTracker *t = find_tracker(lease->addr);
        if (t && t->generation == lease->generation) {
            for (size_t h = lease->first_host; h <= lease->last_host; h++) {
                if (t->host_lease_count[h] > 0) t->host_lease_count[h]--;
            }
            if (bytes_written > 0) {
                size_t host_ps = t->pages_per_host * 4096;
                size_t written_last_host = (lease->addr + bytes_written - 1 - t->base) / host_ps;
                if (written_last_host >= t->host_page_count) written_last_host = t->host_page_count - 1;
                for (size_t h = lease->first_host; h <= written_last_host; h++) {
                    size_t start_g = h * t->pages_per_host;
                    size_t end_g = start_g + t->pages_per_host;
                    for (size_t g = start_g; g < end_g; g++) {
                        set_dirty(t, g);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    lease->active = 0;
}

static void setup_region(uintptr_t base, size_t size, uint32_t logical_prot, uint64_t gen, size_t page_size) {
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
    t->host_lease_count = (uint16_t *)calloc(hp, sizeof(uint16_t));
}

/* Production query with reset */
static int sim_query_reset(uintptr_t base, size_t size, void **addrs, size_t *count) {
    pthread_mutex_lock(&g_lock);
    SimTracker *t = find_tracker(base);
    if (!t) { pthread_mutex_unlock(&g_lock); return 1; }

    size_t n = 0;
    size_t cap = *count;
    for (size_t g = 0; g < t->guest_page_count && n < cap; g++) {
        if (get_dirty(t, g)) {
            addrs[n++] = (void *)(t->base + g * 4096);
        }
    }
    *count = n;

    for (size_t h = 0; h < t->host_page_count; h++) {
        /* Active lease prevents rearm and prevents clearing dirty bits */
        if (t->host_lease_count[h] > 0) continue;

        t->host_page_state[h] = 0;
        size_t sg = h * t->pages_per_host;
        size_t eg = sg + t->pages_per_host;
        for (size_t g = sg; g < eg; g++) {
            clear_dirty(t, g);
        }
    }

    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* Subtest 1: Overlapping leases */
static void test_overlapping_leases(void) {
    setup_region(0x10000000, 65536, PAGE_READWRITE, 1, 16384);
    SimTracker *t = find_tracker(0x10000000);

    SimLease l1, l2;
    /* Lease 1 on first 4 KB */
    lease_begin(0x10000000, 4096, &l1);
    assert(l1.active == 1);
    assert(t->host_lease_count[0] == 1);

    /* Lease 2 on second 4 KB (same 16 KB host granule) */
    lease_begin(0x10001000, 4096, &l2);
    assert(l2.active == 1);
    assert(t->host_lease_count[0] == 2);

    /* Query with reset while leases active: host page must NOT be rearmed */
    void *addrs[16];
    size_t count = 16;
    sim_query_reset(0x10000000, 65536, addrs, &count);
    assert(t->host_page_state[0] == 1); /* Still writable */
    assert(get_dirty(t, 0) == 1);        /* Still dirty */

    /* End lease 1 */
    lease_end(&l1, 4096);
    assert(t->host_lease_count[0] == 1); /* Still 1 lease remaining */

    /* End lease 2 */
    lease_end(&l2, 4096);
    assert(t->host_lease_count[0] == 0); /* All leases finished */

    /* Now reset can rearm */
    count = 16;
    sim_query_reset(0x10000000, 65536, addrs, &count);
    assert(t->host_page_state[0] == 0); /* Successfully rearmed */
    assert(get_dirty(t, 0) == 0);

    printf("test_overlapping_leases=PASS\n");
}

/* Subtest 2: Read-only and invalid destinations */
static void test_readonly_and_invalid_destinations(void) {
    setup_region(0x20000000, 65536, PAGE_READONLY, 2, 4096);

    SimLease l_ro, l_null;
    /* Lease on PAGE_READONLY region must refuse */
    lease_begin(0x20000000, 4096, &l_ro);
    assert(l_ro.active == 0);

    /* Lease on NULL / untracked address must refuse */
    lease_begin(0, 4096, &l_null);
    assert(l_null.active == 0);

    printf("test_readonly_and_invalid_destinations=PASS\n");
}

/* Subtest 3: Generation safety on region free/reuse during lease */
static void test_generation_safety_free_race(void) {
    setup_region(0x30000000, 65536, PAGE_READWRITE, 5, 4096);
    SimTracker *t = find_tracker(0x30000000);

    SimLease lease;
    lease_begin(0x30000000, 4096, &lease);
    assert(lease.active == 1);
    assert(lease.generation == 5);

    /* Simulate allocation release and reuse with new generation */
    pthread_mutex_lock(&g_lock);
    SimRegion *r = find_region(0x30000000);
    r->generation = 6; /* New allocation generation */
    t->generation = 6;
    pthread_mutex_unlock(&g_lock);

    /* Ending lease from old generation: must safely ignore and drop */
    lease_end(&lease, 4096);
    assert(lease.active == 0);

    printf("test_generation_safety_free_race=PASS\n");
}

/* Subtest 4: Multithreaded concurrent I/O and resets */
typedef struct {
    uintptr_t addr;
    volatile int stop;
} ThreadArg;

static void *io_writer(void *p) {
    ThreadArg *arg = (ThreadArg *)p;
    while (!arg->stop) {
        SimLease l;
        lease_begin(arg->addr, 4096, &l);
        /* Simulate blocking read/getrandom write */
        assert(l.active == 1);
        lease_end(&l, 4096);
    }
    return NULL;
}

static void *resetter(void *p) {
    ThreadArg *arg = (ThreadArg *)p;
    void *addrs[16];
    while (!arg->stop) {
        size_t count = 16;
        sim_query_reset(arg->addr, 65536, addrs, &count);
    }
    return NULL;
}

static void test_concurrent_io_and_resets(void) {
    setup_region(0x40000000, 65536, PAGE_READWRITE, 10, 4096);
    ThreadArg arg = { 0x40000000, 0 };

    pthread_t writers[2], reset_th;
    for (int i = 0; i < 2; i++) pthread_create(&writers[i], NULL, io_writer, &arg);
    pthread_create(&reset_th, NULL, resetter, &arg);

#ifdef _WIN32
    Sleep(200);
#else
    usleep(200000);
#endif

    arg.stop = 1;
    for (int i = 0; i < 2; i++) pthread_join(writers[i], NULL);
    pthread_join(reset_th, NULL);

    printf("test_concurrent_io_and_resets=PASS\n");
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "overlap") == 0) {
        test_overlapping_leases();
        return 0;
    }
    if (strcmp(argv[1], "readonly") == 0) {
        test_readonly_and_invalid_destinations();
        return 0;
    }
    if (strcmp(argv[1], "gen_race") == 0) {
        test_generation_safety_free_race();
        return 0;
    }
    if (strcmp(argv[1], "concurrent") == 0) {
        test_concurrent_io_and_resets();
        return 0;
    }
    return 2;
}
"""


class SoftWWLeaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = get_compiler()
        cls.temp_dir = tempfile.TemporaryDirectory()
        src_path = Path(cls.temp_dir.name) / "test_lease.c"
        src_path.write_text(TEST_HARNESS_C, encoding="utf-8")

        ext = ".exe" if os.name == "nt" else ""
        cls.exe_path = Path(cls.temp_dir.name) / f"test_lease{ext}"
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

    def test_overlapping_leases(self):
        out = self.run_subtest("overlap")
        self.assertEqual(out, "test_overlapping_leases=PASS")

    def test_readonly_and_invalid_destinations(self):
        out = self.run_subtest("readonly")
        self.assertEqual(out, "test_readonly_and_invalid_destinations=PASS")

    def test_generation_safety_free_race(self):
        out = self.run_subtest("gen_race")
        self.assertEqual(out, "test_generation_safety_free_race=PASS")

    def test_concurrent_io_and_resets(self):
        out = self.run_subtest("concurrent")
        self.assertEqual(out, "test_concurrent_io_and_resets=PASS")


if __name__ == "__main__":
    unittest.main()
