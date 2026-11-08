/* Behavior test: provide unversioned Android sonames through LD_LIBRARY_PATH. */
#include <assert.h>
#include <dlfcn.h>
#include <string.h>
#include <windows.h>
int main(void) {
    static const char *names[] = {"vulkan-1.dll", "SDL3.dll", "libopus-0.dll", "wt_glslang.dll"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        HMODULE m = LoadLibraryA(names[i]);
        assert(m && "Android unversioned library must load");
        int (*marker)(void) = (int (*)(void))GetProcAddress(m, "wt_test_marker");
        assert(marker && marker() == 73);
        dlclose(m);
    }
    assert(!LoadLibraryA("absent-windows-system-library.dll"));
    return 0;
}
