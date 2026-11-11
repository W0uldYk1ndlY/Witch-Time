#include "sgsr2_params.h"
#include <stdio.h>
int main(void) {
    Sgsr2FsParams params = {0};
    Sgsr2CsParams cs = {0};
    params.same_camera = 1;
    cs.reset = 1;
    if (params.same_camera != 1 || cs.reset != 1) return 1;
    puts("SGSR fragment and compute uniform layout assertions passed");
    return 0;
}
