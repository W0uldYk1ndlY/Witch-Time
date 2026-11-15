#!/usr/bin/env python3
"""Run production sampler lookup and PC-menu flush with recorded Vulkan calls.

The lookup-cost assertion catches a return to scanning every existing sampler.
The other cases catch false cache hits, lost entries at capacity, and stale
samplers after graphics settings change. No Vulkan device is required.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRAW = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
MENU = (ROOT / "runtime/nvn_vk_pcmenu.c").read_text(encoding="utf-8")
FRONT = (ROOT / "runtime/nvn_front.h").read_text(encoding="utf-8")


def production_source():
    params = re.search(r"typedef struct \{[^\n]+\} SamplerParams;", FRONT).group(0)
    start = DRAW.index("/* ---------------------------------------------------------------- samplers")
    end = DRAW.index("\n}\n", DRAW.index("static VkSampler sampler_get(", start)) + 3
    job_start = MENU.index("void nvn_vk_pcmenu_job(void *arg) {")
    job_end = MENU.index("\n}\n", job_start) + 3
    return (params + "\n#define memcmp counted_memcmp\n" + DRAW[start:end] +
            "\n#undef memcmp\n" + MENU[job_start:job_end])


PRELUDE = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_options.h"

typedef long long LONG64;
/* Temporal-source diagnostics are outside this lookup/creation fixture. */
static uint64_t wt_tsc_epoch(void) { return 0; }
static int wt_tsc_requested(void) { return 0; }
static int wt_tsc_enabled(void) { return 0; }
static void wt_tsc_sampler_loss(void) { assert(0); }
static void wt_tsc_sampler(const uint64_t *words) { (void)words; assert(0); }
static struct { VkDevice device; VkPhysicalDeviceProperties props; int alive; VkSwapchainKHR swapchain; } V;
static struct { VkPhysicalDeviceFeatures2 f2; VkPhysicalDeviceVulkan12Features f12; int has_cbc; } D;
static GfxOptions options;
static const GfxOptions *gfx_options_test(void) { return &options; }
#define gfx_options gfx_options_test
static unsigned comparisons, created, destroyed, waited;
static int next_work, expected_failure;
static jmp_buf failure;
static VkSamplerCreateInfo create_info[8192];
static int counted_memcmp(const void *a, const void *b, size_t n) {
    comparisons++;
    return memcmp(a, b, n);
}
static void vkd_fail(const char *reason, ...) {
    assert(expected_failure && !strcmp(reason, "more than 2048 distinct samplers"));
    longjmp(failure, 1);
}
static void nvn_vk_missing(const char *reason) { (void)reason; }
static VkResult vkCreateSampler(VkDevice d, const VkSamplerCreateInfo *ci,
                               const VkAllocationCallbacks *alloc, VkSampler *out) {
    (void)d; (void)alloc;
    assert(created < 8192);
    create_info[created] = *ci;
    create_info[created].pNext = NULL;
    *out = (VkSampler)(uintptr_t)++created;
    return VK_SUCCESS;
}
static void vkDestroySampler(VkDevice d, VkSampler s, const VkAllocationCallbacks *alloc) {
    (void)d; (void)s; (void)alloc; destroyed++;
}
#define VKC(call) assert((call) == VK_SUCCESS)
static struct { int pending; } g_pcm_shot;
static struct { void *win; } g_pcm_last;
static long long g_vk_recreated;
static int pcmenu_take_work(void) { int result = next_work; next_work = 0; return result; }
static void cmd_wait_all(void) { waited++; }
static void pcm_shot_write(void) { }
static void present_mode_pick(void) { assert(0); }
static void swapchain_destroy(void) { assert(0); }
static void InterlockedIncrement64(long long *v) { ++*v; }
static void rs_apply(void) { assert(0); }
static void pcm_upload(int wait) { (void)wait; }
static int pcmenu_is_open(void) { return 0; }
static void vk_present2(void *job, int progress) { (void)job; (void)progress; assert(0); }
'''


CASES = r'''
static SamplerParams params(void) {
    SamplerParams p = {0};
    p.min = 5; p.mag = 1; p.ws = p.wt = p.wr = 1;
    p.lodmax = 20.0f; p.aniso = 1.0f;
    return p;
}
static SamplerParams numbered(unsigned n) {
    SamplerParams p = params(); p.lodmax += (float)n; return p;
}
static void lookup_cost(void) {
    for (unsigned n = 0; n < 1024; n++) {
        SamplerParams p = numbered(n);
        assert(sampler_get(&p) == (VkSampler)(uintptr_t)(n + 1));
    }
    comparisons = 0;
    for (unsigned n = 1024; n-- > 0;) {
        SamplerParams p = numbered(n);
        assert(sampler_get(&p) == (VkSampler)(uintptr_t)(n + 1));
    }
    fprintf(stderr, "1024 cached lookups: %u full-key comparisons\n", comparisons);
    assert(created == 1024);
    assert(comparisons <= 8192 && "cached lookup must not scan all existing samplers");
}
static void colliding_keys(void) {
    /* These byte keys hash to bucket 4095 under the production FNV-1a index;
     * the fourth insertion exercises probing across the end of the table. */
    const uint32_t bits[4] = {0x3f000484, 0x3f000fc1, 0x3f003a3a, 0x3f003d47};
    SamplerParams p[4];
    for (unsigned n = 0; n < 4; n++) {
        p[n] = params(); memcpy(&p[n].bias, &bits[n], 4);
        assert(sampler_get(&p[n]) == (VkSampler)(uintptr_t)(n + 1));
    }
    for (unsigned n = 4; n-- > 0;)
        assert(sampler_get(&p[n]) == (VkSampler)(uintptr_t)(n + 1));
    assert(created == 4 && g_nsmp == 4);
}
static void exact_bytes(void) {
    SamplerParams base = params();
    assert(sizeof base == 48);
    assert(sampler_get(&base) == (VkSampler)(uintptr_t)1);
    /* Includes every float's representation, integer fields and explicit pad. */
    for (unsigned n = 0; n < sizeof base; n++) {
        SamplerParams p = base;
        ((unsigned char *)&p)[n] ^= 1;
        VkSampler want = (VkSampler)(uintptr_t)(n + 2);
        assert(sampler_get(&p) == want);
        assert(sampler_get(&p) == want);
    }
    assert(created == 49);
    const uint32_t bits[3] = {0x80000000, 0x7fc00001, 0x7fc00002};
    for (unsigned n = 0; n < 3; n++) {
        SamplerParams p = base; memcpy(&p.bias, &bits[n], 4);
        VkSampler want = (VkSampler)(uintptr_t)(n + 50);
        assert(sampler_get(&p) == want);
        assert(sampler_get(&p) == want);
    }
    assert(sampler_get(&base) == (VkSampler)(uintptr_t)1);
    assert(created == 52);
}
static void capacity(void) {
    for (unsigned n = 0; n < 2048; n++) {
        SamplerParams p = numbered(n);
        assert(sampler_get(&p) == (VkSampler)(uintptr_t)(n + 1));
    }
    assert(g_nsmp == 2048);
    for (unsigned n = 2048; n-- > 0;) {
        SamplerParams p = numbered(n);
        assert(sampler_get(&p) == (VkSampler)(uintptr_t)(n + 1));
    }
    assert(created == 2048);
    expected_failure = 1;
    if (!setjmp(failure)) {
        SamplerParams p = numbered(2048);
        (void)sampler_get(&p);
        assert(0 && "the existing 2048-sampler limit must remain enforced");
    }
    assert(g_nsmp == 2048);
}
static void settings_flush(void) {
    SamplerParams p = params(); p.bias = 0.25f;
    options.aniso = 2; options.texture_lod_bias = 0.5f;
    VkSampler first = sampler_get(&p);
    assert(create_info[0].maxAnisotropy == 2.0f && create_info[0].mipLodBias == 0.75f);
    options.aniso = 8; options.texture_lod_bias = -1.0f;
    next_work = 1; nvn_vk_pcmenu_job(NULL);
    assert(waited == 1 && g_nsmp == 0 && g_nsmp_old == 1);
    assert(g_smp_old[0] == first && destroyed == 0);
    VkSampler second = sampler_get(&p);
    assert(second != first && created == 2);
    assert(create_info[1].maxAnisotropy == 8.0f && create_info[1].mipLodBias == -0.75f);
    assert(sampler_get(&p) == second && created == 2);

    /* Refused flushes preserve both the active objects and their lookup index. */
    g_nsmp_old = 4096;
    options.aniso = 16;
    next_work = 1; nvn_vk_pcmenu_job(NULL);
    assert(g_nsmp == 1 && g_nsmp_old == 4096 && destroyed == 0);
    assert(sampler_get(&p) == second && created == 2);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    V.alive = 1;
    V.props.limits.maxSamplerAnisotropy = 16.0f;
    V.props.limits.maxSamplerLodBias = 4.0f;
    D.f2.features.samplerAnisotropy = VK_TRUE;
    D.f12.samplerFilterMinmax = VK_TRUE; D.has_cbc = 1;
    if (!strcmp(argv[1], "cost")) lookup_cost();
    else if (!strcmp(argv[1], "collision")) colliding_keys();
    else if (!strcmp(argv[1], "bytes")) exact_bytes();
    else if (!strcmp(argv[1], "capacity")) capacity();
    else if (!strcmp(argv[1], "flush")) settings_flush();
    else assert(0);
    return 0;
}
'''


class SamplerCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get("HOST_CC", ""))
        if not cc:
            cc = [shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")]
        if not cc[0]:
            raise RuntimeError("A C compiler is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="vulkan-sampler-cache-")
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / "test.c"
        cls.exe = Path(cls.temp.name) / ("test.exe" if os.name == "nt" else "test")
        source.write_text(PRELUDE + production_source() + CASES, encoding="utf-8")
        subprocess.run(cc + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-Wno-unused-variable",
                            "-Wno-missing-field-initializers", "-I", str(ROOT / "runtime"),
                            "-I", str(ROOT / "deps/android-arm64/include")] +
                       shlex.split(os.environ.get("CPPFLAGS", "")) +
                       [str(source), "-o", str(cls.exe)], check=True, capture_output=True, text=True)

    def run_case(self, name):
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cached_lookup_avoids_linear_scan(self):
        self.run_case("cost")

    def test_colliding_keys_and_probe_wraparound(self):
        self.run_case("collision")

    def test_every_parameter_byte_and_float_bit_pattern(self):
        self.run_case("bytes")

    def test_existing_limit_and_hits_at_capacity(self):
        self.run_case("capacity")

    def test_menu_flush_recreates_with_new_graphics_settings(self):
        self.run_case("flush")


if __name__ == "__main__":
    unittest.main()
