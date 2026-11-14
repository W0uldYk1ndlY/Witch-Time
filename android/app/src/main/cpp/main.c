/* Android entry only; it does not pretend to run a game without the private build. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <jni.h>
#include <string.h>

#include "diag_bootstrap.h"
#include "temporal_source_capture.h"
#include "driver_restart.h"

static WtDriverRestart driver_restart = WT_DRIVER_RESTART_INIT;
static void (*driver_menu_cancelled)(void);

static void release_driver_menu(void) {
    pthread_mutex_lock(&driver_restart.lock);
    if (!driver_restart.nonce[0] && driver_menu_cancelled) driver_menu_cancelled();
    pthread_mutex_unlock(&driver_restart.lock);
}

void wt_android_request_gpu_driver_picker(void) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (!env || !activity) { release_driver_menu(); return; }
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID show = cls ? (*env)->GetMethodID(env, cls, "showGpuDriverSettings", "()V") : NULL;
    if (show) (*env)->CallVoidMethod(env, activity, show);
    if (!show || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        release_driver_menu();
    }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

JNIEXPORT jboolean JNICALL Java_org_witchtime_android_WitchTimeActivity_nativePrepareDriverRestart(
    JNIEnv *env, jobject activity, jstring nonce) {
    (void)activity;
    if (!nonce) return JNI_FALSE;
    void *export_library = dlopen("libsave_export.so", RTLD_NOW | RTLD_NOLOAD);
    if (export_library) {
        int (*exporting)(void) = (int (*)(void))dlsym(export_library, "wt_save_export_in_progress");
        int blocked = !exporting || exporting();
        dlclose(export_library);
        if (blocked) return JNI_FALSE;
    }
    const char *token = (*env)->GetStringUTFChars(env, nonce, NULL);
    if (!token) return JNI_FALSE;
    int ready = wt_driver_restart_prepare(&driver_restart, token);
    (*env)->ReleaseStringUTFChars(env, nonce, token);
    return ready ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_org_witchtime_android_WitchTimeActivity_nativeCancelDriverRestart(
    JNIEnv *env, jobject activity, jstring nonce) {
    (void)activity;
    if (!nonce) return;
    const char *token = (*env)->GetStringUTFChars(env, nonce, NULL);
    if (!token) return;
    wt_driver_restart_cancel(&driver_restart, token);
    (*env)->ReleaseStringUTFChars(env, nonce, token);
    release_driver_menu();
}
JNIEXPORT void JNICALL Java_org_witchtime_android_WitchTimeActivity_nativeGpuDriverChooserCancelled(
    JNIEnv *env, jobject activity) {
    (void)env; (void)activity;
    release_driver_menu();
}
typedef struct { JNIEnv *env; jobject activity; } WtRestartJava;
static int seal_driver_restart(void *context, const char *nonce) {
    WtRestartJava *java = context;
    JNIEnv *env = java->env;
    jclass cls = (*env)->GetObjectClass(env, java->activity);
    jmethodID seal = cls ? (*env)->GetMethodID(env, cls, "sealGpuDriverRestart", "(Ljava/lang/String;)Z") : NULL;
    jstring token = seal ? (*env)->NewStringUTF(env, nonce) : NULL;
    int sealed = token && (*env)->CallBooleanMethod(env, java->activity, seal, token) == JNI_TRUE;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); sealed = 0; }
    if (token) (*env)->DeleteLocalRef(env, token);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    return sealed;
}
static void exit_driver_restart(void *context) { (void)context; _exit(0); }
JNIEXPORT jboolean JNICALL Java_org_witchtime_android_WitchTimeActivity_nativeCommitDriverRestart(
    JNIEnv *env, jobject activity, jstring nonce) {
    if (!nonce) return JNI_FALSE;
    const char *token = (*env)->GetStringUTFChars(env, nonce, NULL);
    if (!token) return JNI_FALSE;
    WtRestartJava java = {env, activity};
    int committed = wt_driver_restart_commit(&driver_restart, token, seal_driver_restart, exit_driver_restart, &java);
    (*env)->ReleaseStringUTFChars(env, nonce, token);
    return committed ? JNI_TRUE : JNI_FALSE;
}

static int fail(const char *message) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Witch Time cannot start", message, NULL);
    return 1;
}

typedef struct {
    void *loader;
    void (*presented)(void);
} WtDriver;

static WtDriver prepare_driver(void) {
    WtDriver selected = {0};
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    jclass cls = NULL;
    jobjectArray request = NULL;
    char *values[5] = {0};
    if (!env || !activity) goto done;
    cls = (*env)->GetObjectClass(env, activity);
    if (!cls) goto done;
    jmethodID prepare = (*env)->GetMethodID(env, cls, "prepareGpuDriver", "()[Ljava/lang/String;");
    if (!prepare) goto done;
    request = (jobjectArray)(*env)->CallObjectMethod(env, activity, prepare);
    if ((*env)->ExceptionCheck(env) || !request) goto done;
    if ((*env)->GetArrayLength(env, request) != 5) goto done;
    for (int i = 0; i < 5; ++i) {
        jstring value = (jstring)(*env)->GetObjectArrayElement(env, request, i);
        if ((*env)->ExceptionCheck(env) || !value) goto done;
        const char *text = (*env)->GetStringUTFChars(env, value, NULL);
        if (text) {
            values[i] = SDL_strdup(text);
            (*env)->ReleaseStringUTFChars(env, value, text);
        }
        (*env)->DeleteLocalRef(env, value);
        if (!values[i]) goto done;
    }
    struct stat marker;
    if (values[0][0] != '/' || values[1][0] != '/' || !*values[2] ||
        strchr(values[2], '/') || values[3][0] != '/' ||
        values[1][strlen(values[1]) - 1] != '/' ||
        lstat(values[3], &marker) || !S_ISREG(marker.st_mode)) goto done;
    char bridge_path[4096];
    int length = snprintf(bridge_path, sizeof(bridge_path), "%s/libwt_gpu_driver.so", values[0]);
    if (length < 0 || (size_t)length >= sizeof(bridge_path)) goto done;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GPU driver requested custom: %s (effective GPU determined by Vulkan; loader may fall back)", values[2]);
    /* The bridge's dependency constructors are unsafe until the marker exists. */
    void *bridge = dlopen(bridge_path, RTLD_NOW | RTLD_LOCAL);
    if (!bridge) goto done;
    void *(*open_driver)(const char *, const char *, const char *, const char *, const char *) =
        (void *(*)(const char *, const char *, const char *, const char *, const char *))dlsym(bridge, "wt_android_driver_open");
    void (*presented)(void) = (void (*)(void))dlsym(bridge, "wt_android_driver_presented");
    void (*restore_environment)(void) = (void (*)(void))dlsym(bridge, "wt_android_driver_restore_environment");
    if (!open_driver || !presented || !restore_environment || !dlsym(bridge, "vkGetInstanceProcAddr")) goto done;
    void *loader = open_driver(values[0], values[1], values[2], values[3], values[4]);
    if (!loader || !SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, bridge_path)) { restore_environment(); goto done; }
    selected.loader = loader;
    selected.presented = presented;
done:
    if (env) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GPU driver preparation failed; using system Vulkan.");
        }
        if (request) (*env)->DeleteLocalRef(env, request);
        if (cls) (*env)->DeleteLocalRef(env, cls);
        if (activity) (*env)->DeleteLocalRef(env, activity);
    }
    for (int i = 0; i < 5; ++i) SDL_free(values[i]);
    if (!selected.loader) {
        SDL_ResetHint(SDL_HINT_VULKAN_LIBRARY);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GPU driver selected: system (custom failure retains pending quarantine)");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GPU driver selected: custom loader (not an effective GPU identification)");
    }
    return selected;
}

int SDL_main(int argc, char **argv) {
    /* Serialize a concurrent ordinary launch against the separate export-only Activity. */
    void *export_library = dlopen("libsave_export.so", RTLD_NOW | RTLD_NOLOAD);
    if (export_library) {
        int (*exporting)(void) = (int (*)(void))dlsym(export_library, "wt_save_export_in_progress");
        int blocked = !exporting || exporting();
        dlclose(export_library);
        if (blocked) return fail("Committed save export is active; game startup refused.");
    }
    SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
    const char *root = SDL_GetAndroidExternalStoragePath();
    if (!root || chdir(root)) return fail("Cannot open the app's private external files directory.");

    char run_id[DIAG_BOOTSTRAP_MAX_RUN_ID_LEN + 4] = {0};
    char digest[DIAG_BOOTSTRAP_SHA256_HEX_LEN + 4] = {0};
    int diag_mode = 0;
    char run_dir[256] = {0};

    int parse_res = diag_bootstrap_parse_args(argc, argv, run_id, sizeof(run_id), digest, sizeof(digest));
    if (parse_res > 0) {
        if (!diag_bootstrap_is_diagnostic_variant()) {
            /* Ignored ordinary-release request: ordinary builds ignore automated diagnostic parameters */
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WT_DIAG: Ignored diagnostic parameters on non-diagnostic build.");
            diag_mode = 0;
        } else {
            diag_mode = 1;
        }
    }

    if (diag_mode) {
#if !defined(_WIN32)
        umask(0000);
#endif
        snprintf(run_dir, sizeof(run_dir), "diagnostics/%s", run_id);

        char err_reason[128] = {0};
        int val_res = diag_bootstrap_validate_request(run_dir, run_id, digest, err_reason, sizeof(err_reason));
        if (val_res != DIAG_BOOTSTRAP_OK) {
            diag_bootstrap_write_outcome_rejection(run_dir, run_id, "REQUEST_VALIDATION_FAILED", err_reason);
            return fail("Diagnostic request validation failed; stopping launch before runtime initialization.");
        }

        /* Startup ordering: write startup acknowledgement before runtime dlopen */
        if (diag_bootstrap_save_export_requested())
            return fail("Save export requires SaveExportActivity; game startup refused.");
        if (diag_bootstrap_write_acknowledgement(run_dir, run_id, digest) != DIAG_BOOTSTRAP_OK) {
            diag_bootstrap_write_outcome_rejection(run_dir, run_id, "ACKNOWLEDGEMENT_WRITE_FAILED", "Failed to write manifest.json");
            return fail("Cannot write startup acknowledgement.");
        }

        /* Apply session-owned isolated directories and environment */
        diag_bootstrap_apply_session_environment(run_dir, run_id);
    } else {
        /* Android does not expose printf on logcat. Keep diagnostics before any
         * library constructor or capability check can terminate the process. */
        if (!freopen("runtime.log", "a", stdout) || !freopen("runtime.log", "a", stderr))
            return fail("Cannot open runtime.log in the app's files directory.");
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "\n--- Witch Time Android launch ---\n");
        mkdir("userdata", 0700);
        mkdir("cache", 0700);
        mkdir("config", 0700);
        setenv("WT_ROMFS", "targets/romfs", 1);
        setenv("WT_USERDATA", "userdata", 1);
        setenv("WT_SHADER_CACHE", "cache/shaders", 1);
        setenv("WT_CONFIG", "config/controllers.ini", 1);
        setenv("WT_GRAPHICS_CONFIG", "config/graphics.ini", 1);
        setenv("WT_MODS", "none", 1);
        setenv("WT_VOLUME", "100", 1);
    }

    /* adb push or Android's file manager can supply these personal files. No broad storage access. */
    if (access("targets/exefs/main.elf", R_OK) || access("targets/exefs/sdk.elf", R_OK) || access("targets/romfs", R_OK))
        return fail("Missing your locally converted game files. Copy targets/exefs/main.elf, sdk.elf and targets/romfs into this app's external files directory. See android/README.md. Game files are not included.");

    WtDriver driver = {0};
    if (!diag_mode) driver = prepare_driver();

    if (!getenv("WT_VSYNC")) setenv("WT_VSYNC", "clock", 0);
    if (!getenv("WT_WW_SOFT")) setenv("WT_WW_SOFT", "1", 0);
    if (!getenv("WT_VK_WRITERWAIT")) setenv("WT_VK_WRITERWAIT", "1", 0);
    if (!getenv("WT_VK_LOADCLEAR")) setenv("WT_VK_LOADCLEAR", "1", 0);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Optimization flags: WT_WW_SOFT=%s, WT_VK_WRITERWAIT=%s, WT_VK_LOADCLEAR=%s",
                getenv("WT_WW_SOFT"), getenv("WT_VK_WRITERWAIT"), getenv("WT_VK_LOADCLEAR"));
    fprintf(stderr, "Optimization flags: WT_WW_SOFT=%s, WT_VK_WRITERWAIT=%s, WT_VK_LOADCLEAR=%s\n",
            getenv("WT_WW_SOFT"), getenv("WT_VK_WRITERWAIT"), getenv("WT_VK_LOADCLEAR"));
    void *runtime = dlopen("libwitchtime.so", RTLD_NOW | RTLD_GLOBAL);
    if (!runtime) return fail(dlerror());
    if (!diag_mode) {
        WtRestartHooks hooks = {
            (int (*)(unsigned))dlsym(runtime, "wt_gpu_restart_prepare"),
            (void (*)(void))dlsym(runtime, "wt_gpu_restart_cancel"),
            (int (*)(unsigned))dlsym(runtime, "wt_fs_restart_prepare"),
            (void (*)(void))dlsym(runtime, "wt_fs_restart_cancel")
        };
        void (*set_chooser)(void (*)(void)) =
            (void (*)(void (*)(void)))dlsym(runtime, "pcmenu_android_set_driver_chooser");
        void (*menu_cancelled)(void) = (void (*)(void))dlsym(runtime, "pcmenu_android_driver_chooser_cancelled");
        if (hooks.gpu_prepare && hooks.gpu_cancel && hooks.fs_prepare && hooks.fs_cancel &&
            set_chooser && menu_cancelled) {
            pthread_mutex_lock(&driver_restart.lock);
            driver_restart.hooks = hooks;
            driver_menu_cancelled = menu_cancelled;
            pthread_mutex_unlock(&driver_restart.lock);
            set_chooser(wt_android_request_gpu_driver_picker);
        }
    }
    if (driver.loader) {
        void (*set_driver)(void *, void (*)(void)) =
            (void (*)(void *, void (*)(void)))dlsym(runtime, "nvn_vk_android_set_driver");
        if (!set_driver) return fail("Custom Vulkan loader requires runtime driver injection; stopping before renderer initialization.");
        set_driver(driver.loader, driver.presented);
    }
    int (*run)(int, char **) = (int (*)(int, char **))dlsym(runtime, "display_android_run");
    if (!run) return fail("libwitchtime.so does not export display_android_run.");
    int (*capture_finish)(void) = NULL;
    if (diag_mode && diag_bootstrap_temporal_source_requested()) {
        int (*capture_start)(const char *, int, WtTscBind) =
            (int (*)(const char *, int, WtTscBind))dlsym(runtime, "wt_tsc_start");
        capture_finish = (int (*)(void))dlsym(runtime, "wt_tsc_finish");
        WtTscBind bind = (WtTscBind)dlsym(runtime, "game_temporal_source_bind");
        if (!capture_start || !capture_finish || capture_start(run_dir, 1, bind) != 0) {
            if (capture_finish) capture_finish();
            diag_bootstrap_write_outcome_rejection(run_dir, run_id, "TEMPORAL_SOURCE_CAPTURE_UNAVAILABLE", "Missing export or capture initialization failed");
            return fail("Temporal source capture unavailable; stopping before runtime execution.");
        }
    }
    char *game_argv[] = { "witch-time", "targets/exefs/main.elf", NULL };
    int result = run(2, game_argv);
    /* Covers early runtime returns before core diagnostics initialized; normal teardown is already finalized. */
    if (capture_finish && capture_finish() != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Temporal source capture incomplete.");
        if (result == 0) result = 1;
    }
    return result;
}
