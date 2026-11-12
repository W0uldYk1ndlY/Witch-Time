#include "../runtime/diag_replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_WIN32)
#include <windows.h>
static void remove_file(const char *path) {
    DeleteFileA(path);
}
#else
#include <unistd.h>
static void remove_file(const char *path) {
    unlink(path);
}
#endif

/* 1. Test missing and empty files */
static void test_missing_and_empty_files(void) {
    printf("[1/8] Testing missing and empty files...\n");
    diag_replay_session_t sess;

    /* Missing file */
    diag_replay_error_t err = diag_replay_load("tests/non_existent_replay.bin", 0, 0, 0, &sess);
    assert(err == DIAG_REPLAY_ERR_FILE_NOT_FOUND);
    assert(sess.has_failed == true);
    assert(sess.is_loaded == false);

    /* Empty file */
    const char *empty_path = "tests/test_empty.bin";
    FILE *f = fopen(empty_path, "wb");
    assert(f != NULL);
    fclose(f);

    err = diag_replay_load(empty_path, 0, 0, 0, &sess);
    assert(err == DIAG_REPLAY_ERR_EMPTY_FILE);
    assert(sess.has_failed == true);
    assert(sess.is_loaded == false);
    remove_file(empty_path);

    printf("      Missing and empty files rejected (passed)\n");
}

/* 2. Test bad headers and version mismatch */
static void test_bad_headers_and_version(void) {
    printf("[2/8] Testing bad headers and version checks...\n");
    const char *bad_hdr_path = "tests/test_bad_hdr.bin";
    diag_replay_session_t sess;

    /* Write corrupted magic */
    diag_replay_header_t hdr = {0};
    hdr.magic = 0x11223344; /* Bad magic */
    hdr.version_major = DIAG_REPLAY_VERSION_MAJOR;
    hdr.version_minor = DIAG_REPLAY_VERSION_MINOR;
    hdr.record_count = 0;

    FILE *f = fopen(bad_hdr_path, "wb");
    assert(f != NULL);
    fwrite(&hdr, 1, sizeof(hdr), f);
    diag_replay_footer_t footer = { DIAG_REPLAY_EOF_MARKER, 0, 0 };
    fwrite(&footer, 1, sizeof(footer), f);
    fclose(f);

    diag_replay_error_t err = diag_replay_load(bad_hdr_path, 0, 0, 0, &sess);
    assert(err == DIAG_REPLAY_ERR_BAD_HEADER);
    assert(sess.has_failed == true);

    /* Write unsupported major version */
    hdr.magic = DIAG_REPLAY_MAGIC;
    hdr.version_major = 99; /* Unsupported major version */
    f = fopen(bad_hdr_path, "wb");
    fwrite(&hdr, 1, sizeof(hdr), f);
    fwrite(&footer, 1, sizeof(footer), f);
    fclose(f);

    err = diag_replay_load(bad_hdr_path, 0, 0, 0, &sess);
    assert(err == DIAG_REPLAY_ERR_UNSUPPORTED_VERSION);
    assert(sess.has_failed == true);

    remove_file(bad_hdr_path);
    printf("      Bad headers and unsupported versions rejected (passed)\n");
}

/* 3. Test record order and corruption */
static void test_record_order_and_corruption(void) {
    printf("[3/8] Testing record ordering and sequence corruption...\n");
    const char *path = "tests/test_order.bin";
    diag_replay_session_t sess;

    diag_replay_header_t hdr = {0};
    hdr.magic = DIAG_REPLAY_MAGIC;
    hdr.version_major = DIAG_REPLAY_VERSION_MAJOR;
    hdr.version_minor = DIAG_REPLAY_VERSION_MINOR;
    hdr.source_policy = DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1;
    hdr.mapping_policy = DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A;
    hdr.confirm_policy = DIAG_REPLAY_CONFIRM_A;
    hdr.record_count = 2;

    /* Non-monotonic polls: poll 100 then poll 90 */
    diag_replay_record_t recs[2] = {0};
    recs[0].poll = 100;
    recs[0].attributes = DIAG_REPLAY_ATTR_CONNECTED;
    recs[1].poll = 90; /* Order violation */
    recs[1].attributes = DIAG_REPLAY_ATTR_CONNECTED;

    assert(diag_replay_write(path, &hdr, recs, 2) == DIAG_REPLAY_OK);

    diag_replay_error_t err = diag_replay_load(path,
                                              DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1,
                                              DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A,
                                              DIAG_REPLAY_CONFIRM_A,
                                              &sess);
    assert(err == DIAG_REPLAY_ERR_ORDER_VIOLATION);
    assert(sess.has_failed == true);
    assert(sess.is_loaded == false);

    remove_file(path);
    printf("      Order violation rejected strictly (passed)\n");
}

/* 4. Test policy restrictions: source, mapping, confirm */
static void test_policy_restrictions(void) {
    printf("[4/8] Testing policy restrictions (source, mapping, confirm)...\n");
    const char *path = "tests/test_policies.bin";
    diag_replay_session_t sess;

    diag_replay_header_t hdr = {0};
    hdr.magic = DIAG_REPLAY_MAGIC;
    hdr.version_major = DIAG_REPLAY_VERSION_MAJOR;
    hdr.version_minor = DIAG_REPLAY_VERSION_MINOR;
    hdr.source_policy = 99; /* Unknown source */
    hdr.mapping_policy = DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A;
    hdr.confirm_policy = DIAG_REPLAY_CONFIRM_A;
    hdr.record_count = 0;

    assert(diag_replay_write(path, &hdr, NULL, 0) == DIAG_REPLAY_OK);

    /* Required source is SYNTHETIC_FULLKEY_P1 */
    diag_replay_error_t err = diag_replay_load(path,
                                              DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1,
                                              DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A,
                                              DIAG_REPLAY_CONFIRM_A,
                                              &sess);
    assert(err == DIAG_REPLAY_ERR_UNSUPPORTED_SOURCE_POLICY);

    /* Incompatible confirm policy */
    hdr.source_policy = DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1;
    hdr.confirm_policy = DIAG_REPLAY_CONFIRM_B; /* B confirm instead of A */
    assert(diag_replay_write(path, &hdr, NULL, 0) == DIAG_REPLAY_OK);

    err = diag_replay_load(path,
                          DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1,
                          DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A,
                          DIAG_REPLAY_CONFIRM_A, /* Requires A */
                          &sess);
    assert(err == DIAG_REPLAY_ERR_INCOMPATIBLE_CONFIRM);

    remove_file(path);
    printf("      Source and confirm policy enforcement (passed)\n");
}

/* 5. Test invalid buttons and attributes */
static void test_invalid_buttons_and_attributes(void) {
    printf("[5/8] Testing out-of-range button bits and invalid attributes...\n");
    const char *path = "tests/test_range.bin";
    diag_replay_session_t sess;

    diag_replay_header_t hdr = {0};
    hdr.magic = DIAG_REPLAY_MAGIC;
    hdr.version_major = DIAG_REPLAY_VERSION_MAJOR;
    hdr.source_policy = DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1;
    hdr.mapping_policy = DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A;
    hdr.confirm_policy = DIAG_REPLAY_CONFIRM_A;
    hdr.record_count = 1;

    diag_replay_record_t rec = {0};
    rec.poll = 1;
    rec.buttons = 0x8000000000000000ULL; /* Invalid button bit */
    rec.attributes = DIAG_REPLAY_ATTR_CONNECTED;

    assert(diag_replay_write(path, &hdr, &rec, 1) == DIAG_REPLAY_OK);

    diag_replay_error_t err = diag_replay_load(path, 0, 0, 0, &sess);
    assert(err == DIAG_REPLAY_ERR_INVALID_BUTTONS);
    assert(sess.has_failed == true);

    /* Invalid attributes */
    rec.buttons = DIAG_REPLAY_BTN_A;
    rec.attributes = 0x8000; /* Invalid attribute bit */
    assert(diag_replay_write(path, &hdr, &rec, 1) == DIAG_REPLAY_OK);

    err = diag_replay_load(path, 0, 0, 0, &sess);
    assert(err == DIAG_REPLAY_ERR_INVALID_ATTRIBUTES);
    assert(sess.has_failed == true);

    remove_file(path);
    printf("      Invalid buttons and attributes rejected (passed)\n");
}

/* 6. Test missing EOF marker and size ceilings */
static void test_missing_eof_and_size_ceilings(void) {
    printf("[6/8] Testing missing EOF marker and size mismatch...\n");
    const char *path = "tests/test_eof.bin";
    diag_replay_session_t sess;

    diag_replay_header_t hdr = {0};
    hdr.magic = DIAG_REPLAY_MAGIC;
    hdr.version_major = DIAG_REPLAY_VERSION_MAJOR;
    hdr.source_policy = DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1;
    hdr.mapping_policy = DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A;
    hdr.confirm_policy = DIAG_REPLAY_CONFIRM_A;
    hdr.record_count = 1;

    diag_replay_record_t rec = {0};
    rec.poll = 1;
    rec.buttons = DIAG_REPLAY_BTN_A;
    rec.attributes = DIAG_REPLAY_ATTR_CONNECTED;

    /* Write file with corrupted EOF marker */
    FILE *f = fopen(path, "wb");
    fwrite(&hdr, 1, sizeof(hdr), f);
    fwrite(&rec, 1, sizeof(rec), f);
    diag_replay_footer_t bad_footer = { 0x00000000, 1, 0 }; /* Corrupt EOF */
    fwrite(&bad_footer, 1, sizeof(bad_footer), f);
    fclose(f);

    diag_replay_error_t err = diag_replay_load(path, 0, 0, 0, &sess);
    assert(err == DIAG_REPLAY_ERR_MISSING_END_MARKER);
    assert(sess.has_failed == true);

    /* Truncated file size (has header and record, but missing footer) */
    f = fopen(path, "wb");
    fwrite(&hdr, 1, sizeof(hdr), f);
    fwrite(&rec, 1, sizeof(rec), f);
    fclose(f);

    err = diag_replay_load(path, 0, 0, 0, &sess);
    assert(err == DIAG_REPLAY_ERR_SIZE_MISMATCH);
    assert(sess.has_failed == true);

    remove_file(path);
    printf("      EOF marker and size checks enforced (passed)\n");
}

/* 7. Test no live/legacy fallback on failure */
static void test_no_fallback_on_failure(void) {
    printf("[7/8] Testing zero live/legacy fallback on failure...\n");
    diag_replay_session_t sess;
    memset(&sess, 0, sizeof(sess));
    sess.has_failed = true; /* Failed session */

    uint64_t buttons = 0xFF;
    int16_t lx = 100, ly = 200, rx = 300, ry = 400;
    uint32_t attr = 1;

    /* When polled, failed session MUST zero out all fields (neutral release) and return false */
    bool polled = diag_replay_poll(&sess, 0, 0, 0, &buttons, &lx, &ly, &rx, &ry, &attr);
    assert(!polled);
    assert(buttons == 0);
    assert(lx == 0 && ly == 0);
    assert(rx == 0 && ry == 0);
    assert(attr == 0);

    printf("      Failed session enforces neutral release without live fallback (passed)\n");
}

/* 8. Test valid replay roundtrip and polling */
static void test_valid_roundtrip_and_polling(void) {
    printf("[8/8] Testing valid replay creation, loading, and anchor polling...\n");
    const char *path = "tests/test_valid.bin";
    diag_replay_session_t sess;

    diag_replay_header_t hdr = {0};
    hdr.magic = DIAG_REPLAY_MAGIC;
    hdr.version_major = DIAG_REPLAY_VERSION_MAJOR;
    hdr.version_minor = DIAG_REPLAY_VERSION_MINOR;
    hdr.source_policy = DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1;
    hdr.mapping_policy = DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A;
    hdr.confirm_policy = DIAG_REPLAY_CONFIRM_A;
    hdr.record_count = 3;
    hdr.total_polls = 100;

    diag_replay_record_t recs[3] = {0};
    /* Record 0: at anchor 0, poll 5 */
    recs[0].poll = 5;
    recs[0].anchor = 0;
    recs[0].anchor_poll = 5;
    recs[0].buttons = DIAG_REPLAY_BTN_A;
    recs[0].lx = 16000;
    recs[0].ly = 0;
    recs[0].attributes = DIAG_REPLAY_ATTR_CONNECTED;

    /* Record 1: at anchor 0, poll 10 */
    recs[1].poll = 10;
    recs[1].anchor = 0;
    recs[1].anchor_poll = 10;
    recs[1].buttons = DIAG_REPLAY_BTN_B | DIAG_REPLAY_BTN_ZL;
    recs[1].lx = 0;
    recs[1].ly = -16000;
    recs[1].attributes = DIAG_REPLAY_ATTR_CONNECTED;

    /* Record 2: at anchor 1, poll 2 */
    recs[2].poll = 20;
    recs[2].anchor = 1;
    recs[2].anchor_poll = 2;
    recs[2].buttons = DIAG_REPLAY_BTN_PLUS;
    recs[2].lx = 0;
    recs[2].ly = 0;
    recs[2].attributes = DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED;

    assert(diag_replay_write(path, &hdr, recs, 3) == DIAG_REPLAY_OK);

    diag_replay_error_t err = diag_replay_load(path,
                                              DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1,
                                              DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A,
                                              DIAG_REPLAY_CONFIRM_A,
                                              &sess);
    assert(err == DIAG_REPLAY_OK);
    assert(sess.is_loaded == true);
    assert(sess.has_failed == false);
    assert(sess.record_count == 3);

    /* Poll before first event (anchor 0, poll 2): should be neutral state */
    uint64_t b = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint32_t attr = 0;
    assert(diag_replay_poll(&sess, 2, 0, 2, &b, &lx, &ly, &rx, &ry, &attr));
    assert(b == 0 && lx == 0);

    /* Poll at anchor 0, poll 6: should deliver record 0 */
    assert(diag_replay_poll(&sess, 6, 0, 6, &b, &lx, &ly, &rx, &ry, &attr));
    assert(b == DIAG_REPLAY_BTN_A);
    assert(lx == 16000 && ly == 0);

    /* Poll at anchor 0, poll 12: should deliver record 1 */
    assert(diag_replay_poll(&sess, 12, 0, 12, &b, &lx, &ly, &rx, &ry, &attr));
    assert(b == (DIAG_REPLAY_BTN_B | DIAG_REPLAY_BTN_ZL));
    assert(lx == 0 && ly == -16000);

    /* Poll at anchor 1, poll 3: should deliver record 2 */
    assert(diag_replay_poll(&sess, 20, 1, 3, &b, &lx, &ly, &rx, &ry, &attr));
    assert(b == DIAG_REPLAY_BTN_PLUS);
    assert(attr == (DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED));
    assert(!sess.is_eof);
    assert(diag_replay_poll(&sess, 99, 1, 82, &b, &lx, &ly, &rx, &ry, &attr));
    assert(b == DIAG_REPLAY_BTN_PLUS && !sess.is_eof);
    assert(diag_replay_poll(&sess, 100, 1, 83, &b, &lx, &ly, &rx, &ry, &attr));
    assert(b == 0 && lx == 0 && ly == 0 && sess.is_eof);

    diag_replay_free(&sess);
    remove_file(path);
    printf("      Valid replay loaded and polled cleanly (passed)\n");
}

static void test_global_poll_duration(void) {
    const char *path = "tests/test_global_poll.bin";
    diag_replay_header_t h = {0};
    h.total_polls = 400;
    const uint64_t polls[] = {0, 60, 75, 180, 195, 300};
    diag_replay_record_t r[6] = {0};
    for (int i = 0; i < 6; i++) {
        r[i].poll = polls[i];
        r[i].anchor = -1;
        r[i].buttons = (i & 1) ? DIAG_REPLAY_BTN_A : 0;
        r[i].attributes = DIAG_REPLAY_ATTR_CONNECTED;
    }
    diag_replay_session_t s;
    assert(diag_replay_write(path, &h, r, 6) == DIAG_REPLAY_OK);
    assert(diag_replay_load(path, 0, 0, 0, &s) == DIAG_REPLAY_OK);
    assert(s.next_global_poll == 0);
    for (uint64_t p = 0; p <= 400; p++) {
        uint64_t b = ~0ULL;
        uint32_t attr = 0;
        /* Anchor transitions reset anchor-relative time, never the global clock. */
        assert(diag_replay_poll(&s, p, (int)(p / 100), (uint32_t)(p % 100),
                                &b, NULL, NULL, NULL, NULL, &attr));
        assert(s.is_eof == (p == 400));
        uint64_t expected = ((p >= 60 && p < 75) || (p >= 180 && p < 195) ||
                             (p >= 300 && p < 400)) ? DIAG_REPLAY_BTN_A : 0;
        assert(b == expected);
        assert(attr == DIAG_REPLAY_ATTR_CONNECTED);
    }
    diag_replay_free(&s);

    /* A 64-bit deadline and exclusive endpoint must not wrap at 32 bits or UINT64_MAX. */
    h.total_polls = UINT64_MAX;
    r[0].poll = UINT64_MAX - 1;
    r[0].buttons = DIAG_REPLAY_BTN_A;
    assert(diag_replay_write(path, &h, r, 1) == DIAG_REPLAY_OK);
    assert(diag_replay_load(path, 0, 0, 0, &s) == DIAG_REPLAY_OK);
    uint64_t b;
    assert(diag_replay_poll(&s, UINT64_MAX - 2, -1, 0, &b, NULL, NULL, NULL, NULL, NULL));
    assert(b == 0 && !s.is_eof);
    assert(diag_replay_poll(&s, UINT64_MAX - 1, -1, 0, &b, NULL, NULL, NULL, NULL, NULL));
    assert(b == DIAG_REPLAY_BTN_A && !s.is_eof);
    assert(diag_replay_poll(&s, UINT64_MAX, -1, 0, &b, NULL, NULL, NULL, NULL, NULL));
    assert(b == 0 && s.is_eof);
    diag_replay_free(&s);

    /* Unanchored records at/after the exclusive endpoint cannot ever be delivered. */
    h.total_polls = r[0].poll;
    assert(diag_replay_write(path, &h, r, 1) == DIAG_REPLAY_OK);
    assert(diag_replay_load(path, 0, 0, 0, &s) == DIAG_REPLAY_ERR_CORRUPT_RECORD);
    assert(s.has_failed && !s.is_loaded);
    h.total_polls = 0;
    r[0].poll = 0;
    assert(diag_replay_write(path, &h, r, 1) == DIAG_REPLAY_OK);
    assert(diag_replay_load(path, 0, 0, 0, &s) == DIAG_REPLAY_ERR_CORRUPT_RECORD);
    /* Zero-duration anchored streams complete without delivering any state. */
    r[0].anchor = 0;
    assert(diag_replay_write(path, &h, r, 1) == DIAG_REPLAY_OK);
    assert(diag_replay_load(path, 0, 0, 0, &s) == DIAG_REPLAY_OK);
    assert(diag_replay_poll(&s, 0, 0, 0, &b, NULL, NULL, NULL, NULL, NULL));
    assert(s.is_eof && b == 0);
    diag_replay_free(&s);

    /* Empty files retain the false/neutral sample contract, but duration still completes. */
    for (uint64_t duration = 0; duration <= 1; duration++) {
        h.total_polls = duration;
        assert(diag_replay_write(path, &h, NULL, 0) == DIAG_REPLAY_OK);
        assert(diag_replay_load(path, 0, 0, 0, &s) == DIAG_REPLAY_OK);
        b = ~0ULL;
        assert(!diag_replay_poll(&s, 0, -1, 0, &b, NULL, NULL, NULL, NULL, NULL));
        assert(b == 0 && s.is_eof == (duration == 0));
        assert(!diag_replay_poll(&s, duration, -1, 0, &b, NULL, NULL, NULL, NULL, NULL));
        assert(s.is_eof);
        diag_replay_free(&s);
    }
    remove_file(path);
    puts("Global poll boundaries, held input, duration, overflow and empty replay passed");
}

int main(void) {
    printf("=== Witch-Time Validated Replay Parser Unit Test ===\n");
    test_missing_and_empty_files();
    test_bad_headers_and_version();
    test_record_order_and_corruption();
    test_policy_restrictions();
    test_invalid_buttons_and_attributes();
    test_missing_eof_and_size_ceilings();
    test_no_fallback_on_failure();
    test_valid_roundtrip_and_polling();
    test_global_poll_duration();
    printf("=== All 9 Validated Replay Tests Passed ===\n");
    return 0;
}
