/* Synthetic pixel-grid sentinel check: this does not execute a Vulkan rasterizer. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check(int w, int h, int rw, int rh) {
    unsigned char grid[37 * 29];
    assert(w <= 37 && h <= 29 && rw <= w && rh <= h);
    memset(grid, 0, sizeof grid);
    for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) {
        int gx = (2 * x + 1) * w / (2 * rw), gy = (2 * y + 1) * h / (2 * rh);
        grid[gy * w + gx] = 1;
    }
    int missing = 0;
    for (int i = 0; i < w * h; i++) missing += !grid[i];
    assert(missing > 0); /* The sentinel must detect the sparse original pass. */
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) grid[y * w + x] = 1;
    for (int i = 0; i < w * h; i++) assert(grid[i]);
}
int main(void) {
    check(17, 13, 11, 9);
    check(37, 29, 19, 15);
    check(19, 17, 18, 16);
    puts("synthetic full-grid sentinel checks passed (CPU model only)");
    return 0;
}
