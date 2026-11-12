#ifndef WT_TEMPORAL_SOURCE_CAPTURE_H
#define WT_TEMPORAL_SOURCE_CAPTURE_H
#include <stdint.h>
#include <stddef.h>

#define WT_TSC_VERSION 2u
#define WT_TSC_FRAMES 4u
#define WT_TSC_RECORDS 1024u
#define WT_TSC_BYTES (256u * 1024u)
#define WT_TSC_OPEN 1u
#define WT_TSC_CLOSED 0x80000000u
#define WT_TSC_TERMINAL 0x40000000u
#define WT_TSC_WAITING 0x20000000u
#define WT_TSC_RAW_RECORDS (WT_TSC_RECORDS - 1u) /* One existing slot holds the pre-arm receipt. */
#define WT_TSC_C14_SITE 0x71008013a0ULL
#define WT_TSC_VTABLE_SITE 0x7100803178ULL
#define WT_TSC_TARGET_SITE 0x7100803184ULL
#define WT_TSC_VELOCITY_TARGET 0x7100d5740cULL
#define WT_TSC_VELOCITY_VTABLE 0x7102256000ULL
static inline int wt_tsc_gate_open(uint32_t state) {
    return (state & (WT_TSC_OPEN | WT_TSC_WAITING | WT_TSC_CLOSED | WT_TSC_TERMINAL)) == WT_TSC_OPEN;
}
static inline int wt_tsc_gate_waiting(uint32_t state) {
    return (state & (WT_TSC_OPEN | WT_TSC_WAITING | WT_TSC_CLOSED | WT_TSC_TERMINAL)) == WT_TSC_WAITING;
}
enum { WT_TSC_GUEST = 1, WT_TSC_BEGIN, WT_TSC_PROGRAM, WT_TSC_RANGE, WT_TSC_BACKEND, WT_TSC_TRIGGER };
/* Private local evidence only. Zero join means unknown, never a qualified motion source. */
typedef struct {
    uint32_t version, size, kind, reserved;
    uint64_t frame, sequence, site, word[16];
} WtTscRecord;
typedef void (*WtTscEmit)(uint64_t epoch, const WtTscRecord *);
typedef int (*WtTscBind)(uint32_t version, uint32_t record_size,
                        uint64_t epoch, const uint32_t *active, WtTscEmit emit);

/* start/finish belong to the run owner; frame is called at the eligible-scene boundary. */
int wt_tsc_start(const char *validated_run_dir, int requested, WtTscBind bind);
void wt_tsc_frame(uint64_t frame, int eligible);
int wt_tsc_finish(void);
/* Required before finish: include core producer-slot exhaustion and dropped hook records. */
void wt_tsc_core_loss(uint64_t lost_records);
extern uint32_t wt_tsc_active;
extern uint32_t wt_tsc_requested_run;
static inline int wt_tsc_requested(void) { return __atomic_load_n(&wt_tsc_requested_run, __ATOMIC_ACQUIRE) == 1; }
static inline int wt_tsc_enabled(void) { return wt_tsc_gate_open(__atomic_load_n(&wt_tsc_active, __ATOMIC_ACQUIRE)); }
/* Optional frontend hooks: begin and EVERY recorded command, not only selected binds. */
void wt_tsc_begin(uint64_t command_buffer);
void wt_tsc_command(uint64_t command_buffer);
void wt_tsc_ranges(uint64_t command_buffer, uint32_t stage, uint32_t first,
                   uint32_t count, const uint64_t *address_size_pairs);
void wt_tsc_program(uint64_t command_buffer, uint64_t program, uint64_t serial, uint64_t stage_mask);
/* Standalone selected-backend evidence: no submission/guest join is implied. */
void wt_tsc_backend(const uint64_t identity[16]);
uint64_t wt_tsc_code_hash(unsigned stage, const void *code, size_t size);
#endif
