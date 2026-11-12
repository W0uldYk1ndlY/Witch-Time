#ifndef WITCHTIME_DIAG_H
#define WITCHTIME_DIAG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_MAX_PRODUCERS 16
#define DIAG_RING_CAPACITY 1024  /* must be a power of 2 */
#define DIAG_PAYLOAD_WORDS 4

/* Event types */
typedef enum diag_event_type {
    DIAG_EV_NONE = 0,
    DIAG_EV_FRAME_START = 1,
    DIAG_EV_RENDER_END = 2,
    DIAG_EV_GPU_SUBMIT = 3,
    DIAG_EV_GPU_COMPLETE = 4,
    DIAG_EV_PRESENT_QUEUED = 5,
    DIAG_EV_PRESENT_CALLED = 6,
    DIAG_EV_PRESENT_DISCARD = 7,
    DIAG_EV_SURFACE_GEN = 8,
    DIAG_EV_INPUT_POLL = 9,
    DIAG_EV_CHECKPOINT = 10,
    DIAG_EV_LIFECYCLE = 11,
    DIAG_EV_AUDIO_STATUS = 12,
    DIAG_EV_FRAME_SAMPLE = 13,
    DIAG_EV_GPU_SAMPLE = 14,
    DIAG_EV_DOMAIN_ANCHOR = 15,
    DIAG_EV_SEMANTIC_OBS = 16,
    DIAG_EV_OPT_WW_STATS = 20,
    DIAG_EV_OPT_WW_MEM = 21,
    DIAG_EV_OPT_MEM_INSPECT = 22,
    DIAG_EV_OPT_GPU_WAIT = 23,
    DIAG_EV_OPT_CLEAR_FOLD = 24,
    DIAG_EV_THREAD_ROLE = 25,
    DIAG_EV_THREAD_WAIT = 26,
    DIAG_EV_SHADER_QUEUE = 27,
    DIAG_EV_FILE_OP = 28,
    DIAG_EV_MEM_STREAM = 29,
    DIAG_EV_MEM_ALLOC = 30,
    DIAG_EV_AUDIO_COUNTERS = 31,
    DIAG_EV_TEMPORAL_SELECTION = 32,
    DIAG_EV_TEMPORAL_DIMENSIONS = 33,
    DIAG_EV_TEMPORAL_STATE = 34,
    DIAG_EV_TEMPORAL_FAILURE = 35,
    DIAG_EV_GPU_COMPONENT = 36,
    DIAG_EV_CUSTOM = 99,
} diag_event_type_t;

/* Sentinel for unavailable measurement - strictly distinguished from measured 0 */
#define DIAG_MEASUREMENT_UNAVAILABLE ((int64_t)-1)

/* Thread roles */
#define DIAG_THREAD_ROLE_UNKNOWN        0
#define DIAG_THREAD_ROLE_MAIN           1
#define DIAG_THREAD_ROLE_RENDER         2
#define DIAG_THREAD_ROLE_AUDIO          3
#define DIAG_THREAD_ROLE_SHADER_WORKER  4
#define DIAG_THREAD_ROLE_PREPARATION    5
#define DIAG_THREAD_ROLE_GUEST          6
#define DIAG_THREAD_ROLE_HOST_OTHER     7

/* Sync wait primitive kinds */
#define DIAG_WAIT_MUTEX                 1
#define DIAG_WAIT_CONDVAR               2
#define DIAG_WAIT_SEMAPHORE             3
#define DIAG_WAIT_EVENT                 4
#define DIAG_WAIT_TIMER                 5

/* File operation kinds */
#define DIAG_FILE_OP_OPEN               1
#define DIAG_FILE_OP_READ               2
#define DIAG_FILE_OP_WRITE              3
#define DIAG_FILE_OP_COMMIT             4

/* Continuous counters state */
typedef struct diag_continuous_counters {
    /* Thread & wait */
    uint64_t total_wait_events;
    uint64_t total_wait_ns;
    /* Shader queue */
    int64_t shader_queued_jobs;
    int64_t shader_completed_jobs;
    int64_t shader_active_workers;
    int64_t shader_avg_compile_ns;
    /* File I/O */
    uint64_t file_ops_count;
    uint64_t file_total_duration_ns;
    uint64_t file_total_bytes;
    /* Memory & Streaming */
    uint64_t mem_compare_bytes;
    uint64_t mem_copy_bytes;
    uint64_t mem_stream_bytes;
    int64_t  mem_allocated_bytes;
    int64_t  mem_peak_allocated_bytes;
    /* Audio */
    int64_t audio_appended_frames;
    int64_t audio_released_frames;
    int64_t audio_underruns;
    int64_t audio_starved;
} diag_continuous_counters_t;

/* Fixed-size 64-byte diagnostic record: scalar values only, no transient pointers */
typedef struct diag_record {
    uint64_t seq;           /* strictly monotonic sequence number across session */
    uint64_t timestamp_ns;  /* CLOCK_MONOTONIC or domain timestamp */
    uint32_t type;          /* diag_event_type_t */
    uint32_t producer_id;   /* registered producer slot index */
    uint32_t thread_id;     /* OS thread ID */
    uint32_t generation;    /* producer lifetime generation */
    int64_t  payload[DIAG_PAYLOAD_WORDS]; /* 32 bytes of inline bounded values/ids */
} diag_record_t;

/* Producer statistics */
typedef struct diag_producer_stats {
    uint64_t emitted_count;
    uint64_t dropped_count;
    uint32_t generation;
    uint32_t active;
    char name[32];
} diag_producer_stats_t;

/* Core lifecycle and configuration */
bool diag_is_enabled(void);
int  diag_init(const char *run_id);
void diag_shutdown(void);
/* Close admissions; wait at most 50 ms for accepted emitters/registrations.
 * True proves final drain is stable. False means capture is unavailable;
 * rings remain valid for late completion. Failure is sticky until init.
 * No re-init with live producers. */
bool diag_seal_producers(void);
uint64_t diag_time_ns(void);

/* Explicit producer slots are caller-owned SPSC; coordinate retirement/reuse
 * with the sole producer and consumer before unregistering/re-registering. */
int  diag_register_producer(const char *name);
void diag_unregister_producer(int producer_id);

/* Producer emit API (non-blocking, lock-free, zero allocation, no-op if disabled).
 * An invalid id while enabled counts one total drop (e.g. registration failure). */
void diag_emit(int producer_id, uint32_t type, int64_t p0, int64_t p1, int64_t p2, int64_t p3);

/* Consumer drain API (used by background chunk writer) */
size_t diag_drain_producer(int producer_id, diag_record_t *out_records, size_t max_count);
bool   diag_get_producer_stats(int producer_id, diag_producer_stats_t *out_stats);
uint64_t diag_get_total_emitted(void);
uint64_t diag_get_total_dropped(void);

/* Hooks use one lazy TLS slot per actual calling thread, shared across event
 * domains. At most 16 lifetime hook/explicit producer slots per session; hook
 * slots are never recycled. Exhaustion counts drops, with no shared fallback.
 * Init invalidates TLS ids/failures; requires no live producers from old session. */
/* Graphics pipeline instrumentation hooks (no-op when disabled) */
void diag_hook_render_end(int64_t frame, int64_t draws, int64_t skipped, int64_t clears);
void diag_hook_gpu_submit(int64_t slot, int64_t serial, int64_t flushes, int64_t cmd_count);
void diag_hook_gpu_complete(int64_t slot, int64_t serial, int64_t wait_ticks, int64_t gpu_ns);
void diag_hook_present_queued(int64_t frame, int64_t image_index, int64_t target_tick, int64_t is_fight);
void diag_hook_present_called(int64_t frame, int64_t image_index, int64_t result_code, int64_t is_progress);
void diag_hook_present_discard(int64_t frame, int64_t image_index, int64_t reason, int64_t surface_gen);
void diag_hook_surface_gen(int64_t generation, int64_t width, int64_t height, int64_t format);

/* Scene-aligned evidence only. Modes: native=0, legacy FSR=1..5, SGSR=6..8;
 * variant: unavailable=-1, 2-pass-fs=0, 2-pass-cs=1, 3-pass-cs=2.
 * Reason/pass ids belong to the backend contract; 0 means none, -1 unknown. */
#define DIAG_TEMPORAL_SUPPORTED   (1u << 0)
#define DIAG_TEMPORAL_INITIALIZED (1u << 1)
#define DIAG_TEMPORAL_DISPATCHED  (1u << 2)
#define DIAG_TEMPORAL_CONSUMED    (1u << 3)
#define DIAG_TEMPORAL_SKIPPED     (1u << 4) /* no fully proven output treatment; dispatch may still be recorded */
#define DIAG_TEMPORAL_REACTIVE    (1u << 5) /* consumed, not merely generated */
typedef struct diag_temporal_observation {
    uint64_t scene_frame;
    int32_t selected_mode, effective_mode, variant;
    uint32_t scene_width, scene_height, output_width, output_height;
    uint32_t surface_generation, flags;
    int32_t motion_support; /* unavailable=0, camera-only=1, partial=2, full=3 */
    int32_t reset_reason, fallback_reason, failing_pass, error_code;
} diag_temporal_observation_t;
void diag_hook_temporal(const diag_temporal_observation_t *observation);

enum { DIAG_TEMPORAL_NO_SCENE = 1, DIAG_TEMPORAL_UNSUPPORTED = 2,
       DIAG_TEMPORAL_SETUP_FAILED = 3, DIAG_TEMPORAL_NO_DISPATCH = 4,
       DIAG_TEMPORAL_NO_CONSUMER = 5, DIAG_TEMPORAL_FALLBACK = 6,
       DIAG_TEMPORAL_CAMERA_CONFLICT = 7, DIAG_TEMPORAL_INPUT_MISMATCH = 8,
       DIAG_TEMPORAL_OUTPUT_UNSUPPORTED = 9 };
/* Shared production decision, also tested without Vulkan. Consumed means a
 * matching output binding AND recorded consumer draw, not GPU completion. */
static inline void diag_temporal_decide(diag_temporal_observation_t *o, int latched_mode,
                                        int eligible, int bound, int consumer_recorded) {
    o->effective_mode = 0;
    o->flags &= ~(DIAG_TEMPORAL_CONSUMED | DIAG_TEMPORAL_SKIPPED);
    if (!latched_mode) return;
    if (!o->fallback_reason) {
        if (!eligible) o->fallback_reason = DIAG_TEMPORAL_NO_SCENE;
        else if (!(o->flags & DIAG_TEMPORAL_SUPPORTED)) o->fallback_reason = DIAG_TEMPORAL_UNSUPPORTED;
        else if (!(o->flags & DIAG_TEMPORAL_INITIALIZED)) o->fallback_reason = DIAG_TEMPORAL_SETUP_FAILED;
        else if (!(o->flags & DIAG_TEMPORAL_DISPATCHED)) o->fallback_reason = DIAG_TEMPORAL_NO_DISPATCH;
        else if (!bound || !consumer_recorded) o->fallback_reason = DIAG_TEMPORAL_NO_CONSUMER;
    }
    if (o->fallback_reason) o->flags |= DIAG_TEMPORAL_SKIPPED;
    else { o->flags |= DIAG_TEMPORAL_CONSUMED; o->effective_mode = latched_mode; }
}

/* State: completed=0, pending=1, unsupported=2, failed=3, wrapped=4,
 * capacity-overflow=5, lost-tail=6, no-results=7. Only 0/4 carry measured ns.
 * pass id upper 32 bits and state lower 32 bits share p1; p2 is submission serial. */
void diag_stream_gpu_component(int64_t frame, uint32_t pass_id, int64_t serial,
                               int64_t gpu_ns, uint32_t state);

/* Optimization instrumentation hooks */
void diag_hook_opt_ww(int64_t backend, int64_t faults, int64_t rearms, int64_t queries);
void diag_hook_opt_ww_mem(int64_t page_size, int64_t coverage_bytes, int64_t tracker_mem_bytes, int64_t leases);
void diag_hook_opt_mem_inspect(int64_t hash_words_visited, int64_t snapshot_compare_bytes, int64_t copies, int64_t uploads);
void diag_hook_opt_gpu_wait(int64_t required_serial, int64_t chosen_serial, int64_t wait_needed, int64_t slot);
void diag_hook_opt_clear_fold(int64_t eligible_clears, int64_t folded_clears, int64_t rejected_clears, int64_t attachment);

/* Streaming frame/GPU samples and domain anchors */
void diag_stream_frame_sample(int64_t frame, int64_t cpu_ns, int64_t wall_ns, int64_t flags);
void diag_stream_gpu_sample(int64_t frame, int64_t gpu_ns, int64_t slot, int64_t status);
void diag_stream_domain_anchor(int64_t anchor_type, int64_t monotonic_ns, int64_t wall_ns, int64_t frame);

/* Continuous state and subsystem instrumentation hooks */
void diag_hook_thread_role(uint32_t thread_id, uint32_t role, uint64_t name_hash);
void diag_hook_thread_wait(uint32_t thread_id, uint32_t wait_type, int64_t wait_ns, int32_t result);
void diag_hook_shader_queue(int64_t queued_jobs, int64_t completed_jobs, int64_t active_workers, int64_t avg_compile_ns);
void diag_hook_file_op(uint32_t op_type, int64_t duration_ns, int64_t bytes_transferred, int32_t status);
void diag_hook_mem_stream(int64_t compare_bytes, int64_t copy_bytes, int64_t stream_bytes, int64_t uploads);
void diag_hook_mem_alloc(int64_t current_allocated_bytes, int64_t peak_allocated_bytes, int64_t total_alloc_count, int64_t total_free_count);
void diag_hook_audio_counters(int64_t appended_frames, int64_t released_frames, int64_t underruns, int64_t starved);

/* Lifecycle states */
typedef enum diag_lifecycle_state {
    DIAG_LIFECYCLE_UNKNOWN = 0,
    DIAG_LIFECYCLE_CREATED = 1,
    DIAG_LIFECYCLE_STARTED = 2,
    DIAG_LIFECYCLE_RESUMED = 3,  /* Foreground / active */
    DIAG_LIFECYCLE_PAUSED = 4,
    DIAG_LIFECYCLE_STOPPED = 5,  /* Background / expected inactivity */
    DIAG_LIFECYCLE_DESTROYED = 6
} diag_lifecycle_state_t;

/* Thermal status levels */
typedef enum diag_thermal_status {
    DIAG_THERMAL_STATUS_NONE = 0,
    DIAG_THERMAL_STATUS_LIGHT = 1,
    DIAG_THERMAL_STATUS_MODERATE = 2,
    DIAG_THERMAL_STATUS_SEVERE = 3,
    DIAG_THERMAL_STATUS_CRITICAL = 4,
    DIAG_THERMAL_STATUS_EMERGENCY = 5,
    DIAG_THERMAL_STATUS_SHUTDOWN = 6,
    DIAG_THERMAL_STATUS_UNSUPPORTED = -1
} diag_thermal_status_t;

/* Live state snapshot */
typedef struct diag_live_state {
    diag_lifecycle_state_t lifecycle_state;
    bool is_foreground;
    uint32_t surface_generation;
    float thermal_headroom;         /* negative or NAN if unsupported */
    diag_thermal_status_t thermal_status;
    uint64_t thermal_poll_timestamp_ns;
    uint64_t last_render_timestamp_ns;
    uint64_t last_semantic_timestamp_ns;
    int32_t last_chapter;
    int32_t last_verse;
    bool has_semantic_obs;
} diag_live_state_t;

void diag_hook_lifecycle(diag_lifecycle_state_t state);
bool diag_hook_thermal(float headroom, diag_thermal_status_t status);
void diag_get_live_state(diag_live_state_t *out_state);
void diag_reset_live_state(void);

/* Retrieve and reset continuous counter aggregates */
void diag_get_continuous_counters(diag_continuous_counters_t *out_counters);
void diag_reset_continuous_counters(void);

/* Raw crash breadcrumb for async-safe platform crash preservation */
#define DIAG_CRASH_MAGIC 0x43525348 /* 'CRSH' */

typedef struct diag_crash_breadcrumb {
    uint32_t magic;              /* DIAG_CRASH_MAGIC */
    uint32_t signal_num;         /* Signal number (e.g. SIGSEGV, SIGBUS) */
    uint64_t fault_address;      /* Fault address from siginfo */
    uint32_t thread_id;          /* Operating system thread ID */
    uint32_t exit_reason;        /* 0 = crash */
    uint64_t monotonic_ns;       /* Monotonic timestamp */
    uint64_t global_seq;         /* Last emitted diagnostic sequence */
    uint64_t reserved[3];        /* Padding to exactly 64 bytes */
} diag_crash_breadcrumb_t;

/* Initialize crash channel (pre-opens crash_breadcrumb.bin for async-signal-safe writing) */
int  diag_init_crash_channel(const char *session_dir);
void diag_close_crash_channel(void);

/* Async-signal-safe fatal handler hook: no printf, no malloc, no mutex, no unwinding */
void diag_handle_fatal_signal(int sig, void *siginfo, void *ucontext);

/* Helper to setup thread-specific alternate stack */
int  diag_setup_altstack(void);

/* Native ATrace scopes and counters */
void diag_atrace_init(bool enabled);
void diag_atrace_begin(const char *name);
void diag_atrace_end(void);
void diag_atrace_counter(const char *name, int64_t val);
bool diag_atrace_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* WITCHTIME_DIAG_H */
