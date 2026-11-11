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
    assert(fsr_output_writable(1, 1, 0));
    assert(fsr_output_writable(1, 0, 1));
    assert(!fsr_output_writable(1, 0, 0));
    assert(!fsr_output_writable(0, 1, 1));
    assert(fsr_descriptor_limits(9, 9, 2, 2, 2, 2, 15));
    assert(!fsr_descriptor_limits(8, 9, 2, 2, 2, 2, 15));
    assert(!fsr_descriptor_limits(9, 8, 2, 2, 2, 2, 15));
    assert(!fsr_descriptor_limits(9, 9, 1, 2, 2, 2, 15));
    assert(!fsr_descriptor_limits(9, 9, 2, 1, 2, 2, 15));
    assert(!fsr_descriptor_limits(9, 9, 2, 2, 1, 2, 15));
    assert(!fsr_descriptor_limits(9, 9, 2, 2, 2, 1, 15));
    assert(!fsr_descriptor_limits(9, 9, 2, 2, 2, 2, 14));
    assert(fsr_lut_supported(1, 0, 0));
    assert(!fsr_lut_supported(1, 0, 1));
    assert(fsr_lut_supported(1, 1, 1));
    assert(!fsr_lut_supported(0, 1, 0));
    assert(fsr_dispatch_fits(17, 9, 3, 2));
    assert(!fsr_dispatch_fits(17, 9, 2, 2));
    assert(!fsr_dispatch_fits(17, 9, 3, 1));
    assert(!fsr_dispatch_fits(0, 9, 3, 2));
    puts("FSR limit checks passed");
    return 0;
}
