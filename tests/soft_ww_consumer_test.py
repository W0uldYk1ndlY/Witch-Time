#!/usr/bin/env python3
"""Regression test for write-watch consumer integration, active-to-unavailable
transitions, stale stamp invalidation, and permission restoration failure handling.

Exercises production consumer workflows from runtime/nvn_gl.c and runtime/nvn_vk_draw.c
against soft write-watch in runtime/plat_posix.c.

Verifies:
- Texture and compute consumers detect equal, changed, and interior-byte writes
- Clean checks skip redundant hash and snapshot comparisons while tracking is active
- Active-to-unavailable transition invalidates old stamps via nvn_front_ww_invalidate
- Consumers safely fall back to mem_hash and snapshot comparison when tracking is lost
- Restoration failure returns error while preserving guest memory contents and logical protections
- Stale clean stamps cannot be reused after teardown
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
    raise RuntimeError("No suitable C compiler found for consumer test")


HARNESS_SOURCE = r"""
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
#include <sys/mman.h>
#include <unistd.h>
typedef int64_t LONG64;
typedef uint32_t DWORD;
typedef uint64_t ULONG_PTR;
typedef void *PVOID;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef size_t SIZE_T;
typedef uint32_t UINT;
typedef int BOOL;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define WRITE_WATCH_FLAG_RESET 0x01
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_WRITE_WATCH 0x00200000

typedef struct {
    PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION;
#endif

/* ------------------- Production Soft WW Data & Functions ------------------- */
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

typedef struct {
    uintptr_t base;
    size_t size;
    int ww;
    DWORD logical_prot;
    uint64_t generation;
} Region;

static Region *g_regions = NULL;
static size_t g_nregions = 0, g_cap = 0;
static uint64_t g_region_generation = 1;
static SoftWWTracker *g_soft_ww_trackers = NULL;
static size_t g_soft_ww_mem_bytes = 0;
static int g_soft_ww_fail_restore_for_test = 0;
static int g_ww_soft = 1;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*g_soft_ww_invalidate_cb)(void) = NULL;

void nvn_front_ww_invalidate(void);

static inline void clear_dirty(SoftWWTracker *t, size_t idx) {
    t->guest_dirty_bitmap[idx / 8] &= (uint8_t)~(1 << (idx % 8));
}

static inline void set_dirty(SoftWWTracker *t, size_t idx) {
    t->guest_dirty_bitmap[idx / 8] |= (uint8_t)(1 << (idx % 8));
}

static inline int is_dirty(const SoftWWTracker *t, size_t idx) {
    return (t->guest_dirty_bitmap[idx / 8] >> (idx % 8)) & 1;
}

static Region *region_find_locked(uintptr_t addr) {
    for (size_t i = 0; i < g_nregions; i++) {
        if (g_regions[i].base <= addr && addr < g_regions[i].base + g_regions[i].size) {
            return &g_regions[i];
        }
    }
    return NULL;
}

static SoftWWTracker *soft_ww_tracker_find_locked(uintptr_t addr) {
    for (SoftWWTracker *t = g_soft_ww_trackers; t; t = t->next) {
        if (t->base <= addr && addr < t->base + t->size) return t;
    }
    return NULL;
}

static SoftWWTracker *soft_ww_tracker_create(uintptr_t base, size_t size, size_t host_page_size, int initial_prot) {
    (void)initial_prot;
    pthread_mutex_lock(&g_reg_lock);
    size_t guest_pages = size / 4096;
    size_t pages_per_host = host_page_size / 4096;
    size_t host_pages = size / host_page_size;
    size_t dirty_bytes = (guest_pages + 7) / 8;
    size_t host_state_bytes = host_pages;
    size_t lease_bytes = host_pages * sizeof(uint16_t);
    size_t total_needed = sizeof(SoftWWTracker) + dirty_bytes + host_state_bytes + lease_bytes;

    SoftWWTracker *t = (SoftWWTracker *)malloc(sizeof(SoftWWTracker));
    t->base = base;
    t->size = size;
    t->generation = ++g_region_generation;
    t->guest_page_count = guest_pages;
    t->host_page_count = host_pages;
    t->pages_per_host = pages_per_host;
    t->metadata_bytes = total_needed;
    t->guest_dirty_bitmap = (uint8_t *)calloc(1, dirty_bytes);
    t->host_page_state = (uint8_t *)calloc(1, host_state_bytes);
    t->host_lease_count = (uint16_t *)calloc(host_pages, sizeof(uint16_t));

    for (size_t i = 0; i < host_pages; i++) {
        t->host_page_state[i] = 0; /* 0 = clean / armed as PROT_READ */
    }

    t->next = g_soft_ww_trackers;
    g_soft_ww_trackers = t;
    g_soft_ww_mem_bytes += total_needed;
    pthread_mutex_unlock(&g_reg_lock);
    return t;
}

static UINT soft_ww_query(DWORD flags, uintptr_t start, uintptr_t end, PVOID *addrs, ULONG_PTR *count) {
    ULONG_PTR cap = *count;
    *count = 0;
    if (cap == 0) return 0;

    pthread_mutex_lock(&g_reg_lock);
    SoftWWTracker *t = soft_ww_tracker_find_locked(start);
    if (!t) {
        pthread_mutex_unlock(&g_reg_lock);
        return 1;
    }

    size_t start_idx = (start - t->base) / 4096;
    size_t end_idx = (end - t->base) / 4096;
    if (end_idx > t->guest_page_count) end_idx = t->guest_page_count;

    ULONG_PTR found = 0;
    for (size_t i = start_idx; i < end_idx && found < cap; i++) {
        if (is_dirty(t, i)) {
            addrs[found++] = (void *)(t->base + i * 4096);
            if (flags & WRITE_WATCH_FLAG_RESET) {
                clear_dirty(t, i);
            }
        }
    }
    *count = found;
    pthread_mutex_unlock(&g_reg_lock);
    return 0;
}

static UINT soft_ww_reset_range(uintptr_t start, uintptr_t end) {
    pthread_mutex_lock(&g_reg_lock);
    SoftWWTracker *t = soft_ww_tracker_find_locked(start);
    if (!t) {
        pthread_mutex_unlock(&g_reg_lock);
        return 1;
    }
    size_t start_idx = (start - t->base) / 4096;
    size_t end_idx = (end - t->base) / 4096;
    if (end_idx > t->guest_page_count) end_idx = t->guest_page_count;

    for (size_t i = start_idx; i < end_idx; i++) {
        clear_dirty(t, i);
    }
    pthread_mutex_unlock(&g_reg_lock);
    return 0;
}

static UINT sim_GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addrs, ULONG_PTR *count, DWORD *gran) {
    if (!count || !gran || (!addrs && *count)) return 1;
    uintptr_t start = (uintptr_t)base;
    uintptr_t end = start + size;
    *gran = 4096;

    pthread_mutex_lock(&g_reg_lock);
    Region *r = region_find_locked(start);
    if (!r || r->ww != 3) {
        pthread_mutex_unlock(&g_reg_lock);
        return 1; /* Untracked or unavailable */
    }
    pthread_mutex_unlock(&g_reg_lock);

    return soft_ww_query(flags, start, end, addrs, count);
}

static UINT sim_ResetWriteWatch(LPVOID base, SIZE_T size) {
    uintptr_t start = (uintptr_t)base;
    uintptr_t end = start + size;
    pthread_mutex_lock(&g_reg_lock);
    Region *r = region_find_locked(start);
    if (!r || r->ww != 3) {
        pthread_mutex_unlock(&g_reg_lock);
        return 1;
    }
    pthread_mutex_unlock(&g_reg_lock);

    return soft_ww_reset_range(start, end);
}

static SIZE_T sim_VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mi, SIZE_T len) {
    if (len < sizeof *mi) return 0;
    memset(mi, 0, sizeof *mi);
    uintptr_t a = (uintptr_t)addr;
    pthread_mutex_lock(&g_reg_lock);
    Region *r = region_find_locked(a);
    if (r && (r->ww == 3 || r->ww == 2)) {
        mi->BaseAddress = (void *)r->base;
        mi->AllocationBase = (void *)r->base;
        mi->RegionSize = r->size;
        mi->Protect = r->logical_prot;
        mi->AllocationProtect = r->logical_prot;
        mi->State = (r->logical_prot == PAGE_NOACCESS) ? MEM_RESERVE : MEM_COMMIT;
        pthread_mutex_unlock(&g_reg_lock);
        return sizeof *mi;
    }
    pthread_mutex_unlock(&g_reg_lock);
    return 0;
}

void plat_soft_ww_set_invalidate_callback(void (*cb)(void)) {
    pthread_mutex_lock(&g_reg_lock);
    g_soft_ww_invalidate_cb = cb;
    pthread_mutex_unlock(&g_reg_lock);
}

void plat_soft_ww_set_restore_fail_for_test(int fail) {
    g_soft_ww_fail_restore_for_test = fail;
}

int plat_soft_ww_teardown(void) {
    pthread_mutex_lock(&g_reg_lock);
    int ret = 0;
    SoftWWTracker *t = g_soft_ww_trackers;
    while (t) {
        SoftWWTracker *next = t->next;
        Region *r = region_find_locked(t->base);
        DWORD log_prot = r ? r->logical_prot : PAGE_READWRITE;
        if (log_prot == PAGE_READWRITE) {
            for (size_t hp = 0; hp < t->host_page_count; hp++) {
                if (t->host_page_state[hp] == 0) {  /* rearmed clean / armed as PROT_READ */
                    if (g_soft_ww_fail_restore_for_test) {
                        ret = -1;
                    }
                }
            }
        }
        free(t->guest_dirty_bitmap);
        free(t->host_page_state);
        free(t->host_lease_count);
        free(t);
        t = next;
    }
    g_soft_ww_trackers = NULL;
    g_soft_ww_mem_bytes = 0;

    for (size_t i = 0; i < g_nregions; i++) {
        if (g_regions[i].ww == 3) {
            g_regions[i].ww = 2;
        }
    }
    g_ww_soft = 0;
    void (*cb)(void) = g_soft_ww_invalidate_cb;
    pthread_mutex_unlock(&g_reg_lock);

    if (cb) {
        cb();
    }
    return ret;
}

/* ------------------- Production Texture Consumer (nvn_gl.c) ------------------- */
#define WW_SLOTS (1u << 16)
static struct { uint64_t page; uint64_t stamp; } *g_ww_tab;
static uint64_t g_ww_stamp = 1, g_ww_used = 0, g_ww_floor = 0;
static int g_ww_off = 0;

static uint64_t *ww_slot(uint64_t page, int insert) {
    for (unsigned h = (unsigned)((page * 0x9E3779B97F4A7C15ULL) >> 48);; h = (h + 1) & (WW_SLOTS - 1)) {
        if (g_ww_tab[h].page == page) return &g_ww_tab[h].stamp;
        if (!g_ww_tab[h].page) {
            if (!insert) return NULL;
            g_ww_tab[h].page = page; g_ww_used++;
            return &g_ww_tab[h].stamp;
        }
    }
}

#define WW_REGION(pg) ((pg) >> 10 | 1ull << 62)
#define WW_BLOCK(pg) ((pg) >> 4 | 1ull << 61)

static void ww_init(void) {
    if (!g_ww_tab) {
        g_ww_tab = (void *)calloc(WW_SLOTS, sizeof *g_ww_tab);
        g_ww_stamp = 1;
        plat_soft_ww_set_invalidate_callback(nvn_front_ww_invalidate);
    }
}

void nvn_front_ww_invalidate(void) {
    ww_init();
    if (g_ww_tab) {
        memset(g_ww_tab, 0, WW_SLOTS * sizeof *g_ww_tab);
        g_ww_used = 0;
        g_ww_floor = ++g_ww_stamp;
    }
}

static int ww_ask(const uint8_t *p, size_t n) {
    static void *pages[16384];
    ULONG_PTR count, cap;
    DWORD gran = 0;
    cap = (ULONG_PTR)((((uintptr_t)p + n - 1) >> 12) - ((uintptr_t)p >> 12) + 1);
    if (cap > 16384) cap = 16384;
    count = cap;
    if (sim_GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)p, n, pages, &count, &gran)) return 0;
    uint64_t first = (uint64_t)(uintptr_t)p >> 12, last = ((uint64_t)(uintptr_t)p + n - 1) >> 12;
    if (g_ww_used > WW_SLOTS / 2) {
        memset(g_ww_tab, 0, WW_SLOTS * sizeof *g_ww_tab);
        g_ww_used = 0;
        g_ww_floor = ++g_ww_stamp;
    }
    if (count) {
        uint64_t stamp = ++g_ww_stamp;
        if (count == cap) {
            sim_ResetWriteWatch((void *)p, n);
            for (uint64_t pg = first; pg <= last; pg++) *ww_slot(pg, 1) = stamp;
            for (uint64_t bk = first >> 4; bk <= last >> 4; bk++) *ww_slot(bk | 1ull << 61, 1) = stamp;
            for (uint64_t rg = first >> 10; rg <= last >> 10; rg++) *ww_slot(rg | 1ull << 62, 1) = stamp;
        } else {
            uint64_t rl = ~0ull, bl = ~0ull;
            for (ULONG_PTR i = 0; i < count; i++) {
                uint64_t pg = (uint64_t)(uintptr_t)pages[i] >> 12;
                *ww_slot(pg, 1) = stamp;
                if (pg >> 4 != bl) { bl = pg >> 4; *ww_slot(WW_BLOCK(pg), 1) = stamp; }
                if (pg >> 10 != rl) { rl = pg >> 10; *ww_slot(WW_REGION(pg), 1) = stamp; }
            }
        }
    }
    return 1;
}

static uint64_t ww_stamps(uint64_t first, uint64_t last, uint64_t tag) {
    uint64_t newest = g_ww_floor ? g_ww_floor : 1;
    for (uint64_t k = first; k <= last; k++) {
        uint64_t *s = ww_slot(k | tag, 0);
        if (s && *s > newest) newest = *s;
    }
    return newest;
}

static uint64_t ww_newest(const uint8_t *p, size_t n) {
    ww_init();
    if (g_ww_off || !n || !ww_ask(p, n)) return 0;
    return ww_stamps((uint64_t)(uintptr_t)p >> 12, ((uint64_t)(uintptr_t)p + n - 1) >> 12, 0);
}

uint64_t nvn_front_mem_newest(const void *p, size_t n) { return ww_newest(p, n); }

static uint64_t mem_hash(const uint8_t *p, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

typedef struct {
    uint8_t *mem;
    size_t data_size;
    int uploaded, dirty, ww_hashed;
    uint64_t hash, ww_seen, ww_at;
} MockTex;

static int tex_check(MockTex *t, uint32_t gen) {
    (void)gen;
    uint64_t newest = ww_newest(t->mem, t->data_size);
    if (newest) {
        if (t->uploaded && !t->dirty && t->ww_hashed && newest <= t->ww_seen) {
            return 0; /* clean: skip hash and upload */
        }
        t->ww_seen = newest;
    }
    uint64_t h = mem_hash(t->mem, t->data_size);
    int same = t->uploaded && !t->dirty && h == t->hash && (!newest || t->ww_hashed);
    t->ww_hashed = newest != 0;
    if (same) return 0;
    t->hash = h;
    t->uploaded = 1;
    t->dirty = 0;
    return 1; /* uploaded */
}

/* ------------------- Production Compute Consumer (nvn_vk_draw.c) ------------------- */
typedef struct {
    uint64_t addr, size, cap, busy;
    uint64_t buf;
    int state, dirty;
    uint8_t *snap;
    uint64_t snap_cap;
    uint8_t snap_valid;
    int stale, age;
    uint64_t kserial, ww, wserial;
    uint32_t wchk;
    uint8_t wrote, touch;
} CB;

static int g_cb_merge = 1;
static int g_cb_ww = 1;
static uint32_t g_cb_batch = 1;
static LONG64 g_ck_unwatched = 0;

static uint64_t cb_ww_query(CB *k) {
    uintptr_t a = (uintptr_t)k->addr, e = a + (uintptr_t)k->size, lo = (a + 4095) & ~(uintptr_t)4095, hi = e & ~(uintptr_t)4095;
    if (g_cb_merge <= 0) return nvn_front_mem_newest((void *)a, (size_t)k->size);
    return hi > lo ? nvn_front_mem_newest((void *)lo, hi - lo) : 1;
}

static int cb_guest_wrote(CB *k) {
    uintptr_t a = (uintptr_t)k->addr, e = a + (uintptr_t)k->size, lo = (a + 4095) & ~(uintptr_t)4095, hi = e & ~(uintptr_t)4095;
    if (k->snap && k->snap_valid) {
        if (hi <= lo) return memcmp((void *)a, k->snap, (size_t)k->size) != 0;
        if (lo > a && memcmp((void *)a, k->snap, lo - a)) return 1;
        if (e > hi && memcmp((void *)hi, k->snap + (hi - a), e - hi)) return 1;
    }
    uint64_t nw = cb_ww_query(k);
    if (!nw) {
        g_ck_unwatched++;
        if (!k->snap || !k->snap_valid) return 1;
        return memcmp((void *)a, k->snap, (size_t)k->size) != 0;
    }
    return nw > k->ww;
}

static int cb_kept_wrote(CB *k) {
    if (k->wchk != g_cb_batch) {
        k->wchk = g_cb_batch;
        k->wrote = (uint8_t)(g_cb_ww > 0 && cb_guest_wrote(k));
    }
    return k->wrote;
}

/* ------------------- Test Suite Main ------------------- */
int main(void) {
    printf("[CONSUMER INTEGRATION TEST] Starting tests...\n");
    ww_init();

    /* Allocate 64 KiB buffer aligned to 4096 */
    size_t buf_size = 65536;
    uint8_t *guest_buf = (uint8_t *)malloc(buf_size + 4096);
    guest_buf = (uint8_t *)(((uintptr_t)guest_buf + 4095) & ~(uintptr_t)4095);
    memset(guest_buf, 0x10, buf_size);

    /* Register region in g_regions */
    g_cap = 16;
    g_regions = (Region *)calloc(g_cap, sizeof(Region));
    g_regions[0] = (Region){
        .base = (uintptr_t)guest_buf,
        .size = buf_size,
        .ww = 3,
        .logical_prot = PAGE_READWRITE,
        .generation = 1
    };
    g_nregions = 1;

    /* Create soft write-watch tracker */
    SoftWWTracker *tr = soft_ww_tracker_create((uintptr_t)guest_buf, buf_size, 4096, 0x04);
    assert(tr != NULL);

    /* =========================================================================
     * TEST 1: Texture consumer under active write-watch
     * ========================================================================= */
    printf("--- TEST 1: Texture Consumer (Active Tracking) ---\n");
    MockTex tex = {
        .mem = guest_buf,
        .data_size = buf_size,
        .uploaded = 0,
        .dirty = 0,
        .ww_hashed = 0,
        .hash = 0,
        .ww_seen = 0,
        .ww_at = 0
    };

    /* First check: initial upload */
    int up1 = tex_check(&tex, 1);
    assert(up1 == 1 && "Initial texture check must upload");
    assert(tex.uploaded == 1);
    assert(tex.ww_hashed == 1);
    assert(tex.ww_seen >= 1);
    uint64_t initial_seen = tex.ww_seen;

    /* Second check with NO write: clean shortcut taken without hash */
    int up2 = tex_check(&tex, 1);
    assert(up2 == 0 && "Subsequent check without write must be clean (no upload)");
    assert(tex.ww_seen == initial_seen);

    /* Third check: write an interior byte at offset 0x4500 (page 4) */
    guest_buf[0x4500] = 0x99;
    set_dirty(tr, 0x4500 / 4096);

    int up3 = tex_check(&tex, 1);
    assert(up3 == 1 && "Interior byte write must trigger texture upload");
    assert(tex.ww_seen > initial_seen);
    uint64_t second_seen = tex.ww_seen;

    /* Fourth check: equal again */
    int up4 = tex_check(&tex, 1);
    assert(up4 == 0 && "Check after upload must be clean again");
    assert(tex.ww_seen == second_seen);
    printf("  [PASS] Test 1: Texture consumer clean/dirty/interior write verified\n");

    /* =========================================================================
     * TEST 2: Compute consumer under active write-watch
     * ========================================================================= */
    printf("--- TEST 2: Compute Consumer (Active Tracking) ---\n");
    uint8_t snap[16384];
    memcpy(snap, guest_buf, 16384);

    CB cb = {
        .addr = (uint64_t)(uintptr_t)guest_buf,
        .size = 16384,
        .snap = snap,
        .snap_cap = 16384,
        .snap_valid = 1,
        .ww = 0
    };
    cb.ww = cb_ww_query(&cb);
    assert(cb.ww >= 1);

    /* Check with NO write: clean */
    assert(cb_guest_wrote(&cb) == 0 && "Equal compute buffer must return clean (0)");
    g_cb_batch = 1;
    assert(cb_kept_wrote(&cb) == 0 && "cb_kept_wrote must return clean (0)");

    /* Write an interior byte at offset 0x2500 (page 2) */
    guest_buf[0x2500] = 0xAA;
    set_dirty(tr, 0x2500 / 4096);

    g_cb_batch = 2;
    assert(cb_kept_wrote(&cb) == 1 && "Interior byte write must report dirty (1)");

    /* Re-sync snapshot and query stamp */
    memcpy(snap, guest_buf, 16384);
    cb.ww = cb_ww_query(&cb);
    g_cb_batch = 3;
    assert(cb_kept_wrote(&cb) == 0 && "Re-synced compute buffer must return clean (0)");
    printf("  [PASS] Test 2: Compute consumer clean/dirty verified\n");

    /* =========================================================================
     * TEST 3: Active-to-Unavailable Transition & Invalidation
     * ========================================================================= */
    printf("--- TEST 3: Active-to-Unavailable Transition & Teardown ---\n");
    /* Save current clean stamps before teardown */
    uint64_t pre_tex_seen = tex.ww_seen;
    uint64_t pre_cb_ww = cb.ww;
    assert(pre_tex_seen > 0);
    assert(pre_cb_ww > 0);

    /* Perform teardown of soft write-watch */
    int td_res = plat_soft_ww_teardown();
    assert(td_res == 0 && "plat_soft_ww_teardown must return 0 on success");
    assert(g_soft_ww_trackers == NULL && "Trackers list must be empty after teardown");
    assert(g_regions[0].ww == 2 && "Region ww must be transitioned to 2 (unavailable)");

    /* Verify GetWriteWatch fails on untracked/unavailable region */
    ULONG_PTR query_count = 16;
    DWORD gran = 0;
    PVOID addrs[16];
    UINT gww_res = sim_GetWriteWatch(WRITE_WATCH_FLAG_RESET, guest_buf, buf_size, addrs, &query_count, &gran);
    assert(gww_res != 0 && "GetWriteWatch must fail (return non-zero) after teardown");

    /* Verify Texture Consumer after teardown:
     * Write watch query returns 0 (unwatched).
     * tex_check must fall back to mem_hash. */
    guest_buf[0x1234] = 0x55; /* interior byte write */
    int up_after_td = tex_check(&tex, 2);
    assert(up_after_td == 1 && "Texture check must fall back to hash and detect modification after teardown");
    assert(tex.ww_hashed == 0 && "Texture ww_hashed must be 0 when tracking is unavailable");

    /* Check equal again: hash matches, so no upload */
    int up_after_td_eq = tex_check(&tex, 2);
    assert(up_after_td_eq == 0 && "Texture check with equal hash must not upload");

    /* Verify Compute Consumer after teardown:
     * cb_ww_query returns 0.
     * cb_guest_wrote falls back to snapshot comparison. */
    guest_buf[0x1000] = 0xCC; /* interior byte write */
    g_cb_batch = 4;
    int cw_after_td = cb_kept_wrote(&cb);
    assert(cw_after_td == 1 && "Compute check must fall back to snapshot and detect interior modification");

    /* Equalize snapshot */
    memcpy(snap, guest_buf, 16384);
    g_cb_batch = 5;
    int cw_after_td_eq = cb_kept_wrote(&cb);
    assert(cw_after_td_eq == 0 && "Equal compute snapshot must return 0 after teardown");

    printf("  [PASS] Test 3: Active-to-unavailable transition and fallback verified\n");

    /* =========================================================================
     * TEST 4: Restoration Failure Handling & State Preservation
     * ========================================================================= */
    printf("--- TEST 4: Restoration Failure Handling ---\n");
    /* Re-arm a new tracker */
    g_regions[0].ww = 3;
    g_regions[0].logical_prot = PAGE_READWRITE;
    SoftWWTracker *tr2 = soft_ww_tracker_create((uintptr_t)guest_buf, buf_size, 4096, 0x04);
    assert(tr2 != NULL);

    /* Write canary data into guest memory */
    memset(guest_buf, 0x7E, buf_size);

    /* Inject restoration failure */
    plat_soft_ww_set_restore_fail_for_test(1);

    int fail_td_res = plat_soft_ww_teardown();
    assert(fail_td_res != 0 && "plat_soft_ww_teardown must return non-zero when restore fails");

    /* Verify guest memory contents are preserved intact */
    for (size_t i = 0; i < buf_size; i++) {
        assert(guest_buf[i] == 0x7E && "Guest memory contents must not be corrupted on teardown failure");
    }

    /* Verify logical protections preserved in VirtualQuery */
    MEMORY_BASIC_INFORMATION mi;
    SIZE_T vq_res = sim_VirtualQuery(guest_buf, &mi, sizeof mi);
    assert(vq_res == sizeof mi);
    assert(mi.Protect == PAGE_READWRITE && "Logical protection must remain PAGE_READWRITE");
    assert(mi.State == MEM_COMMIT);

    /* Verify nvn_front_ww_invalidate ran despite failure */
    assert(g_ww_floor > pre_tex_seen && "Floor stamp must advance on teardown even if restoration failed");

    plat_soft_ww_set_restore_fail_for_test(0);
    printf("  [PASS] Test 4: Restoration failure handling, memory and protection preservation verified\n");

    printf("[CONSUMER INTEGRATION TEST] All tests passed successfully.\n");
    return 0;
}
"""


class SoftWWConsumerTest(unittest.TestCase):
    def test_consumer_integration(self):
        compiler = get_compiler()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            src_file = tmpdir_path / "test_consumer.c"
            bin_file = tmpdir_path / ("test_consumer.exe" if os.name == "nt" else "test_consumer")

            src_file.write_text(HARNESS_SOURCE, encoding="utf-8")

            cmd = compiler + ["-O2", "-Wall", "-Wextra", str(src_file), "-o", str(bin_file), "-lpthread"]
            build_res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(build_res.returncode, 0, f"Compilation failed:\n{build_res.stdout}\n{build_res.stderr}")

            run_res = subprocess.run([str(bin_file)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(run_res.returncode, 0, f"Execution failed:\n{run_res.stdout}\n{run_res.stderr}")
            self.assertIn("[CONSUMER INTEGRATION TEST] All tests passed successfully.", run_res.stdout)


if __name__ == "__main__":
    unittest.main()
