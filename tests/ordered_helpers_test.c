/* Host and AArch64 verification harness for ordered memory helpers and payload publication. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "runtime.h"

#ifdef _WIN32
#include <windows.h>
typedef HANDLE thread_t;
static DWORD WINAPI producer_thread(LPVOID arg);
static DWORD WINAPI consumer_thread(LPVOID arg);
static int thread_create(thread_t *t, LPTHREAD_START_ROUTINE fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t ? 0 : -1;
}
static void thread_join(thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#define THREAD_RET DWORD WINAPI
#define THREAD_ARG LPVOID
#else
#include <pthread.h>
typedef pthread_t thread_t;
#define THREAD_RET void *
#define THREAD_ARG void *
static int thread_create(thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
static void thread_join(thread_t t) {
    pthread_join(t, NULL);
}
#endif

/* Exported functions with fixed signatures for codegen disassembly inspection */
uint8_t harness_load8(uint64_t a) { return rd_ordered8(a); }
uint16_t harness_load16(uint64_t a) { return rd_ordered16(a); }
uint32_t harness_load32(uint64_t a) { return rd_ordered32(a); }
uint64_t harness_load64(uint64_t a) { return rd_ordered64(a); }

void harness_store8(uint64_t a, uint8_t v) { wr_ordered8(a, v); }
void harness_store16(uint64_t a, uint16_t v) { wr_ordered16(a, v); }
void harness_store32(uint64_t a, uint32_t v) { wr_ordered32(a, v); }
void harness_store64(uint64_t a, uint64_t v) { wr_ordered64(a, v); }

void harness_barrier(void) { dmb(); }

/* Payload publication channel */
#define ROUNDS 10000
typedef struct {
    uint64_t payload[4];
    volatile uint32_t ready;
    volatile uint32_t stop;
} PublicationChannel;

static PublicationChannel g_chan;

static THREAD_RET producer_thread(THREAD_ARG arg) {
    (void)arg;
    for (int r = 1; r <= ROUNDS; r++) {
        g_chan.payload[0] = (uint64_t)r * 0x1111111111111111ULL;
        g_chan.payload[1] = (uint64_t)r * 0x2222222222222222ULL;
        g_chan.payload[2] = (uint64_t)r * 0x3333333333333333ULL;
        g_chan.payload[3] = (uint64_t)r * 0x4444444444444444ULL;
        dmb();
        wr_ordered32((uint64_t)(uintptr_t)&g_chan.ready, (uint32_t)r);
        while (rd_ordered32((uint64_t)(uintptr_t)&g_chan.ready) != 0) {
            dmb();
        }
    }
    wr_ordered32((uint64_t)(uintptr_t)&g_chan.stop, 1);
    return 0;
}

static THREAD_RET consumer_thread(THREAD_ARG arg) {
    (void)arg;
    int expected = 1;
    while (expected <= ROUNDS) {
        dmb();
        uint32_t r = rd_ordered32((uint64_t)(uintptr_t)&g_chan.ready);
        if (r == (uint32_t)expected) {
            dmb();
            assert(g_chan.payload[0] == (uint64_t)expected * 0x1111111111111111ULL);
            assert(g_chan.payload[1] == (uint64_t)expected * 0x2222222222222222ULL);
            assert(g_chan.payload[2] == (uint64_t)expected * 0x3333333333333333ULL);
            assert(g_chan.payload[3] == (uint64_t)expected * 0x4444444444444444ULL);
            expected++;
            wr_ordered32((uint64_t)(uintptr_t)&g_chan.ready, 0);
        }
    }
    return 0;
}

static void test_smoke(void) {
    uint8_t b = 0;
    uint16_t h = 0;
    uint32_t w = 0;
    uint64_t d = 0;

    wr_ordered8((uint64_t)(uintptr_t)&b, 0x5a);
    assert(rd_ordered8((uint64_t)(uintptr_t)&b) == 0x5a);

    wr_ordered16((uint64_t)(uintptr_t)&h, 0x1234);
    assert(rd_ordered16((uint64_t)(uintptr_t)&h) == 0x1234);

    wr_ordered32((uint64_t)(uintptr_t)&w, 0x89abcdef);
    assert(rd_ordered32((uint64_t)(uintptr_t)&w) == 0x89abcdef);

    wr_ordered64((uint64_t)(uintptr_t)&d, 0x0123456789abcdefULL);
    assert(rd_ordered64((uint64_t)(uintptr_t)&d) == 0x0123456789abcdefULL);

    dmb();
}

int main(void) {
    printf("[HOST SMOKE] Testing ordered scalar helpers...\n");
    test_smoke();

    printf("[HOST SMOKE] Testing multi-threaded publication channel (%d rounds)...\n", ROUNDS);
    memset(&g_chan, 0, sizeof g_chan);

    thread_t prod, cons;
    if (thread_create(&cons, consumer_thread, NULL) != 0 ||
        thread_create(&prod, producer_thread, NULL) != 0) {
        fprintf(stderr, "thread_create failed, skipping threaded run\n");
        return 1;
    }

    thread_join(prod);
    thread_join(cons);

    printf("[HOST SMOKE] PASS: Ordered helpers and publication channel verified.\n");
    return 0;
}
