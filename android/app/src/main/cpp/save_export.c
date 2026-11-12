#define _GNU_SOURCE
#include "save_export.h"
#include "diag_bootstrap.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Fail closed, rather than silently exporting a subset. */
#define MAX_ENTRIES 1024
#define MAX_DEPTH 16
#define MAX_PATH_BYTES 1024
#define MAX_FILE_BYTES (32u * 1024u * 1024u)
#define MAX_TOTAL_BYTES (128u * 1024u * 1024u)
typedef struct {
    char path[MAX_PATH_BYTES], hash[65];
    struct stat stat;
    int seen;
} Entry;
typedef struct {
    Entry entries[MAX_ENTRIES];
    unsigned count, files;
    uint64_t bytes;
    int verify;
} Export;

static int same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_mode == b->st_mode && a->st_nlink == b->st_nlink && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static int directory(int parent, const char *name) {
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}
static int readable(int fd, mode_t mode) {
    struct stat s;
    return fchmod(fd, mode) || fstat(fd, &s) || (s.st_mode & 0777) != mode ? -1 : 0;
}
static int create_file(int parent, const char *name) {
    return openat(parent, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
}
static int write_all(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}
static int read_all(int fd, void *data, size_t size) {
    char *p = data;
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    char extra;
    return read(fd, &extra, 1) == 0 ? 0 : -1;
}
/* JSON escaping supports arbitrary filename bytes without interpreting them as paths. */
static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 32 || *p >= 127 || *p == '"' || *p == '\\') fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}
static int file_copy(int src, int dst, const struct stat *before, char hash[65]) {
    size_t size = (size_t)before->st_size;
    unsigned char *data = malloc(size ? size : 1);
    struct stat after;
    int result = -1;
    if (!data || read_all(src, data, size) || fstat(src, &after) || !same(before, &after)) goto done;
    diag_bootstrap_compute_sha256(data, size, hash);
    if (dst >= 0) {
        char copied[65];
        if (write_all(dst, data, size) || fsync(dst) || lseek(dst, 0, SEEK_SET) < 0 ||
            read_all(dst, data, size)) goto done;
        diag_bootstrap_compute_sha256(data, size, copied);
        if (strcmp(hash, copied) || readable(dst, 0666)) goto done;
    }
    /* Re-read the source too; metadata alone is not content verification. */
    if (lseek(src, 0, SEEK_SET) < 0 || read_all(src, data, size) || fstat(src, &after) ||
        !same(before, &after)) goto done;
    char again[65];
    diag_bootstrap_compute_sha256(data, size, again);
    result = strcmp(hash, again) ? -1 : 0;
done:
    free(data);
    return result;
}
static int walk(Export *x, int src, int dst, const char *prefix, unsigned depth) {
    if (depth > MAX_DEPTH) return -1;
    struct stat before, after;
    if (fstat(src, &before)) return -1;
    int scan = directory(src, ".");
    DIR *dir = scan < 0 ? NULL : fdopendir(scan);
    if (!dir) { if (scan >= 0) close(scan); return -1; }
    int result = -1;
    for (;;) {
        errno = 0;
        struct dirent *d = readdir(dir);
        if (!d) { if (errno) goto done; break; }
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        if (strchr(d->d_name, '/') || strchr(d->d_name, '\\')) goto done;
        char path[MAX_PATH_BYTES];
        int n = snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", d->d_name);
        if (n < 0 || (size_t)n >= sizeof path) goto done;
        struct stat st, opened;
        if (fstatat(src, d->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) || st.st_dev != before.st_dev ||
            (S_ISREG(st.st_mode) && (st.st_nlink != 1 || st.st_size < 0 ||
             (uint64_t)st.st_size > MAX_FILE_BYTES))) goto done;
        Entry *e = NULL;
        if (x->verify) {
            for (unsigned i = 0; i < x->count; i++) if (!strcmp(path, x->entries[i].path)) e = &x->entries[i];
            if (!e || e->seen || !same(&e->stat, &st)) goto done;
            e->seen = 1;
        } else {
            if (x->count == MAX_ENTRIES) goto done;
            e = &x->entries[x->count++];
            strcpy(e->path, path); e->stat = st;
            if (S_ISREG(st.st_mode)) {
                x->bytes += (uint64_t)st.st_size;
                if (x->bytes > MAX_TOTAL_BYTES) goto done;
                x->files++;
            }
        }
        int in = openat(src, d->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                        (S_ISDIR(st.st_mode) ? O_DIRECTORY : O_NONBLOCK));
        if (in < 0) goto done;
        int out = -1, ok = -1;
        if (fstat(in, &opened) || !same(&st, &opened)) goto child_done;
        if (S_ISDIR(st.st_mode)) {
            if (!x->verify) {
                if (mkdirat(dst, d->d_name, 0700)) goto child_done;
                out = directory(dst, d->d_name);
                if (out < 0 || readable(out, 0755)) goto child_done;
            }
            ok = walk(x, in, out, path, depth + 1);
        } else {
            if (!x->verify && (out = create_file(dst, d->d_name)) < 0) goto child_done;
            char hash[65];
            ok = file_copy(in, out, &st, hash);
            if (!ok) {
                if (x->verify) ok = strcmp(hash, e->hash) ? -1 : 0;
                else strcpy(e->hash, hash);
            }
        }
        if (fstatat(src, d->d_name, &opened, AT_SYMLINK_NOFOLLOW) || !same(&st, &opened)) ok = -1;
child_done:
        close(in);
        if (out >= 0 && close(out)) ok = -1;
        if (ok) goto done;
    }
    result = fstat(src, &after) || !same(&before, &after) ? -1 : 0;
done:
    closedir(dir);
    return result;
}
/* nnfs.c journal format: these are siblings of save, never members to filter out. */
static int committed_only(int userdata) {
    int scan = directory(userdata, ".");
    DIR *dir = scan < 0 ? NULL : fdopendir(scan);
    if (!dir) { if (scan >= 0) close(scan); return -1; }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *d = readdir(dir);
        if (!d) { if (errno) result = -1; break; }
        if (!strcmp(d->d_name, "save.commit") || !strncmp(d->d_name, "save.work.", 10) ||
            !strncmp(d->d_name, "save.journal.", 13)) { result = -1; break; }
    }
    closedir(dir);
    return result;
}
static int manifest(int dst, Export *x, const char *run, const char *digest, int complete) {
    int fd = create_file(dst, "manifest.json");
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -1; }
    fprintf(f, "{\"schema\":\"committed-save-export-v1\",\"complete\":%s,\"run_id\":", complete ? "true" : "false");
    json_string(f, run);
    fprintf(f, ",\"request_sha256\":\"%s\",\"source\":\"userdata/save\",\"path_encoding\":\"filename-bytes-as-u00xx\",\"file_count\":%u,\"entry_count\":%u,\"total_bytes\":%llu,\"entries\":[",
        digest, x->files, x->count, (unsigned long long)x->bytes);
    for (unsigned i = 0; i < x->count; i++) {
        Entry *e = &x->entries[i];
        fprintf(f, "%s{\"path\":", i ? "," : ""); json_string(f, e->path);
        fprintf(f, ",\"directory\":%s,\"size\":%llu,\"device\":%llu,\"inode\":%llu,\"mtime_sec\":%lld,\"mtime_nsec\":%ld,\"source_sha256\":\"%s\",\"copied_sha256\":\"%s\"}",
            S_ISDIR(e->stat.st_mode) ? "true" : "false", (unsigned long long)e->stat.st_size,
            (unsigned long long)e->stat.st_dev, (unsigned long long)e->stat.st_ino,
            (long long)e->stat.st_mtim.tv_sec, e->stat.st_mtim.tv_nsec, e->hash, complete ? e->hash : "");
    }
    fputs("]}\n", f);
    int ok = !ferror(f) && !fflush(f) && !fsync(fd) && !readable(fd, 0666);
    if (fclose(f)) ok = 0;
    if (!ok) unlinkat(dst, "manifest.json", 0); /* Only our freshly created destination file. */
    return ok ? 0 : -1;
}

int wt_save_export(const char *root, const char *run_id, const char *digest) {
    if (!diag_bootstrap_is_diagnostic_variant() || !root || root[0] != '/' ||
        !diag_bootstrap_validate_run_id(run_id) || !digest || strlen(digest) != 64 ||
        strspn(digest, "0123456789abcdef") != 64) return -1;
    int base = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int diagnostics = -1, run = -1, req = -1, user = -1, src = -1, dst = -1, tree = -1;
    int result = -1;
    Export *x = calloc(1, sizeof *x);
    struct stat st, after, user_before, root_before;
    if (base < 0 || !x) goto done;
    diagnostics = directory(base, "diagnostics");
    if (diagnostics < 0 || (run = directory(diagnostics, run_id)) < 0) goto done;
    req = openat(run, "request.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (req < 0 || fstat(req, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > DIAG_BOOTSTRAP_MAX_REQUEST_BYTES) goto done;
    unsigned char request[DIAG_BOOTSTRAP_MAX_REQUEST_BYTES];
    if (read_all(req, request, (size_t)st.st_size) || fstat(req, &after) || !same(&st, &after) ||
        diag_bootstrap_validate_request_data(request, (size_t)st.st_size, run_id, digest, NULL, 0) ||
        !diag_bootstrap_save_export_requested()) goto done;
    user = directory(base, "userdata");
    if (user < 0 || committed_only(user) || fstat(user, &user_before)) goto done;
    src = directory(user, "save");
    if (src < 0 || fstat(src, &root_before)) goto done;
    if (mkdirat(run, "committed-save-export", 0700)) goto done;
    dst = directory(run, "committed-save-export");
    if (dst < 0 || readable(dst, 0755) || mkdirat(dst, "save", 0700)) goto done;
    tree = directory(dst, "save");
    if (tree < 0 || readable(tree, 0755)) goto done;
    /* An incomplete marker is permanent; only a final complete manifest authorizes use. */
    int marker = create_file(dst, "INCOMPLETE");
    if (marker < 0) goto done;
    static const char warning[] = "Require manifest.json complete=true\n";
    int marker_ok = !write_all(marker, warning, sizeof warning - 1) && !readable(marker, 0666);
    if (close(marker)) marker_ok = 0;
    if (!marker_ok || walk(x, src, tree, "", 0)) goto report;
#ifdef WT_SAVE_EXPORT_TEST_AFTER_COPY
    WT_SAVE_EXPORT_TEST_AFTER_COPY();
#endif
    x->verify = 1;
    if (walk(x, src, -1, "", 0)) goto report;
    for (unsigned i = 0; i < x->count; i++) if (!x->entries[i].seen) goto report;
    if (committed_only(user) || fstat(user, &after) || !same(&user_before, &after) ||
        fstatat(base, "userdata", &after, AT_SYMLINK_NOFOLLOW) || !same(&user_before, &after) ||
        fstatat(user, "save", &after, AT_SYMLINK_NOFOLLOW) || !same(&root_before, &after)) goto report;
    result = 0;
report:
    if (manifest(dst, x, run_id, digest, result == 0)) result = -1;
done:
    if (tree >= 0) close(tree);
    if (dst >= 0) close(dst);
    if (src >= 0) close(src);
    if (user >= 0) close(user);
    if (req >= 0) close(req);
    if (run >= 0) close(run);
    if (diagnostics >= 0) close(diagnostics);
    if (base >= 0) close(base);
    free(x);
    return result;
}
