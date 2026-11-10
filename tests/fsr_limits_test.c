#include "fsr_limits.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(!fsr_limit_error(256, 256, 8, 4100, 9, 9, 15, 1, 1));
    assert(fsr_limit_error(255, 256, 8, 4100, 9, 9, 15, 1, 1));
    assert(fsr_limit_error(256, 255, 8, 4100, 9, 9, 15, 1, 1));
    assert(fsr_limit_error(256, 256, 7, 4100, 9, 9, 15, 1, 1));
    assert(fsr_limit_error(256, 256, 8, 4099, 9, 9, 15, 1, 1));
    assert(fsr_limit_error(256, 256, 8, 4100, 8, 9, 15, 1, 1));
    assert(fsr_limit_error(256, 256, 8, 4100, 9, 8, 15, 1, 1));
    assert(fsr_limit_error(256, 256, 8, 4100, 9, 9, 14, 1, 1));
    assert(fsr_limit_error(256, 256, 8, 4100, 9, 9, 15, 0, 1));
    assert(fsr_limit_error(256, 256, 8, 4100, 9, 9, 15, 1, 0));
    puts("FSR limit checks passed");
    return 0;
}
