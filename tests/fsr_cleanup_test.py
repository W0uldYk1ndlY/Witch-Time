"""Exercise the production FSR partial-setup cleanup using mocked Vulkan handles."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime" / "nvn_vk_fsr.c").read_text(encoding="utf-8")
match = re.search(r'static void fsr_release_setup\(void\) \{.*?\n\}', source, re.S)
assert match, "production cleanup function not found"
setup = source.split("static int fsr_setup(void)", 1)[1].split("static void fsr_resize", 1)[0]
assert setup.count("fsr_release_setup(); fsr_off(") == 2, "compile and pipeline failures must clean up"

harness = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#define P_N 8
#define VK_NULL_HANDLE 0
typedef struct { int block; } VMem;
static struct { int device; } V;
static struct { int pipe[P_N], pl[P_N], dl[P_N], smp[2], ubo; VMem ubo_mem; } F;
static int pipes, layouts, descriptors, samplers, buffers, frees;
static void vkDestroyPipeline(int d, int v, void *a) { (void)d; (void)a; assert(v); pipes++; }
static void vkDestroyPipelineLayout(int d, int v, void *a) { (void)d; (void)a; assert(v); layouts++; }
static void vkDestroyDescriptorSetLayout(int d, int v, void *a) { (void)d; (void)a; assert(v); descriptors++; }
static void vkDestroySampler(int d, int v, void *a) { (void)d; (void)a; assert(v); samplers++; }
static void vkDestroyBuffer(int d, int v, void *a) { (void)d; (void)a; assert(v); buffers++; }
static void vmem_free(VMem m) { assert(m.block == 1); frees++; }
''' + match.group(0) + r'''
int main(void) {
    for (int n = 0; n <= P_N; ++n) {
        memset(&F, 0, sizeof F);
        pipes = layouts = descriptors = samplers = buffers = frees = 0;
        for (int i = 0; i < n; ++i) F.pipe[i] = F.pl[i] = F.dl[i] = 1;
        F.smp[0] = F.smp[1] = F.ubo = F.ubo_mem.block = 1;
        fsr_release_setup();
        assert(pipes == n && layouts == n && descriptors == n);
        assert(samplers == 2 && buffers == 1 && frees == 1);
        fsr_release_setup();
        assert(pipes == n && layouts == n && descriptors == n);
        assert(samplers == 2 && buffers == 1 && frees == 1);
    }
    memset(&F, 0, sizeof F);
    pipes = layouts = descriptors = samplers = buffers = frees = 0;
    F.dl[0] = F.pl[0] = 1; /* compile or pipeline creation failed before its first pipeline */
    F.smp[0] = F.smp[1] = F.ubo = F.ubo_mem.block = 1;
    fsr_release_setup();
    assert(pipes == 0 && layouts == 1 && descriptors == 1 && samplers == 2 && buffers == 1 && frees == 1);
    memset(&F, 0, sizeof F);
    pipes = layouts = descriptors = samplers = buffers = frees = 0;
    F.pipe[0] = F.dl[0] = F.pl[0] = F.dl[1] = F.pl[1] = 1;
    fsr_release_setup();
    assert(pipes == 1 && layouts == 2 && descriptors == 2 && samplers == 0 && buffers == 0 && frees == 0);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="fsr-cleanup-") as temp:
    temp = Path(temp)
    c_file, exe = temp / "test.c", temp / ("test.exe" if os.name == "nt" else "test")
    c_file.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("FSR partial-setup cleanup checks passed (mock Vulkan handles)")
