/* Host executable: dependency and filesystem calls are mocked, never a real driver. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static int mock_lstat(const char *, struct stat *);
static int mock_unlink(const char *);
static char *mock_getenv(const char *name) { (void)name; return NULL; }
static int mock_setenv(const char *name, const char *value, int overwrite) { (void)name; (void)value; (void)overwrite; return 0; }
static int mock_unsetenv(const char *name) { (void)name; return 0; }
#define getenv mock_getenv
#define setenv mock_setenv
#define unsetenv mock_unsetenv
#define lstat mock_lstat
#define unlink mock_unlink
#include "../android/app/src/main/cpp/wt_gpu_driver.c"

static int mode, opens, lookups, removals, dispatches;
static void *const exact_loader = (void *)(uintptr_t)0x1234;
static PFN_vkVoidFunction VKAPI_CALL exact_gipa(VkInstance instance, const char *name) {
    assert(instance == (VkInstance)(uintptr_t)0x4567);
    assert(!strcmp(name, "vkCreateDevice"));
    dispatches++;
    return (PFN_vkVoidFunction)(uintptr_t)0x5678;
}
static int mock_lstat(const char *path, struct stat *info) {
    assert(!strcmp(path, "/pending"));
    memset(info, 0, sizeof(*info));
    info->st_mode = S_IFREG;
    info->st_dev = 1;
    info->st_ino = (mode == 4 && opens) ? 3 : 2;
    return mode == 5 ? -1 : 0;
}
static int mock_unlink(const char *path) {
    assert(!strcmp(path, "/pending"));
    removals++;
    return 0;
}
void *adrenotools_open_libvulkan(int flags, int features, const char *tmp,
        const char *native, const char *package, const char *name,
        const char *redirect, void **mapping) {
    assert(flags == (RTLD_NOW | RTLD_LOCAL));
    assert(features == ADRENOTOOLS_DRIVER_CUSTOM);
    assert(!tmp && !redirect && !mapping);
    assert(!strcmp(native, "/native") && !strcmp(package, "/driver/"));
    assert(!strcmp(name, "custom.so"));
    opens++;
    return mode == 1 ? NULL : exact_loader;
}
void *dlsym(void *handle, const char *name) {
    assert(handle == exact_loader && !strcmp(name, "vkGetInstanceProcAddr"));
    lookups++;
    return mode == 2 ? NULL : (void *)exact_gipa;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    mode = atoi(argv[1]);
    assert(!vkGetInstanceProcAddr(NULL, "before"));
    wt_android_driver_presented();
    assert(!removals);
    void *result = wt_android_driver_open("/native", mode == 3 ? "/driver" : "/driver/", "custom.so", "/pending", "");
    if (mode == 0 || mode == 4) {
        assert(result == exact_loader);
        assert(vkGetInstanceProcAddr((VkInstance)(uintptr_t)0x4567, "vkCreateDevice") == (PFN_vkVoidFunction)(uintptr_t)0x5678);
        assert(dispatches == 1 && lookups == 1);
    } else {
        assert(!result);
        assert(!vkGetInstanceProcAddr(NULL, "failed"));
    }
    assert(wt_android_driver_open("/other", "/other/", "other.so", "/other", "") == result);
    wt_android_driver_presented();
    wt_android_driver_presented();
    assert(removals == (mode == 0 ? 1 : 0));
    assert(opens == (mode == 3 || mode == 5 ? 0 : 1));
    return 0;
}
