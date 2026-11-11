#include "diag.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#if defined(__GNUC__) || defined(__clang__)
#define DIAG_ATOMIC_LOAD(p, order) __atomic_load_n((p), (order))
#define DIAG_ATOMIC_STORE(p, v, order) __atomic_store_n((p), (v), (order))
#define DIAG_ATOMIC_ADD(p, v, order) __atomic_fetch_add((p), (v), (order))
#define DIAG_ATOMIC_CAS(p, expected_p, desired, succ, fail) \
    __atomic_compare_exchange_n((p), (expected_p), (desired), 0, (succ), (fail))
#define DIAG_ORDER_RELAXED __ATOMIC_RELAXED
#define DIAG_ORDER_ACQUIRE __ATOMIC_ACQUIRE
#define DIAG_ORDER_RELEASE __ATOMIC_RELEASE
#define DIAG_ORDER_SEQ_CST __ATOMIC_SEQ_CST
#else
#include <stdatomic.h>
#define DIAG_ATOMIC_LOAD(p, order) atomic_load_explicit((p), (order))
#define DIAG_ATOMIC_STORE(p, v, order) atomic_store_explicit((p), (v), (order))
#define DIAG_ATOMIC_ADD(p, v, order) atomic_fetch_add_explicit((p), (v), (order))
#define DIAG_ATOMIC_CAS(p, expected_p, desired, succ, fail) \
    atomic_compare_exchange_strong_explicit((p), (expected_p), (desired), (succ), (fail))
#define DIAG_ORDER_RELAXED memory_order_relaxed
#define DIAG_ORDER_ACQUIRE memory_order_acquire
#define DIAG_ORDER_RELEASE memory_order_release
#define DIAG_ORDER_SEQ_CST memory_order_seq_cst
#endif

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

/* Producer SPSC ring buffer structure */
typedef struct diag_spsc_ring {
    /* Producer cache-line */
    uint32_t head;
    uint32_t generation;
    uint32_t active;
    uint32_t thread_id;
    uint64_t emitted_count;
    uint64_t dropped_count;
    char     name[32];
    uint8_t  _pad1[64 - ((sizeof(uint32_t) * 4) + (sizeof(uint64_t) * 2) + 32)];

    /* Consumer cache-line */
    uint32_t tail;
    uint8_t  _pad2[64 - sizeof(uint32_t)];

    /* Records payload buffer: 1024 * 64 bytes = 64 KiB */
    diag_record_t buffer[DIAG_RING_CAPACITY];
} diag_spsc_ring_t;

static uint32_t g_diag_enabled = 0;
static uint64_t g_diag_global_seq = 1;
static uint64_t g_diag_total_emitted = 0;
static uint64_t g_diag_total_dropped = 0;
static char g_diag_run_id[128] = {0};
static int g_diag_gfx_prod = -1;
static int g_diag_fs_prod = -1;
static int g_diag_audio_prod = -1;

/* Continuous atomic counters state */
static uint64_t g_cont_wait_events = 0;
static uint64_t g_cont_wait_ns = 0;
static int64_t  g_cont_shader_queued = 0;
static int64_t  g_cont_shader_completed = 0;
static int64_t  g_cont_shader_active_workers = 0;
static int64_t  g_cont_shader_avg_compile_ns = 0;
static uint64_t g_cont_file_ops = 0;
static uint64_t g_cont_file_duration_ns = 0;
static uint64_t g_cont_file_bytes = 0;
static uint64_t g_cont_mem_compare_bytes = 0;
static uint64_t g_cont_mem_copy_bytes = 0;
static uint64_t g_cont_mem_stream_bytes = 0;
static int64_t  g_cont_mem_allocated_bytes = 0;
static int64_t  g_cont_mem_peak_allocated_bytes = 0;
static int64_t  g_cont_audio_appended = 0;
static int64_t  g_cont_audio_released = 0;
static int64_t  g_cont_audio_underruns = 0;
static int64_t  g_cont_audio_starved = 0;

/* Live state variables */
static diag_lifecycle_state_t g_lifecycle_state = DIAG_LIFECYCLE_UNKNOWN;
static uint32_t g_surface_gen = 0;
static float g_thermal_headroom = -1.0f;
static diag_thermal_status_t g_thermal_status = DIAG_THERMAL_STATUS_UNSUPPORTED;
static uint64_t g_thermal_poll_timestamp_ns = 0;
static uint64_t g_last_render_timestamp_ns = 0;

/* Ring buffers for all producers */
static diag_spsc_ring_t g_diag_rings[DIAG_MAX_PRODUCERS];

static inline uint64_t diag_timestamp_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int freq_init = 0;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    if (freq.QuadPart > 0) {
        return (uint64_t)((counter.QuadPart * 1000000000ULL) / freq.QuadPart);
    }
    return 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static inline uint32_t diag_get_tid(void) {
#if defined(__ANDROID__)
    return (uint32_t)gettid();
#elif defined(_WIN32)
    return (uint32_t)GetCurrentThreadId();
#elif defined(__linux__)
    return (uint32_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (uint32_t)tid;
#else
    return 1;
#endif
}

bool diag_is_enabled(void) {
    return DIAG_ATOMIC_LOAD(&g_diag_enabled, DIAG_ORDER_RELAXED) != 0;
}

uint64_t diag_time_ns(void) {
    return diag_timestamp_ns();
}

int diag_init(const char *run_id) {
    if (run_id) {
        strncpy(g_diag_run_id, run_id, sizeof(g_diag_run_id) - 1);
        g_diag_run_id[sizeof(g_diag_run_id) - 1] = '\0';
    } else {
        g_diag_run_id[0] = '\0';
    }

    DIAG_ATOMIC_STORE(&g_diag_global_seq, 1, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_diag_total_emitted, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_diag_total_dropped, 0, DIAG_ORDER_RELAXED);
    diag_reset_continuous_counters();
    diag_reset_live_state();

    for (int i = 0; i < DIAG_MAX_PRODUCERS; i++) {
        diag_spsc_ring_t *ring = &g_diag_rings[i];
        DIAG_ATOMIC_STORE(&ring->head, 0, DIAG_ORDER_RELAXED);
        DIAG_ATOMIC_STORE(&ring->tail, 0, DIAG_ORDER_RELAXED);
        DIAG_ATOMIC_STORE(&ring->active, 0, DIAG_ORDER_RELAXED);
        ring->generation = 0;
        ring->thread_id = 0;
        ring->emitted_count = 0;
        ring->dropped_count = 0;
        ring->name[0] = '\0';
    }

    DIAG_ATOMIC_STORE(&g_diag_enabled, 1, DIAG_ORDER_SEQ_CST);
    g_diag_gfx_prod = diag_register_producer("gfx");
    g_diag_fs_prod = diag_register_producer("fs");
    g_diag_audio_prod = diag_register_producer("audio");
    return 0;
}

void diag_shutdown(void) {
    DIAG_ATOMIC_STORE(&g_diag_enabled, 0, DIAG_ORDER_SEQ_CST);
    g_diag_gfx_prod = -1;
    g_diag_fs_prod = -1;
    g_diag_audio_prod = -1;
    for (int i = 0; i < DIAG_MAX_PRODUCERS; i++) {
        diag_spsc_ring_t *ring = &g_diag_rings[i];
        DIAG_ATOMIC_STORE(&ring->active, 0, DIAG_ORDER_RELEASE);
    }
}

int diag_register_producer(const char *name) {
    if (!diag_is_enabled()) {
        return -1;
    }
    uint32_t tid = diag_get_tid();
    for (int i = 0; i < DIAG_MAX_PRODUCERS; i++) {
        diag_spsc_ring_t *ring = &g_diag_rings[i];
        uint32_t expected = 0;
        if (DIAG_ATOMIC_CAS(&ring->active, &expected, 1, DIAG_ORDER_SEQ_CST, DIAG_ORDER_RELAXED)) {
            ring->generation++;
            ring->thread_id = tid;
            DIAG_ATOMIC_STORE(&ring->head, 0, DIAG_ORDER_RELAXED);
            DIAG_ATOMIC_STORE(&ring->tail, 0, DIAG_ORDER_RELAXED);
            ring->emitted_count = 0;
            ring->dropped_count = 0;
            if (name) {
                strncpy(ring->name, name, sizeof(ring->name) - 1);
                ring->name[sizeof(ring->name) - 1] = '\0';
            } else {
                ring->name[0] = '\0';
            }
            return i;
        }
    }
    return -1; /* All producer slots full */
}

void diag_unregister_producer(int producer_id) {
    if (producer_id < 0 || producer_id >= DIAG_MAX_PRODUCERS) {
        return;
    }
    diag_spsc_ring_t *ring = &g_diag_rings[producer_id];
    ring->generation++;
    DIAG_ATOMIC_STORE(&ring->active, 0, DIAG_ORDER_RELEASE);
}

void diag_emit(int producer_id, uint32_t type, int64_t p0, int64_t p1, int64_t p2, int64_t p3) {
    if (!diag_is_enabled()) {
        return;
    }
    if (producer_id < 0 || producer_id >= DIAG_MAX_PRODUCERS) {
        return;
    }
    diag_spsc_ring_t *ring = &g_diag_rings[producer_id];
    if (!DIAG_ATOMIC_LOAD(&ring->active, DIAG_ORDER_RELAXED)) {
        return;
    }

    uint32_t head = DIAG_ATOMIC_LOAD(&ring->head, DIAG_ORDER_RELAXED);
    uint32_t tail = DIAG_ATOMIC_LOAD(&ring->tail, DIAG_ORDER_ACQUIRE);
    uint32_t next_head = (head + 1) & (DIAG_RING_CAPACITY - 1);

    if (next_head == tail) {
        /* Drop-new semantics: non-blocking loss on full ring */
        DIAG_ATOMIC_ADD(&ring->dropped_count, 1, DIAG_ORDER_RELAXED);
        DIAG_ATOMIC_ADD(&g_diag_total_dropped, 1, DIAG_ORDER_RELAXED);
        return;
    }

    uint64_t seq = DIAG_ATOMIC_ADD(&g_diag_global_seq, 1, DIAG_ORDER_RELAXED);

    /* Write record completely BEFORE updating head */
    diag_record_t *rec = &ring->buffer[head];
    rec->seq = seq;
    rec->timestamp_ns = diag_timestamp_ns();
    rec->type = type;
    rec->producer_id = (uint32_t)producer_id;
    rec->thread_id = ring->thread_id;
    rec->generation = ring->generation;
    rec->payload[0] = p0;
    rec->payload[1] = p1;
    rec->payload[2] = p2;
    rec->payload[3] = p3;

    /* Release store ensures record is completely written before visible to consumer */
    DIAG_ATOMIC_STORE(&ring->head, next_head, DIAG_ORDER_RELEASE);
    DIAG_ATOMIC_ADD(&ring->emitted_count, 1, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_ADD(&g_diag_total_emitted, 1, DIAG_ORDER_RELAXED);
}

size_t diag_drain_producer(int producer_id, diag_record_t *out_records, size_t max_count) {
    if (producer_id < 0 || producer_id >= DIAG_MAX_PRODUCERS || !out_records || max_count == 0) {
        return 0;
    }
    diag_spsc_ring_t *ring = &g_diag_rings[producer_id];

    uint32_t tail = DIAG_ATOMIC_LOAD(&ring->tail, DIAG_ORDER_RELAXED);
    uint32_t head = DIAG_ATOMIC_LOAD(&ring->head, DIAG_ORDER_ACQUIRE);

    size_t count = 0;
    while (tail != head && count < max_count) {
        out_records[count] = ring->buffer[tail];
        tail = (tail + 1) & (DIAG_RING_CAPACITY - 1);
        count++;
    }

    if (count > 0) {
        DIAG_ATOMIC_STORE(&ring->tail, tail, DIAG_ORDER_RELEASE);
    }
    return count;
}

bool diag_get_producer_stats(int producer_id, diag_producer_stats_t *out_stats) {
    if (producer_id < 0 || producer_id >= DIAG_MAX_PRODUCERS || !out_stats) {
        return false;
    }
    diag_spsc_ring_t *ring = &g_diag_rings[producer_id];
    out_stats->emitted_count = DIAG_ATOMIC_LOAD(&ring->emitted_count, DIAG_ORDER_RELAXED);
    out_stats->dropped_count = DIAG_ATOMIC_LOAD(&ring->dropped_count, DIAG_ORDER_RELAXED);
    out_stats->generation = ring->generation;
    out_stats->active = DIAG_ATOMIC_LOAD(&ring->active, DIAG_ORDER_RELAXED);
    strncpy(out_stats->name, ring->name, sizeof(out_stats->name) - 1);
    out_stats->name[sizeof(out_stats->name) - 1] = '\0';
    return true;
}

uint64_t diag_get_total_emitted(void) {
    return DIAG_ATOMIC_LOAD(&g_diag_total_emitted, DIAG_ORDER_RELAXED);
}

uint64_t diag_get_total_dropped(void) {
    return DIAG_ATOMIC_LOAD(&g_diag_total_dropped, DIAG_ORDER_RELAXED);
}

void diag_hook_render_end(int64_t frame, int64_t draws, int64_t skipped, int64_t clears) {
    DIAG_ATOMIC_STORE(&g_last_render_timestamp_ns, diag_timestamp_ns(), DIAG_ORDER_RELAXED);
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_RENDER_END, frame, draws, skipped, clears);
}

void diag_hook_gpu_submit(int64_t slot, int64_t serial, int64_t flushes, int64_t cmd_count) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_GPU_SUBMIT, slot, serial, flushes, cmd_count);
}

void diag_hook_gpu_complete(int64_t slot, int64_t serial, int64_t wait_ticks, int64_t gpu_ns) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_GPU_COMPLETE, slot, serial, wait_ticks, gpu_ns);
}

void diag_hook_present_queued(int64_t frame, int64_t image_index, int64_t target_tick, int64_t is_fight) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_PRESENT_QUEUED, frame, image_index, target_tick, is_fight);
}

void diag_hook_present_called(int64_t frame, int64_t image_index, int64_t result_code, int64_t is_progress) {
    DIAG_ATOMIC_STORE(&g_last_render_timestamp_ns, diag_timestamp_ns(), DIAG_ORDER_RELAXED);
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_PRESENT_CALLED, frame, image_index, result_code, is_progress);
}

void diag_hook_present_discard(int64_t frame, int64_t image_index, int64_t reason, int64_t surface_gen) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_PRESENT_DISCARD, frame, image_index, reason, surface_gen);
}

void diag_hook_surface_gen(int64_t generation, int64_t width, int64_t height, int64_t format) {
    DIAG_ATOMIC_STORE(&g_surface_gen, (uint32_t)generation, DIAG_ORDER_RELAXED);
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_SURFACE_GEN, generation, width, height, format);
}

void diag_stream_frame_sample(int64_t frame, int64_t cpu_ns, int64_t wall_ns, int64_t flags) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_FRAME_SAMPLE, frame, cpu_ns, wall_ns, flags);
}

void diag_stream_gpu_sample(int64_t frame, int64_t gpu_ns, int64_t slot, int64_t status) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_GPU_SAMPLE, frame, gpu_ns, slot, status);
}

void diag_stream_domain_anchor(int64_t anchor_type, int64_t monotonic_ns, int64_t wall_ns, int64_t frame) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_DOMAIN_ANCHOR, anchor_type, monotonic_ns, wall_ns, frame);
}

void diag_hook_opt_ww(int64_t backend, int64_t faults, int64_t rearms, int64_t queries) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_OPT_WW_STATS, backend, faults, rearms, queries);
}

void diag_hook_opt_ww_mem(int64_t page_size, int64_t coverage_bytes, int64_t tracker_mem_bytes, int64_t leases) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_OPT_WW_MEM, page_size, coverage_bytes, tracker_mem_bytes, leases);
}

void diag_hook_opt_mem_inspect(int64_t hash_words_visited, int64_t snapshot_compare_bytes, int64_t copies, int64_t uploads) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_OPT_MEM_INSPECT, hash_words_visited, snapshot_compare_bytes, copies, uploads);
}

void diag_hook_opt_gpu_wait(int64_t required_serial, int64_t chosen_serial, int64_t wait_needed, int64_t slot) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_OPT_GPU_WAIT, required_serial, chosen_serial, wait_needed, slot);
}

void diag_hook_opt_clear_fold(int64_t eligible_clears, int64_t folded_clears, int64_t rejected_clears, int64_t attachment) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_OPT_CLEAR_FOLD, eligible_clears, folded_clears, rejected_clears, attachment);
}

void diag_hook_thread_role(uint32_t thread_id, uint32_t role, uint64_t name_hash) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_THREAD_ROLE, (int64_t)thread_id, (int64_t)role, (int64_t)name_hash, 0);
}

void diag_hook_thread_wait(uint32_t thread_id, uint32_t wait_type, int64_t wait_ns, int32_t result) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    if (wait_ns != DIAG_MEASUREMENT_UNAVAILABLE && wait_ns >= 0) {
        DIAG_ATOMIC_ADD(&g_cont_wait_events, 1, DIAG_ORDER_RELAXED);
        DIAG_ATOMIC_ADD(&g_cont_wait_ns, (uint64_t)wait_ns, DIAG_ORDER_RELAXED);
    }
    diag_emit(g_diag_gfx_prod, DIAG_EV_THREAD_WAIT, (int64_t)thread_id, (int64_t)wait_type, wait_ns, (int64_t)result);
}

void diag_hook_shader_queue(int64_t queued_jobs, int64_t completed_jobs, int64_t active_workers, int64_t avg_compile_ns) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    if (queued_jobs >= 0) DIAG_ATOMIC_STORE(&g_cont_shader_queued, queued_jobs, DIAG_ORDER_RELAXED);
    if (completed_jobs >= 0) DIAG_ATOMIC_STORE(&g_cont_shader_completed, completed_jobs, DIAG_ORDER_RELAXED);
    if (active_workers >= 0) DIAG_ATOMIC_STORE(&g_cont_shader_active_workers, active_workers, DIAG_ORDER_RELAXED);
    if (avg_compile_ns >= 0) DIAG_ATOMIC_STORE(&g_cont_shader_avg_compile_ns, avg_compile_ns, DIAG_ORDER_RELAXED);
    diag_emit(g_diag_gfx_prod, DIAG_EV_SHADER_QUEUE, queued_jobs, completed_jobs, active_workers, avg_compile_ns);
}

void diag_hook_file_op(uint32_t op_type, int64_t duration_ns, int64_t bytes_transferred, int32_t status) {
    if (!diag_is_enabled() || g_diag_fs_prod < 0) return;
    if (duration_ns != DIAG_MEASUREMENT_UNAVAILABLE && duration_ns >= 0) {
        DIAG_ATOMIC_ADD(&g_cont_file_ops, 1, DIAG_ORDER_RELAXED);
        DIAG_ATOMIC_ADD(&g_cont_file_duration_ns, (uint64_t)duration_ns, DIAG_ORDER_RELAXED);
    }
    if (bytes_transferred > 0) {
        DIAG_ATOMIC_ADD(&g_cont_file_bytes, (uint64_t)bytes_transferred, DIAG_ORDER_RELAXED);
    }
    diag_emit(g_diag_fs_prod, DIAG_EV_FILE_OP, (int64_t)op_type, duration_ns, bytes_transferred, (int64_t)status);
}

void diag_hook_mem_stream(int64_t compare_bytes, int64_t copy_bytes, int64_t stream_bytes, int64_t uploads) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    if (compare_bytes > 0) DIAG_ATOMIC_ADD(&g_cont_mem_compare_bytes, (uint64_t)compare_bytes, DIAG_ORDER_RELAXED);
    if (copy_bytes > 0) DIAG_ATOMIC_ADD(&g_cont_mem_copy_bytes, (uint64_t)copy_bytes, DIAG_ORDER_RELAXED);
    if (stream_bytes > 0) DIAG_ATOMIC_ADD(&g_cont_mem_stream_bytes, (uint64_t)stream_bytes, DIAG_ORDER_RELAXED);
    diag_emit(g_diag_gfx_prod, DIAG_EV_MEM_STREAM, compare_bytes, copy_bytes, stream_bytes, uploads);
}

void diag_hook_mem_alloc(int64_t current_allocated_bytes, int64_t peak_allocated_bytes, int64_t total_alloc_count, int64_t total_free_count) {
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    if (current_allocated_bytes >= 0) {
        DIAG_ATOMIC_STORE(&g_cont_mem_allocated_bytes, current_allocated_bytes, DIAG_ORDER_RELAXED);
    }
    if (peak_allocated_bytes >= 0) {
        int64_t cur_peak = DIAG_ATOMIC_LOAD(&g_cont_mem_peak_allocated_bytes, DIAG_ORDER_RELAXED);
        if (peak_allocated_bytes > cur_peak) {
            DIAG_ATOMIC_STORE(&g_cont_mem_peak_allocated_bytes, peak_allocated_bytes, DIAG_ORDER_RELAXED);
        }
    }
    diag_emit(g_diag_gfx_prod, DIAG_EV_MEM_ALLOC, current_allocated_bytes, peak_allocated_bytes, total_alloc_count, total_free_count);
}

void diag_hook_audio_counters(int64_t appended_frames, int64_t released_frames, int64_t underruns, int64_t starved) {
    if (!diag_is_enabled() || g_diag_audio_prod < 0) return;
    if (appended_frames >= 0) DIAG_ATOMIC_STORE(&g_cont_audio_appended, appended_frames, DIAG_ORDER_RELAXED);
    if (released_frames >= 0) DIAG_ATOMIC_STORE(&g_cont_audio_released, released_frames, DIAG_ORDER_RELAXED);
    if (underruns >= 0) DIAG_ATOMIC_STORE(&g_cont_audio_underruns, underruns, DIAG_ORDER_RELAXED);
    if (starved >= 0) DIAG_ATOMIC_STORE(&g_cont_audio_starved, starved, DIAG_ORDER_RELAXED);
    diag_emit(g_diag_audio_prod, DIAG_EV_AUDIO_COUNTERS, appended_frames, released_frames, underruns, starved);
}

void diag_get_continuous_counters(diag_continuous_counters_t *out_counters) {
    if (!out_counters) return;
    out_counters->total_wait_events = DIAG_ATOMIC_LOAD(&g_cont_wait_events, DIAG_ORDER_RELAXED);
    out_counters->total_wait_ns = DIAG_ATOMIC_LOAD(&g_cont_wait_ns, DIAG_ORDER_RELAXED);
    out_counters->shader_queued_jobs = DIAG_ATOMIC_LOAD(&g_cont_shader_queued, DIAG_ORDER_RELAXED);
    out_counters->shader_completed_jobs = DIAG_ATOMIC_LOAD(&g_cont_shader_completed, DIAG_ORDER_RELAXED);
    out_counters->shader_active_workers = DIAG_ATOMIC_LOAD(&g_cont_shader_active_workers, DIAG_ORDER_RELAXED);
    out_counters->shader_avg_compile_ns = DIAG_ATOMIC_LOAD(&g_cont_shader_avg_compile_ns, DIAG_ORDER_RELAXED);
    out_counters->file_ops_count = DIAG_ATOMIC_LOAD(&g_cont_file_ops, DIAG_ORDER_RELAXED);
    out_counters->file_total_duration_ns = DIAG_ATOMIC_LOAD(&g_cont_file_duration_ns, DIAG_ORDER_RELAXED);
    out_counters->file_total_bytes = DIAG_ATOMIC_LOAD(&g_cont_file_bytes, DIAG_ORDER_RELAXED);
    out_counters->mem_compare_bytes = DIAG_ATOMIC_LOAD(&g_cont_mem_compare_bytes, DIAG_ORDER_RELAXED);
    out_counters->mem_copy_bytes = DIAG_ATOMIC_LOAD(&g_cont_mem_copy_bytes, DIAG_ORDER_RELAXED);
    out_counters->mem_stream_bytes = DIAG_ATOMIC_LOAD(&g_cont_mem_stream_bytes, DIAG_ORDER_RELAXED);
    out_counters->mem_allocated_bytes = DIAG_ATOMIC_LOAD(&g_cont_mem_allocated_bytes, DIAG_ORDER_RELAXED);
    out_counters->mem_peak_allocated_bytes = DIAG_ATOMIC_LOAD(&g_cont_mem_peak_allocated_bytes, DIAG_ORDER_RELAXED);
    out_counters->audio_appended_frames = DIAG_ATOMIC_LOAD(&g_cont_audio_appended, DIAG_ORDER_RELAXED);
    out_counters->audio_released_frames = DIAG_ATOMIC_LOAD(&g_cont_audio_released, DIAG_ORDER_RELAXED);
    out_counters->audio_underruns = DIAG_ATOMIC_LOAD(&g_cont_audio_underruns, DIAG_ORDER_RELAXED);
    out_counters->audio_starved = DIAG_ATOMIC_LOAD(&g_cont_audio_starved, DIAG_ORDER_RELAXED);
}

void diag_reset_continuous_counters(void) {
    DIAG_ATOMIC_STORE(&g_cont_wait_events, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_wait_ns, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_shader_queued, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_shader_completed, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_shader_active_workers, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_shader_avg_compile_ns, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_file_ops, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_file_duration_ns, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_file_bytes, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_mem_compare_bytes, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_mem_copy_bytes, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_mem_stream_bytes, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_mem_allocated_bytes, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_mem_peak_allocated_bytes, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_audio_appended, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_audio_released, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_audio_underruns, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_cont_audio_starved, 0, DIAG_ORDER_RELAXED);
}

#include "diag_shim.h"

static diag_semantic_observation_t g_last_semantic_obs;
static bool g_has_semantic_obs = false;
static GameDiagBinding g_host_diag_binding;
static bool g_host_shim_bound = false;

void diag_publish_semantic_observation(int32_t chapter,
                                      int32_t verse,
                                      int32_t difficulty,
                                      int32_t area,
                                      bool movie_active,
                                      int32_t draws) {
    diag_semantic_observation_t obs;
    obs.chapter = chapter;
    obs.verse = verse;
    obs.difficulty = difficulty;
    obs.area = area;
    obs.movie_active = movie_active;
    obs.draws_seen = draws;
    obs.timestamp_ns = diag_timestamp_ns();

    g_last_semantic_obs = obs;
    g_has_semantic_obs = true;

    if (diag_is_enabled() && g_diag_gfx_prod >= 0) {
        int64_t a = ((int64_t)chapter << 32) | (uint32_t)verse;
        int64_t b = ((int64_t)difficulty << 32) | (uint32_t)area;
        int64_t c = ((int64_t)(movie_active ? 1 : 0) << 32) | (uint32_t)draws;
        diag_emit(g_diag_gfx_prod, DIAG_EV_SEMANTIC_OBS, a, b, c, (int64_t)obs.timestamp_ns);
    }

    if (g_host_shim_bound && g_host_diag_binding.on_semantic_observation) {
        g_host_diag_binding.on_semantic_observation(&obs);
    }
}

bool diag_get_last_semantic_observation(diag_semantic_observation_t *out_obs) {
    if (!g_has_semantic_obs) return false;
    if (out_obs) *out_obs = g_last_semantic_obs;
    return true;
}

bool diag_try_bind_shim(void *module_handle, const char *game_hash, uint32_t program_id) {
    if (!module_handle) return false;

    typedef int (*game_diag_bind_fn)(const GameDiagBinding *, GameDiagBinding *);
    game_diag_bind_fn bind_fn = NULL;

#if defined(_WIN32)
    bind_fn = (game_diag_bind_fn)(void *)GetProcAddress((HMODULE)module_handle, "game_diag_bind");
#else
    bind_fn = (game_diag_bind_fn)dlsym(module_handle, "game_diag_bind");
#endif

    if (!bind_fn) {
        /* Graceful fallback for older library without diagnostic shim symbols */
        g_host_shim_bound = false;
        return false;
    }

    GameDiagBinding in_bind;
    memset(&in_bind, 0, sizeof(in_bind));
    in_bind.version = DIAG_SHIM_VERSION;
    in_bind.program_id = program_id;
    in_bind.game_hash = game_hash ? game_hash : DIAG_SHIM_SUPPORTED_GAME_HASH;
    in_bind.capabilities = DIAG_SHIM_CAP_SEMANTIC_OBSERVATIONS | DIAG_SHIM_CAP_REPLAY_VALIDATION;

    GameDiagBinding out_bind;
    memset(&out_bind, 0, sizeof(out_bind));

    int res = bind_fn(&in_bind, &out_bind);
    if (res == DIAG_SHIM_OK) {
        g_host_diag_binding = out_bind;
        g_host_shim_bound = true;
        return true;
    }

    g_host_shim_bound = false;
    return false;
}

bool diag_is_shim_bound(void) {
    return g_host_shim_bound;
}

void diag_hook_lifecycle(diag_lifecycle_state_t state) {
    DIAG_ATOMIC_STORE(&g_lifecycle_state, state, DIAG_ORDER_RELAXED);
    if (!diag_is_enabled() || g_diag_gfx_prod < 0) return;
    diag_emit(g_diag_gfx_prod, DIAG_EV_LIFECYCLE, (int64_t)state, 0, 0, 0);
}

bool diag_hook_thermal(float headroom, diag_thermal_status_t status) {
    uint64_t now = diag_timestamp_ns();
    uint64_t last_poll = DIAG_ATOMIC_LOAD(&g_thermal_poll_timestamp_ns, DIAG_ORDER_RELAXED);
    if (last_poll > 0 && now >= last_poll && (now - last_poll) < 1000000000ULL) {
        return false; /* Rate-limited: max 1 poll per second */
    }
    DIAG_ATOMIC_STORE(&g_thermal_poll_timestamp_ns, now, DIAG_ORDER_RELAXED);

    if (isnan(headroom) || headroom < 0.0f) {
        g_thermal_headroom = -1.0f;
        g_thermal_status = DIAG_THERMAL_STATUS_UNSUPPORTED;
    } else {
        g_thermal_headroom = headroom;
        g_thermal_status = status;
    }

    if (diag_is_enabled() && g_diag_gfx_prod >= 0) {
        int64_t hr_bits = 0;
        memcpy(&hr_bits, &headroom, sizeof(float));
        diag_emit(g_diag_gfx_prod, DIAG_EV_CUSTOM, hr_bits, (int64_t)status, 0, 0);
    }
    return true;
}

void diag_get_live_state(diag_live_state_t *out_state) {
    if (!out_state) return;
    out_state->lifecycle_state = DIAG_ATOMIC_LOAD(&g_lifecycle_state, DIAG_ORDER_RELAXED);
    out_state->is_foreground = (out_state->lifecycle_state == DIAG_LIFECYCLE_RESUMED ||
                                out_state->lifecycle_state == DIAG_LIFECYCLE_STARTED);
    out_state->surface_generation = DIAG_ATOMIC_LOAD(&g_surface_gen, DIAG_ORDER_RELAXED);
    out_state->thermal_headroom = g_thermal_headroom;
    out_state->thermal_status = g_thermal_status;
    out_state->thermal_poll_timestamp_ns = DIAG_ATOMIC_LOAD(&g_thermal_poll_timestamp_ns, DIAG_ORDER_RELAXED);
    out_state->last_render_timestamp_ns = DIAG_ATOMIC_LOAD(&g_last_render_timestamp_ns, DIAG_ORDER_RELAXED);
    if (g_has_semantic_obs) {
        out_state->last_semantic_timestamp_ns = g_last_semantic_obs.timestamp_ns;
        out_state->last_chapter = g_last_semantic_obs.chapter;
        out_state->last_verse = g_last_semantic_obs.verse;
        out_state->has_semantic_obs = true;
    } else {
        out_state->last_semantic_timestamp_ns = 0;
        out_state->last_chapter = -1;
        out_state->last_verse = -1;
        out_state->has_semantic_obs = false;
    }
}

void diag_reset_live_state(void) {
    DIAG_ATOMIC_STORE(&g_lifecycle_state, DIAG_LIFECYCLE_UNKNOWN, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_surface_gen, 0, DIAG_ORDER_RELAXED);
    g_thermal_headroom = -1.0f;
    g_thermal_status = DIAG_THERMAL_STATUS_UNSUPPORTED;
    DIAG_ATOMIC_STORE(&g_thermal_poll_timestamp_ns, 0, DIAG_ORDER_RELAXED);
    DIAG_ATOMIC_STORE(&g_last_render_timestamp_ns, 0, DIAG_ORDER_RELAXED);
}

/* ---------------------------------------------------------------- crash channel */
typedef char diag_crash_breadcrumb_size_check[(sizeof(diag_crash_breadcrumb_t) == 64) ? 1 : -1];

static int g_crash_fd = -1;

int diag_init_crash_channel(const char *session_dir) {
    if (!session_dir || session_dir[0] == '\0') {
        return -1;
    }
    char path[512];
    size_t len = strlen(session_dir);
    if (len > 4 && strcmp(session_dir + len - 4, ".bin") == 0) {
        snprintf(path, sizeof(path), "%s", session_dir);
    } else {
        snprintf(path, sizeof(path), "%s/crash_breadcrumb.bin", session_dir);
    }

    diag_close_crash_channel();

#if defined(_WIN32)
    g_crash_fd = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    g_crash_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (g_crash_fd >= 0) {
        chmod(path, 0666);
    }
#endif
    return g_crash_fd >= 0 ? 0 : -1;
}

void diag_close_crash_channel(void) {
    if (g_crash_fd >= 0) {
#if defined(_WIN32)
        _close(g_crash_fd);
#else
        close(g_crash_fd);
#endif
        g_crash_fd = -1;
    }
}

void diag_handle_fatal_signal(int sig, void *siginfo, void *ucontext) {
    (void)ucontext;
    diag_crash_breadcrumb_t crumb;
    memset(&crumb, 0, sizeof(crumb));
    crumb.magic = DIAG_CRASH_MAGIC;
    crumb.signal_num = (uint32_t)sig;
    crumb.exit_reason = 0; /* 0 = crash */
    crumb.thread_id = diag_get_tid();
    crumb.monotonic_ns = diag_time_ns();
    crumb.global_seq = DIAG_ATOMIC_LOAD(&g_diag_global_seq, DIAG_ORDER_RELAXED);

#if defined(_WIN32)
    if (siginfo) {
        EXCEPTION_POINTERS *ep = (EXCEPTION_POINTERS *)siginfo;
        if (ep->ExceptionRecord) {
            crumb.signal_num = (uint32_t)ep->ExceptionRecord->ExceptionCode;
            if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                ep->ExceptionRecord->NumberParameters >= 2) {
                crumb.fault_address = (uint64_t)ep->ExceptionRecord->ExceptionInformation[1];
            } else {
                crumb.fault_address = (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
            }
        }
    }
#elif defined(__linux__) || defined(__ANDROID__)
    if (siginfo) {
        siginfo_t *si = (siginfo_t *)siginfo;
        crumb.fault_address = (uint64_t)(uintptr_t)si->si_addr;
    }
#endif

    if (g_crash_fd >= 0) {
#if defined(_WIN32)
        _write(g_crash_fd, &crumb, (unsigned int)sizeof(crumb));
#else
        ssize_t ret = write(g_crash_fd, &crumb, sizeof(crumb));
        (void)ret;
        fsync(g_crash_fd);
#endif
    }

#if !defined(_WIN32)
    /* Chain to platform default signal handler (debuggerd / tombstone generator) */
    signal(sig, SIG_DFL);
    raise(sig);
#endif
}

int diag_setup_altstack(void) {
#if !defined(_WIN32)
    static uint8_t s_altstack[65536];
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = s_altstack;
    ss.ss_size = sizeof(s_altstack);
    ss.ss_flags = 0;
    return sigaltstack(&ss, NULL);
#else
    return 0;
#endif
}

/* ---------------------------------------------------------------- atrace */
static bool g_atrace_active = false;
#if defined(__ANDROID__)
typedef void (*pfn_atrace_begin_t)(const char *);
typedef void (*pfn_atrace_end_t)(void);
typedef void (*pfn_atrace_counter_t)(const char *, int64_t);

static pfn_atrace_begin_t g_pfn_atrace_begin = NULL;
static pfn_atrace_end_t g_pfn_atrace_end = NULL;
static pfn_atrace_counter_t g_pfn_atrace_counter = NULL;
#endif

void diag_atrace_init(bool enabled) {
    if (!enabled) {
        g_atrace_active = false;
        return;
    }
#if defined(__ANDROID__)
    void *handle = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) handle = RTLD_DEFAULT;
    g_pfn_atrace_begin = (pfn_atrace_begin_t)dlsym(handle, "ATrace_beginSection");
    g_pfn_atrace_end = (pfn_atrace_end_t)dlsym(handle, "ATrace_endSection");
    g_pfn_atrace_counter = (pfn_atrace_counter_t)dlsym(handle, "ATrace_setCounter");
    g_atrace_active = (g_pfn_atrace_begin != NULL && g_pfn_atrace_end != NULL);
#else
    g_atrace_active = false;
#endif
}

void diag_atrace_begin(const char *name) {
#if defined(__ANDROID__)
    if (g_atrace_active && g_pfn_atrace_begin && name) {
        g_pfn_atrace_begin(name);
    }
#else
    (void)name;
#endif
}

void diag_atrace_end(void) {
#if defined(__ANDROID__)
    if (g_atrace_active && g_pfn_atrace_end) {
        g_pfn_atrace_end();
    }
#endif
}

void diag_atrace_counter(const char *name, int64_t val) {
#if defined(__ANDROID__)
    if (g_atrace_active && g_pfn_atrace_counter && name) {
        g_pfn_atrace_counter(name, val);
    }
#else
    (void)name;
    (void)val;
#endif
}

bool diag_atrace_is_active(void) {
    return g_atrace_active;
}


