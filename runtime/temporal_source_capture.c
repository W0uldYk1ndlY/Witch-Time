#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "temporal_source_capture.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static int artifact_readable(FILE *file) {
#if !defined(_WIN32) || defined(WT_TSC_TEST_POSIX_EXPORT)
    int fd = fileno(file);
    struct stat st;
    /* Match diagnostic export authority, without chmod following a substituted pathname. */
    if (fchmod(fd, 0666) || fstat(fd, &st) || (st.st_mode & 0666) != 0666) return -1;
#else
    (void)file; /* Windows keeps its existing ACL/CRT permission behavior. */
#endif
    return 0;
}

uint32_t wt_tsc_active = WT_TSC_CLOSED;
#define admission wt_tsc_active
#define WT_TSC_COUNT_MASK 0x3ffffffeu
static uint32_t lock;
static FILE *output, *summary;
static WtTscBind game_bind;
static uint64_t frame_now, first_frame, generation;
static uint32_t reserved, overflow, unavailable, started, window_complete;
static uint64_t core_loss;
static uint32_t core_loss_checked;
uint32_t wt_tsc_requested_run;
#define requested_run wt_tsc_requested_run
static int io_error, bound;
static struct { WtTscRecord record; uint32_t published; } slots[WT_TSC_RECORDS];
static struct { uint64_t cb, generation, ordinal; } buffers[32];
static void enter(void) { while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {} }
static void leave(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }
static void stop(void) {
    __atomic_fetch_or(&admission, WT_TSC_CLOSED, __ATOMIC_ACQ_REL);
}
static void lost(void) { __atomic_fetch_add(&unavailable, 1, __ATOMIC_RELAXED); }
static void append(WtTscRecord *r) {
    if (!wt_tsc_enabled()) return;
    uint64_t frame_snapshot = __atomic_load_n(&frame_now, __ATOMIC_ACQUIRE);
    uint32_t state = __atomic_load_n(&admission, __ATOMIC_ACQUIRE);
    do { if (!wt_tsc_gate_open(state)) return; }
    while (!__atomic_compare_exchange_n(&admission, &state, state + 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
#ifdef WT_TSC_TEST_AFTER_ADMIT
    WT_TSC_TEST_AFTER_ADMIT();
#endif
    /* Freeze the label across admission; a racing interior-frame change is explicit loss. */
    if (frame_snapshot != __atomic_load_n(&frame_now, __ATOMIC_ACQUIRE)) {
        lost(); __atomic_fetch_sub(&admission, 2, __ATOMIC_RELEASE); return;
    }
    uint32_t index = __atomic_fetch_add(&reserved, 1, __ATOMIC_ACQ_REL);
    if (index >= WT_TSC_RECORDS || index >= WT_TSC_BYTES / sizeof *r) {
        __atomic_fetch_add(&overflow, 1, __ATOMIC_RELAXED); stop();
    } else {
        r->version = WT_TSC_VERSION; r->size = sizeof *r;
        r->frame = frame_snapshot; r->sequence = (uint64_t)index + 1;
        slots[index].record = *r;
#ifdef WT_TSC_TEST_BEFORE_PUBLISH
        WT_TSC_TEST_BEFORE_PUBLISH();
#endif
        __atomic_store_n(&slots[index].published, 1, __ATOMIC_RELEASE);
    }
    __atomic_fetch_sub(&admission, 2, __ATOMIC_RELEASE);
}
static void guest_emit(const WtTscRecord *in) {
    if (!wt_tsc_enabled()) return;
    if (!in || in->version != WT_TSC_VERSION || in->size != sizeof *in || in->kind != WT_TSC_GUEST) {
        lost(); stop();
    } else { WtTscRecord r = *in; append(&r); }
}
int wt_tsc_start(const char *dir, int requested, WtTscBind bind) {
    /* The run owner must finish before restarting; do not overwrite live state. */
    if (__atomic_load_n(&requested_run, __ATOMIC_ACQUIRE) || (__atomic_load_n(&admission, __ATOMIC_ACQUIRE) & WT_TSC_COUNT_MASK) ||
        __atomic_load_n(&lock, __ATOMIC_ACQUIRE)) return -1;
    if (!requested) return 0;
    char path[1024], summary_path[1024];
    if (!dir || !*dir || snprintf(path, sizeof path, "%s/temporal_source.bin", dir) >= (int)sizeof path ||
        snprintf(summary_path, sizeof summary_path, "%s/temporal_source.json", dir) >= (int)sizeof summary_path) return -1;
    FILE *new_output = fopen(path, "wbx");
    if (!new_output) return -1;
    FILE *new_summary = fopen(summary_path, "wx");
    if (!new_summary) { fclose(new_output); remove(path); return -1; }
    if (artifact_readable(new_output) || artifact_readable(new_summary)) {
        fclose(new_summary); fclose(new_output);
        remove(summary_path); remove(path);
        return -1;
    }
    output = new_output; summary = new_summary;
    io_error = bound = 0;
    __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&window_complete, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&first_frame, 0, __ATOMIC_RELEASE); generation = 0;
    __atomic_store_n(&frame_now, 0, __ATOMIC_RELEASE);
    reserved = overflow = unavailable = core_loss_checked = 0; core_loss = 0;
    __atomic_store_n(&admission, WT_TSC_CLOSED, __ATOMIC_RELEASE);
    memset(slots, 0, sizeof slots);
    memset(buffers, 0, sizeof buffers);
    game_bind = bind;
    if (!bind || bind(WT_TSC_VERSION, sizeof(WtTscRecord), &admission, guest_emit)) lost();
    else bound = 1;
    __atomic_store_n(&requested_run, 1, __ATOMIC_RELEASE);
    return bound ? 0 : -1;
}
void wt_tsc_frame(uint64_t frame, int eligible) {
    if (!wt_tsc_requested()) return;
    enter();
    if (!wt_tsc_requested()) { leave(); return; }
#ifdef WT_TSC_TEST_BEFORE_ARM
    WT_TSC_TEST_BEFORE_ARM();
#endif
    uint32_t was_started = __atomic_load_n(&started, __ATOMIC_ACQUIRE);
    uint64_t first = __atomic_load_n(&first_frame, __ATOMIC_ACQUIRE);
    if (was_started && frame < first) { lost(); stop(); leave(); return; }
    if (was_started && frame - first >= WT_TSC_FRAMES) {
        __atomic_store_n(&window_complete, 1, __ATOMIC_RELEASE);
        stop(); leave(); return; /* Never publish the excluded boundary label. */
    }
    __atomic_store_n(&frame_now, frame, __ATOMIC_RELEASE);
    if (bound && !was_started && eligible) {
        __atomic_store_n(&first_frame, frame, __ATOMIC_RELEASE);
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
        uint32_t closed = WT_TSC_CLOSED;
        /* Terminal closure and first arm share one atomic word; finalization always wins last. */
        __atomic_compare_exchange_n(&admission, &closed, WT_TSC_OPEN, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    leave();
}
static int buffer_of(uint64_t cb) {
    for (int i = 0; i < 32; i++) if (buffers[i].generation && buffers[i].cb == cb) return i;
    return -1;
}
void wt_tsc_begin(uint64_t cb) {
    enter();
    if (!wt_tsc_enabled()) { leave(); return; }
    int i = buffer_of(cb);
    if (i < 0) for (int j = 0; j < 32; j++) if (!buffers[j].generation) { i = j; break; }
    if (i < 0) { lost(); stop(); leave(); return; }
    buffers[i].cb = cb; buffers[i].generation = ++generation; buffers[i].ordinal = 0;
    WtTscRecord r = {0}; r.kind = WT_TSC_BEGIN; r.word[0] = cb; r.word[1] = generation;
    append(&r); leave();
}
void wt_tsc_command(uint64_t cb) {
    enter();
    if (wt_tsc_enabled()) {
        int i = buffer_of(cb);
        if (i < 0) { lost(); }
        else buffers[i].ordinal++;
    }
    leave();
}
static int command_record(WtTscRecord *r, uint64_t cb) {
    int i = buffer_of(cb);
    if (i < 0 || !buffers[i].ordinal) { lost(); return 0; }
    r->word[0] = cb; r->word[1] = buffers[i].generation; r->word[2] = buffers[i].ordinal;
    return 1;
}
void wt_tsc_ranges(uint64_t cb, uint32_t stage, uint32_t first, uint32_t count, const uint64_t *pairs) {
    enter();
    if (wt_tsc_enabled()) {
        /* Only two slots, even when a caller supplies a huge batch count. */
        for (uint32_t slot = 9; slot <= 11; slot += 2) if (slot >= first && slot - first < count) {
            WtTscRecord r = {0}; r.kind = WT_TSC_RANGE;
            if (!pairs || !command_record(&r, cb)) { if (!pairs) { lost(); stop(); } break; }
            r.word[3] = stage; r.word[4] = slot;
            r.word[5] = pairs[2u * (slot - first)]; r.word[6] = pairs[2u * (slot - first) + 1u];
            append(&r);
        }
    }
    leave();
}
void wt_tsc_program(uint64_t cb, uint64_t program, uint64_t serial, uint64_t mask) {
    enter();
    if (wt_tsc_enabled()) {
        WtTscRecord r = {0}; r.kind = WT_TSC_PROGRAM;
        if (command_record(&r, cb)) {
            r.word[3] = program; r.word[4] = serial; r.word[5] = mask; append(&r);
        }
    }
    leave();
}
void wt_tsc_backend(const uint64_t identity[16]) {
    WtTscRecord r = {0}; r.kind = WT_TSC_BACKEND;
    memcpy(r.word, identity, sizeof r.word); append(&r);
}
uint64_t wt_tsc_code_hash(unsigned stage, const void *code, size_t size) {
    if (!code || !size) return 0;
    uint64_t h = 0xcbf29ce484222325ULL ^ stage;
    const unsigned char *p = code;
    for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}
void wt_tsc_core_loss(uint64_t count) {
    __atomic_fetch_add(&core_loss, count, __ATOMIC_RELAXED);
    __atomic_store_n(&core_loss_checked, 1, __ATOMIC_RELEASE);
}
int wt_tsc_finish(void) {
    uint32_t running = 1;
    if (!__atomic_compare_exchange_n(&requested_run, &running, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return running == 0 ? 0 : -1;
    __atomic_fetch_or(&admission, WT_TSC_CLOSED | WT_TSC_TERMINAL, __ATOMIC_ACQ_REL);
    /* Never wait for a paused guest writer or read an unpublished slot. */
    uint32_t pending = (__atomic_load_n(&admission, __ATOMIC_ACQUIRE) & WT_TSC_COUNT_MASK) >> 1;
    uint32_t tracking_busy = __atomic_load_n(&lock, __ATOMIC_ACQUIRE);
    uint32_t count = __atomic_load_n(&reserved, __ATOMIC_ACQUIRE);
    if (count > WT_TSC_RECORDS) count = WT_TSC_RECORDS;
    uint32_t records = 0, bytes = 0, unpublished = 0, guest_records = 0, backend_records = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!__atomic_load_n(&slots[i].published, __ATOMIC_ACQUIRE)) { unpublished++; continue; }
        const WtTscRecord *r = &slots[i].record;
        if (fwrite(r, sizeof *r, 1, output) != 1) { io_error = 1; break; }
        records++; bytes += sizeof *r;
        if (r->kind == WT_TSC_GUEST) guest_records++;
        if (r->kind == WT_TSC_BACKEND) backend_records++;
    }
    if (bound) game_bind(WT_TSC_VERSION, sizeof(WtTscRecord), NULL, NULL);
    if (fclose(output)) io_error = 1;
    uint32_t loss = __atomic_load_n(&unavailable, __ATOMIC_ACQUIRE);
    uint32_t over = __atomic_load_n(&overflow, __ATOMIC_ACQUIRE);
    uint64_t transport_loss = __atomic_load_n(&core_loss, __ATOMIC_ACQUIRE);
    uint32_t loss_checked = __atomic_load_n(&core_loss_checked, __ATOMIC_ACQUIRE);
    int complete = !pending && !tracking_busy && !unpublished && !io_error && !loss && !over && !transport_loss &&
        loss_checked && bound && __atomic_load_n(&window_complete, __ATOMIC_ACQUIRE) && guest_records;
    FILE *f = summary;
    if (f) {
        if (fprintf(f, "{\"schema_version\":1,\"collector\":\"temporal_source_capture\","
            "\"private_local_only\":true,\"record_size\":%u,\"records\":%u,\"bytes\":%u,"
            "\"record_cap\":%u,\"byte_cap\":%u,\"frame_window\":%u,\"first_frame\":%llu,"
            "\"overflow\":%u,\"unavailable\":%u,\"io_error\":%s,\"eligible_seen\":%s,"
            "\"complete\":%s,\"window_complete\":%s,\"pending_writers\":%u,\"unpublished_slots\":%u,"
            "\"tracking_busy\":%s,"
            "\"core_loss_checked\":%s,\"core_lost_records\":%llu,"
            "\"guest_bound\":%s,\"guest_records\":%u,\"guest_sites\":\"%s\",\"backend_selection_records\":%u,\"join\":\"unknown\",\"backend_execution\":\"unavailable\","
            "\"motion_source_qualified\":false}\n",
            (unsigned)sizeof(WtTscRecord), records, bytes, WT_TSC_RECORDS, WT_TSC_BYTES, WT_TSC_FRAMES,
            (unsigned long long)__atomic_load_n(&first_frame, __ATOMIC_ACQUIRE), over, loss, io_error ? "true" : "false",
            __atomic_load_n(&started, __ATOMIC_ACQUIRE) ? "true" : "false", complete ? "true" : "false",
            __atomic_load_n(&window_complete, __ATOMIC_ACQUIRE) ? "true" : "false", pending, unpublished,
            tracking_busy ? "true" : "false",
            loss_checked ? "true" : "false", (unsigned long long)transport_loss,
            bound ? "true" : "false", guest_records,
            guest_records ? "raw_evidence" : "unavailable", backend_records) < 0) io_error = 1;
        if (fclose(f)) io_error = 1;
    }
    output = summary = NULL;
    __atomic_store_n(&requested_run, 0, __ATOMIC_RELEASE);
    return io_error || !complete ? -1 : 0;
}
