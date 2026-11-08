/* AAPCS64 call-boundary context, used where Bionic has no swapcontext().
 * x18 belongs to Android (TLS/shadow call stack); never use or overwrite it.
 * Signal masks and TLS remain properties of the executing thread. */
#ifndef WT_FIBER_AARCH64_H
#define WT_FIBER_AARCH64_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint64_t x19_x30[12];
    uint64_t sp;
    uint64_t d8_d15[8];
    uint32_t fpcr, fpsr;
} WtFiberContext;
_Static_assert(offsetof(WtFiberContext, sp) == 96, "fiber SP offset");
_Static_assert(offsetof(WtFiberContext, d8_d15) == 104, "fiber SIMD offset");
_Static_assert(offsetof(WtFiberContext, fpcr) == 168, "fiber FPCR offset");
_Static_assert(sizeof(WtFiberContext) == 176, "fiber context size");
void wt_fiber_swap(WtFiberContext *from, const WtFiberContext *to);
#endif
