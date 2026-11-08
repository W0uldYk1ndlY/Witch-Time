/* POSIX diagnostic stack helpers. Bionic exposes backtrace() only at API 33;
 * the NDK unwinder supports the API 29 baseline without private libc APIs.
 * Like glibc backtrace(), this is best-effort in an existing crash handler,
 * not a guarantee of async-signal-safe unwinding or complete guest stacks. */
#ifndef WT_POSIX_STACKTRACE_H
#define WT_POSIX_STACKTRACE_H
#include <stdint.h>
#include <ucontext.h>
#ifdef __ANDROID__
#include <unwind.h>
typedef struct { void **pc; int used, cap; } WtUnwindState;
static _Unwind_Reason_Code wt_unwind_frame(struct _Unwind_Context *ctx, void *arg) {
    WtUnwindState *s = (WtUnwindState *)arg;
    uintptr_t pc = (uintptr_t)_Unwind_GetIP(ctx);
    if (pc) s->pc[s->used++] = (void *)pc;
    return s->used == s->cap ? _URC_END_OF_STACK : _URC_NO_REASON;
}
static inline int plat_backtrace(void **pc, int cap) {
    if (!pc || cap <= 0) return 0;
    WtUnwindState s = {pc, 0, cap};
    _Unwind_Backtrace(wt_unwind_frame, &s);
    return s.used;
}
#else
#include <execinfo.h>
static inline int plat_backtrace(void **pc, int cap) { return pc && cap > 0 ? backtrace(pc, cap) : 0; }
#endif
static inline void *plat_signal_pc(const void *context) {
    const ucontext_t *uc = (const ucontext_t *)context;
#if defined(__aarch64__)
    return (void *)(uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    return (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc;
    return NULL;
#endif
}
#endif
