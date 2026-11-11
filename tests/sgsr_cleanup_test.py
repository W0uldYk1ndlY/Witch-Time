"""Execute production SGSR setup cleanup for every partial handle combination."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "runtime/nvn_vk_sgsr.c").read_text(encoding="utf-8")
cleanup = re.search(r'static void sgsr_release_setup\(void\) \{.*?\n\}', source, re.S).group(0)
harness = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#define VK_NULL_HANDLE 0
static struct { int device; } V;
static struct { int pipe[2], pl[2], dl[2], nearest, linear; } SG;
static int destroyed[9];
static void destroy(int d, int handle, void *allocation) {
    (void)d; (void)allocation; assert(handle > 0 && handle <= 8);
    assert(!destroyed[handle]); destroyed[handle]++;
}
#define vkDestroyPipeline destroy
#define vkDestroyPipelineLayout destroy
#define vkDestroyDescriptorSetLayout destroy
#define vkDestroySampler destroy
''' + cleanup + r'''
int main(void) {
    for (unsigned mask = 0; mask < 256; mask++) {
        memset(&SG, 0, sizeof SG); memset(destroyed, 0, sizeof destroyed);
        int *handles[] = {&SG.pipe[0], &SG.pl[0], &SG.dl[0], &SG.pipe[1], &SG.pl[1], &SG.dl[1], &SG.nearest, &SG.linear};
        for (int i = 0; i < 8; i++) if (mask & (1u << i)) *handles[i] = i + 1;
        sgsr_release_setup(); sgsr_release_setup();
        for (int i = 0; i < 8; i++) {
            assert(!*handles[i]); assert(destroyed[i + 1] == !!(mask & (1u << i)));
        }
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sgsr-cleanup-") as directory:
    directory = Path(directory)
    c, exe = directory / "test.c", directory / ("test.exe" if os.name == "nt" else "test")
    c.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("SGSR setup cleanup: 256 partial handle combinations passed (mock Vulkan)")
