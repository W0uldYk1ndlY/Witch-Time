#!/usr/bin/env python3
"""Exercise production Android WSI retry branches with scripted platform calls.
No window, GPU, device lifecycle, or presentation correctness is simulated.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "runtime/nvn_vk.c").read_text()


def function(name):
    signature = "static int " + name + "("
    start = SOURCE.index(signature)
    return SOURCE[start:SOURCE.index("\n}", start) + 2]


PRELUDE = r'''
#define VK_NO_PROTOTYPES
#define __ANDROID__ 1
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void SDL_Window;
typedef int LONG;
typedef struct { int right, bottom; } RECT;
static struct {
    VkInstance instance; void *hwnd; VkPhysicalDevice gpu; VkDevice device;
    VkSurfaceKHR surface; VkSwapchainKHR swapchain;
    int android_surface_lost, hdr;
    VkSurfaceFormatKHR hdr10, scrgb, format;
    VkPresentModeKHR mode;
    uint32_t image_count; VkImage images[16]; VkExtent2D extent;
} V;
static int g_sc_src, calls, sleeps, create_ok, closed;
static int permanent_failure, native_available = 1, generation;
static int64_t g_diag_surface_gen;
static void diag_hook_surface_gen(int64_t value, int64_t width, int64_t height, int64_t format) {
    assert(value == g_diag_surface_gen && width == 1280 && height == 720); (void)format;
}
typedef uint64_t Uint64;
static Uint64 ticks;
#define SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER "native-window"
static unsigned SDL_GetWindowProperties(SDL_Window *w) { (void)w; return 1; }
static void *SDL_GetPointerProperty(unsigned p, const char *key, void *fallback) {
    (void)p; (void)key; return native_available ? (void *)(uintptr_t)1 : fallback;
}
static Uint64 SDL_GetTicks(void) { return ticks; }
static VkResult caps_result = VK_SUCCESS, create_result = VK_SUCCESS;
volatile LONG g_display_close;
static LONG InterlockedCompareExchange(volatile LONG *v, LONG replacement, LONG expected) {
    LONG old = *v; if (old == expected) *v = replacement; return old;
}
static int display_android_wait_foreground(void) { return generation; }
static void Sleep(unsigned ms) {
    ticks += ms; sleeps++; if (!permanent_failure) create_ok = 1;
    if (sleeps > 400) { fprintf(stderr, "retry budget exceeded\n"); exit(95); }
}
static const char *SDL_GetError(void) { return permanent_failure ? "VK_ERROR_OUT_OF_DEVICE_MEMORY" : "native_window temporarily unavailable"; }
static int SDL_Vulkan_CreateSurface(SDL_Window *w, VkInstance i, const void *a, VkSurfaceKHR *surface) {
    (void)w; (void)i; (void)a; calls++;
    if (create_ok) *surface = (VkSurfaceKHR)(uintptr_t)1;
    return create_ok;
}
static int GetClientRect(void *w, RECT *r) { (void)w; r->right = 1280; r->bottom = 720; return 1; }
static int hdr_mode_want(void) { return 0; }
static void hdr_views(int mode) { (void)mode; }
static void nvn_vk_missing(const char *reason) { fprintf(stderr, "%s\n", reason); exit(94); }
static void vk_fatal(const char *reason, VkResult result) { fprintf(stderr, "%s: %d\n", reason, result); exit(93); }
static VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice gpu, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps) {
    (void)gpu; assert(surface); calls++;
    *caps = (VkSurfaceCapabilitiesKHR){0};
    caps->currentExtent.width = 1280; caps->currentExtent.height = 720;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    return caps_result;
}
static VkResult vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const void *a, VkSwapchainKHR *sc) {
    (void)d; (void)ci; (void)a; calls++;
    if (create_result == VK_SUCCESS) *sc = (VkSwapchainKHR)(uintptr_t)2;
    return create_result;
}
static VkResult vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *images) {
    (void)d; (void)sc; (void)images; *n = 2; return VK_SUCCESS;
}
'''


class AndroidSurfaceTest(unittest.TestCase):
    def run_c(self, code, expected_exit=0, expected_error=None):
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "surface.c", Path(tmp) / "surface"
            src.write_text(PRELUDE + code)
            cc = shlex.split(os.environ.get("HOST_CC", "cc"))
            result = subprocess.run(cc + ["-std=gnu11", "-I", str(ROOT / "deps/android-arm64/include"), str(src), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, expected_exit, result.stderr)
            if expected_error:
                self.assertIn(expected_error, result.stderr)

    def test_swapchain_surface_loss_is_retryable(self):
        self.run_c(function("swapchain_create") + r'''
int main(void) {
    V.surface = (VkSurfaceKHR)(uintptr_t)1;
    create_result = VK_ERROR_SURFACE_LOST_KHR;
    assert(!swapchain_create()); assert(V.android_surface_lost); assert(!V.swapchain);
    V.surface = VK_NULL_HANDLE; calls = 0;
    assert(!swapchain_create()); assert(calls == 0);
    V.surface = (VkSurfaceKHR)(uintptr_t)1; V.android_surface_lost = 0;
    create_result = VK_ERROR_OUT_OF_DATE_KHR;
    assert(!swapchain_create()); assert(!V.swapchain);
    create_result = VK_SUCCESS;
    assert(swapchain_create()); assert(V.swapchain); assert(V.image_count == 2); assert(g_diag_surface_gen == 1);
    return 0;
}
''')

    def test_temporary_surface_failure_and_startup_retry(self):
        self.run_c(function("android_surface_create") + r'''
int main(void) {
    /* Java dropped its window before SDL delivers background: not fatal. */
    assert(!android_surface_create(0)); assert(!V.surface); assert(V.android_surface_lost);
    assert(calls == 1); assert(sleeps == 1);
    assert(android_surface_create(0)); assert(V.surface); assert(!V.android_surface_lost);
    /* Initial creation waits without blocking SDL's separate event thread. */
    V.surface = VK_NULL_HANDLE; create_ok = 0; calls = sleeps = 0;
    assert(android_surface_create(1)); assert(calls == 2); assert(sleeps == 1);
    g_display_close = 1; calls = 0;
    assert(!android_surface_create(1)); assert(calls == 0);
    return 0;
}
''')

    def test_permanent_startup_failure_stops(self):
        self.run_c(function("android_surface_create") + r'''
int main(void) { permanent_failure = 1; android_surface_create(1); return 96; }
''', expected_exit=93, expected_error="VK_ERROR_OUT_OF_DEVICE_MEMORY")

    def test_permanent_resume_failure_stops(self):
        self.run_c(function("android_surface_create") + r'''
int main(void) { permanent_failure = 1; for (;;) android_surface_create(0); }
''', expected_exit=93, expected_error="VK_ERROR_OUT_OF_DEVICE_MEMORY")

    def test_missing_window_is_allowed_to_wait(self):
        self.run_c(function("android_surface_create") + r'''
int main(void) {
    native_available = 0; permanent_failure = 1;
    for (int i = 0; i < 300; i++) assert(!android_surface_create(0));
    assert(calls == 0); assert(ticks >= 5000);
    native_available = create_ok = 1;
    assert(android_surface_create(0));
    return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
