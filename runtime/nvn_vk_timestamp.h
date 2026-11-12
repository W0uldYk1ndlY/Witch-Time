#ifndef NVN_VK_TIMESTAMP_H
#define NVN_VK_TIMESTAMP_H
#include <stdint.h>

typedef struct { uint64_t value, available; } VkTimestampResult;

/* Return the existing diagnostic query state; Vulkan success/not-ready are 0/1. */
static inline int nvn_vk_timestamp_delta(VkTimestampResult start, VkTimestampResult end,
                                        uint32_t bits, int result, uint64_t *ticks) {
    *ticks = 0;
    if (!bits || bits > 64) return 2; /* UNSUPPORTED */
    if (result != 0 && result != 1) return 3; /* QUERY_FAILED */
    if (result == 1 || !start.available || !end.available) return 1; /* PENDING */
    uint64_t mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    start.value &= mask; end.value &= mask;
    /* shortcut: spans must be shorter than one counter period; multiple wraps cannot be recovered. */
    *ticks = (end.value - start.value) & mask;
    return end.value < start.value ? 4 : 0; /* TIMESTAMP_WRAPPED / COMPLETED */
}
#endif
