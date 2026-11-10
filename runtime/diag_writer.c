#include "diag_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define wt_mkdir(p) _mkdir(p)
typedef HANDLE wt_thread_t;
typedef DWORD (WINAPI *wt_thread_fn)(void *);

static inline int wt_atomic_rename(const char *src, const char *dst) {
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return 0;
    }
    return -1;
}

static inline wt_thread_t wt_thread_spawn(wt_thread_fn fn, void *arg) {
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}

static inline void wt_thread_close(wt_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline void wt_sleep(uint32_t ms) {
    Sleep(ms);
}

static inline uint64_t wt_time_ns(void) {
    static LARGE_INTEGER freq;
    static int freq_init = 0;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (freq.QuadPart > 0) {
        return (uint64_t)((c.QuadPart * 1000000000ULL) / freq.QuadPart);
    }
    return 0;
}

#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#define wt_mkdir(p) mkdir((p), 0755)
typedef pthread_t wt_thread_t;
typedef void *(*wt_thread_fn)(void *);

static inline int wt_atomic_rename(const char *src, const char *dst) {
    return rename(src, dst);
}

static inline wt_thread_t wt_thread_spawn(wt_thread_fn fn, void *arg) {
    pthread_t t;
    pthread_create(&t, NULL, fn, arg);
    return t;
}

static inline void wt_thread_close(wt_thread_t t) {
    pthread_join(t, NULL);
}

static inline void wt_sleep(uint32_t ms) {
    usleep(ms * 1000);
}

static inline uint64_t wt_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

/* Text sink circular buffer */
#define TEXT_SINK_CAPACITY (256 * 1024)
static char g_text_sink_buf[TEXT_SINK_CAPACITY];
static size_t g_text_sink_len = 0;
static size_t g_text_sink_dropped = 0;

/* Internal writer state */
static diag_writer_config_t g_cfg;
static diag_writer_state_t g_state = DIAG_WRITER_IDLE;
static uint64_t g_total_records_written = 0;
static uint64_t g_total_bytes_written = 0;
static uint32_t g_chunks_committed = 0;
static uint32_t g_incidents_written = 0;
static bool g_reserve_held = false;
static volatile int g_thread_running = 0;
static wt_thread_t g_flusher_thread;
static bool g_thread_active = false;

/* Writer-owned prehistory ring buffer */
#define DIAG_PREHISTORY_CAPACITY 1024
static diag_record_t g_prehistory_ring[DIAG_PREHISTORY_CAPACITY];
static size_t g_prehistory_count = 0;
static size_t g_prehistory_head = 0;
static uint32_t g_incident_storms = 0;

/* Helper: check if file exists */
static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

/* Helper: make directory recursively */
static int make_dir_p(const char *dir) {
    char tmp[256];
    size_t len = strlen(dir);
    if (len >= sizeof(tmp)) return -1;
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
            wt_mkdir(tmp);
            tmp[i] = c;
        }
    }
    return wt_mkdir(tmp);
}

#if defined(_WIN32)
static DWORD WINAPI flusher_thread_proc(void *arg) {
#else
static void *flusher_thread_proc(void *arg) {
#endif
    (void)arg;
    while (g_thread_running) {
        wt_sleep(g_cfg.flush_interval_ms > 0 ? g_cfg.flush_interval_ms : 50);
        if (!g_thread_running) break;
        diag_writer_flush_chunks();
        diag_writer_write_status();
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

int diag_writer_init(const diag_writer_config_t *cfg) {
    if (!cfg || cfg->output_dir[0] == '\0' || cfg->run_id[0] == '\0') {
        return -1;
    }

    /* Check if output directory already contains artifacts of a prior session */
    char check_path[300];
    snprintf(check_path, sizeof(check_path), "%s/status.json", cfg->output_dir);
    if (file_exists(check_path)) {
        return -2; /* Prior session exists, refuse overwrite */
    }
    snprintf(check_path, sizeof(check_path), "%s/manifest.json", cfg->output_dir);
    if (file_exists(check_path)) {
        return -2;
    }
    snprintf(check_path, sizeof(check_path), "%s/events-000001.jsonl", cfg->output_dir);
    if (file_exists(check_path)) {
        return -2;
    }

    make_dir_p(cfg->output_dir);

    memcpy(&g_cfg, cfg, sizeof(diag_writer_config_t));
    if (g_cfg.storage_budget_bytes == 0) {
        g_cfg.storage_budget_bytes = 128ULL * 1024ULL * 1024ULL; /* 128 MiB */
    }
    if (g_cfg.incident_reserve_bytes == 0) {
        g_cfg.incident_reserve_bytes = 16ULL * 1024ULL * 1024ULL; /* 16 MiB */
    }
    if (g_cfg.chunk_record_limit == 0) {
        g_cfg.chunk_record_limit = 1000;
    }

    if (g_cfg.disable_unbuffered_logging) {
        /* Opt-out of unbuffered legacy stdout */
        setvbuf(stdout, NULL, _IOFBF, 64 * 1024);
    }

    g_state = DIAG_WRITER_ACTIVE;
    g_total_records_written = 0;
    g_total_bytes_written = 0;
    g_chunks_committed = 0;
    g_incidents_written = 0;
    g_incident_storms = 0;
    g_prehistory_count = 0;
    g_prehistory_head = 0;
    g_reserve_held = false;
    g_text_sink_len = 0;
    g_text_sink_dropped = 0;
    g_thread_active = false;

    /* Pre-open crash breadcrumb channel in session directory for async-signal-safe write */
    diag_init_crash_channel(cfg->output_dir);

    /* Write initial status snapshot */
    diag_writer_write_status();

    return 0;
}

int diag_writer_start(void) {
    if (g_state != DIAG_WRITER_ACTIVE) {
        return -1;
    }
    if (g_cfg.flush_interval_ms > 0 && !g_thread_active) {
        g_thread_running = 1;
        g_flusher_thread = wt_thread_spawn(flusher_thread_proc, NULL);
        g_thread_active = true;
    }
    return 0;
}

int diag_writer_flush_chunks(void) {
    if (g_state == DIAG_WRITER_IDLE || g_state == DIAG_WRITER_ERROR) {
        return -1;
    }

    /* Check regular storage budget excluding first-incident reserve */
    uint64_t regular_budget_limit = 0;
    if (g_cfg.storage_budget_bytes > g_cfg.incident_reserve_bytes) {
        regular_budget_limit = g_cfg.storage_budget_bytes - g_cfg.incident_reserve_bytes;
    }

    if (g_total_bytes_written >= regular_budget_limit) {
        g_reserve_held = true;
        g_state = DIAG_WRITER_EXHAUSTED;
        return 0; /* Storage exhausted for regular telemetry; reserve held for incidents */
    }

    /* Drain available records from all active producers */
    diag_record_t records[256];
    size_t batch_limit = 256;
    if (batch_limit > g_cfg.chunk_record_limit) {
        batch_limit = g_cfg.chunk_record_limit;
    }

    for (int prod = 0; prod < DIAG_MAX_PRODUCERS; prod++) {
        size_t drained = diag_drain_producer(prod, records, batch_limit);
        if (drained == 0) {
            continue;
        }

        /* Estimate chunk size (~80 byte header + ~120 bytes per record) to ensure regular telemetry never cuts into incident reserve */
        size_t est_chunk_bytes = 80 + drained * 120;
        if (g_total_bytes_written + est_chunk_bytes > regular_budget_limit) {
            g_reserve_held = true;
            g_state = DIAG_WRITER_EXHAUSTED;
            break;
        }

        char tmp_path[320];
        char final_path[320];
        uint32_t chunk_id = g_chunks_committed + 1;
        snprintf(tmp_path, sizeof(tmp_path), "%s/events-%06u.jsonl.tmp", g_cfg.output_dir, chunk_id);
        snprintf(final_path, sizeof(final_path), "%s/events-%06u.jsonl", g_cfg.output_dir, chunk_id);

        FILE *f = fopen(tmp_path, "w");
        if (!f) {
            g_state = DIAG_WRITER_ERROR;
            return -1;
        }

        /* Write chunk header line */
        int header_len = fprintf(f, "{\"chunk_id\":%u,\"start_seq\":%llu,\"end_seq\":%llu,\"count\":%zu}\n",
                                 chunk_id,
                                 (unsigned long long)records[0].seq,
                                 (unsigned long long)records[drained - 1].seq,
                                 drained);
        if (header_len < 0) {
            fclose(f);
            remove(tmp_path);
            g_state = DIAG_WRITER_ERROR;
            return -1;
        }

        size_t chunk_bytes = (size_t)header_len;
        bool write_err = false;

        for (size_t i = 0; i < drained; i++) {
            const diag_record_t *r = &records[i];
            int n = fprintf(f, "{\"seq\":%llu,\"ts\":%llu,\"type\":%u,\"prod\":%u,\"tid\":%u,\"gen\":%u,\"p0\":%lld,\"p1\":%lld,\"p2\":%lld,\"p3\":%lld}\n",
                            (unsigned long long)r->seq,
                            (unsigned long long)r->timestamp_ns,
                            r->type,
                            r->producer_id,
                            r->thread_id,
                            r->generation,
                            (long long)r->payload[0],
                            (long long)r->payload[1],
                            (long long)r->payload[2],
                            (long long)r->payload[3]);
            if (n < 0) {
                write_err = true;
                break;
            }
            chunk_bytes += (size_t)n;
        }

        if (write_err) {
            fclose(f);
            remove(tmp_path);
            g_state = DIAG_WRITER_ERROR;
            return -1;
        }

        fflush(f);
        fclose(f);

        /* Atomic publication */
        if (wt_atomic_rename(tmp_path, final_path) != 0) {
            remove(tmp_path);
            g_state = DIAG_WRITER_ERROR;
            return -1;
        }

        g_chunks_committed++;
        g_total_records_written += drained;
        g_total_bytes_written += chunk_bytes;

        /* Retain committed records in writer-owned prehistory ring */
        for (size_t r = 0; r < drained; r++) {
            g_prehistory_ring[g_prehistory_head] = records[r];
            g_prehistory_head = (g_prehistory_head + 1) % DIAG_PREHISTORY_CAPACITY;
            if (g_prehistory_count < DIAG_PREHISTORY_CAPACITY) {
                g_prehistory_count++;
            }
        }
    }

    return 0;
}

int diag_writer_write_status(void) {
    if (g_state == DIAG_WRITER_IDLE) {
        return -1;
    }

    char tmp_path[320];
    char final_path[320];
    snprintf(tmp_path, sizeof(tmp_path), "%s/status.json.tmp", g_cfg.output_dir);
    snprintf(final_path, sizeof(final_path), "%s/status.json", g_cfg.output_dir);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        return -1;
    }

    uint32_t active_count = 0;
    for (int i = 0; i < DIAG_MAX_PRODUCERS; i++) {
        diag_producer_stats_t s;
        if (diag_get_producer_stats(i, &s) && s.active) {
            active_count++;
        }
    }

    const char *state_str = "idle";
    switch (g_state) {
        case DIAG_WRITER_ACTIVE: state_str = "active"; break;
        case DIAG_WRITER_EXHAUSTED: state_str = "storage_exhausted"; break;
        case DIAG_WRITER_STOPPED: state_str = "stopped"; break;
        case DIAG_WRITER_ERROR: state_str = "error"; break;
        default: break;
    }

    uint64_t remaining_budget = 0;
    if (g_cfg.storage_budget_bytes > g_total_bytes_written) {
        remaining_budget = g_cfg.storage_budget_bytes - g_total_bytes_written;
    }

    bool healthy = (g_state == DIAG_WRITER_ACTIVE || g_state == DIAG_WRITER_STOPPED);

    diag_live_state_t live;
    diag_get_live_state(&live);

    uint64_t now_ns = wt_time_ns();
    uint64_t render_age_ms = 0;
    if (live.last_render_timestamp_ns > 0 && now_ns >= live.last_render_timestamp_ns) {
        render_age_ms = (now_ns - live.last_render_timestamp_ns) / 1000000ULL;
    }
    uint64_t semantic_freshness_ms = 0;
    if (live.last_semantic_timestamp_ns > 0 && now_ns >= live.last_semantic_timestamp_ns) {
        semantic_freshness_ms = (now_ns - live.last_semantic_timestamp_ns) / 1000000ULL;
    }

    bool expected_inactivity = (live.lifecycle_state == DIAG_LIFECYCLE_STOPPED || live.lifecycle_state == DIAG_LIFECYCLE_PAUSED);
    bool is_active_stall = live.is_foreground && (render_age_ms > 5000);

    const char *lifecycle_str = "unknown";
    switch (live.lifecycle_state) {
        case DIAG_LIFECYCLE_CREATED: lifecycle_str = "created"; break;
        case DIAG_LIFECYCLE_STARTED: lifecycle_str = "started"; break;
        case DIAG_LIFECYCLE_RESUMED: lifecycle_str = "resumed"; break;
        case DIAG_LIFECYCLE_PAUSED: lifecycle_str = "paused"; break;
        case DIAG_LIFECYCLE_STOPPED: lifecycle_str = "stopped"; break;
        case DIAG_LIFECYCLE_DESTROYED: lifecycle_str = "destroyed"; break;
        default: break;
    }

    const char *thermal_status_str = "unsupported";
    switch (live.thermal_status) {
        case DIAG_THERMAL_STATUS_NONE: thermal_status_str = "none"; break;
        case DIAG_THERMAL_STATUS_LIGHT: thermal_status_str = "light"; break;
        case DIAG_THERMAL_STATUS_MODERATE: thermal_status_str = "moderate"; break;
        case DIAG_THERMAL_STATUS_SEVERE: thermal_status_str = "severe"; break;
        case DIAG_THERMAL_STATUS_CRITICAL: thermal_status_str = "critical"; break;
        case DIAG_THERMAL_STATUS_EMERGENCY: thermal_status_str = "emergency"; break;
        case DIAG_THERMAL_STATUS_SHUTDOWN: thermal_status_str = "shutdown"; break;
        default: break;
    }

    char thermal_buf[96];
    if (live.thermal_headroom >= 0.0f) {
        snprintf(thermal_buf, sizeof(thermal_buf), "{\"headroom\": %.2f, \"status\": \"%s\"}", live.thermal_headroom, thermal_status_str);
    } else {
        snprintf(thermal_buf, sizeof(thermal_buf), "null");
    }

    char semantic_freshness_buf[32];
    if (live.has_semantic_obs) {
        snprintf(semantic_freshness_buf, sizeof(semantic_freshness_buf), "%llu", (unsigned long long)semantic_freshness_ms);
    } else {
        snprintf(semantic_freshness_buf, sizeof(semantic_freshness_buf), "null");
    }

    fprintf(f, "{\n"
               "  \"schema_version\": \"1.0\",\n"
               "  \"run_id\": \"%s\",\n"
               "  \"timestamp_ns\": %llu,\n"
               "  \"healthy\": %s,\n"
               "  \"state\": \"%s\",\n"
               "  \"lifecycle\": {\n"
               "    \"state\": \"%s\",\n"
               "    \"is_foreground\": %s\n"
               "  },\n"
               "  \"surface_generation\": %u,\n"
               "  \"thermal\": %s,\n"
               "  \"progress\": {\n"
               "    \"render_age_ms\": %llu,\n"
               "    \"semantic_freshness_ms\": %s,\n"
               "    \"last_chapter\": %d,\n"
               "    \"last_verse\": %d,\n"
               "    \"is_active_stall\": %s,\n"
               "    \"expected_inactivity\": %s\n"
               "  },\n"
               "  \"recorder\": {\n"
               "    \"total_emitted\": %llu,\n"
               "    \"total_dropped\": %llu,\n"
               "    \"chunks_committed\": %u,\n"
               "    \"total_records_written\": %llu,\n"
               "    \"total_bytes_written\": %llu,\n"
               "    \"remaining_budget\": %llu,\n"
               "    \"reserve_held\": %s,\n"
               "    \"incidents_written\": %u,\n"
               "    \"active_producers\": %u\n"
               "  },\n"
               "  \"total_emitted\": %llu,\n"
               "  \"total_dropped\": %llu,\n"
               "  \"chunks_committed\": %u,\n"
               "  \"total_records_written\": %llu,\n"
               "  \"total_bytes_written\": %llu,\n"
               "  \"remaining_budget\": %llu,\n"
               "  \"reserve_held\": %s,\n"
               "  \"incidents_written\": %u,\n"
               "  \"active_producers\": %u\n"
               "}\n",
            g_cfg.run_id,
            (unsigned long long)now_ns,
            healthy ? "true" : "false",
            state_str,
            lifecycle_str,
            live.is_foreground ? "true" : "false",
            live.surface_generation,
            thermal_buf,
            (unsigned long long)render_age_ms,
            semantic_freshness_buf,
            live.last_chapter,
            live.last_verse,
            is_active_stall ? "true" : "false",
            expected_inactivity ? "true" : "false",
            (unsigned long long)diag_get_total_emitted(),
            (unsigned long long)diag_get_total_dropped(),
            g_chunks_committed,
            (unsigned long long)g_total_records_written,
            (unsigned long long)g_total_bytes_written,
            (unsigned long long)remaining_budget,
            g_reserve_held ? "true" : "false",
            g_incidents_written,
            active_count,
            (unsigned long long)diag_get_total_emitted(),
            (unsigned long long)diag_get_total_dropped(),
            g_chunks_committed,
            (unsigned long long)g_total_records_written,
            (unsigned long long)g_total_bytes_written,
            (unsigned long long)remaining_budget,
            g_reserve_held ? "true" : "false",
            g_incidents_written,
            active_count);

    fflush(f);
    fclose(f);

    return wt_atomic_rename(tmp_path, final_path);
}

int diag_writer_write_incident(const char *name, const void *data, size_t size) {
    if (!name || (!data && size > 0) || g_state == DIAG_WRITER_IDLE) {
        return -1;
    }

    /* Incident capture can use the reserve, up to the total storage budget */
    if (g_total_bytes_written + size > g_cfg.storage_budget_bytes) {
        return -2; /* Exceeds hard storage ceiling */
    }

    char tmp_path[320];
    char final_path[320];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.tmp", g_cfg.output_dir, name);
    snprintf(final_path, sizeof(final_path), "%s/%s", g_cfg.output_dir, name);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        return -1;
    }

    if (size > 0) {
        size_t written = fwrite(data, 1, size, f);
        if (written != size) {
            fclose(f);
            remove(tmp_path);
            return -1;
        }
    }

    fflush(f);
    fclose(f);

    if (wt_atomic_rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        return -1;
    }

    g_incidents_written++;
    g_total_bytes_written += size;
    diag_writer_write_status();
    return 0;
}

int diag_writer_trigger_incident(const char *trigger_type,
                                 const char *reason_code,
                                 const char *message,
                                 const void *aux_data,
                                 size_t aux_size) {
    if (!trigger_type || !reason_code || g_state == DIAG_WRITER_IDLE) {
        return -1;
    }

    /* 1. Deep incident ceiling check: strictly bounded to DIAG_MAX_INCIDENTS (3).
     * Subsequent storms are tracked without overwriting earlier incidents. */
    if (g_incidents_written >= DIAG_MAX_INCIDENTS) {
        g_incident_storms++;
        diag_writer_write_status();
        return -3;
    }

    /* 2. Flush pending producer records to ensure recent history is committed */
    diag_writer_flush_chunks();

    /* 3. Disk budget check: incident data uses the 16 MiB reserve, bounded by storage_budget_bytes */
    size_t est_bytes = 4096 + (g_prehistory_count * 128) + aux_size;
    if (g_total_bytes_written + est_bytes > g_cfg.storage_budget_bytes) {
        g_state = DIAG_WRITER_EXHAUSTED;
        return -2;
    }

    uint32_t inc_id = g_incidents_written + 1;
    char inc_dir[320];
    snprintf(inc_dir, sizeof(inc_dir), "%s/incidents/incident-%03u", g_cfg.output_dir, inc_id);
    if (make_dir_p(inc_dir) != 0) {
        g_state = DIAG_WRITER_ERROR;
        return -1;
    }

    uint64_t now_ns = wt_time_ns();
    size_t inc_total_bytes = 0;

    /* 4. Dump writer-owned prehistory.jsonl */
    char pre_tmp[360];
    char pre_final[360];
    snprintf(pre_tmp, sizeof(pre_tmp), "%s/prehistory.jsonl.tmp", inc_dir);
    snprintf(pre_final, sizeof(pre_final), "%s/prehistory.jsonl", inc_dir);

    FILE *f_pre = fopen(pre_tmp, "w");
    if (!f_pre) {
        g_state = DIAG_WRITER_ERROR;
        return -1;
    }

    uint64_t start_seq = 0;
    uint64_t end_seq = 0;
    size_t count = g_prehistory_count;

    if (count > 0) {
        size_t start_idx = (g_prehistory_count == DIAG_PREHISTORY_CAPACITY) ? g_prehistory_head : 0;
        start_seq = g_prehistory_ring[start_idx].seq;
        size_t last_idx = (g_prehistory_head + DIAG_PREHISTORY_CAPACITY - 1) % DIAG_PREHISTORY_CAPACITY;
        if (g_prehistory_count < DIAG_PREHISTORY_CAPACITY) {
            last_idx = g_prehistory_count - 1;
        }
        end_seq = g_prehistory_ring[last_idx].seq;
    }

    int hlen = fprintf(f_pre, "{\"incident_id\":%u,\"start_seq\":%llu,\"end_seq\":%llu,\"count\":%zu}\n",
                       inc_id, (unsigned long long)start_seq, (unsigned long long)end_seq, count);
    if (hlen < 0) {
        fclose(f_pre);
        remove(pre_tmp);
        return -1;
    }
    inc_total_bytes += (size_t)hlen;

    size_t cur = (g_prehistory_count == DIAG_PREHISTORY_CAPACITY) ? g_prehistory_head : 0;
    for (size_t i = 0; i < count; i++) {
        const diag_record_t *r = &g_prehistory_ring[cur];
        int n = fprintf(f_pre, "{\"seq\":%llu,\"ts\":%llu,\"type\":%u,\"prod\":%u,\"tid\":%u,\"gen\":%u,\"p0\":%lld,\"p1\":%lld,\"p2\":%lld,\"p3\":%lld}\n",
                        (unsigned long long)r->seq,
                        (unsigned long long)r->timestamp_ns,
                        r->type,
                        r->producer_id,
                        r->thread_id,
                        r->generation,
                        (long long)r->payload[0],
                        (long long)r->payload[1],
                        (long long)r->payload[2],
                        (long long)r->payload[3]);
        if (n < 0) {
            fclose(f_pre);
            remove(pre_tmp);
            return -1;
        }
        inc_total_bytes += (size_t)n;
        cur = (cur + 1) % DIAG_PREHISTORY_CAPACITY;
    }
    fflush(f_pre);
    fclose(f_pre);

    if (wt_atomic_rename(pre_tmp, pre_final) != 0) {
        remove(pre_tmp);
        return -1;
    }

    /* 5. Write incident.json metadata */
    char inc_tmp[360];
    char inc_final[360];
    snprintf(inc_tmp, sizeof(inc_tmp), "%s/incident.json.tmp", inc_dir);
    snprintf(inc_final, sizeof(inc_final), "%s/incident.json", inc_dir);

    FILE *f_inc = fopen(inc_tmp, "w");
    if (!f_inc) {
        return -1;
    }

    int inc_len = fprintf(f_inc,
            "{\n"
            "  \"schema_version\": \"1.0\",\n"
            "  \"incident_id\": \"incident-%03u\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"trigger_type\": \"%s\",\n"
            "  \"reason_code\": \"%s\",\n"
            "  \"message\": \"%s\",\n"
            "  \"timestamp_ns\": %llu,\n"
            "  \"prehistory\": {\n"
            "    \"event_count\": %zu,\n"
            "    \"start_seq\": %llu,\n"
            "    \"end_seq\": %llu\n"
            "  },\n"
            "  \"capture_coverage\": {\n"
            "    \"pre_trigger_screenshot\": \"none\",\n"
            "    \"deep_trace\": \"unavailable\",\n"
            "    \"post_trigger_coverage\": \"started\",\n"
            "    \"collector_start_ns\": %llu\n"
            "  },\n"
            "  \"recovery\": null\n"
            "}\n",
            inc_id,
            g_cfg.run_id,
            trigger_type,
            reason_code,
            message ? message : "",
            (unsigned long long)now_ns,
            count,
            (unsigned long long)start_seq,
            (unsigned long long)end_seq,
            (unsigned long long)now_ns);

    if (inc_len < 0) {
        fclose(f_inc);
        remove(inc_tmp);
        return -1;
    }
    inc_total_bytes += (size_t)inc_len;
    fflush(f_inc);
    fclose(f_inc);

    if (wt_atomic_rename(inc_tmp, inc_final) != 0) {
        remove(inc_tmp);
        return -1;
    }

    /* 6. Write optional aux_data */
    if (aux_data && aux_size > 0) {
        char aux_path[360];
        snprintf(aux_path, sizeof(aux_path), "%s/aux.bin", inc_dir);
        FILE *f_aux = fopen(aux_path, "wb");
        if (f_aux) {
            fwrite(aux_data, 1, aux_size, f_aux);
            fclose(f_aux);
            inc_total_bytes += aux_size;
        }
    }

    g_incidents_written++;
    g_total_bytes_written += inc_total_bytes;
    diag_writer_write_status();
    return (int)inc_id;
}

int diag_writer_record_recovery(uint32_t incident_id,
                                const char *recovery_reason,
                                uint64_t resume_timestamp_ns) {
    if (incident_id == 0 || incident_id > g_incidents_written || !recovery_reason) {
        return -1;
    }

    char inc_dir[320];
    snprintf(inc_dir, sizeof(inc_dir), "%s/incidents/incident-%03u", g_cfg.output_dir, incident_id);

    char inc_path[360];
    char inc_tmp[360];
    snprintf(inc_path, sizeof(inc_path), "%s/incident.json", inc_dir);
    snprintf(inc_tmp, sizeof(inc_tmp), "%s/incident.json.tmp", inc_dir);

    /* Read existing file to string */
    FILE *f_in = fopen(inc_path, "rb");
    if (!f_in) return -1;
    fseek(f_in, 0, SEEK_END);
    long sz = ftell(f_in);
    fseek(f_in, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f_in); return -1; }
    size_t rd = fread(buf, 1, sz, f_in);
    buf[rd] = '\0';
    fclose(f_in);

    /* Replace `"recovery": null` with linked recovery metadata */
    char *rec_pos = strstr(buf, "\"recovery\": null");
    if (!rec_pos) {
        free(buf);
        return -1;
    }

    FILE *f_out = fopen(inc_tmp, "w");
    if (!f_out) {
        free(buf);
        return -1;
    }

    size_t prefix_len = (size_t)(rec_pos - buf);
    fwrite(buf, 1, prefix_len, f_out);

    fprintf(f_out,
            "\"recovery\": {\n"
            "    \"recovered\": true,\n"
            "    \"reason\": \"%s\",\n"
            "    \"resume_timestamp_ns\": %llu\n"
            "  }\n"
            "}\n",
            recovery_reason,
            (unsigned long long)resume_timestamp_ns);

    free(buf);
    fflush(f_out);
    fclose(f_out);

    if (wt_atomic_rename(inc_tmp, inc_path) != 0) {
        remove(inc_tmp);
        return -1;
    }

    diag_writer_write_status();
    return 0;
}

void diag_writer_stop(void) {
    if (g_thread_active) {
        g_thread_running = 0;
        wt_thread_close(g_flusher_thread);
        g_thread_active = false;
    }

    /* Drain all pending records */
    diag_writer_flush_chunks();
    g_state = DIAG_WRITER_STOPPED;
    diag_writer_write_status();
}

void diag_writer_shutdown(void) {
    diag_writer_stop();
    diag_close_crash_channel();
    g_state = DIAG_WRITER_IDLE;
}

void diag_writer_get_stats(diag_writer_stats_t *out_stats) {
    if (!out_stats) return;
    out_stats->state = g_state;
    out_stats->total_records_written = g_total_records_written;
    out_stats->total_bytes_written = g_total_bytes_written;
    out_stats->chunks_committed = g_chunks_committed;
    out_stats->incidents_written = g_incidents_written;
    out_stats->reserve_held = g_reserve_held;
    if (g_cfg.storage_budget_bytes > g_total_bytes_written) {
        out_stats->remaining_budget = g_cfg.storage_budget_bytes - g_total_bytes_written;
    } else {
        out_stats->remaining_budget = 0;
    }
}

void diag_text_sink_write(const char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (g_text_sink_len + len <= TEXT_SINK_CAPACITY) {
        memcpy(g_text_sink_buf + g_text_sink_len, buf, len);
        g_text_sink_len += len;
    } else {
        size_t avail = TEXT_SINK_CAPACITY - g_text_sink_len;
        if (avail > 0) {
            memcpy(g_text_sink_buf + g_text_sink_len, buf, avail);
            g_text_sink_len += avail;
            g_text_sink_dropped += (len - avail);
        } else {
            g_text_sink_dropped += len;
        }
    }
}

size_t diag_text_sink_get_buffered(char *out_buf, size_t max_len) {
    if (!out_buf || max_len == 0) return 0;
    size_t to_copy = g_text_sink_len < max_len ? g_text_sink_len : max_len;
    memcpy(out_buf, g_text_sink_buf, to_copy);
    return to_copy;
}
