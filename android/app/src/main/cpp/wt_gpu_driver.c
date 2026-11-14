/* Loaded only after Java has durably armed the crash-quarantine marker. */
#include "wt_gpu_driver.h"
#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void *loader;
static PFN_vkGetInstanceProcAddr loader_gipa;
static char *owned_marker;
static dev_t marker_device;
static ino_t marker_inode;
static int attempted;
static char *previous_tu_debug;
static int changed_tu_debug;

void wt_android_driver_restore_environment(void) {
    if (!changed_tu_debug) return;
    if (previous_tu_debug) setenv("TU_DEBUG", previous_tu_debug, 1);
    else unsetenv("TU_DEBUG");
    free(previous_tu_debug);
    previous_tu_debug = NULL;
    changed_tu_debug = 0;
}

void *wt_android_driver_open(const char *native_library_dir, const char *package_dir,
                             const char *library_name, const char *pending_marker,
                             const char *bundled_profile_id) {
    if (attempted) return loader;
    attempted = 1;
    struct stat marker;
    if (!native_library_dir || native_library_dir[0] != '/' ||
        !package_dir || package_dir[0] != '/' || !*package_dir ||
        package_dir[strlen(package_dir) - 1] != '/' ||
        !library_name || !*library_name || strchr(library_name, '/') ||
        !pending_marker || pending_marker[0] != '/' ||
        lstat(pending_marker, &marker) || !S_ISREG(marker.st_mode)) return NULL;
    owned_marker = strdup(pending_marker);
    if (!owned_marker) return NULL;
    marker_device = marker.st_dev;
    marker_inode = marker.st_ino;
    /* Java supplies this ID only after the immutable bundled manifest/digest check. */
    if (bundled_profile_id && !strcmp(bundled_profile_id, "turnip-adreno830-unleashed-mesa26.3-gralloc")) {
        const char *original = getenv("TU_DEBUG");
        if (original && !(previous_tu_debug = strdup(original))) return NULL;
        if (setenv("TU_DEBUG", "sysmem,nobin", 1)) { free(previous_tu_debug); previous_tu_debug = NULL; return NULL; }
        changed_tu_debug = 1;
    }
    void *handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL,
        ADRENOTOOLS_DRIVER_CUSTOM, NULL, native_library_dir, package_dir,
        library_name, NULL, NULL);
    if (!handle) { wt_android_driver_restore_environment(); return NULL; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
    if (!gipa) { wt_android_driver_restore_environment(); return NULL; }
    /* Keep the exact namespace loader and its dispatch alive for this process. */
    loader_gipa = gipa;
    loader = handle;
    return loader;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    return loader_gipa ? loader_gipa(instance, name) : NULL;
}

void wt_android_driver_presented(void) {
    if (!loader || !owned_marker) return;
    struct stat marker;
    /* Do not delete a replacement marker owned by a later launch. */
    if (!lstat(owned_marker, &marker) && marker.st_dev == marker_device && marker.st_ino == marker_inode)
        unlink(owned_marker);
    free(owned_marker);
    owned_marker = NULL;
}
