#!/bin/sh
# Host behavioral tests; optional NDK cross-link checks (no dump required).
# ANDROID_NDK_HOME=/path/to/android-ndk-r28c tests/test_posix_platform.sh
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-cc}
out=$(mktemp -d "${TMPDIR:-/tmp}/wt-platform-tests.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
includes="-D_GNU_SOURCE -Iruntime/posix -Iruntime"
"$CC" $includes tests/plat_selftest.c runtime/plat_posix.c -pthread -ldl -o "$out/plat"
"$out/plat"
"$CC" $includes tests/plat_pages_test.c runtime/plat_posix.c -pthread -ldl \
    -Wl,--wrap=sysconf,--wrap=mmap,--wrap=mprotect -o "$out/pages"
"$out/pages" 4096
"$out/pages" 16384
"$CC" $includes tests/plat_fiber_test.c tests/plat_fiber_registers_aarch64.S \
    runtime/plat_posix.c -pthread -ldl -lm -o "$out/fibers"
"$out/fibers"
"$CC" $includes tests/plat_diagnostics_test.c runtime/plat_posix.c -pthread -ldl -o "$out/diagnostics"
"$out/diagnostics"
"$CC" $includes -D__ANDROID__ tests/plat_diagnostics_test.c runtime/plat_posix.c \
    runtime/posix/fiber_aarch64.S -pthread -ldl -o "$out/android-diagnostics-host"
"$out/android-diagnostics-host"
"$CC" -Wall -Wextra -Werror -c tests/freestanding_setjmp_test.c -o "$out/setjmp.o"
"$CC" $includes -c tests/plat_yield_test.c -o "$out/yield.o"
printf 'int wt_test_marker(void) { return 73; }\n' | \
    "$CC" -x c -shared -fPIC - -o "$out/libvulkan.so"
for lib in libSDL3.so libopus.so libwt_glslang.so; do cp "$out/libvulkan.so" "$out/$lib"; done
# On a native AArch64 host, __ANDROID__ selects the assembly backend too.
"$CC" $includes -D__ANDROID__ tests/plat_modules_test.c runtime/plat_posix.c \
    runtime/posix/fiber_aarch64.S -pthread -ldl -o "$out/modules"
LD_LIBRARY_PATH="$out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$out/modules"
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    ndkcc="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    "$ndkcc" $includes tests/plat_selftest.c runtime/plat_posix.c \
        runtime/posix/fiber_aarch64.S -o "$out/android-platform"
    "$ndkcc" $includes tests/plat_fiber_test.c tests/plat_fiber_registers_aarch64.S \
        runtime/plat_posix.c runtime/posix/fiber_aarch64.S -lm -o "$out/android-fibers"
    "$ndkcc" $includes tests/plat_diagnostics_test.c runtime/plat_posix.c \
        runtime/posix/fiber_aarch64.S -o "$out/android-diagnostics"
    "$ndkcc" -Wall -Wextra -Werror -c tests/freestanding_setjmp_test.c -o "$out/android-setjmp.o"
    "$ndkcc" $includes -c tests/plat_yield_test.c -o "$out/android-yield.o"
    echo "Android arm64 API 29: compile/link checks passed (not executed)"
fi
