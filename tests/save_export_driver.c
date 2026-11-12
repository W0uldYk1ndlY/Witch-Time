/* Disposable POSIX host fixture driver, never opens an Android/device path. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *test_root;
static void after_copy(void) {
    if (getenv("WT_EXPORT_TEST_MUTATE")) {
        char path[2048];
        snprintf(path, sizeof path, "%s/userdata/save/slot.bin", test_root);
        int fd = open(path, O_WRONLY | O_TRUNC);
        if (fd >= 0) { write(fd, "changed", 7); close(fd); }
    }
}
#define WT_SAVE_EXPORT_TEST_AFTER_COPY after_copy
#include "../android/app/src/main/cpp/save_export.c"
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    test_root = argv[1];
    return wt_save_export(argv[1], argv[2], argv[3]) ? 1 : 0;
}
