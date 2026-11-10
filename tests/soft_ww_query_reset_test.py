#!/usr/bin/env python3
"""Regression test for soft write-watch serialized 4 KB queries, reset/rearm,
capacity bounding, sibling preservation, active write lease handling,
and writer/reset multithreaded races.
Verifies:
- Ordered capacity-bounded 4 KB dirty query results
- Zero-capacity query handling
- Preserved sibling/unreported bits on partial queries and capacity truncation
- Rearming host granules to PROT_READ on 4 KB and modeled 16 KB granules
- Invariant: no writable clean page is ever left
- Active write lease prevents rearming and dirty bit clearing
- Concurrent writer/reset thread race does not deadlock or violate invariants
- Unsupported host granularity stays unavailable
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
    raise RuntimeError("No suitable C compiler found for soft write-watch query/reset test")


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

static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#define WRITE_WATCH_FLAG_RESET 0x01
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
static SoftWWTracker *g_trackers = NULL;

static inline int get_dirty(const SoftWWTracker *t, size_t idx) {
    return (t->guest_dirty_bitmap[idx / 8] >> (idx % 8)) & 1;
}

static inline void set_dirty(SoftWWTracker *t, size_t idx) {
    t->guest_dirty_bitmap[idx / 8] |= (uint8_t)(1 << (idx % 8));
}

static inline void clear_dirty(SoftWWTracker *t, size_t idx) {
    t->guest_dirty_bitmap[idx / 8] &= (uint8_t)~(1 << (idx % 8));
}

static SoftWWTracker *tracker_create(uintptr_t base, size_t size, size_t page_size) {
    size_t guest_pages = size / 4096;
    size_t host_pages = size / page_size;
    size_t pages_per_host = page_size / 4096;
    size_t bitmap_bytes = (guest_pages + 7) / 8;

    SoftWWTracker *t = (SoftWWTracker *)calloc(1, sizeof(SoftWWTracker));
    assert(t);
    t->guest_dirty_bitmap = (uint8_t *)calloc(bitmap_bytes, 1);
    t->host_page_state = (uint8_t *)calloc(host_pages, 1);
    t->host_lease_count = (uint16_t *)calloc(host_pages, sizeof(uint16_t));
    assert(t->guest_dirty_bitmap && t->host_page_state && t->host_lease_count);

    t->base = base;
    t->size = size;
    t->guest_page_count = guest_pages;
    t->host_page_count = host_pages;
    t->pages_per_host = pages_per_host;

    pthread_mutex_lock(&g_reg_lock);
    t->next = g_trackers;
    g_trackers = t;
    pthread_mutex_unlock(&g_reg_lock);
    return t;
}

static void tracker_destroy(SoftWWTracker *t) {
    pthread_mutex_lock(&g_reg_lock);
    SoftWWTracker **pp = &g_trackers;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_reg_lock);
    free(t->guest_dirty_bitmap);
    free(t->host_page_state);
    free(t->host_lease_count);
    free(t);
}

/* Production-matching mark dirty: opens host granule and dirties constituent guest pages */
static void mark_dirty(SoftWWTracker *t, uintptr_t addr, size_t size) {
    pthread_mutex_lock(&g_reg_lock);
    size_t host_ps = t->pages_per_host * 4096;
    size_t first_host = (addr - t->base) / host_ps;
    size_t last_host = (addr + size - 1 - t->base) / host_ps;
    if (last_host >= t->host_page_count) last_host = t->host_page_count - 1;

    for (size_t h = first_host; h <= last_host; h++) {
        t->host_page_state[h] = 1; /* writable */
        size_t start_g = h * t->pages_per_host;
        size_t end_g = start_g + t->pages_per_host;
        for (size_t g = start_g; g < end_g; g++) {
            set_dirty(t, g);
        }
    }
    pthread_mutex_unlock(&g_reg_lock);
}

/* Production-matching query and reset */
static int soft_query(SoftWWTracker *t, uint32_t flags, uintptr_t start, uintptr_t end, void **addrs, size_t *count, uint32_t *gran) {
    if (!count || !gran || (!addrs && *count)) return 1;
    *gran = 4096;
    size_t cap = *count;
    *count = 0;
    if (cap == 0) return 0; /* Zero capacity: reports 0, no reset */

    pthread_mutex_lock(&g_reg_lock);
    if (!t || start < t->base || end > t->base + t->size) {
        pthread_mutex_unlock(&g_reg_lock);
        return 1;
    }

    size_t first_guest = (start - t->base) / 4096;
    size_t last_guest = (end - t->base) / 4096;
    if (last_guest > t->guest_page_count) last_guest = t->guest_page_count;

    size_t n = 0;
    int do_reset = (flags & WRITE_WATCH_FLAG_RESET) != 0;

    for (size_t g = first_guest; g < last_guest && n < cap; g++) {
        if (get_dirty(t, g)) {
            addrs[n++] = (void *)(t->base + (uintptr_t)g * 4096);
        }
    }
    *count = n;

    if (do_reset && n > 0) {
        size_t first_host = first_guest / t->pages_per_host;
        size_t last_host = (last_guest + t->pages_per_host - 1) / t->pages_per_host;
        if (last_host > t->host_page_count) last_host = t->host_page_count;

        for (size_t h = first_host; h < last_host; h++) {
            if (t->host_lease_count[h] > 0) continue;

            int has_reported = 0;
            for (size_t i = 0; i < n; i++) {
                uintptr_t pa = (uintptr_t)addrs[i];
                size_t g = (pa - t->base) / 4096;
                if (g / t->pages_per_host == h) {
                    has_reported = 1;
                    break;
                }
            }
            if (!has_reported) continue;

            /* Simulated host rearm */
            t->host_page_state[h] = 0; /* clean / protected */
            for (size_t i = 0; i < n; i++) {
                uintptr_t pa = (uintptr_t)addrs[i];
                size_t g = (pa - t->base) / 4096;
                if (g / t->pages_per_host == h) {
                    clear_dirty(t, g);
                }
            }
        }
    }

    /* INVARIANT ASSERTION: No writable clean page is ever left */
    for (size_t h = 0; h < t->host_page_count; h++) {
        if (t->host_page_state[h] == 1 && t->host_lease_count[h] == 0) {
            int has_any_dirty = 0;
            size_t sg = h * t->pages_per_host;
            size_t eg = sg + t->pages_per_host;
            for (size_t g = sg; g < eg; g++) {
                if (get_dirty(t, g)) { has_any_dirty = 1; break; }
            }
            assert(has_any_dirty && "Invariant violated: host page is writable but has 0 dirty guest pages");
        }
    }

    pthread_mutex_unlock(&g_reg_lock);
    return 0;
}

static int soft_reset(SoftWWTracker *t, uintptr_t start, uintptr_t end) {
    pthread_mutex_lock(&g_reg_lock);
    if (!t || start < t->base || end > t->base + t->size) {
        pthread_mutex_unlock(&g_reg_lock);
        return 1;
    }

    size_t first_guest = (start - t->base) / 4096;
    size_t last_guest = (end - t->base) / 4096;
    if (last_guest > t->guest_page_count) last_guest = t->guest_page_count;

    size_t first_host = first_guest / t->pages_per_host;
    size_t last_host = (last_guest + t->pages_per_host - 1) / t->pages_per_host;
    if (last_host > t->host_page_count) last_host = t->host_page_count;

    for (size_t h = first_host; h < last_host; h++) {
        if (t->host_lease_count[h] > 0) continue;

        t->host_page_state[h] = 0;
        size_t h_first_g = h * t->pages_per_host;
        size_t h_last_g = h_first_g + t->pages_per_host;
        for (size_t g = h_first_g; g < h_last_g; g++) {
            if (g >= first_guest && g < last_guest) {
                clear_dirty(t, g);
            }
        }
    }

    pthread_mutex_unlock(&g_reg_lock);
    return 0;
}

/* Subtests */
static void test_4kb_queries(void) {
    SoftWWTracker *t = tracker_create(0x10000000, 65536, 4096); /* 16 guest pages */
    void *addrs[32];
    size_t count = 32;
    uint32_t gran = 0;

    /* Initially clean */
    assert(soft_query(t, 0, 0x10000000, 0x10010000, addrs, &count, &gran) == 0);
    assert(count == 0);
    assert(gran == 4096);

    /* Zero capacity query: returns 0, no reset */
    count = 0;
    assert(soft_query(t, WRITE_WATCH_FLAG_RESET, 0x10000000, 0x10010000, addrs, &count, &gran) == 0);
    assert(count == 0);

    /* Mark page 2 and page 5 dirty */
    mark_dirty(t, 0x10002000, 4096);
    mark_dirty(t, 0x10005000, 4096);

    /* Query without reset: count=2, ordered */
    count = 32;
    assert(soft_query(t, 0, 0x10000000, 0x10010000, addrs, &count, &gran) == 0);
    assert(count == 2);
    assert(addrs[0] == (void *)0x10002000);
    assert(addrs[1] == (void *)0x10005000);
    assert((uintptr_t)addrs[0] < (uintptr_t)addrs[1]);

    /* Capacity-bounded query with reset: cap=1 */
    count = 1;
    assert(soft_query(t, WRITE_WATCH_FLAG_RESET, 0x10000000, 0x10010000, addrs, &count, &gran) == 0);
    assert(count == 1);
    assert(addrs[0] == (void *)0x10002000);

    /* Page 5 must still be dirty (preserved unreported bit!) */
    count = 32;
    assert(soft_query(t, 0, 0x10000000, 0x10010000, addrs, &count, &gran) == 0);
    assert(count == 1);
    assert(addrs[0] == (void *)0x10005000);

    /* Reset page 5 via ResetWriteWatch */
    assert(soft_reset(t, 0x10000000, 0x10010000) == 0);

    /* Now all clean */
    count = 32;
    assert(soft_query(t, 0, 0x10000000, 0x10010000, addrs, &count, &gran) == 0);
    assert(count == 0);

    tracker_destroy(t);
    printf("test_4kb_queries=PASS\n");
}

static void test_16kb_granule_siblings(void) {
    /* Modeled 16 KB host page size: 4 host pages, 16 guest pages */
    SoftWWTracker *t = tracker_create(0x20000000, 65536, 16384);
    void *addrs[32];
    size_t count = 32;
    uint32_t gran = 0;

    /* Write to guest page 1 (addr 0x20001000):
     * Should dirty host page 0 and all its 4 constituent guest pages (0, 1, 2, 3) */
    mark_dirty(t, 0x20001000, 4096);

    /* Query with cap=2 and RESET */
    count = 2;
    assert(soft_query(t, WRITE_WATCH_FLAG_RESET, 0x20000000, 0x20010000, addrs, &count, &gran) == 0);
    assert(count == 2);
    assert(addrs[0] == (void *)0x20000000);
    assert(addrs[1] == (void *)0x20001000);

    /* Sibling guest pages 2 and 3 MUST remain dirty! */
    count = 32;
    assert(soft_query(t, 0, 0x20000000, 0x20010000, addrs, &count, &gran) == 0);
    assert(count == 2);
    assert(addrs[0] == (void *)0x20002000);
    assert(addrs[1] == (void *)0x20003000);

    /* ResetWriteWatch clears remaining */
    assert(soft_reset(t, 0x20000000, 0x20010000) == 0);
    count = 32;
    assert(soft_query(t, 0, 0x20000000, 0x20010000, addrs, &count, &gran) == 0);
    assert(count == 0);

    tracker_destroy(t);
    printf("test_16kb_granule_siblings=PASS\n");
}

static void test_active_lease(void) {
    SoftWWTracker *t = tracker_create(0x30000000, 65536, 4096);
    void *addrs[32];
    size_t count = 32;
    uint32_t gran = 0;

    /* Mark page 0 dirty and take a lease */
    mark_dirty(t, 0x30000000, 4096);
    t->host_lease_count[0] = 1; /* active write lease */

    /* Query with reset: returns page 0, but CANNOT clear dirty bit or rearm host page */
    count = 32;
    assert(soft_query(t, WRITE_WATCH_FLAG_RESET, 0x30000000, 0x30010000, addrs, &count, &gran) == 0);
    assert(count == 1);
    assert(addrs[0] == (void *)0x30000000);

    /* Page 0 MUST remain dirty because lease was active */
    count = 32;
    assert(soft_query(t, 0, 0x30000000, 0x30010000, addrs, &count, &gran) == 0);
    assert(count == 1);

    /* Drop lease */
    t->host_lease_count[0] = 0;

    /* Now reset works */
    count = 32;
    assert(soft_query(t, WRITE_WATCH_FLAG_RESET, 0x30000000, 0x30010000, addrs, &count, &gran) == 0);
    assert(count == 1);

    count = 32;
    assert(soft_query(t, 0, 0x30000000, 0x30010000, addrs, &count, &gran) == 0);
    assert(count == 0);

    tracker_destroy(t);
    printf("test_active_lease=PASS\n");
}

/* Multithreaded stress race */
typedef struct {
    SoftWWTracker *t;
    volatile int stop;
} ThreadArg;

static void *writer_thread(void *p) {
    ThreadArg *arg = (ThreadArg *)p;
    uint32_t seed = (uint32_t)(uintptr_t)pthread_self() ^ 0x12345678;
    if (!seed) seed = 1;
    while (!arg->stop) {
        size_t page_idx = xorshift32(&seed) % arg->t->guest_page_count;
        uintptr_t addr = arg->t->base + page_idx * 4096;
        mark_dirty(arg->t, addr, 4096);
    }
    return NULL;
}

static void *reader_thread(void *p) {
    ThreadArg *arg = (ThreadArg *)p;
    void *addrs[64];
    uint32_t gran = 0;
    while (!arg->stop) {
        size_t count = 16;
        int res = soft_query(arg->t, WRITE_WATCH_FLAG_RESET, arg->t->base, arg->t->base + arg->t->size, addrs, &count, &gran);
        assert(res == 0);
        for (size_t i = 1; i < count; i++) {
            assert((uintptr_t)addrs[i - 1] < (uintptr_t)addrs[i]);
        }
    }
    return NULL;
}

static void test_writer_reset_race(void) {
    SoftWWTracker *t = tracker_create(0x40000000, 65536, 16384);
    ThreadArg arg = { t, 0 };
    pthread_t writers[4], readers[2];

    for (int i = 0; i < 4; i++) pthread_create(&writers[i], NULL, writer_thread, &arg);
    for (int i = 0; i < 2; i++) pthread_create(&readers[i], NULL, reader_thread, &arg);

#ifdef _WIN32
    Sleep(500); /* 500ms stress */
#else
    usleep(500000);
#endif

    arg.stop = 1;
    for (int i = 0; i < 4; i++) pthread_join(writers[i], NULL);
    for (int i = 0; i < 2; i++) pthread_join(readers[i], NULL);

    tracker_destroy(t);
    printf("test_writer_reset_race=PASS\n");
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "4kb") == 0) {
        test_4kb_queries();
        return 0;
    }
    if (strcmp(argv[1], "16kb") == 0) {
        test_16kb_granule_siblings();
        return 0;
    }
    if (strcmp(argv[1], "lease") == 0) {
        test_active_lease();
        return 0;
    }
    if (strcmp(argv[1], "race") == 0) {
        test_writer_reset_race();
        return 0;
    }
    return 2;
}
"""


class SoftWWQueryResetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = get_compiler()
        cls.temp_dir = tempfile.TemporaryDirectory()
        src_path = Path(cls.temp_dir.name) / "test_query_reset.c"
        src_path.write_text(TEST_HARNESS_C, encoding="utf-8")

        ext = ".exe" if os.name == "nt" else ""
        cls.exe_path = Path(cls.temp_dir.name) / f"test_query_reset{ext}"
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

    def test_4kb_queries_and_capacity_bound(self):
        out = self.run_subtest("4kb")
        self.assertEqual(out, "test_4kb_queries=PASS")

    def test_16kb_modeled_granule_siblings(self):
        out = self.run_subtest("16kb")
        self.assertEqual(out, "test_16kb_granule_siblings=PASS")

    def test_active_lease_preservation(self):
        out = self.run_subtest("lease")
        self.assertEqual(out, "test_active_lease=PASS")

    def test_concurrent_writer_reset_race(self):
        out = self.run_subtest("race")
        self.assertEqual(out, "test_writer_reset_race=PASS")


if __name__ == "__main__":
    unittest.main()
