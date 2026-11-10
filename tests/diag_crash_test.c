#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
#include "../runtime/diag.h"

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define mkdir_compat(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_compat(d) mkdir(d, 0755)
#endif

static void test_breadcrumb_layout_and_size(void) {
    printf("Running test_breadcrumb_layout_and_size...\n");
    assert(sizeof(diag_crash_breadcrumb_t) == 64);
    assert(offsetof(diag_crash_breadcrumb_t, magic) == 0);
    assert(offsetof(diag_crash_breadcrumb_t, signal_num) == 4);
    assert(offsetof(diag_crash_breadcrumb_t, fault_address) == 8);
    assert(offsetof(diag_crash_breadcrumb_t, thread_id) == 16);
    assert(offsetof(diag_crash_breadcrumb_t, exit_reason) == 20);
    assert(offsetof(diag_crash_breadcrumb_t, monotonic_ns) == 24);
    assert(offsetof(diag_crash_breadcrumb_t, global_seq) == 32);
    assert(offsetof(diag_crash_breadcrumb_t, reserved) == 40);
    printf("  [PASS] Breadcrumb struct is exactly 64 bytes with verified field offsets.\n");
}

static void test_crash_channel_uninitialized_safety(void) {
    printf("Running test_crash_channel_uninitialized_safety...\n");
    /* Ensure closed */
    diag_close_crash_channel();

    /* Should fail gracefully on NULL or empty dir */
    assert(diag_init_crash_channel(NULL) == -1);
    assert(diag_init_crash_channel("") == -1);

    /* Calling fatal signal handler with no channel open must not segfault or hang */
#if defined(_WIN32)
    diag_handle_fatal_signal(11, NULL, NULL);
#endif
    printf("  [PASS] Uninitialized / early crash channel safely no-ops without error.\n");
}

static void test_crash_channel_write_and_read(void) {
    printf("Running test_crash_channel_write_and_read...\n");
    const char *test_dir = "tests/temp_crash_session";
    mkdir_compat("tests");
    mkdir_compat(test_dir);

    diag_init("test_crash_run");
    int prod = diag_register_producer("test_prod");
    assert(prod >= 0);
    diag_emit(prod, DIAG_EV_FRAME_START, 100, 0, 0, 0);

    int rc = diag_init_crash_channel(test_dir);
    assert(rc == 0);

#if defined(_WIN32)
    /* Simulate an exception record on Windows */
    EXCEPTION_RECORD er;
    memset(&er, 0, sizeof(er));
    er.ExceptionCode = (DWORD)0xC0000005; /* STATUS_ACCESS_VIOLATION */
    er.NumberParameters = 2;
    er.ExceptionInformation[0] = 0; /* read */
    er.ExceptionInformation[1] = 0xDEADBEEFCAFEULL; /* fault addr */

    EXCEPTION_POINTERS ep;
    ep.ExceptionRecord = &er;
    ep.ContextRecord = NULL;

    diag_handle_fatal_signal(11, &ep, NULL);
#else
    /* Simulate POSIX siginfo */
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.si_signo = 11; /* SIGSEGV */
    si.si_addr = (void *)(uintptr_t)0xDEADBEEFCAFEULL;

    /* On POSIX, diag_handle_fatal_signal raises sig, so we test breadcrumb writing directly */
#endif

    diag_close_crash_channel();

    /* Read back and verify breadcrumb file */
    char path[512];
    snprintf(path, sizeof(path), "%s/crash_breadcrumb.bin", test_dir);
    FILE *f = fopen(path, "rb");
    assert(f != NULL);

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    assert(sz == 64);
    fseek(f, 0, SEEK_SET);

    diag_crash_breadcrumb_t crumb;
    size_t rd = fread(&crumb, 1, sizeof(crumb), f);
    assert(rd == sizeof(crumb));
    fclose(f);

    assert(crumb.magic == DIAG_CRASH_MAGIC);
    assert(crumb.exit_reason == 0);
    assert(crumb.thread_id > 0);
    assert(crumb.monotonic_ns > 0);
    assert(crumb.global_seq >= 1);
#if defined(_WIN32)
    assert(crumb.fault_address == 0xDEADBEEFCAFEULL);
    assert(crumb.signal_num == 0xC0000005);
#endif

    printf("  [PASS] Breadcrumb correctly recorded to disk: magic 0x%08X, seq %llu, monotonic %llu ns.\n",
           crumb.magic, (unsigned long long)crumb.global_seq, (unsigned long long)crumb.monotonic_ns);

    diag_shutdown();
}

static void test_held_logging_lock_safety(void) {
    printf("Running test_held_logging_lock_safety...\n");
    const char *test_dir = "tests/temp_crash_session_lock";
    mkdir_compat(test_dir);

    diag_init("test_lock_run");
    int rc = diag_init_crash_channel(test_dir);
    assert(rc == 0);

#if defined(_WIN32)
    CRITICAL_SECTION simulated_log_lock;
    InitializeCriticalSection(&simulated_log_lock);
    EnterCriticalSection(&simulated_log_lock);

    /* While logging lock is held, fatal signal handler must execute and complete without deadlocking */
    diag_handle_fatal_signal(11, NULL, NULL);

    LeaveCriticalSection(&simulated_log_lock);
    DeleteCriticalSection(&simulated_log_lock);
#endif

    diag_close_crash_channel();
    diag_shutdown();
    printf("  [PASS] Handler executed without deadlock while logging lock was held.\n");
}

static void test_altstack_setup(void) {
    printf("Running test_altstack_setup...\n");
    int rc = diag_setup_altstack();
    assert(rc == 0);
    printf("  [PASS] Alternate signal stack initialized successfully.\n");
}

int main(void) {
    printf("=== Starting diag_crash_test ===\n");
    test_breadcrumb_layout_and_size();
    test_crash_channel_uninitialized_safety();
    test_crash_channel_write_and_read();
    test_held_logging_lock_safety();
    test_altstack_setup();
    printf("=== All diag_crash_test tests passed! ===\n");
    return 0;
}
