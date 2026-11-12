#include "../runtime/diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define VK_SUCCESS 0
#define VK_SUBOPTIMAL_KHR 1000001003
#define VK_ERROR_OUT_OF_DATE_KHR -1000001004

/* 1. Test deferred present queue and flush */
static void test_deferred_flush(void) {
    printf("[1/5] Testing deferred present queue and flush...\n");
    assert(diag_init("run_gfx_deferred") == 0);

    /* Queue a deferred present at tick 1000 */
    diag_hook_present_queued(1, 0, 1000, 1);

    /* Drain and verify QUEUED event is present */
    diag_record_t recs[8];
    size_t n = diag_drain_producer(0, recs, 8); /* First calling thread claims slot 0. */
    assert(n == 1);
    assert(recs[0].type == DIAG_EV_PRESENT_QUEUED);
    assert(recs[0].payload[0] == 1);     /* frame */
    assert(recs[0].payload[1] == 0);     /* image_index */
    assert(recs[0].payload[2] == 1000);  /* target_tick */
    assert(recs[0].payload[3] == 1);     /* is_fight */

    /* Flush deferred present */
    diag_hook_present_called(1, 0, VK_SUCCESS, 0);

    n = diag_drain_producer(0, recs, 8);
    assert(n == 1);
    assert(recs[0].type == DIAG_EV_PRESENT_CALLED);
    assert(recs[0].payload[0] == 1);
    assert(recs[0].payload[1] == 0);
    assert(recs[0].payload[2] == VK_SUCCESS);
    assert(recs[0].payload[3] == 0);     /* not a progress present */

    diag_shutdown();
    printf("      Deferred flush distinguish queue from call (passed)\n");
}

/* 2. Test discarded surface generation */
static void test_discarded_surface_gen(void) {
    printf("[2/5] Testing discarded surface generation...\n");
    assert(diag_init("run_gfx_surface") == 0);

    /* Swapchain creation gen 1 */
    diag_hook_surface_gen(1, 1920, 1080, 44);

    /* Queue present on gen 1 */
    diag_hook_present_queued(2, 0, 2000, 0);

    /* Foreground switch or surface loss recreates swapchain to gen 2 */
    diag_hook_surface_gen(1, 0, 0, 0); /* destroyed */
    diag_hook_surface_gen(2, 2560, 1440, 44); /* recreated */

    /* Pending deferred present on gen 1 is discarded */
    diag_hook_present_discard(2, 0, 1, 2);

    diag_record_t recs[16];
    size_t n = diag_drain_producer(0, recs, 16);
    assert(n == 5);
    assert(recs[0].type == DIAG_EV_SURFACE_GEN && recs[0].payload[0] == 1 && recs[0].payload[1] == 1920);
    assert(recs[1].type == DIAG_EV_PRESENT_QUEUED && recs[1].payload[0] == 2);
    assert(recs[2].type == DIAG_EV_SURFACE_GEN && recs[2].payload[1] == 0);
    assert(recs[3].type == DIAG_EV_SURFACE_GEN && recs[3].payload[0] == 2 && recs[3].payload[1] == 2560);
    assert(recs[4].type == DIAG_EV_PRESENT_DISCARD && recs[4].payload[0] == 2 && recs[4].payload[3] == 2);

    diag_shutdown();
    printf("      Surface recreation and present discard tracking (passed)\n");
}

/* 3. Test progress / held-image repetition */
static void test_progress_held_image_repetition(void) {
    printf("[3/5] Testing progress / held-image repetition...\n");
    assert(diag_init("run_gfx_progress") == 0);

    /* Frame 10 is held while assets load; 4 progress bar presents occur */
    for (int p = 0; p < 4; p++) {
        diag_hook_present_called(10, 0, VK_SUCCESS, 1); /* is_progress = 1 */
    }

    /* Then normal game present */
    diag_hook_present_called(11, 1, VK_SUCCESS, 0); /* is_progress = 0 */

    diag_record_t recs[8];
    size_t n = diag_drain_producer(0, recs, 8);
    assert(n == 5);

    for (int i = 0; i < 4; i++) {
        assert(recs[i].type == DIAG_EV_PRESENT_CALLED);
        assert(recs[i].payload[0] == 10);
        assert(recs[i].payload[3] == 1); /* Identified as progress present */
    }

    assert(recs[4].type == DIAG_EV_PRESENT_CALLED);
    assert(recs[4].payload[0] == 11);
    assert(recs[4].payload[3] == 0); /* Normal gameplay present */

    diag_shutdown();
    printf("      Progress vs normal present differentiation (passed)\n");
}

/* 4. Test no-first-frame startup */
static void test_no_first_frame_startup(void) {
    printf("[4/5] Testing no-first-frame startup (GPU work before presentation)...\n");
    assert(diag_init("run_gfx_startup") == 0);

    /* Cold shader pre-warm: command buffers submitted and completed without presenting */
    diag_hook_gpu_submit(0, 1, 1, 4);
    diag_hook_gpu_complete(0, 1, 500, 250000);
    diag_hook_gpu_submit(1, 2, 2, 8);
    diag_hook_gpu_complete(1, 2, 600, 400000);

    diag_record_t recs[8];
    size_t n = diag_drain_producer(0, recs, 8);
    assert(n == 4);

    assert(recs[0].type == DIAG_EV_GPU_SUBMIT && recs[0].payload[1] == 1);
    assert(recs[1].type == DIAG_EV_GPU_COMPLETE && recs[1].payload[1] == 1);
    assert(recs[2].type == DIAG_EV_GPU_SUBMIT && recs[2].payload[1] == 2);
    assert(recs[3].type == DIAG_EV_GPU_COMPLETE && recs[3].payload[1] == 2);

    /* Verify no PRESENT_CALLED event was emitted */
    for (size_t i = 0; i < n; i++) {
        assert(recs[i].type != DIAG_EV_PRESENT_CALLED);
    }

    diag_shutdown();
    printf("      GPU submit/complete tracked before any frame present (passed)\n");
}

/* 5. Test render-end instrumentation */
static void test_render_end(void) {
    printf("[5/5] Testing render-end instrumentation...\n");
    assert(diag_init("run_gfx_render_end") == 0);

    diag_hook_render_end(42, 230, 0, 3);

    diag_record_t recs[4];
    size_t n = diag_drain_producer(0, recs, 4);
    assert(n == 1);
    assert(recs[0].type == DIAG_EV_RENDER_END);
    assert(recs[0].payload[0] == 42);  /* frame */
    assert(recs[0].payload[1] == 230); /* draws */
    assert(recs[0].payload[2] == 0);   /* skipped */
    assert(recs[0].payload[3] == 3);   /* clears */

    diag_shutdown();
    printf("      Render-end draw/clear metrics published (passed)\n");
}

static void test_temporal_observations(void) {
    diag_hook_temporal(NULL);
    assert(diag_init("run_gfx_temporal") == 0);
    diag_temporal_observation_t o = {
        .scene_frame = 17, .selected_mode = 7, .effective_mode = 0, .variant = 1,
        .scene_width = 853, .scene_height = 479, .output_width = 1280, .output_height = 720,
        .surface_generation = 3, .flags = DIAG_TEMPORAL_SKIPPED,
        .motion_support = 1, .reset_reason = 2, .fallback_reason = 4,
        .failing_pass = 1, .error_code = -3,
    };
    diag_hook_temporal(&o);
    diag_record_t recs[8];
    assert(diag_drain_producer(0, recs, 8) == 4);
    for (int i = 0; i < 4; i++) {
        assert(recs[i].type == DIAG_EV_TEMPORAL_SELECTION + (uint32_t)i);
        assert(recs[i].payload[0] == 17);
    }
    assert(recs[0].payload[1] == 7 && recs[0].payload[2] == 0 && recs[0].payload[3] == 1);
    assert((uint64_t)recs[1].payload[1] == ((uint64_t)853 << 32 | 479));
    assert((uint64_t)recs[1].payload[2] == ((uint64_t)1280 << 32 | 720));
    assert(recs[1].payload[3] == 3);
    assert(recs[2].payload[1] == DIAG_TEMPORAL_SKIPPED && recs[2].payload[3] == 2);
    assert(recs[3].payload[1] == 4 && recs[3].payload[2] == 1 && recs[3].payload[3] == -3);
    o.effective_mode = 7;
    o.flags = DIAG_TEMPORAL_SUPPORTED | DIAG_TEMPORAL_INITIALIZED | DIAG_TEMPORAL_DISPATCHED | DIAG_TEMPORAL_CONSUMED;
    o.fallback_reason = 0;
    diag_hook_temporal(&o);
    assert(diag_drain_producer(0, recs, 8) == 4);
    assert(recs[2].payload[1] == 15 && recs[3].payload[1] == 0);
    diag_shutdown();
    diag_hook_temporal(&o);
    assert(diag_drain_producer(0, recs, 8) == 0);
}

static void test_temporal_decisions_and_components(void) {
    diag_temporal_observation_t o = {0};
    diag_temporal_decide(&o, 2, 0, 0, 0);
    assert(o.fallback_reason == DIAG_TEMPORAL_NO_SCENE && !o.effective_mode);
    o = (diag_temporal_observation_t){0};
    diag_temporal_decide(&o, 2, 1, 0, 0);
    assert(o.fallback_reason == DIAG_TEMPORAL_UNSUPPORTED);
    o = (diag_temporal_observation_t){.flags = DIAG_TEMPORAL_SUPPORTED};
    diag_temporal_decide(&o, 2, 1, 0, 0);
    assert(o.fallback_reason == DIAG_TEMPORAL_SETUP_FAILED);
    o = (diag_temporal_observation_t){.flags = DIAG_TEMPORAL_SUPPORTED | DIAG_TEMPORAL_INITIALIZED | DIAG_TEMPORAL_DISPATCHED};
    diag_temporal_decide(&o, 2, 1, 1, 0);
    assert(o.fallback_reason == DIAG_TEMPORAL_NO_CONSUMER && !(o.flags & DIAG_TEMPORAL_CONSUMED));
    o.fallback_reason = 0;
    diag_temporal_decide(&o, 2, 1, 1, 1);
    assert(o.effective_mode == 2 && (o.flags & DIAG_TEMPORAL_CONSUMED) && !(o.flags & DIAG_TEMPORAL_SKIPPED));
    o = (diag_temporal_observation_t){.flags = DIAG_TEMPORAL_SUPPORTED | DIAG_TEMPORAL_INITIALIZED | DIAG_TEMPORAL_DISPATCHED};
    diag_temporal_decide(&o, 7, 1, 0, 0); /* SGSR scene copy is not output-consumer evidence. */
    assert(o.fallback_reason == DIAG_TEMPORAL_NO_CONSUMER && !o.effective_mode);

    assert(diag_init("run_components") == 0);
    diag_record_t records[8];
    for (uint32_t state = 0; state < 8; state++) diag_stream_gpu_component(42, 3, 99, 0, state);
    assert(diag_drain_producer(0, records, 8) == 8);
    for (uint32_t state = 0; state < 8; state++) {
        assert(records[state].type == DIAG_EV_GPU_COMPONENT && records[state].payload[0] == 42);
        assert((uint64_t)records[state].payload[1] == ((uint64_t)3 << 32 | state));
        assert(records[state].payload[2] == 99);
        assert(records[state].payload[3] == (state == 0 || state == 4 ? 0 : -1));
    }
    diag_stream_gpu_component(43, 2, 100, -1, 0);
    assert(diag_drain_producer(0, records, 8) == 1);
    assert((uint32_t)records[0].payload[1] == 3 && records[0].payload[3] == -1);
    diag_shutdown();
}

int main(void) {
    printf("=== Witch-Time GFX Diagnostics Instrumentation Test ===\n");
    test_deferred_flush();
    test_discarded_surface_gen();
    test_progress_held_image_repetition();
    test_no_first_frame_startup();
    test_render_end();
    test_temporal_observations();
    test_temporal_decisions_and_components();
    printf("=== All 7 GFX Diagnostics Tests Passed ===\n");
    return 0;
}
