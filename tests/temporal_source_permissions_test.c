/* Exercise the actual descriptor-export branch; Windows injects POSIX syscall results. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <unistd.h>
#define make_dir(p) mkdir(p, 0700)
#endif
static unsigned opens, closes, chmod_calls, stat_calls, bind_calls;
static int fail_chmod, fail_stat, clamp_mode, descriptors[2];
static FILE *tracked_open(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (f) { assert(opens < 2); descriptors[opens++] = fileno(f); }
    return f;
}
static int tracked_close(FILE *f) { closes++; return fclose(f); }
static int export_chmod(int fd, int mode) {
    assert(fd == descriptors[0] || (opens == 2 && fd == descriptors[1]));
    assert(mode == 0666); chmod_calls++;
    if ((int)chmod_calls == fail_chmod) { errno = EPERM; return -1; }
#ifdef _WIN32
    assert(_get_osfhandle(fd) != -1); return 0;
#else
    return fchmod(fd, mode);
#endif
}
static int export_stat(int fd, struct stat *st) {
    stat_calls++;
    if ((int)stat_calls == fail_stat) { errno = EIO; return -1; }
#ifdef _WIN32
    assert(_get_osfhandle(fd) != -1); memset(st, 0, sizeof *st); st->st_mode = 0666;
#else
    if (fstat(fd, st)) return -1;
#endif
    if (clamp_mode) st->st_mode = 0660;
    return 0;
}
#define WT_TSC_TEST_POSIX_EXPORT 1
#define fopen tracked_open
#define fclose tracked_close
#define fchmod export_chmod
#define fstat export_stat
#include "../runtime/temporal_source_capture.c"
#undef fopen
#undef fclose
#undef fchmod
#undef fstat

static int bind_capture(uint32_t v, uint32_t size, const uint32_t *gate, WtTscEmit emit) {
    assert(v == WT_TSC_VERSION && size == sizeof(WtTscRecord));
    if (gate && emit) bind_calls++;
    return 0;
}
static void reset(void) {
    opens = closes = chmod_calls = stat_calls = bind_calls = 0;
    fail_chmod = fail_stat = clamp_mode = 0;
}
static int start_restrictive(const char *dir) {
#ifndef _WIN32
    mode_t previous = umask(0777);
#endif
    int result = wt_tsc_start(dir, 1, bind_capture);
#ifndef _WIN32
    umask(previous);
#endif
    return result;
}
static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static void sentinel(const char *path) {
    FILE *f = fopen(path, "wx"); assert(f); assert(fputs("preserve", f) >= 0); assert(!fclose(f));
}
static void preserved(const char *path) {
    char text[16] = {0}; FILE *f = fopen(path, "r"); assert(f);
    assert(fread(text, 1, 8, f) == 8); fclose(f); assert(!strcmp(text, "preserve"));
}
int main(int argc, char **argv) {
    assert(argc == 2); char dir[1100], bin[1200], json[1200];
    for (int scenario = 0; scenario < 7; scenario++) {
        snprintf(dir, sizeof dir, "%s/permissions_%d", argv[1], scenario); assert(!make_dir(dir));
        snprintf(bin, sizeof bin, "%s/temporal_source.bin", dir);
        snprintf(json, sizeof json, "%s/temporal_source.json", dir);
        reset();
        if (scenario == 1 || scenario == 2) fail_chmod = scenario;
        if (scenario == 3) fail_stat = 1;
        if (scenario == 4) clamp_mode = 1;
        if (scenario == 5) sentinel(json);
        if (scenario == 6) sentinel(bin);
        int result = start_restrictive(dir);
        if (scenario == 0) {
            assert(!result && opens == 2 && !closes && chmod_calls == 2 && stat_calls == 2 && bind_calls == 1);
#ifndef _WIN32
            struct stat st;
            assert(!stat(bin, &st) && (st.st_mode & 0777) == 0666);
            assert(!stat(json, &st) && (st.st_mode & 0777) == 0666);
#endif
            assert(wt_tsc_finish() == -1); /* Readability is not capture qualification. */
        } else {
            assert(result == -1 && opens == closes && !bind_calls);
            assert(!wt_tsc_requested() && !wt_tsc_enabled() && !output && !summary);
            assert(!wt_tsc_finish());
            if (scenario == 5) { preserved(json); assert(!exists(bin) && !chmod_calls); }
            else if (scenario == 6) { preserved(bin); assert(!exists(json) && !chmod_calls); }
            else {
                assert(!exists(bin) && !exists(json));
                reset(); assert(!start_restrictive(dir)); assert(wt_tsc_finish() == -1);
            }
        }
        assert(opens == closes);
    }
#ifdef _WIN32
    puts("Descriptor permission/error/cleanup contracts passed (POSIX syscalls mocked; Windows ACL behavior unchanged)");
#else
    puts("Descriptor permissions passed under umask 0777; permission/stat/clamping failures and cleanup passed");
#endif
    return 0;
}
