/* Cache files are optional acceleration data, never trusted driver inputs.
 * Limits comfortably exceed individual translated shaders and normal key lists.
 * The driver-cache ceiling also accommodates the existing ~0.9 GiB desktop cache. */
#ifndef WT_VK_CACHE_H
#define WT_VK_CACHE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WT_VK_DRIVER_CACHE_LIMIT ((size_t)1 << 30)
#define WT_VK_SPIRV_CACHE_LIMIT ((size_t)16 << 20)
#define WT_VK_RECORD_CACHE_LIMIT ((size_t)64 << 20)

/* Caller owns the stream. Failure is a cache miss, including an allocation
 * failure or a file that changes size while it is being read. */
static inline void *wt_vk_cache_read(FILE *fp, size_t limit, size_t multiple, size_t *size) {
    *size = 0;
    if (!fp || !multiple || fseek(fp, 0, SEEK_END)) return NULL;
    long end = ftell(fp);
    if (end <= 0 || (uint64_t)end > limit || (size_t)end % multiple || fseek(fp, 0, SEEK_SET)) return NULL;
    size_t n = (size_t)end;
    void *data = malloc(n);
    if (!data) return NULL;
    if (fread(data, 1, n, fp) != n || fgetc(fp) != EOF || ferror(fp)) { free(data); return NULL; }
    *size = n;
    return data;
}

static inline uint32_t wt_vk_cache_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* VkPipelineCacheHeaderVersionOne has a fixed 32-byte, little-endian layout,
 * independent of C structure packing and the host byte order. */
static inline int wt_vk_pipeline_cache_valid(const void *data, size_t n, uint32_t vendor,
                                            uint32_t device, const uint8_t uuid[16]) {
    if (!data || n < 32 || n > WT_VK_DRIVER_CACHE_LIMIT) return 0;
    const unsigned char *p = data;
    return wt_vk_cache_u32(p) == 32 && wt_vk_cache_u32(p + 4) == 1 &&
           wt_vk_cache_u32(p + 8) == vendor && wt_vk_cache_u32(p + 12) == device &&
           !memcmp(p + 16, uuid, 16);
}

/* Structural validation, not a replacement for SPIR-V semantic validation.
 * This catches truncated writes even for offline preparation, which previously
 * treated mere file existence as a valid compiled shader. */
static inline int wt_vk_spirv_cache_valid(const void *data, size_t n) {
    if (!data || n <= 20 || n > WT_VK_SPIRV_CACHE_LIMIT || n % 4) return 0;
    const unsigned char *p = data;
    uint32_t version = wt_vk_cache_u32(p + 4);
    if (wt_vk_cache_u32(p) != 0x07230203u || version < 0x00010000u || version > 0x00010600u ||
        (version & 0xff0000ffu) || !wt_vk_cache_u32(p + 12) || wt_vk_cache_u32(p + 16)) return 0;
    for (size_t i = 5, words = n / 4; i < words;) {
        uint32_t count = wt_vk_cache_u32(p + 4 * i) >> 16;
        if (!count || count > words - i) return 0;
        i += count;
    }
    return 1;
}
#endif
