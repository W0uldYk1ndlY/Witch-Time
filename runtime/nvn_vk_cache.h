#ifndef WT_NVN_VK_CACHE_H
#define WT_NVN_VK_CACHE_H

/* Disk caches are optional: a damaged file or a failed publication is a miss.
 * Keep I/O off the global shader lock and publish complete files by rename.
 * Limits leave room for large shaders and the existing ~1 GB desktop caches. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#define VKC_PATH_CAP 384
#define VKC_SPV_LIMIT ((size_t)64 << 20)
#define VKC_PIPELINE_LIMIT ((size_t)2 << 30)
#define VKC_RECORD_LIMIT ((size_t)64 << 20)
#define VKC_STAGE_REC_VERSION 2u

static inline int vkc_stream_size(FILE *fp, size_t limit, size_t *size) {
#ifdef _WIN32
    if (_fseeki64(fp, 0, SEEK_END)) return 0;
    int64_t end = _ftelli64(fp);
    if (_fseeki64(fp, 0, SEEK_SET)) return 0;
#else
    if (fseeko(fp, 0, SEEK_END)) return 0;
    int64_t end = (int64_t)ftello(fp);
    if (fseeko(fp, 0, SEEK_SET)) return 0;
#endif
    if (end <= 0 || (uint64_t)end > (uint64_t)limit) return 0;
    *size = (size_t)end;
    return 1;
}

static inline void *vkc_read_stream(FILE *fp, size_t limit, size_t *size) {
    size_t n = 0;
    *size = 0;
    if (!vkc_stream_size(fp, limit, &n)) return NULL;
    void *data = malloc(n);
    if (!data) return NULL;
    if (fread(data, 1, n, fp) != n || fgetc(fp) != EOF || ferror(fp)) { free(data); return NULL; }
    *size = n;
    return data;
}

static inline void *vkc_read_file(const char *path, size_t limit, size_t *size) {
    *size = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    void *data = vkc_read_stream(fp, limit, size);
    fclose(fp);
    return data;
}

static inline int vkc_write_file(const char *path, const void *data, size_t size) {
    if (!data || !size) return 0;
#ifdef VKC_NORMALIZE_PATH
    /* Match plat_fopen on POSIX: Windows separators and case-insensitive paths
     * must resolve before both the temporary and its destination are formed. */
    char normalized[VKC_PATH_CAP];
    if (strlen(path) >= sizeof normalized) return 0;
    path = VKC_NORMALIZE_PATH(path, normalized, sizeof normalized);
#endif
    char temporary[VKC_PATH_CAP + 48];
    int fd = -1;
    /* Exclusive creation makes a stale temporary or another process harmless.
     * The sequence is shared across workers; no file I/O holds a renderer lock. */
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
#ifdef _WIN32
        static volatile LONG sequence;
        unsigned long serial = (unsigned long)InterlockedIncrement(&sequence);
        unsigned long pid = (unsigned long)_getpid();
#else
        static unsigned long sequence;
        unsigned long serial = __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED);
        unsigned long pid = (unsigned long)getpid();
#endif
        int n = snprintf(temporary, sizeof temporary, "%s.tmp.%lu.%lu", path, pid, serial);
        if (n < 0 || (size_t)n >= sizeof temporary) return 0;
#ifdef _WIN32
        fd = _open(temporary, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return 0;
#ifdef _WIN32
    FILE *fp = _fdopen(fd, "wb");
    if (!fp) { _close(fd); remove(temporary); return 0; }
#else
    FILE *fp = fdopen(fd, "wb");
    if (!fp) { close(fd); remove(temporary); return 0; }
#endif
    int ok = fwrite(data, 1, size, fp) == size;
    /* fclose checks the buffered flush too. No fsync on shader compilation: a
     * power-loss cache miss is cheaper than forcing storage barriers per stage. */
    if (fclose(fp)) ok = 0;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    if (ok) ok = rename(temporary, path) == 0;
#endif
    if (!ok) remove(temporary);
    return ok;
}

static inline int vkc_spirv_valid(const uint32_t *words, size_t size) {
    if (!words || size <= 20 || size > VKC_SPV_LIMIT || size % 4) return 0;
    if (words[0] != 0x07230203 || words[1] < 0x00010000 || words[1] > 0x00010600 ||
        (words[1] & 0x000000ff) || !words[3] || words[3] > 0x003fffff || words[4]) return 0;
    /* Bounds check every instruction. This detects torn/truncated files before
     * the driver sees them, including in offline preparation; it is not a full
     * SPIR-V semantic validator (the compiler/driver retain that responsibility). */
    size_t count = size / 4;
    for (size_t i = 5; i < count; ) {
        size_t n = words[i] >> 16;
        if (!n || n > count - i) return 0;
        i += n;
    }
    return 1;
}

static inline uint32_t vkc_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline int vkc_pipeline_header_valid(const void *data, size_t size, uint32_t vendor,
                                             uint32_t device, const uint8_t uuid[16]) {
    if (!data || size < 32) return 0;
    const uint8_t *p = data;
    /* VkPipelineCacheHeaderVersionOne is exactly 32 bytes, little endian,
     * independent of host structure packing. Unknown versions are cache misses. */
    return vkc_u32le(p) == 32 && vkc_u32le(p + 4) == 1 && vkc_u32le(p + 8) == vendor &&
           vkc_u32le(p + 12) == device && !memcmp(p + 16, uuid, 16);
}

static inline void *vkc_read_pipeline(const char *path, uint32_t vendor, uint32_t device,
                                      const uint8_t uuid[16], size_t *size) {
    *size = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    uint8_t header[32];
    void *data = NULL;
    /* Reject incompatible caches before allocating or reading their payload. */
    if (fread(header, 1, sizeof header, fp) == sizeof header &&
        vkc_pipeline_header_valid(header, sizeof header, vendor, device, uuid))
        data = vkc_read_stream(fp, VKC_PIPELINE_LIMIT, size);
    fclose(fp);
    if (data && !vkc_pipeline_header_valid(data, *size, vendor, device, uuid)) {
        free(data); data = NULL; *size = 0;
    }
    return data;
}
#endif
