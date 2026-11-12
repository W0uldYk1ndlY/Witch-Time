/* gcc -std=gnu11 -pthread tests/diag_hook_ownership_test.c -o diag_hook_ownership_test */
#include <assert.h>
#include <pthread.h>
#include "../runtime/diag.c"

enum { EVENTS_PER_THREAD = 500, TOTAL_EVENTS = 2 * EVENTS_PER_THREAD };
static unsigned ready, go;
typedef struct publisher { int index; uint32_t tid; } publisher_t;

static void *publish_hooks(void *arg) {
    publisher_t *p = arg;
    p->tid = diag_get_tid();
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) {}
    for (int i = 0; i < EVENTS_PER_THREAD; i++) {
        if (p->index == 0) diag_hook_present_queued(i, p->index, 0, 0);
        else diag_hook_gpu_submit(i, p->index, 0, 0);
    }
    return NULL;
}

static void test_two_hook_publishers(void) {
    for (int round = 0; round < 20; round++) {
        assert(diag_init("hook-ownership") == 0);
        (void)diag_time_ns();
        ready = go = 0;
        publisher_t p[2] = {{0, 0}, {1, 0}};
        pthread_t threads[2];
        for (int i = 0; i < 2; i++)
            assert(pthread_create(&threads[i], NULL, publish_hooks, &p[i]) == 0);
        uint64_t start = diag_time_ns();
        while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 2)
            assert(diag_time_ns() - start < 1000000000ULL);
        __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
        for (int i = 0; i < 2; i++) assert(pthread_join(threads[i], NULL) == 0);
        assert(p[0].tid != p[1].tid);
        assert(diag_seal_producers());
        assert(diag_get_total_emitted() == TOTAL_EVENTS);
        assert(diag_get_total_dropped() == 0);
        unsigned char seen[TOTAL_EVENTS + 1] = {0};
        int owner_slot[2] = {-1, -1};
        size_t total = 0;
        for (int slot = 0; slot < DIAG_MAX_PRODUCERS; slot++) {
            diag_record_t records[DIAG_RING_CAPACITY];
            size_t n = diag_drain_producer(slot, records, DIAG_RING_CAPACITY);
            if (!n) continue;
            assert(n == EVENTS_PER_THREAD);
            int owner = (int)records[0].payload[1];
            assert(owner == 0 || owner == 1);
            assert(owner_slot[owner] == -1);
            owner_slot[owner] = slot;
            for (size_t i = 0; i < n; i++) {
                diag_record_t *r = &records[i];
                assert(r->producer_id == (uint32_t)slot);
                assert(r->thread_id == p[owner].tid);
                assert(r->payload[0] == (int64_t)i && r->payload[1] == owner);
                assert(r->type == (owner ? DIAG_EV_GPU_SUBMIT : DIAG_EV_PRESENT_QUEUED));
                assert(r->seq > 0 && r->seq <= TOTAL_EVENTS && !seen[r->seq]);
                seen[r->seq] = 1;
            }
            total += n;
        }
        assert(owner_slot[0] >= 0 && owner_slot[1] >= 0 && owner_slot[0] != owner_slot[1]);
        assert(total == TOTAL_EVENTS);
        for (int seq = 1; seq <= TOTAL_EVENTS; seq++) assert(seen[seq]);
        diag_shutdown();
    }
}

static void test_tls_epoch_and_exhaustion(void) {
    assert(diag_init("tls-first") == 0);
    diag_hook_gpu_submit(1, 0, 0, 0);
    diag_record_t record;
    assert(diag_drain_producer(0, &record, 1) == 1);
    diag_shutdown();

    assert(diag_init("tls-next") == 0);
    assert(diag_register_producer("reserved") == 0);
    diag_hook_gpu_submit(2, 0, 0, 0); /* Same thread, old cached slot 0 must invalidate. */
    assert(diag_drain_producer(0, &record, 1) == 0);
    assert(diag_drain_producer(1, &record, 1) == 1);
    assert(record.thread_id == diag_get_tid() && record.payload[0] == 2);
    diag_shutdown();

    assert(diag_init("tls-full") == 0);
    for (int i = 0; i < DIAG_MAX_PRODUCERS; i++)
        assert(diag_register_producer("reserved") == i);
    diag_hook_gpu_submit(3, 0, 0, 0);
    assert(diag_get_total_emitted() == 0 && diag_get_total_dropped() == 1);
    diag_unregister_producer(DIAG_MAX_PRODUCERS - 1);
    diag_hook_gpu_submit(4, 0, 0, 0); /* Failed registration is cached within this epoch. */
    assert(diag_get_total_emitted() == 0 && diag_get_total_dropped() == 2);
    assert(diag_seal_producers());
    diag_emit(-1, DIAG_EV_CUSTOM, 0, 0, 0, 0);
    assert(diag_get_total_dropped() == 2); /* No late loss-counter admission. */
    diag_shutdown();

    assert(diag_init("tls-after-full") == 0);
    diag_hook_gpu_submit(5, 0, 0, 0); /* Cached failure also invalidates. */
    assert(diag_drain_producer(0, &record, 1) == 1 && record.payload[0] == 5);
    assert(diag_get_total_dropped() == 0);
    diag_shutdown();
}

static void test_initializing_slot_is_not_ready(void) {
    assert(diag_init("initializing") == 0);
    DIAG_ATOMIC_STORE(&g_diag_rings[0].active, 2, DIAG_ORDER_RELEASE);
    diag_producer_stats_t stats;
    assert(diag_get_producer_stats(0, &stats) && stats.active == 0 && !stats.name[0]);
    diag_emit(0, DIAG_EV_CUSTOM, 0, 0, 0, 0);
    assert(diag_get_total_emitted() == 0);
    diag_shutdown();
}

int main(void) {
    test_two_hook_publishers();
    test_tls_epoch_and_exhaustion();
    test_initializing_slot_is_not_ready();
    puts("hook ownership: 20 concurrent rounds, actual tids/full seq coverage, TLS epochs, explicit exhaustion, ready publication passed");
    return 0;
}
