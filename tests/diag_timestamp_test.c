#include "nvn_vk_timestamp.h"
#include <assert.h>
#include <stdio.h>

static void check(uint64_t a, uint64_t b, unsigned bits, uint64_t aa, uint64_t ba,
                  int result, int state, uint64_t expected) {
    uint64_t ticks = UINT64_MAX;
    assert(nvn_vk_timestamp_delta((VkTimestampResult){a, aa}, (VkTimestampResult){b, ba}, bits, result, &ticks) == state);
    assert(ticks == expected);
}
int main(void) {
    check(0, 0, 64, 1, 1, 0, 0, 0);
    check(0, 9, 64, 1, 1, 0, 0, 9);
    check(9, 9, 36, 1, 1, 0, 0, 0);
    check(0, 9, 0, 1, 1, 0, 2, 0);
    check(0, 9, 65, 1, 1, 0, 2, 0);
    check((UINT64_C(1) << 36) - 3, 2, 36, 1, 1, 0, 4, 5);
    check(UINT64_MAX - 2, 2, 64, 1, 1, 0, 4, 5);
    check(255, 0, 8, 1, 1, 0, 4, 1);
    check(256, 265, 8, 1, 1, 0, 0, 9);
    check(0, 9, 64, 0, 1, 0, 1, 0);
    check(0, 9, 64, 1, 0, 0, 1, 0);
    check(0, 9, 64, 1, 1, 1, 1, 0);
    check(0, 9, 64, 1, 1, -4, 3, 0);
    check(0, 9, 64, 0, 0, -1, 3, 0);
    puts("production timestamp helper: 14 cases passed");
    return 0;
}
