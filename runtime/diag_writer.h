#ifndef WITCHTIME_DIAG_WRITER_H
#define WITCHTIME_DIAG_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "diag.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum diag_writer_state {
    DIAG_WRITER_IDLE = 0,
    DIAG_WRITER_ACTIVE = 1,
    DIAG_WRITER_EXHAUSTED = 2,
    DIAG_WRITER_STOPPED = 3,
    DIAG_WRITER_ERROR = 4
} diag_writer_state_t;

typedef struct diag_writer_config {
    char output_dir[256];             /* Directory for session artifacts */
    char run_id[128];                 /* Session run ID */
    uint64_t storage_budget_bytes;    /* Total disk budget (default: 128 MiB) */
    uint64_t incident_reserve_bytes;  /* First-incident disk reserve (default: 16 MiB) */
    uint32_t chunk_record_limit;      /* Max records per chunk (default: 1000) */
    uint32_t flush_interval_ms;       /* Background flush interval (0 for manual) */
    bool disable_unbuffered_logging;  /* Opt-out of unbuffered legacy stdout */
} diag_writer_config_t;

typedef struct diag_writer_stats {
    diag_writer_state_t state;
    uint64_t total_records_written;
    uint64_t total_bytes_written;
    uint64_t remaining_budget;
    uint32_t chunks_committed;
    uint32_t incidents_written;
    bool reserve_held;
} diag_writer_stats_t;

/* Status snapshot structure (serializes to status.json) */
typedef struct diag_status_snapshot {
    char schema_version[16];
    char run_id[128];
    uint64_t timestamp_ns;
    bool healthy;
    diag_writer_state_t state;
    uint64_t total_emitted;
    uint64_t total_dropped;
    uint32_t chunks_committed;
    uint64_t total_bytes_written;
    uint64_t remaining_budget;
    bool reserve_held;
    uint32_t incidents_written;
    uint32_t active_producers;
    /* Stage B continuous and live status additions */
    diag_lifecycle_state_t lifecycle_state;
    bool is_foreground;
    uint32_t surface_generation;
    float thermal_headroom;
    diag_thermal_status_t thermal_status;
    uint64_t render_age_ms;
    uint64_t semantic_freshness_ms;
    int32_t last_chapter;
    int32_t last_verse;
    bool is_active_stall;
    bool expected_inactivity;
} diag_status_snapshot_t;

/* Initialize writer. Returns 0 on success, -1 on invalid args, -2 if output directory already exists with prior run */
int  diag_writer_init(const diag_writer_config_t *cfg);

/* Start background flusher thread if flush_interval_ms > 0 */
int  diag_writer_start(void);

/* Drain all active producers and commit completed chunk(s) */
int  diag_writer_flush_chunks(void);

/* Atomically publish status.json */
int  diag_writer_write_status(void);

/* Write first-incident data using the disk reserve */
int  diag_writer_write_incident(const char *name, const void *data, size_t size);

#define DIAG_MAX_INCIDENTS 3

typedef struct diag_incident_info {
    uint32_t incident_id;
    char trigger_type[64];
    char reason_code[64];
    char message[256];
    uint64_t timestamp_ns;
    uint32_t prehistory_events;
    bool recovered;
} diag_incident_info_t;

/* Trigger a new bounded incident with writer-owned prehistory.
 * Returns incident_id (1..3) on success,
 * -1 on invalid args, -2 on disk exhaustion, -3 if max incidents (3) reached. */
int diag_writer_trigger_incident(const char *trigger_type,
                                 const char *reason_code,
                                 const char *message,
                                 const void *aux_data,
                                 size_t aux_size);

/* Record a linked recovery for an existing incident */
int diag_writer_record_recovery(uint32_t incident_id,
                                const char *recovery_reason,
                                uint64_t resume_timestamp_ns);

/* Stop background flusher, perform final flush, publish final status */
void diag_writer_stop(void);

/* Full shutdown and cleanup */
void diag_writer_shutdown(void);

/* Retrieve current stats */
void diag_writer_get_stats(diag_writer_stats_t *out_stats);

/* Bounded text sink */
void diag_text_sink_write(const char *buf, size_t len);
size_t diag_text_sink_get_buffered(char *out_buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* WITCHTIME_DIAG_WRITER_H */
