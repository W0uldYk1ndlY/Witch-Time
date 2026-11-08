/* Repeated context switches, independent stacks, arguments and FP state.
 * On arm64, the assembly call boundary also verifies every callee-saved GPR
 * and the ABI-preserved low halves of v8-v15. */
#include <assert.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

static void *main_fiber, *worker[2];
static int visits[2];
#if defined(__aarch64__)
extern int wt_test_switch_preserved(void *fiber);
static void switch_checked(void *fiber) { assert(wt_test_switch_preserved(fiber)); }
#else
static void switch_checked(void *fiber) { SwitchToFiber(fiber); }
#endif
static void run_fiber(void *arg) {
    unsigned index = (unsigned)(uintptr_t)arg;
    volatile uint64_t canary[128];
    for (unsigned j = 0; j < 128; j++) canary[j] = 0x123456789abcdef0ull + index + j;
    int mode = index ? FE_UPWARD : FE_DOWNWARD;
    assert(fesetround(mode) == 0);
    for (;;) {
        assert(fegetround() == mode);
        for (unsigned j = 0; j < 128; j++) assert(canary[j] == 0x123456789abcdef0ull + index + j);
        visits[index]++;
        switch_checked(main_fiber);
    }
}
int main(void) {
    assert(fesetround(FE_TONEAREST) == 0);
    main_fiber = ConvertThreadToFiber(NULL); assert(main_fiber);
    worker[0] = CreateFiberEx(0, 1 << 20, 0, run_fiber, (void *)(uintptr_t)0);
    worker[1] = CreateFiberEx(0, 1 << 20, 0, run_fiber, (void *)(uintptr_t)1);
    assert(worker[0] && worker[1]);
    for (int i = 1; i <= 1000; i++) {
        switch_checked(worker[0]);
        assert(visits[0] == i && fegetround() == FE_TONEAREST);
        switch_checked(worker[1]);
        assert(visits[1] == i && fegetround() == FE_TONEAREST);
    }
    DeleteFiber(worker[0]); DeleteFiber(worker[1]);
    puts("POSIX fiber switch / stack / FP-state tests: passed");
    return 0;
}
