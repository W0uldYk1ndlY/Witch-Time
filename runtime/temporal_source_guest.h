#ifndef WT_TEMPORAL_SOURCE_GUEST_H
#define WT_TEMPORAL_SOURCE_GUEST_H
#include "temporal_source_capture.h"
/* Included only by game_shim_eh.c (and the isolated guest test). */
static const uint32_t *wt_tsc_guest_active;
static WtTscEmit wt_tsc_guest_emit;
static uint64_t wt_tsc_guest_epoch;
/* The two original sites have no guest call between them; never join across Ctx/SP/thread/epoch. */
static _Thread_local struct { uint64_t epoch, context, sp, vtable; } wt_tsc_guest_table;
__declspec(dllexport) int game_temporal_source_bind(uint32_t version, uint32_t size,
                                                   uint64_t epoch, const uint32_t *active, WtTscEmit emit) {
    if (version != WT_TSC_VERSION || size != sizeof(WtTscRecord) || (!active != !emit) || (!!active != !!epoch)) return -1;
    if (!active) {
        __atomic_store_n(&wt_tsc_guest_epoch, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&wt_tsc_guest_active, NULL, __ATOMIC_RELEASE);
        __atomic_store_n(&wt_tsc_guest_emit, NULL, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&wt_tsc_guest_emit, emit, __ATOMIC_RELEASE);
        __atomic_store_n(&wt_tsc_guest_active, active, __ATOMIC_RELEASE);
        __atomic_store_n(&wt_tsc_guest_epoch, epoch, __ATOMIC_RELEASE);
    }
    return 0;
}
void wt_temporal_guest_site(const Ctx *c, uint64_t site) {
    uint64_t epoch = __atomic_load_n(&wt_tsc_guest_epoch, __ATOMIC_ACQUIRE);
    if (!epoch) return;
    const uint32_t *active = __atomic_load_n(&wt_tsc_guest_active, __ATOMIC_ACQUIRE);
    if (!active) return;
    uint32_t state = __atomic_load_n(active, __ATOMIC_ACQUIRE);
    if (!wt_tsc_gate_open(state) && !wt_tsc_gate_waiting(state)) return;
    if (!c) return;
    WtTscEmit emit = __atomic_load_n(&wt_tsc_guest_emit, __ATOMIC_ACQUIRE);
    if (!emit) return;
    WtTscRecord r = {0};
    r.version = WT_TSC_VERSION; r.size = sizeof r; r.kind = WT_TSC_GUEST; r.site = site;
    if (wt_tsc_gate_waiting(state)) {
        if (site == WT_TSC_VTABLE_SITE) {
            wt_tsc_guest_table.epoch = epoch;
            wt_tsc_guest_table.context = (uint64_t)(uintptr_t)c;
            wt_tsc_guest_table.sp = c->x[31];
            wt_tsc_guest_table.vtable = c->x[8];
            return;
        }
        r.kind = WT_TSC_TRIGGER;
        r.word[2] = (uint64_t)(uintptr_t)c;
        if (site == WT_TSC_TARGET_SITE && c->x[8] == WT_TSC_VELOCITY_TARGET) {
            r.word[0] = c->x[8];
            if (wt_tsc_guest_table.epoch == epoch && wt_tsc_guest_table.context == r.word[2] &&
                wt_tsc_guest_table.sp == c->x[31]) {
                r.word[1] = wt_tsc_guest_table.vtable;
                r.word[3] = 1;
            }
        }
        wt_tsc_guest_table.epoch = 0;
        if (site != WT_TSC_C14_SITE && (site != WT_TSC_TARGET_SITE || r.word[0] != WT_TSC_VELOCITY_TARGET)) return;
        emit(epoch, &r);
        return;
    }
    /* No guest dereferences: all values were already loaded by the original instructions. */
    r.word[0] = c->x[0]; r.word[1] = c->x[1]; r.word[2] = c->x[2];
    r.word[3] = c->x[3]; r.word[4] = c->x[4]; r.word[5] = c->x[8];
    r.word[6] = c->x[19]; r.word[7] = c->x[20]; r.word[8] = c->x[21];
    r.word[9] = c->x[22]; r.word[10] = c->x[24]; r.word[11] = c->x[30];
    r.word[12] = c->x[23]; r.word[13] = c->x[25]; r.word[14] = c->x[28];
    r.word[15] = (uint64_t)(uintptr_t)c;
    emit(epoch, &r);
}
#endif
