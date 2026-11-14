#pragma once

void *wt_android_driver_open(const char *native_library_dir, const char *package_dir,
                             const char *library_name, const char *pending_marker,
                             const char *bundled_profile_id);
void wt_android_driver_restore_environment(void);
void wt_android_driver_presented(void);
