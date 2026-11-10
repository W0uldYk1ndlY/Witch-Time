/* Android entry only; it does not pretend to run a game without the private build. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include "diag_bootstrap.h"

static int fail(const char *message) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Witch Time cannot start", message, NULL);
    return 1;
}

int SDL_main(int argc, char **argv) {
    SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
    const char *root = SDL_GetAndroidExternalStoragePath();
    if (!root || chdir(root)) return fail("Cannot open the app's private external files directory.");

    char run_id[DIAG_BOOTSTRAP_MAX_RUN_ID_LEN + 4] = {0};
    char digest[DIAG_BOOTSTRAP_SHA256_HEX_LEN + 4] = {0};
    int diag_mode = 0;

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
        char run_dir[256];
        snprintf(run_dir, sizeof(run_dir), "diagnostics/%s", run_id);

        char err_reason[128] = {0};
        int val_res = diag_bootstrap_validate_request(run_dir, run_id, digest, err_reason, sizeof(err_reason));
        if (val_res != DIAG_BOOTSTRAP_OK) {
            diag_bootstrap_write_outcome_rejection(run_dir, run_id, "REQUEST_VALIDATION_FAILED", err_reason);
            return fail("Diagnostic request validation failed; stopping launch before runtime initialization.");
        }

        /* Startup ordering: write startup acknowledgement before runtime dlopen */
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
    int (*run)(int, char **) = (int (*)(int, char **))dlsym(runtime, "display_android_run");
    if (!run) return fail("libwitchtime.so does not export display_android_run.");
    char *game_argv[] = { "witch-time", "targets/exefs/main.elf", NULL };
    return run(2, game_argv);
}
