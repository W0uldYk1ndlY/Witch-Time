/* Host-page regression tests. Run with tests/test_posix_platform.sh.
 * The linker wrappers model a 16 KiB kernel's alignment requirements without
 * mocking the platform implementation; actual mmap/mprotect/file I/O still run. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <windows.h>

static long test_page_size = 4096;
long __real_sysconf(int name);
long __wrap_sysconf(int name) {
    return name == _SC_PAGESIZE ? test_page_size : __real_sysconf(name);
}
void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off) {
    if ((uint64_t)off % test_page_size || (p && (uintptr_t)p % test_page_size)) {
        errno = EINVAL; return MAP_FAILED;
    }
    return __real_mmap(p, n, prot, flags, fd, off);
}
int __real_mprotect(void *, size_t, int);
int __wrap_mprotect(void *p, size_t n, int prot) {
    if ((uintptr_t)p % test_page_size) { errno = EINVAL; return -1; }
    return __real_mprotect(p, n, prot);
}

int main(int argc, char **argv) {
    if (argc > 1) test_page_size = strtol(argv[1], NULL, 10);
    assert(test_page_size == 4096 || test_page_size == 16384);
    size_t ps = (size_t)test_page_size;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    assert(si.dwPageSize == ps);
    uint8_t *r = VirtualAlloc((void *)(uintptr_t)0x7200000000ull, ps * 3,
                             MEM_RESERVE, PAGE_NOACCESS);
    assert(r);
    /* Commit/protect must align the original address, not only its size. */
    assert(VirtualAlloc(r + 17, 2, MEM_COMMIT, PAGE_READWRITE) == r + 17);
    MEMORY_BASIC_INFORMATION mi;
    assert(VirtualQuery(r + ps - 1, &mi, sizeof mi) && mi.State == MEM_COMMIT);
    r[ps - 1] = 42;
    DWORD old;
    if (ps > 4096) {
        /* A subpage protection request cannot revoke a neighbor's access. */
        assert(!VirtualProtect(r + ps / 2 + 1, 2, PAGE_READONLY, &old));
        assert(VirtualQuery(r, &mi, sizeof mi) && mi.Protect == PAGE_READWRITE);
        assert(VirtualProtect(r, ps, PAGE_READONLY, &old));
    } else {
        assert(VirtualProtect(r + ps / 2 + 1, 2, PAGE_READONLY, &old));
    }
    assert(old == PAGE_READWRITE);
    assert(VirtualQuery(r, &mi, sizeof mi) && mi.Protect == PAGE_READONLY);
    assert(VirtualProtect(r, ps, PAGE_READWRITE, NULL));
    assert(!VirtualProtect(r, 0, PAGE_NOACCESS, NULL));
    assert(!VirtualAlloc(NULL, SIZE_MAX, MEM_RESERVE, PAGE_NOACCESS));
    assert(!VirtualProtect((void *)(UINTPTR_MAX - 3), 8, PAGE_READONLY, NULL));
    if (ps > 4096) {
        /* Never discard a neighboring live guest page on a 16 KiB host. */
        assert(!VirtualFree(r + 4096, 4096, MEM_DECOMMIT));
        assert(r[ps - 1] == 42);
    }
    assert(VirtualFree(r, ps, MEM_DECOMMIT));
    assert(VirtualAlloc(r, ps, MEM_COMMIT, PAGE_READWRITE) == r && r[ps - 1] == 0);
    assert(VirtualFree(r, 0, MEM_RELEASE));

    /* Write watch: kernel tracking if available on 4 KiB host, otherwise unavailable query error. */
    r = VirtualAlloc((void *)(uintptr_t)0x7200000000ull, ps * 2,
                     MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    assert(r);
    /* Soft write-watch production helper assertions for budget and granule eligibility */
    assert(plat_soft_ww_budget_limit() == 16 * 1024 * 1024);
    assert(plat_soft_ww_is_range_eligible((uintptr_t)r, ps * 2, MEM_WRITE_WATCH, ps) == 1);
    assert(plat_soft_ww_is_range_eligible((uintptr_t)r + 1, ps * 2, MEM_WRITE_WATCH, ps) == 0);
    assert(plat_soft_ww_is_range_eligible((uintptr_t)r, ps * 2, 0, ps) == 0);
    assert(plat_soft_ww_is_range_eligible((uintptr_t)r, ps * 2, MEM_WRITE_WATCH, 65536) == 0);
    r[4096 + 7] = 1;
    PVOID pages[8]; ULONG_PTR count = 8; ULONG gran = 0;
    UINT ww_res = GetWriteWatch(WRITE_WATCH_FLAG_RESET, r, ps * 2, pages, &count, &gran);
    if (ps > 4096) {
        /* On a 16 KiB host, 4 KiB kernel write-watch is unavailable; runtime returns error. */
        assert(ww_res != 0 && "Modeled 16 KiB host must report unavailable write-watch query");
        printf("write watch (pages test, ps=%zu): unavailable (expected on 16 KiB host)\n", ps);
    } else if (ww_res == 0) {
        /* 4 KiB host with kernel tracking available: verify exact dirty page */
        assert(gran == 4096);
        assert(count == 1 && pages[0] == r + 4096);
        printf("write watch (pages test, ps=4096): kernel tracked (exact 4 KiB page reported)\n");
    } else {
        /* 4 KiB host without kernel tracking permission/support: returns error */
        printf("write watch (pages test, ps=4096): unavailable (kernel tracking unsupported/disabled)\n");
    }
    assert(VirtualFree(r, 0, MEM_RELEASE));

    /* Soft write watch query and reset verification under WT_WW_SOFT=1 */
    setenv("WT_WW_SOFT", "1", 1);
    uint8_t *r_soft = VirtualAlloc(NULL, ps * 2, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    assert(r_soft);
    int backend = plat_ww_backend_of((uintptr_t)r_soft, ps * 2);
    if (backend == 3) {
        uint64_t gen = plat_ww_generation_of((uintptr_t)r_soft);
        assert(gen > 0);
        assert(plat_ww_validate_generation((uintptr_t)r_soft, gen));
        assert(!plat_ww_validate_generation((uintptr_t)r_soft, gen - 1));
        assert(plat_soft_ww_mark_dirty((uintptr_t)r_soft, 4096));
        assert(plat_soft_ww_is_page_dirty((uintptr_t)r_soft));
        PVOID soft_pages[8]; ULONG_PTR soft_count = 8; ULONG soft_gran = 0;
        assert(GetWriteWatch(WRITE_WATCH_FLAG_RESET, r_soft, ps * 2, soft_pages, &soft_count, &soft_gran) == 0);
        assert(soft_gran == 4096);
        assert(soft_count >= 1 && soft_pages[0] == r_soft);
        assert(!plat_soft_ww_is_page_dirty((uintptr_t)r_soft));
    }
    assert(VirtualFree(r_soft, 0, MEM_RELEASE));
    unsetenv("WT_WW_SOFT");

    /* Non-host-aligned offsets must remain readable, and EOF is rejected. */
    char path[] = "/tmp/witchtime-page-test-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    assert(ftruncate(fd, (off_t)(ps * 2)) == 0);
    assert(pwrite(fd, "pages", 5, 4097) == 5); close(fd);
    HANDLE file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    HANDLE map = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL); assert(map);
    const char *view = MapViewOfFile(map, FILE_MAP_READ, 0, 4097, 5);
    assert(view && !memcmp(view, "pages", 5));
    assert(UnmapViewOfFile(view));
    assert(!MapViewOfFile(map, FILE_MAP_READ, 0, (DWORD)(ps * 2 + 1), 0));
    CloseHandle(map); CloseHandle(file); unlink(path);
    printf("POSIX host page tests: %ld-byte pages passed\n", test_page_size);
    return 0;
}
