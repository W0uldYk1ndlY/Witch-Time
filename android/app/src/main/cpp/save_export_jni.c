#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "save_export.h"

static atomic_flag busy = ATOMIC_FLAG_INIT;
static atomic_int exporting;
int wt_save_export_in_progress(void) { return atomic_load(&exporting); }
JNIEXPORT jint JNICALL Java_org_witchtime_android_SaveExportActivity_exportCommittedSave(
        JNIEnv *env, jclass type, jstring root, jstring run, jstring digest) {
    (void)type;
    if (!root || !run || !digest || atomic_flag_test_and_set(&busy)) return -1;
    atomic_store(&exporting, 1);
    int result = -1;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) goto done;
    char line[4096];
    int safe = 1;
    while (fgets(line, sizeof line, maps)) {
        if (strstr(line, "libSDL3.so") || strstr(line, "libmain.so") ||
            strstr(line, "libwitchtime.so") || strstr(line, "libgame.so")) safe = 0;
    }
    if (ferror(maps)) safe = 0;
    fclose(maps);
    if (!safe) goto done;
    const char *r = (*env)->GetStringUTFChars(env, root, NULL);
    const char *n = (*env)->GetStringUTFChars(env, run, NULL);
    const char *d = (*env)->GetStringUTFChars(env, digest, NULL);
    if (r && n && d) result = wt_save_export(r, n, d);
    if (d) (*env)->ReleaseStringUTFChars(env, digest, d);
    if (n) (*env)->ReleaseStringUTFChars(env, run, n);
    if (r) (*env)->ReleaseStringUTFChars(env, root, r);
done:
    atomic_store(&exporting, 0);
    atomic_flag_clear(&busy);
    return result;
}
