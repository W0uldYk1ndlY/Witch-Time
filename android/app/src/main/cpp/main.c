/* Android entry only; it does not pretend to run a game without the private build. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static int fail(const char *message) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Witch Time cannot start", message, NULL);
    return 1;
}

int SDL_main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *root = SDL_GetAndroidExternalStoragePath();
    if (!root || chdir(root)) return fail("Cannot open the app's private external files directory.");
    /* Android does not expose printf on logcat. Keep diagnostics before any
     * library constructor or capability check can terminate the process. */
    if (!freopen("runtime.log", "a", stdout) || !freopen("runtime.log", "a", stderr))
        return fail("Cannot open runtime.log in the app's files directory.");
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "\n--- Witch Time Android launch ---\n");
    /* adb push or Android's file manager can supply these personal files. No broad storage access. */
    if (access("targets/exefs/main.elf", R_OK) || access("targets/exefs/sdk.elf", R_OK) || access("targets/romfs", R_OK))
        return fail("Missing your locally converted game files. Copy targets/exefs/main.elf, sdk.elf and targets/romfs into this app's external files directory. See android/README.md. Game files are not included.");
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
    if (!getenv("WT_VK_SCALE")) setenv("WT_VK_SCALE", "0.6", 0);
    if (!getenv("WT_VSYNC")) setenv("WT_VSYNC", "clock", 0);
    void *runtime = dlopen("libwitchtime.so", RTLD_NOW | RTLD_GLOBAL);
    if (!runtime) return fail(dlerror());
    int (*run)(int, char **) = (int (*)(int, char **))dlsym(runtime, "display_android_run");
    if (!run) return fail("libwitchtime.so does not export display_android_run.");
    char *game_argv[] = { "witch-time", "targets/exefs/main.elf", NULL };
    return run(2, game_argv);
}
