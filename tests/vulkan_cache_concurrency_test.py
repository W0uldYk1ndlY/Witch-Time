"""Real POSIX paths, overlapping workers, and interruption during atomic writes."""
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = Path(os.environ.get("WT_VK_M4_TEST_SOURCE", ROOT / "runtime/nvn_vk_m4.c")).read_text(encoding="utf-8")
cache_path = next(line for line in source.splitlines() if line.startswith("static const char *vkc_file("))
cache_include = source[source.index('#include "nvn_vk_cache.h"'):source.index('typedef struct { uint8_t unit, dim, shadow;') ] if '#include "nvn_vk_cache.h"' in source else '#include "nvn_vk_cache.h"\n'
# Include the production path hook if present immediately before the helper.
if '#define VKC_NORMALIZE_PATH' in source:
    cache_include = source[source.rfind('#ifndef _WIN32', 0, source.index('#define VKC_NORMALIZE_PATH')):source.index('typedef struct { uint8_t unit, dim, shadow;')]

prelude = r'''
#include "plat.h"
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
static int stop_write, ready_fd;
static size_t interrupted_fwrite(const void *data, size_t size, size_t n, FILE *fp) {
    if (stop_write) {
        fwrite(data, size, n / 2, fp); fflush(fp);
        assert(write(ready_fd, "x", 1) == 1);
        for (;;) pause();
    }
    return fwrite(data, size, n, fp);
}
#define fwrite interrupted_fwrite
'''
test = r'''
#undef fwrite
static char directory[200];
static const char *vkc_dir(void) { return directory; }
PATH_FUNCTION
static pthread_barrier_t gate;
static int mismatches;
static void *worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    char name[64], expected[320];
    snprintf(name, sizeof name, "worker_%u.bin", id);
    snprintf(expected, sizeof expected, "%s/%s", directory, name);
    const char *actual = vkc_file(name);
    pthread_barrier_wait(&gate);
    if (strcmp(actual, expected)) __atomic_add_fetch(&mismatches, 1, __ATOMIC_RELAXED);
    pthread_barrier_wait(&gate);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 3); snprintf(directory, sizeof directory, "%s", argv[2]);
    if (!strcmp(argv[1], "paths")) {
        char actual[320], supplied[320];
        snprintf(actual, sizeof actual, "%s/Mixed", directory); assert(!mkdir(actual, 0700));
        snprintf(supplied, sizeof supplied, "%s\\mIxEd\\cache.bin", directory);
        assert(vkc_write_file(supplied, "complete", 8));
        snprintf(actual, sizeof actual, "%s/Mixed/cache.bin", directory);
        assert(!access(actual, F_OK));
        size_t n; char *bytes = vkc_read_file(supplied, 32, &n);
        assert(bytes && n == 8 && !memcmp(bytes, "complete", 8)); free(bytes);
    } else if (!strcmp(argv[1], "workers")) {
        pthread_t thread[12];
        assert(!pthread_barrier_init(&gate, NULL, 12));
        for (uintptr_t i = 0; i < 12; ++i) assert(!pthread_create(&thread[i], NULL, worker, (void *)i));
        for (unsigned i = 0; i < 12; ++i) assert(!pthread_join(thread[i], NULL));
        assert(mismatches == 0);
        pthread_barrier_destroy(&gate);
    } else if (!strcmp(argv[1], "interrupted")) {
        char path[320]; snprintf(path, sizeof path, "%s/cache.bin", directory);
        assert(vkc_write_file(path, "previous", 8));
        int pipefd[2]; assert(!pipe(pipefd));
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            close(pipefd[0]); stop_write = 1; ready_fd = pipefd[1];
            vkc_write_file(path, "replacement", 11); _exit(1);
        }
        close(pipefd[1]); char byte;
        assert(read(pipefd[0], &byte, 1) == 1); close(pipefd[0]);
        assert(!kill(child, SIGKILL)); int status; assert(waitpid(child, &status, 0) == child);
        size_t n; char *bytes = vkc_read_file(path, 32, &n);
        assert(bytes && n == 8 && !memcmp(bytes, "previous", 8)); free(bytes);
        assert(vkc_write_file(path, "later", 5)); /* an orphan temporary is harmless */
    } else assert(!"unknown case");
    return 0;
}
'''.replace("PATH_FUNCTION", cache_path)

with tempfile.TemporaryDirectory(prefix="cache-concurrency-") as directory:
    temp = Path(directory)
    unit = temp / "test.c"
    unit.write_text(prelude + cache_include + test, encoding="utf-8")
    exe = temp / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-ffunction-sections", "-fdata-sections", "-I", str(ROOT / "runtime"), str(unit), str(ROOT / "runtime/plat_posix.c"), "-pthread", "-ldl", "-Wl,--gc-sections", "-o", str(exe)], check=True)
    failures = []
    for case in ["paths", "workers", "interrupted"]:
        run = temp / case; run.mkdir()
        result = subprocess.run([str(exe), case, str(run)], capture_output=True, text=True)
        print(f"{case}: {'PASS' if result.returncode == 0 else 'FAIL'}")
        if result.returncode:
            failures.append(case); print(result.stderr.strip())
    if failures:
        raise SystemExit("Cache filesystem failures: " + ", ".join(failures))
print("Windows-style POSIX paths, worker path lifetime, and interrupted writes passed")
