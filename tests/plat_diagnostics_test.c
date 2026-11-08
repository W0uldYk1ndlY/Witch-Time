#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include "posix/stacktrace.h"
int main(void) {
    void *pcs[3] = {NULL, (void *)1, (void *)2};
    assert(plat_backtrace(pcs, 0) == 0 && pcs[0] == NULL);
    assert(plat_backtrace(pcs, 1) == 1 && pcs[0] && pcs[1] == (void *)1 && pcs[2] == (void *)2);
    ucontext_t uc;
    memset(&uc, 0, sizeof uc);
#if defined(__aarch64__)
    uc.uc_mcontext.pc = 0x12345678;
#elif defined(__x86_64__)
    uc.uc_mcontext.gregs[REG_RIP] = 0x12345678;
#endif
    assert(plat_signal_pc(&uc) == (void *)(uintptr_t)0x12345678);
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
#if defined(__ANDROID__)
    assert(plat_mutex_owner(&cs) == UINT32_MAX);
#else
    assert(plat_mutex_owner(&cs) == 0);
#endif
    EnterCriticalSection(&cs);
#if !defined(__ANDROID__)
    assert(plat_mutex_owner(&cs) == GetCurrentThreadId());
#endif
    LeaveCriticalSection(&cs); DeleteCriticalSection(&cs);
    return 0;
}
