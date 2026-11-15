/* Behavioral tests for the allocator included by vulkan_allocator_test.py.
 * Vulkan is the external boundary; allocation placement, free lists, budgets,
 * reclamation, retries and accounting all execute the production C code. */
#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define MIB (UINT64_C(1) << 20)
#define CHECK(condition, ...) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); \
} } while (0)

typedef long long LONG64;
static struct { VkDevice device; VkPhysicalDevice gpu; } V;
static struct { VkPhysicalDeviceMemoryProperties mp; int has_budget; } D;
typedef struct {
    VkDeviceMemory memory;
    uint64_t size;
    uint32_t type;
    int freed;
    void *map;
} DriverBlock;
typedef struct { VkDeviceMemory memory; uint64_t offset, size; int live; } Owner;
typedef struct { char kind; uint32_t type; uint64_t size; VkResult result; } DriverEvent;
static DriverBlock driver[8192];
static Owner owners[16384];
static DriverEvent events[16384];
static int driver_count, owner_count, event_count, alloc_calls, free_calls, budget_queries;
static uint64_t budget[VK_MAX_MEMORY_HEAPS], other_usage[VK_MAX_MEMORY_HEAPS];
static uint64_t allocation_limit[VK_MAX_MEMORY_TYPES], heap_limit[VK_MAX_MEMORY_HEAPS];
static int metadata_fail, metadata_failures;
static jmp_buf fatal_jump;
static int expect_fatal;

static uint64_t heap_used(uint32_t heap) {
    uint64_t n = other_usage[heap];
    for (int i = 0; i < driver_count; i++)
        if (!driver[i].freed && D.mp.memoryTypes[driver[i].type].heapIndex == heap) n += driver[i].size;
    return n;
}
static DriverBlock *block_of(VkDeviceMemory memory) {
    for (int i = 0; i < driver_count; i++) if (driver[i].memory == memory) return &driver[i];
    CHECK(0, "unknown memory handle");
    return NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                               const VkAllocationCallbacks *callbacks, VkDeviceMemory *memory) {
    (void)device; (void)callbacks;
    CHECK(info->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, "invalid allocation structure");
    CHECK(info->memoryTypeIndex < D.mp.memoryTypeCount && info->allocationSize, "invalid memory request");
    uint32_t type = info->memoryTypeIndex, heap = D.mp.memoryTypes[type].heapIndex;
    VkResult result = allocation_limit[type] && info->allocationSize > allocation_limit[type]
        ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
    if (heap_limit[heap] && heap_used(heap) + info->allocationSize > heap_limit[heap])
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    CHECK(event_count < (int)(sizeof events / sizeof events[0]), "event table full");
    events[event_count++] = (DriverEvent){'A', type, info->allocationSize, result};
    alloc_calls++;
    if (result != VK_SUCCESS) return result;
    CHECK(driver_count < (int)(sizeof driver / sizeof driver[0]), "driver block table full");
    DriverBlock *b = &driver[driver_count++];
    b->memory = (VkDeviceMemory)(uintptr_t)driver_count;
    b->size = info->allocationSize; b->type = type;
    *memory = b->memory;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *callbacks) {
    (void)device; (void)callbacks;
    DriverBlock *b = block_of(memory);
    CHECK(!b->freed, "double Vulkan free");
    for (int i = 0; i < owner_count; i++)
        CHECK(!owners[i].live || owners[i].memory != memory, "freed a block with a live or pending GPU owner");
    events[event_count++] = (DriverEvent){'F', b->type, b->size, VK_SUCCESS};
    b->freed = 1; free(b->map); b->map = NULL; free_calls++;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                          VkDeviceSize size, VkMemoryMapFlags flags, void **data) {
    (void)device; (void)flags;
    DriverBlock *b = block_of(memory);
    CHECK(!b->freed && offset == 0 && size == VK_WHOLE_SIZE, "invalid persistent map");
    b->map = malloc((size_t)b->size);
    CHECK(b->map, "test backing allocation failed");
    *data = b->map;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physical,
                                                                VkPhysicalDeviceMemoryProperties2 *properties) {
    (void)physical;
    CHECK(properties->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, "invalid properties structure");
    VkPhysicalDeviceMemoryBudgetPropertiesEXT *out = properties->pNext;
    CHECK(out && out->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT, "budget chain missing");
    properties->memoryProperties = D.mp;
    for (uint32_t i = 0; i < D.mp.memoryHeapCount; i++) {
        out->heapBudget[i] = budget[i]; out->heapUsage[i] = heap_used(i);
    }
    budget_queries++;
}
static void *metadata_calloc(size_t n, size_t size) {
    if (metadata_fail) { metadata_failures++; return NULL; }
    return calloc(n, size);
}
static void *metadata_realloc(void *p, size_t size) {
    if (metadata_fail) { metadata_failures++; return NULL; }
    return realloc(p, size);
}
static const char *vk_result_name(VkResult result) { return result == VK_SUCCESS ? "success" : "allocation failure"; }
static void vkd_fail(const char *format, ...) {
    va_list ap; va_start(ap, format);
    fputs(expect_fatal ? "expected allocator failure: " : "allocator fatal: ", stdout);
    vfprintf(stdout, format, ap); fputc('\n', stdout); va_end(ap);
    if (expect_fatal) longjmp(fatal_jump, 1);
    exit(2);
}
#define VKC(call) do { VkResult r = (call); if (r != VK_SUCCESS) vkd_fail("Vulkan returned %d", r); } while (0)
#define calloc metadata_calloc
#define realloc metadata_realloc
#include "allocator_production.inc"
#undef calloc
#undef realloc

static void initialize(int discrete) {
    D.mp.memoryHeapCount = discrete ? 2 : 1;
    D.mp.memoryTypeCount = discrete ? 2 : 1;
    D.mp.memoryHeaps[0] = (VkMemoryHeap){UINT64_C(2) << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    D.mp.memoryTypes[0] = (VkMemoryType){VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    if (discrete) {
        D.mp.memoryHeaps[1] = (VkMemoryHeap){UINT64_C(2) << 30, 0};
        D.mp.memoryTypes[1] = (VkMemoryType){VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1};
    }
    budget[0] = budget[1] = UINT64_C(2) << 30;
    /* Mention the injectable growth boundary even before production uses it. */
    (void)metadata_realloc;
}
static VMem allocate(uint64_t size, uint64_t alignment, uint32_t bits, VkFlags flags, int cls) {
    VkMemoryRequirements requirements = {size, alignment, bits};
    VMem m = vmem_alloc(requirements, flags, cls);
    CHECK(m.block >= 0 && m.mem && m.size == size && m.off % alignment == 0, "returned allocation has incorrect size/alignment");
    DriverBlock *block = block_of(m.mem);
    CHECK(!block->freed && (bits & (1u << block->type)), "allocation uses an ineligible or freed block");
    CHECK(m.off <= block->size && m.size <= block->size - m.off, "allocation outside memory block");
    for (int i = 0; i < owner_count; i++) if (owners[i].live && owners[i].memory == m.mem)
        CHECK(m.off + m.size <= owners[i].offset || owners[i].offset + owners[i].size <= m.off, "overlapping live allocations");
    CHECK(owner_count < (int)(sizeof owners / sizeof owners[0]), "owner table full");
    owners[owner_count++] = (Owner){m.mem, m.off, m.size, 1};
    return m;
}
static VMem gpu_alloc(uint64_t size, int cls) { return allocate(size, 256, 1, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cls); }
static void retire(VMem m) {
    int found = 0;
    for (int i = 0; i < owner_count; i++) if (owners[i].live && owners[i].memory == m.mem && owners[i].offset == m.off && owners[i].size == m.size) {
        owners[i].live = 0; found = 1; break;
    }
    CHECK(found, "retired an unknown or already freed resource");
    vmem_free(m);
}
static int block_count(void) {
    int n = 0;
    for (int i = 0; i < g_nblk; i++) n += g_blk[i] != NULL;
    return n;
}
static void audit(void) {
    uint64_t total = 0, live = 0, system = 0;
    for (int b = 0; b < g_nblk; b++) if (g_blk[b]) {
        VBlock *k = g_blk[b]; uint64_t free_bytes = 0, live_bytes = 0; int live_count = 0;
        DriverBlock *backing = block_of(k->mem);
        CHECK(!backing->freed && backing->size == k->size && backing->type == k->type, "allocator/driver block mismatch");
        for (int i = 0; i < k->nfree; i++) {
            CHECK(k->fr[i].size && k->fr[i].off <= k->size && k->fr[i].size <= k->size - k->fr[i].off, "invalid free range");
            if (i) CHECK(k->fr[i-1].off + k->fr[i-1].size < k->fr[i].off, "free ranges overlap or failed to coalesce");
            free_bytes += k->fr[i].size;
            for (int j = 0; j < owner_count; j++) if (owners[j].live && owners[j].memory == k->mem)
                CHECK(k->fr[i].off + k->fr[i].size <= owners[j].offset || owners[j].offset + owners[j].size <= k->fr[i].off,
                      "free range overlaps a live owner");
        }
        for (int i = 0; i < owner_count; i++) if (owners[i].live && owners[i].memory == k->mem) {
            live_bytes += owners[i].size; live_count++;
        }
        CHECK(live_count == k->used, "live owner count %d != allocator count %d", live_count, k->used);
        CHECK(free_bytes + live_bytes == k->size, "range loss: block=%d size=%" PRIu64 " free=%" PRIu64 " live=%" PRIu64 " missing=%" PRIu64,
              b, k->size, free_bytes, live_bytes, k->size - free_bytes - live_bytes);
        total += k->size; live += live_bytes; if (k->sys) system += k->size;
    }
    CHECK(g_mem_total == (LONG64)total && g_mem_used == (LONG64)live && g_mem_sys == (LONG64)system,
          "incorrect global byte accounting");
    CHECK(g_mem_leak == 0, "lost-range counter is %lld", g_mem_leak);
    for (int i = 0; i < driver_count; i++) if (!driver[i].freed) {
        int found = 0;
        for (int b = 0; b < g_nblk; b++) found |= g_blk[b] && g_blk[b]->mem == driver[i].memory;
        CHECK(found, "driver allocation has no allocator owner");
    }
}
static void fragmented_frees(void) {
    VMem allocation[1200];
    for (int i = 0; i < 1200; i++) allocation[i] = gpu_alloc(256, 1);
    CHECK(alloc_calls == 1, "small resources unexpectedly require separate Vulkan blocks");
    for (int i = 0; i < 1200; i += 2) retire(allocation[i]);
    printf("fragmented_frees: live=153600 B, lost=%lld B, free ranges=%d\n", g_mem_leak, g_blk[0]->nfree);
    audit();
    for (int i = 1; i < 1200; i += 2) retire(allocation[i]);
    audit(); CHECK(g_mem_used == 0, "live bytes remain after retirement");
}
static void metadata_oom(void) {
    static VMem allocation[1200];
    volatile int n = 0;
    for (; n < 500; n++) allocation[n] = gpu_alloc(256, 1);
    metadata_fail = 1;
    expect_fatal = 1;
    if (!setjmp(fatal_jump)) for (; n < 1200; n++) allocation[n] = gpu_alloc(256, 1);
    expect_fatal = 0;
    /* Longjmp makes modified automatic values indeterminate; the owner ledger
     * is static and records only successful handouts. */
    n = owner_count;
    audit();
    for (int i = 0; i < n; i += 2) retire(allocation[i]);
    for (int i = 1; i < n; i += 2) retire(allocation[i]);
    audit();
    CHECK(metadata_failures > 0, "metadata OOM injection was not reached");
    printf("metadata_oom: %d successful handouts, %d injected failures; all bytes remain reusable\n", n, metadata_failures);
}
static void empty_blocks(void) {
    VMem allocation[12];
    for (int i = 0; i < 12; i++) allocation[i] = gpu_alloc(16 * MIB, 1);
    CHECK(block_count() == 3, "fixture needs three ordinary Vulkan blocks");
    for (int i = 0; i < 12; i++) { retire(allocation[i]); audit(); }
    printf("empty_blocks: retained=%d blocks, driver frees=%d\n", block_count(), free_calls);
    CHECK(block_count() == 1 && g_mem_total == 64 * (LONG64)MIB, "more than one empty block retained for one pool");
    int before = alloc_calls;
    VMem warm = gpu_alloc(MIB, 1);
    CHECK(alloc_calls == before, "warm block was not reused");
    retire(warm); audit();
}
static void pool_isolation(void) {
    VMem allocation[24];
    D.mp.memoryTypeCount = 2;
    D.mp.memoryTypes[1] = (VkMemoryType){VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    for (int p = 0; p < 3; p++) for (int i = 0; i < 8; i++)
        allocation[p * 8 + i] = allocate(16 * MIB, 256, p == 2 ? 2 : 1, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, p == 1 ? 2 : 1);
    for (int i = 0; i < 24; i++) retire(allocation[i]);
    audit();
    CHECK(block_count() == 3, "warm pools must be distinct by both memory type and resource class");
    int seen[2][3] = {{0}};
    for (int b = 0; b < g_nblk; b++) if (g_blk[b]) seen[g_blk[b]->type][g_blk[b]->cls]++;
    CHECK(seen[0][1] == 1 && seen[0][2] == 1 && seen[1][1] == 1, "incorrect warm-pool identity");
}
static void uma_trim(void) {
    VMem idle = gpu_alloc(MIB, 1), pending = gpu_alloc(MIB, 2);
    retire(idle);
    D.has_budget = 1; budget[0] = 96 * MIB;
    int before = event_count;
    VMem result = gpu_alloc(4 * MIB, 3);
    printf("uma_trim: budget queries=%d, retained=%lld B, live=%lld B\n", budget_queries, g_mem_total, g_mem_used);
    CHECK(budget_queries > 0, "UMA skipped budget preflight");
    CHECK(block_of(idle.mem)->freed && !block_of(pending.mem)->freed, "pressure trim failed or touched pending ownership");
    CHECK(events[before].kind == 'F', "pressure trim must happen before new Vulkan allocation");
    CHECK(g_mem_total <= 96 * (LONG64)MIB, "adequate smaller allocation was not used");
    audit(); retire(result); retire(pending); audit();
}
static void uma_shrink(void) {
    D.has_budget = 1; budget[0] = 12 * MIB;
    VMem m = gpu_alloc(4 * MIB, 1);
    CHECK(budget_queries > 0, "UMA budget was not queried");
    CHECK(block_of(m.mem)->size >= 4 * MIB && block_of(m.mem)->size <= 12 * MIB, "preflight allocated a needlessly large block");
    CHECK(alloc_calls == 1, "known budget pressure should shrink before calling the driver");
    audit(); retire(m); audit();
}
static void budget_advisory(void) {
    D.has_budget = 1; budget[0] = MIB; other_usage[0] = 2 * MIB;
    VMem m = gpu_alloc(4 * MIB, 1);
    CHECK(m.mem && alloc_calls > 0, "an advisory budget rejected a required allocation");
    audit(); retire(m); audit();
}
static void oom_retry(void) {
    allocation_limit[0] = 8 * MIB;
    VMem m = gpu_alloc(4 * MIB, 1);
    CHECK(alloc_calls > 1 && events[0].size == 64 * MIB, "driver OOM did not trigger smaller retries");
    CHECK(block_of(m.mem)->size >= 4 * MIB && block_of(m.mem)->size <= 8 * MIB, "retry block cannot satisfy the requested resource");
    for (int i = 1; i < event_count; i++)
        CHECK(events[i].size < events[i-1].size, "retry size did not decrease");
    audit(); retire(m); audit();
}
static void oom_trim(void) {
    VMem idle = gpu_alloc(MIB, 1), pending = gpu_alloc(MIB, 2);
    retire(idle); heap_limit[0] = 128 * MIB;
    int before = event_count;
    VMem m = gpu_alloc(4 * MIB, 3);
    CHECK(events[before].kind == 'A' && events[before].result == VK_ERROR_OUT_OF_DEVICE_MEMORY, "fixture did not produce actual driver OOM");
    CHECK(block_of(idle.mem)->freed && !block_of(pending.mem)->freed, "OOM did not reclaim only the idle block");
    CHECK(events[before+1].kind == 'F' && events[before+2].kind == 'A', "OOM retry must follow empty-block reclamation");
    audit(); retire(m); retire(pending); audit();
}
static void discrete_fallback(void) {
    allocation_limit[0] = MIB;
    VMem m = allocate(4 * MIB, 256, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
    CHECK(block_of(m.mem)->type == 1 && g_mem_sys == (LONG64)block_of(m.mem)->size, "system-heap fallback lost");
    audit(); retire(m); audit();
    /* A required large dedicated allocation must also retire its backing. */
    VMem large = allocate(20 * MIB, 4096, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 2);
    VkDeviceMemory backing = large.mem;
    retire(large); CHECK(block_of(backing)->freed, "dedicated backing was retained"); audit();
}
static void discrete_budget_advisory(void) {
    D.has_budget = 1; budget[0] = MIB; other_usage[0] = 2 * MIB;
    allocation_limit[1] = MIB;
    VMem m = allocate(4 * MIB, 256, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
    CHECK(block_of(m.mem)->type == 0 && g_mem_sys == 0, "failed fallback left a required primary allocation untried");
    CHECK(!(g_vram_full & 1), "successful primary allocation remains marked unavailable");
    audit(); retire(m); audit();
}
static VMem cached_budget_fallback(void) {
    D.has_budget = 1; budget[0] = MIB; other_usage[0] = 2 * MIB;
    VMem first = allocate(4 * MIB, 256, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
    CHECK(block_of(first.mem)->type == 1 && (g_vram_full & 1), "fixture did not cache the advisory fallback");
    CHECK(g_mem_total == 64 * (LONG64)MIB && g_mem_sys == g_mem_total, "unexpected first fallback backing");
    allocation_limit[1] = MIB;
    return first;
}
static void check_cached_retry(int before, VkResult primary_result) {
    CHECK(event_count == before + 6, "cached fallback must make five decreasing alternate attempts and one primary attempt; got %d", event_count - before);
    for (int i = 0; i < 5; i++) {
        DriverEvent event = events[before + i];
        CHECK(event.kind == 'A' && event.type == 1 && event.size == (64 * MIB >> i) && event.result == VK_ERROR_OUT_OF_DEVICE_MEMORY,
              "unexpected alternate retry at index %d", i);
    }
    DriverEvent last = events[before + 5];
    CHECK(last.kind == 'A' && last.type == 0 && last.size == 4 * MIB && last.result == primary_result,
          "cached preference skipped the required primary attempt");
}
static void discrete_cached_budget_advisory(void) {
    VMem first = cached_budget_fallback();
    int before = event_count;
    VMem second = allocate(4 * MIB, 256, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 2);
    check_cached_retry(before, VK_SUCCESS);
    CHECK(block_of(second.mem)->type == 0 && !(g_vram_full & 1), "primary allocation did not recover from cached budget preference");
    CHECK(!block_of(first.mem)->freed && g_mem_sys == 64 * (LONG64)MIB && g_mem_total == 68 * (LONG64)MIB,
          "recovery changed the live fallback owner or byte accounting");
    audit(); retire(second); retire(first); audit();
}
static void discrete_cached_failure_bounded(void) {
    VMem first = cached_budget_fallback();
    allocation_limit[0] = MIB;
    int before = event_count;
    expect_fatal = 1;
    if (!setjmp(fatal_jump)) {
        (void)allocate(4 * MIB, 256, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 2);
        CHECK(0, "both heaps failed but allocation unexpectedly succeeded");
    }
    expect_fatal = 0;
    check_cached_retry(before, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(!block_of(first.mem)->freed && block_count() == 1 && g_mem_total == 64 * (LONG64)MIB,
          "terminal failure lost existing ownership or published failed backing");
    audit(); retire(first); audit();
}
static void discrete_fallback_reuse(void) {
    allocation_limit[0] = MIB;
    VMem allocation[5];
    for (int i = 0; i < 5; i++) allocation[i] = allocate(16 * MIB, 256, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
    retire(allocation[0]); retire(allocation[4]); audit();
    CHECK(block_count() == 2 && g_mem_total == 128 * (LONG64)MIB, "fixture needs live and warm fallback blocks");
    for (int i = 0; i < 3; i++) {
        VMem dedicated = allocate(20 * MIB, 256, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 2);
        retire(dedicated);
        CHECK(!(g_vram_full & 1), "dedicated release did not clear the cached preference");
        int before = event_count;
        VMem next = allocate(4 * MIB, 256, 3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
        int empty = 0;
        for (int b = 0; b < g_nblk; b++)
            if (g_blk[b] && g_blk[b]->type == 1 && g_blk[b]->cls == 1 && !g_blk[b]->used) empty++;
        CHECK(empty <= 1 && g_mem_total == 128 * (LONG64)MIB,
              "fallback transition retained %d empty blocks and %lld bytes", empty, g_mem_total);
        CHECK(next.mem == allocation[1].mem, "available fallback range was not reused");
        for (int event = before; event < event_count; event++)
            CHECK(events[event].kind != 'A' || events[event].type != 1, "fallback transition created unnecessary backing");
        retire(next); audit();
    }
    for (int i = 1; i < 4; i++) retire(allocation[i]);
    audit();
    CHECK(block_count() == 1 && g_mem_total == 64 * (LONG64)MIB, "fallback pool kept extra empty backing after retirement");
}
static void alignment(void) {
    VMem a = gpu_alloc(257, 1), b = allocate(1000, 4096, 1, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
    VMem c = allocate(777, 512, 1, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
    CHECK(a.off == 0 && b.off == 4096 && c.off == 512, "aligned first-fit placement changed unexpectedly");
    audit(); retire(b); audit(); retire(a); audit(); retire(c); audit();
    CHECK(g_blk[0]->nfree == 1 && g_blk[0]->fr[0].off == 0 && g_blk[0]->fr[0].size == 64 * MIB, "alignment padding was not fully recovered");
}
static void finish(const char *name) {
    CHECK(g_mem_used == 0, "scenario left live owners");
    audit();
    printf("PASS %s: allocated=%lld B live=%lld B lost=%lld B; Vulkan calls:", name, g_mem_total, g_mem_used, g_mem_leak);
    for (int i = 0; i < event_count; i++) printf(" %c%u:%" PRIu64 ":%d", events[i].kind, events[i].type, events[i].size, events[i].result);
    putchar('\n');
    vmem_report(0);
    for (int i = 0; i < g_nblk; i++) if (g_blk[i]) {
        vkFreeMemory(V.device, g_blk[i]->mem, NULL); free(g_blk[i]); g_blk[i] = NULL;
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2, "choose one allocator scenario");
    initialize(!strncmp(argv[1], "discrete_", 9));
    if (!strcmp(argv[1], "fragmented_frees")) fragmented_frees();
    else if (!strcmp(argv[1], "metadata_oom")) metadata_oom();
    else if (!strcmp(argv[1], "empty_blocks")) empty_blocks();
    else if (!strcmp(argv[1], "pool_isolation")) pool_isolation();
    else if (!strcmp(argv[1], "uma_trim")) uma_trim();
    else if (!strcmp(argv[1], "uma_shrink")) uma_shrink();
    else if (!strcmp(argv[1], "budget_advisory")) budget_advisory();
    else if (!strcmp(argv[1], "oom_retry")) oom_retry();
    else if (!strcmp(argv[1], "oom_trim")) oom_trim();
    else if (!strcmp(argv[1], "discrete_fallback")) discrete_fallback();
    else if (!strcmp(argv[1], "discrete_budget_advisory")) discrete_budget_advisory();
    else if (!strcmp(argv[1], "discrete_cached_budget_advisory")) discrete_cached_budget_advisory();
    else if (!strcmp(argv[1], "discrete_cached_failure_bounded")) discrete_cached_failure_bounded();
    else if (!strcmp(argv[1], "discrete_fallback_reuse")) discrete_fallback_reuse();
    else if (!strcmp(argv[1], "alignment")) alignment();
    else CHECK(0, "unknown scenario");
    finish(argv[1]);
    return 0;
}
