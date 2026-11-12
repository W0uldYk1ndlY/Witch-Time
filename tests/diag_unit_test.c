#include "../runtime/diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE wt_thread_t;
typedef DWORD (WINAPI *wt_thread_fn)(void *);

static inline wt_thread_t wt_thread_create(wt_thread_fn fn, void *arg) {
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}

static inline void wt_thread_join(wt_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline void wt_sleep_ms(uint32_t ms) {
    Sleep(ms);
}
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t wt_thread_t;
typedef void *(*wt_thread_fn)(void *);

static inline wt_thread_t wt_thread_create(wt_thread_fn fn, void *arg) {
    pthread_t t;
    pthread_create(&t, NULL, fn, arg);
    return t;
}

static inline void wt_thread_join(wt_thread_t t) {
    pthread_join(t, NULL);
}

static inline void wt_sleep_ms(uint32_t ms) {
    usleep(ms * 1000);
}
#endif

/* 1. Test record layout and sizing */
static void test_record_layout(void) {
    printf("[1/6] Testing record layout and sizing...\n");
    assert(sizeof(diag_record_t) == 64);
    assert(sizeof(diag_event_type_t) <= 4);
    printf("      Record size: %zu bytes (passed)\n", sizeof(diag_record_t));
}

/* 2. Test disabled mode / no-op path */
static void test_disabled_mode(void) {
    printf("[2/6] Testing disabled mode...\n");
    diag_shutdown(); /* Ensure disabled */
    assert(!diag_is_enabled());

    /* Register should fail */
    int prod = diag_register_producer("disabled_test");
    assert(prod == -1);

    /* Emit should be a safe no-op */
    diag_emit(0, DIAG_EV_FRAME_START, 1, 2, 3, 4);
    assert(diag_get_total_emitted() == 0);
    assert(diag_get_total_dropped() == 0);

    /* Drain on invalid/unregistered should return 0 */
    diag_record_t recs[16];
    assert(diag_drain_producer(0, recs, 16) == 0);
    printf("      Disabled mode safe no-op (passed)\n");
}

/* 3. Test producer registration, limits, and lifetime reuse */
static void test_registration_and_reuse(void) {
    printf("[3/6] Testing producer registration limits and reuse...\n");
    assert(diag_init("test-run-session") == 0);
    assert(diag_is_enabled());
    int ids[DIAG_MAX_PRODUCERS];
    for (int i = 0; i < DIAG_MAX_PRODUCERS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "prod_%d", i);
        ids[i] = diag_register_producer(name);
        assert(ids[i] == i);
    }

    /* 17th registration must fail (bounded ceiling) */
    int overflow_id = diag_register_producer("overflow");
    assert(overflow_id == -1);

    /* Unregister slot 5 */
    diag_unregister_producer(ids[5]);

    /* Now a new producer can claim slot 5 with incremented generation */
    int reused_id = diag_register_producer("prod_reused_5");
    assert(reused_id == 5);

    diag_producer_stats_t stats;
    assert(diag_get_producer_stats(reused_id, &stats));
    assert(stats.active == 1);
    assert(stats.generation >= 2);
    assert(stats.emitted_count == 0);
    assert(stats.dropped_count == 0);
    assert(strcmp(stats.name, "prod_reused_5") == 0);

    diag_shutdown();
    printf("      Registration bounds and slot reuse (passed)\n");
}

/* 4. Test ring capacity, drop-new semantics, and monotonic sequencing */
static void test_drop_new_and_drain(void) {
    printf("[4/6] Testing ring capacity and drop-new non-blocking semantics...\n");
    assert(diag_init("test-drop-run") == 0);

    int prod = diag_register_producer("drop_producer");
    assert(prod >= 0);

    /* DIAG_RING_CAPACITY is 1024; usable capacity is 1023 because full is next_head == tail */
    const size_t usable_capacity = DIAG_RING_CAPACITY - 1;
    const size_t to_emit = usable_capacity + 150;

    for (size_t i = 0; i < to_emit; i++) {
        /* payload: i, pattern, i*2, 0 */
        diag_emit(prod, DIAG_EV_FRAME_START, (int64_t)i, (int64_t)(0xAA000000 | i), (int64_t)(i * 2), 0);
    }

    diag_producer_stats_t stats;
    assert(diag_get_producer_stats(prod, &stats));
    assert(stats.emitted_count == usable_capacity);
    assert(stats.dropped_count == 150);
    assert(diag_get_total_emitted() == usable_capacity);
    assert(diag_get_total_dropped() == 150);

    /* Now drain all emitted records */
    diag_record_t *drained = (diag_record_t *)malloc(sizeof(diag_record_t) * to_emit);
    assert(drained != NULL);

    size_t total_drained = 0;
    while (total_drained < usable_capacity) {
        size_t n = diag_drain_producer(prod, drained + total_drained, 64);
        if (n == 0) break;
        total_drained += n;
    }
    assert(total_drained == usable_capacity);

    /* Verify no partial records or corrupted payloads */
    for (size_t i = 0; i < total_drained; i++) {
        assert(drained[i].type == DIAG_EV_FRAME_START);
        assert(drained[i].producer_id == (uint32_t)prod);
        assert(drained[i].payload[0] == (int64_t)i);
        assert(drained[i].payload[1] == (int64_t)(0xAA000000 | i));
        assert(drained[i].payload[2] == (int64_t)(i * 2));
        if (i > 0) {
            /* Monotonically increasing sequence numbers */
            assert(drained[i].seq > drained[i - 1].seq);
        }
    }

    /* Subsequent drain should return 0 */
    diag_record_t dummy;
    assert(diag_drain_producer(prod, &dummy, 1) == 0);

    free(drained);
    diag_shutdown();
    printf("      Drop-new semantics and drain integrity (passed)\n");
}

/* 5. Test concurrent multi-producer and consumer */
#define CONCURRENT_PRODUCERS 4
#define ITEMS_PER_PRODUCER 3000

typedef struct producer_ctx {
    int producer_id;
    int thread_num;
    uint32_t items_emitted;
} producer_ctx_t;

#if defined(_WIN32)
static DWORD WINAPI producer_thread_func(void *arg) {
#else
static void *producer_thread_func(void *arg) {
#endif
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        diag_emit(ctx->producer_id, DIAG_EV_CUSTOM,
                  (int64_t)ctx->thread_num,
                  (int64_t)i,
                  (int64_t)(ctx->thread_num * 1000000LL + i),
                  (int64_t)(0x12345678ULL ^ (uint64_t)i));
        ctx->items_emitted++;
        if ((i & 0x7F) == 0) {
            wt_sleep_ms(0); /* Yield */
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_producers_and_consumer(void) {
    printf("[5/6] Testing concurrent multi-producer and background consumer...\n");
    assert(diag_init("test-concurrent-run") == 0);

    producer_ctx_t prod_ctx[CONCURRENT_PRODUCERS];
    wt_thread_t threads[CONCURRENT_PRODUCERS];

    for (int i = 0; i < CONCURRENT_PRODUCERS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "worker_%d", i);
        prod_ctx[i].producer_id = diag_register_producer(name);
        assert(prod_ctx[i].producer_id >= 0);
        prod_ctx[i].thread_num = i + 1;
        prod_ctx[i].items_emitted = 0;
    }

    /* Start producer threads */
    for (int i = 0; i < CONCURRENT_PRODUCERS; i++) {
        threads[i] = wt_thread_create(producer_thread_func, &prod_ctx[i]);
    }

    /* Consumer loop draining active producers while threads run */
    uint64_t drained_per_producer[CONCURRENT_PRODUCERS] = {0};
    int64_t last_idx_per_producer[CONCURRENT_PRODUCERS];
    for (int i = 0; i < CONCURRENT_PRODUCERS; i++) {
        last_idx_per_producer[i] = -1;
    }

    diag_record_t batch[128];
    int active_producers = CONCURRENT_PRODUCERS;

    while (active_producers > 0) {
        size_t batch_drained_this_pass = 0;
        for (int p = 0; p < CONCURRENT_PRODUCERS; p++) {
            size_t n = diag_drain_producer(prod_ctx[p].producer_id, batch, 128);
            if (n > 0) {
                batch_drained_this_pass += n;
                drained_per_producer[p] += n;
                for (size_t k = 0; k < n; k++) {
                    assert(batch[k].type == DIAG_EV_CUSTOM);
                    int thread_num = (int)batch[k].payload[0];
                    int64_t item_idx = batch[k].payload[1];
                    int64_t expected_hash = (int64_t)(thread_num * 1000000LL + item_idx);
                    int64_t expected_val = (int64_t)(0x12345678ULL ^ (uint64_t)item_idx);

                    assert(thread_num == prod_ctx[p].thread_num);
                    assert(batch[k].payload[2] == expected_hash);
                    assert(batch[k].payload[3] == expected_val);

                    /* Strict monotonic ordering per producer */
                    assert(item_idx > last_idx_per_producer[p]);
                    last_idx_per_producer[p] = item_idx;
                }
            }
        }

        /* Check if all producers are done */
        int done_count = 0;
        for (int p = 0; p < CONCURRENT_PRODUCERS; p++) {
            if (prod_ctx[p].items_emitted >= ITEMS_PER_PRODUCER) {
                done_count++;
            }
        }
        if (done_count == CONCURRENT_PRODUCERS && batch_drained_this_pass == 0) {
            break;
        }
        if (batch_drained_this_pass == 0) {
            wt_sleep_ms(1);
        }
    }

    /* Wait for all producer threads to terminate */
    for (int i = 0; i < CONCURRENT_PRODUCERS; i++) {
        wt_thread_join(threads[i]);
    }

    /* Final drain pass to collect any trailing records */
    for (int p = 0; p < CONCURRENT_PRODUCERS; p++) {
        size_t n;
        while ((n = diag_drain_producer(prod_ctx[p].producer_id, batch, 128)) > 0) {
            drained_per_producer[p] += n;
            for (size_t k = 0; k < n; k++) {
                int64_t item_idx = batch[k].payload[1];
                assert(item_idx > last_idx_per_producer[p]);
                last_idx_per_producer[p] = item_idx;
            }
        }
    }

    uint64_t total_drained = 0;
    for (int p = 0; p < CONCURRENT_PRODUCERS; p++) {
        diag_producer_stats_t stats;
        assert(diag_get_producer_stats(prod_ctx[p].producer_id, &stats));
        assert(drained_per_producer[p] == stats.emitted_count);
        total_drained += drained_per_producer[p];
    }
    assert(total_drained + diag_get_total_dropped() == CONCURRENT_PRODUCERS * ITEMS_PER_PRODUCER);
    printf("      Drained %llu items, dropped %llu items across %d threads (passed)\n",
           (unsigned long long)total_drained,
           (unsigned long long)diag_get_total_dropped(),
           CONCURRENT_PRODUCERS);

    diag_shutdown();
}

/* 6. Verify shutdown state clears active flags */
static void test_shutdown(void) {
    printf("[6/6] Testing shutdown cleanup...\n");
    assert(diag_init("test-shutdown") == 0);
    int p = diag_register_producer("to_shutdown");
    assert(p >= 0);

    diag_producer_stats_t stats;
    assert(diag_get_producer_stats(p, &stats));
    assert(stats.active == 1);

    diag_shutdown();
    assert(!diag_is_enabled());
    assert(diag_get_producer_stats(p, &stats));
    assert(stats.active == 0);
    printf("      Shutdown cleanup (passed)\n");
}

int main(void) {
    printf("=== Witch-Time Runtime Diag Unit Test ===\n");
    test_record_layout();
    test_disabled_mode();
    test_registration_and_reuse();
    test_drop_new_and_drain();
    test_concurrent_producers_and_consumer();
    test_shutdown();
    printf("=== All 6 Diag Unit Tests Passed ===\n");
    return 0;
}
