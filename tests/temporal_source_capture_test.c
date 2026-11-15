#include "runtime.h"
#if !defined(_WIN32) && !defined(__declspec)
#define __declspec(x)
#endif
#include "temporal_source_guest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0700)
#endif

static unsigned observations;
static void count_record(uint64_t epoch, const WtTscRecord *r) {
    assert(epoch == 127 && r->site == 123); observations++;
}
static uint64_t bound_epoch;
static WtTscEmit bound_emit;
static int bind_capture(uint32_t version, uint32_t size, uint64_t epoch,
                        const uint32_t *gate, WtTscEmit emit) {
    if (gate) {
        assert(emit && epoch > bound_epoch);
        bound_epoch = epoch; bound_emit = emit;
    } else assert(!epoch && !emit);
    return game_temporal_source_bind(version, size, epoch, gate, emit);
}
static void path_for(char *out, size_t size, const char *root, const char *name) {
    assert(snprintf(out, size, "%s/%s", root, name) < (int)size);
    assert(make_dir(out) == 0);
}
static FILE *open_records(const char *dir) {
    char path[1200]; snprintf(path, sizeof path, "%s/temporal_source.bin", dir);
    FILE *f = fopen(path, "rb"); assert(f); return f;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    Ctx c, before;
    memset(&c, 0xa5, sizeof c); before = c;
    uint32_t gate = 0;
    assert(game_temporal_source_bind(999, sizeof(WtTscRecord), 127, &gate, count_record) == -1);
    assert(game_temporal_source_bind(WT_TSC_VERSION, 1, 127, &gate, count_record) == -1);
    assert(game_temporal_source_bind(WT_TSC_VERSION, sizeof(WtTscRecord), 127, &gate, NULL) == -1);
    assert(game_temporal_source_bind(WT_TSC_VERSION, sizeof(WtTscRecord), 127, NULL, count_record) == -1);
    assert(game_temporal_source_bind(WT_TSC_VERSION, sizeof(WtTscRecord), 0, &gate, count_record) == -1);
    assert(game_temporal_source_bind(WT_TSC_VERSION, sizeof(WtTscRecord), 127, NULL, NULL) == -1);
    assert(game_temporal_source_bind(WT_TSC_VERSION, sizeof(WtTscRecord), 127, &gate, count_record) == 0);
    wt_temporal_guest_site(&c, 123); assert(!observations);
    gate = 1; wt_temporal_guest_site(&c, 123); assert(observations == 1);
    assert(!memcmp(&c, &before, sizeof c));
    assert(game_temporal_source_bind(WT_TSC_VERSION, sizeof(WtTscRecord), 0, NULL, NULL) == 0);
    wt_temporal_guest_site(&c, 123); assert(observations == 1);
    assert(wt_tsc_start(NULL, 0, NULL) == 0);

    char dir[1100];
    path_for(dir, sizeof dir, argv[1], "missing");
    assert(wt_tsc_start(dir, 1, NULL) == -1);
    wt_tsc_frame(1, 1); assert(!wt_tsc_enabled()); assert(wt_tsc_finish() == -1);

    path_for(dir, sizeof dir, argv[1], "window");
    assert(wt_tsc_start(dir, 1, bind_capture) == 0);
    assert(wt_tsc_start(dir, 1, bind_capture) == -1);
    wt_temporal_guest_site(&c, 100);
    wt_tsc_frame(9, 0); assert(!wt_tsc_enabled());
    wt_tsc_frame(10, 1); assert(wt_tsc_enabled());
    wt_temporal_guest_site(&c, 101);
    wt_tsc_begin(42); wt_tsc_command(42);
    uint64_t pairs[10] = {80, 8, 90, 9, 100, 10, 110, 11, 120, 12};
    wt_tsc_ranges(42, 0, 8, 5, pairs);
    /* Non-selected slots must not dereference their payload at all. */
    wt_tsc_ranges(42, 0, UINT32_MAX, UINT32_MAX, NULL);
    wt_tsc_ranges(42, 0, 0, 0, NULL);
    wt_tsc_program(42, 5, 6, 3);
    wt_tsc_begin(42); wt_tsc_command(42);
    wt_tsc_ranges(42, 1, 11, 1, pairs + 6);
    uint64_t identity[16] = {5, 6, 7, 8, 11, 12, 21, 22, 31, 32, 0, 1, 10};
    wt_tsc_backend(identity);
    wt_tsc_frame(13, 0); assert(wt_tsc_enabled());
    wt_tsc_frame(14, 1); assert(!wt_tsc_enabled());
    wt_temporal_guest_site(&c, 102);
    wt_tsc_core_loss(0);
    assert(wt_tsc_finish() == 0);
    assert(!memcmp(&c, &before, sizeof c));
    FILE *f = open_records(dir); WtTscRecord r;
    unsigned n = 0, ranges = 0, begins = 0;
    while (fread(&r, sizeof r, 1, f)) {
        assert(r.version == 2 && r.size == sizeof r && r.sequence == ++n);
        if (r.kind == WT_TSC_GUEST) { assert(r.site == 101); assert(r.word[0] == c.x[0]); }
        if (r.kind == WT_TSC_BEGIN) { assert(r.word[1] == ++begins); }
        if (r.kind == WT_TSC_RANGE) {
            assert(r.word[0] == 42 && r.word[2] == 1);
            assert(r.word[4] == 9 || r.word[4] == 11);
            assert(r.word[5] == 10 * r.word[4]); ranges++;
            if (r.word[3] == 1) assert(r.word[1] == 2);
        }
        if (r.kind == WT_TSC_BACKEND) assert(!memcmp(r.word, identity, sizeof identity));
    }
    fclose(f); assert(n == 8 && ranges == 3 && begins == 2);
    /* Existing evidence must never be truncated by an accidental second start. */
    assert(wt_tsc_start(dir, 1, bind_capture) == -1);

    /* Retained old callbacks must neither record evidence nor invalidate a later run. */
    uint64_t stale_epoch = bound_epoch;
    WtTscEmit stale_emit = bound_emit;
    path_for(dir, sizeof dir, argv[1], "epoch");
    assert(wt_tsc_start(dir, 1, bind_capture) == 0);
    wt_tsc_frame(20, 1);
    WtTscRecord stale = {0};
    stale.version = WT_TSC_VERSION; stale.size = sizeof stale; stale.kind = WT_TSC_GUEST;
    stale.site = 666;
    stale_emit(stale_epoch, &stale);
    stale_emit(stale_epoch, NULL);
    stale_emit(0, NULL);
    assert(wt_tsc_enabled());
    wt_temporal_guest_site(&c, 777);
    wt_tsc_frame(24, 0); wt_tsc_core_loss(0); assert(!wt_tsc_finish());
    f = open_records(dir);
    assert(fread(&r, sizeof r, 1, f) == 1 && r.site == 777 && r.frame == 20 && r.sequence == 1);
    assert(!fread(&r, sizeof r, 1, f)); fclose(f);

    path_for(dir, sizeof dir, argv[1], "malformed_current");
    assert(wt_tsc_start(dir, 1, bind_capture) == 0);
    wt_tsc_frame(1, 1);
    bound_emit(bound_epoch, NULL);
    assert(!wt_tsc_enabled());
    wt_tsc_frame(5, 0); wt_tsc_core_loss(0); assert(wt_tsc_finish() == -1);

    path_for(dir, sizeof dir, argv[1], "budget");
    assert(wt_tsc_start(dir, 1, bind_capture) == 0);
    wt_tsc_frame(100, 1);
    for (unsigned i = 0; i <= WT_TSC_RECORDS; i++) wt_temporal_guest_site(&c, i);
    assert(!wt_tsc_enabled()); assert(wt_tsc_finish() == -1);
    f = open_records(dir); assert(!fseek(f, 0, SEEK_END));
    assert((unsigned long)ftell(f) == WT_TSC_RECORDS * sizeof r); fclose(f);

    path_for(dir, sizeof dir, argv[1], "unknown_generation");
    assert(wt_tsc_start(dir, 1, bind_capture) == 0);
    wt_tsc_frame(1, 1); wt_tsc_command(999);
    wt_tsc_ranges(999, 0, 9, 1, pairs); wt_temporal_guest_site(&c, 1);
    assert(wt_tsc_enabled()); assert(wt_tsc_finish() == -1);
    const char code[] = "abc";
    assert(wt_tsc_code_hash(0, code, 3) == 0xe71fa2190541574bULL);
    assert(wt_tsc_code_hash(0, code, 3) != wt_tsc_code_hash(1, code, 3));
    assert(!wt_tsc_code_hash(0, NULL, 0));
    path_for(dir, sizeof dir, argv[1], "no_scene");
    assert(wt_tsc_start(dir, 1, bind_capture) == 0);
    wt_tsc_frame(1, 0); assert(!wt_tsc_enabled()); assert(wt_tsc_finish() == -1);
    path_for(dir, sizeof dir, argv[1], "buffer_budget");
    assert(wt_tsc_start(dir, 1, bind_capture) == 0);
    wt_tsc_frame(1, 1);
    for (unsigned i = 0; i < 33; i++) wt_tsc_begin(i);
    assert(!wt_tsc_enabled()); assert(wt_tsc_finish() == -1);
    path_for(dir, sizeof dir, argv[1], "core_loss");
    assert(wt_tsc_start(dir, 1, bind_capture) == 0);
    wt_tsc_frame(1, 1); wt_temporal_guest_site(&c, 1); wt_tsc_frame(5, 1);
    wt_tsc_core_loss(1); assert(wt_tsc_finish() == -1);
    puts("temporal source capture contracts passed (mocked guest/frontend/backend)");
    return 0;
}
