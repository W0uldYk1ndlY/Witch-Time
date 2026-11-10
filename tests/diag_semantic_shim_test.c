#include "../runtime/diag_shim.h"
#include "../runtime/diag.h"
#include "../runtime/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Dummy symbols for game_shim.c */
const FnEntry fn_table[] = {{0, 0}};
const int fn_table_count = 1;
const DynSym dyn_syms[] = {{0, 0, 0}};
const int dyn_sym_count = 1;

/* 1. Test input_mark thread-safety and no publish-before-fill */
#define MARK_MAX 64
static char g_marks[MARK_MAX][40];
static volatile LONG g_mark_allocated = 0;
static volatile LONG g_mark_count = 0;

void input_mark(const char *what) {
    if (!what) return;
    LONG i = InterlockedIncrement(&g_mark_allocated) - 1;
    if (i < MARK_MAX) {
        strncpy(g_marks[i], what, sizeof(g_marks[i]) - 1);
        g_marks[i][sizeof(g_marks[i]) - 1] = '\0';
        MemoryBarrier();
        while (g_mark_count < i) {
            #if defined(_WIN32)
            YieldProcessor();
            #endif
        }
        InterlockedIncrement(&g_mark_count);
    }
}

int nnhid_mark_seen(const char *what) {
    LONG n = g_mark_count;
    for (LONG i = 0; i < n && i < MARK_MAX; i++) if (!strcmp(g_marks[i], what)) return 1;
    return 0;
}

#if defined(_WIN32)
static DWORD WINAPI mark_producer_thread(LPVOID arg) {
    int id = (int)(intptr_t)arg;
    char name[32];
    for (int i = 0; i < 10; i++) {
        snprintf(name, sizeof(name), "mark_%d_%d", id, i);
        input_mark(name);
    }
    return 0;
}
#endif

static void test_input_mark_publish_before_fill(void) {
    printf("[1/6] Testing input_mark thread safety (no publish-before-fill)...\n");

    /* Initial state */
    input_mark("movie");
    assert(nnhid_mark_seen("movie") == 1);
    assert(nnhid_mark_seen("non_existent") == 0);

#if defined(_WIN32)
    HANDLE threads[4];
    for (int i = 0; i < 4; i++) {
        threads[i] = CreateThread(NULL, 0, mark_producer_thread, (LPVOID)(intptr_t)i, 0, NULL);
        assert(threads[i] != NULL);
    }
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) {
        CloseHandle(threads[i]);
    }

    /* All produced marks must be seen and intact */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 10; j++) {
            char name[32];
            snprintf(name, sizeof(name), "mark_%d_%d", i, j);
            assert(nnhid_mark_seen(name) == 1);
        }
    }
#endif

    printf("      Concurrent marks published safely without data races (passed)\n");
}

/* 2. Test semantic observation publication */
static int g_callback_calls = 0;
static diag_semantic_observation_t g_callback_last_obs;

static void on_semantic_obs_cb(const diag_semantic_observation_t *obs) {
    g_callback_calls++;
    if (obs) g_callback_last_obs = *obs;
}

static void test_semantic_observation_publication(void) {
    printf("[2/6] Testing semantic observation publication from game thread...\n");

    diag_publish_semantic_observation(2 /* Chapter 1 */, 1 /* Verse 1 */, 1 /* Standard */, 11 /* Shibuya */, true /* Movie */, 120 /* Draws */);

    diag_semantic_observation_t obs;
    bool has_obs = diag_get_last_semantic_observation(&obs);
    assert(has_obs);
    assert(obs.chapter == 2);
    assert(obs.verse == 1);
    assert(obs.difficulty == 1);
    assert(obs.area == 11);
    assert(obs.movie_active == true);
    assert(obs.draws_seen == 120);
    assert(obs.timestamp_ns > 0);

    printf("      Semantic observation published and retrieved accurately (passed)\n");
}

/* 3. Test versioned diagnostic shim binding */
static void test_versioned_shim_binding(void) {
    printf("[3/6] Testing versioned shim binding handshake...\n");

    GameDiagBinding in_bind;
    memset(&in_bind, 0, sizeof(in_bind));
    in_bind.version = DIAG_SHIM_VERSION;
    in_bind.program_id = DIAG_SHIM_PROGRAM_SUPPORTED;
    in_bind.game_hash = DIAG_SHIM_SUPPORTED_GAME_HASH;
    in_bind.capabilities = DIAG_SHIM_CAP_SEMANTIC_OBSERVATIONS | DIAG_SHIM_CAP_REPLAY_VALIDATION;
    in_bind.on_semantic_observation = on_semantic_obs_cb;

    GameDiagBinding out_bind;
    memset(&out_bind, 0, sizeof(out_bind));

    int res = game_diag_bind(&in_bind, &out_bind);
    assert(res == DIAG_SHIM_OK);
    assert(out_bind.version == DIAG_SHIM_VERSION);
    assert(out_bind.program_id == DIAG_SHIM_PROGRAM_SUPPORTED);
    assert(strcmp(out_bind.game_hash, DIAG_SHIM_SUPPORTED_GAME_HASH) == 0);
    assert((out_bind.capabilities & DIAG_SHIM_CAP_SEMANTIC_OBSERVATIONS) != 0);

    /* Publishing observation now updates state */
    diag_semantic_observation_t obs_check;
    diag_publish_semantic_observation(3 /* Chapter 2 */, 2 /* Verse 2 */, 0 /* Casual */, 12 /* Shinjuku */, false, 85);
    assert(diag_get_last_semantic_observation(&obs_check));
    assert(obs_check.chapter == 3 && obs_check.verse == 2);

    printf("      Shim binding successfully established (passed)\n");
}

/* 4. Test Program-1 capability rejection */
static void test_program1_capability_rejection(void) {
    printf("[4/6] Testing Program-1 capability rejection...\n");

    GameDiagBinding in_bind;
    memset(&in_bind, 0, sizeof(in_bind));
    in_bind.version = DIAG_SHIM_VERSION;
    in_bind.program_id = 1; /* Program 1 */
    in_bind.game_hash = DIAG_SHIM_SUPPORTED_GAME_HASH;

    GameDiagBinding out_bind;
    memset(&out_bind, 0, sizeof(out_bind));

    int res = game_diag_bind(&in_bind, &out_bind);
    assert(res == DIAG_SHIM_ERR_UNSUPPORTED_PROGRAM);

    printf("      Program 1 rejected as unsupported by diagnostic capability gate (passed)\n");
}

/* 5. Test unsupported game hash and version rejection */
static void test_unsupported_hash_and_version_rejection(void) {
    printf("[5/6] Testing unsupported game hash and version rejection...\n");

    GameDiagBinding in_bind;
    memset(&in_bind, 0, sizeof(in_bind));
    in_bind.version = DIAG_SHIM_VERSION;
    in_bind.program_id = DIAG_SHIM_PROGRAM_SUPPORTED;
    in_bind.game_hash = "unsupported_unknown_hash";

    GameDiagBinding out_bind;
    int res = game_diag_bind(&in_bind, &out_bind);
    assert(res == DIAG_SHIM_ERR_UNSUPPORTED_HASH);

    /* Version mismatch */
    in_bind.version = 999;
    in_bind.game_hash = DIAG_SHIM_SUPPORTED_GAME_HASH;
    res = game_diag_bind(&in_bind, &out_bind);
    assert(res == DIAG_SHIM_ERR_UNSUPPORTED_VERSION);

    /* Null binding */
    res = game_diag_bind(NULL, &out_bind);
    assert(res == DIAG_SHIM_ERR_NULL_BINDING);

    printf("      Unsupported hash and version rejected (passed)\n");
}

/* 6. Test graceful fallback with older library */
static void test_older_library_fallback(void) {
    printf("[6/6] Testing graceful fallback when library lacks diagnostic symbols...\n");

    /* Null module handle simulates missing shim or older DLL */
    bool bound = diag_try_bind_shim(NULL, DIAG_SHIM_SUPPORTED_GAME_HASH, 0);
    assert(!bound);
    assert(!diag_is_shim_bound());

    /* System launches normally without diagnostic shim */
    diag_semantic_observation_t obs;
    diag_publish_semantic_observation(0, 0, 0, 0, false, 0);
    assert(diag_get_last_semantic_observation(&obs));

    printf("      Older library graceful fallback verified (passed)\n");
}

int main(void) {
    printf("=== Witch-Time Semantic Observations and Shim Binding Test Harness ===\n\n");

    test_input_mark_publish_before_fill();
    test_semantic_observation_publication();
    test_versioned_shim_binding();
    test_program1_capability_rejection();
    test_unsupported_hash_and_version_rejection();
    test_older_library_fallback();

    printf("\nAll 6 semantic observation and shim binding tests PASSED successfully.\n");
    return 0;
}
