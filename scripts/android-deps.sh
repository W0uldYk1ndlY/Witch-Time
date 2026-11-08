#!/usr/bin/env bash
# Build redistributable arm64-v8a dependencies, never any game-derived files.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/android-deps.sh [--verify | --help]

Build SDL3, LGPL FFmpeg, static PIC glslang, and Vulkan headers for Android ARM64.
Requires bash, curl, tar, make, cmake, ninja, a C compiler, and Android NDK r28+.
--verify checks an existing installation without downloading or rebuilding it.

Environment:
  ANDROID_NDK_HOME  Installed Android NDK (ANDROID_NDK_ROOT is also accepted).
  ANDROID_API       Minimum API level, default 28.
  ANDROID_DEPS_DIR  Install prefix, default <checkout>/deps/android-arm64.
  ANDROID_DEPS_WORK Download/source/build cache, default <checkout>/deps/android-build.
  JOBS             Parallel compiler jobs, default 4.
  CMAKE            CMake executable, default cmake.

Builds use pinned upstream archives with SHA-256 checks. FFmpeg is shared LGPL:
no GPL/nonfree codecs, external dependencies, network, encoders, or muxers.
Generated android-env.sh contains include/link flags for wtbuild. Source archives,
licenses, and configuration records remain available for redistribution review.
EOF
}

verify_only=false
case "${1:-}" in
    --help|-h) usage; exit 0 ;;
    --verify) verify_only=true ;;
    '') ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
esac
if (( $# > 1 )); then echo "Unknown argument: $2" >&2; exit 2; fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z "$ndk" || ! -f "$ndk/build/cmake/android.toolchain.cmake" ]]; then
    echo 'Set ANDROID_NDK_HOME to an installed Android NDK r28 or newer.' >&2
    exit 1
fi
ndk=$(cd "$ndk" && pwd)
case "$(uname -s)" in
    Linux) host=linux-x86_64 ;;
    Darwin) host=darwin-x86_64 ;;
    *) echo 'Run this script on Linux or macOS.' >&2; exit 1 ;;
esac
tc="$ndk/toolchains/llvm/prebuilt/$host"
api=${ANDROID_API:-28}
if [[ ! "$api" =~ ^[0-9]+$ ]] || (( api < 28 )); then
    echo 'ANDROID_API must be an integer of at least 28.' >&2; exit 1
fi
if [[ ! -x "$tc/bin/aarch64-linux-android${api}-clang" ]]; then
    echo "Android NDK compiler is missing for API $api in $tc" >&2; exit 1
fi
prefix=${ANDROID_DEPS_DIR:-"$root/deps/android-arm64"}
work=${ANDROID_DEPS_WORK:-"$root/deps/android-build"}
cmake=${CMAKE:-cmake}
jobs=${JOBS:-4}
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then echo 'JOBS must be a positive integer.' >&2; exit 1; fi

verify() {
    local lib elf dynamic alignment
    for file in include/SDL3/SDL.h include/vulkan/vulkan.h \
        include/glslang/Include/glslang_c_interface.h include/libavcodec/avcodec.h \
        lib/libglslang.a lib/libglslang-default-resource-limits.a \
        share/SDL3/java/org/libsdl/app/SDLActivity.java; do
        [[ -s "$prefix/$file" ]] || { echo "Missing dependency: $prefix/$file" >&2; return 1; }
    done
    # nnpad_sdl.c needs this SDL 3.4 controller type, not merely SDL 3 headers.
    "$tc/bin/aarch64-linux-android${api}-clang" -I"$prefix/include" -x c -fsyntax-only - <<'EOF'
#include <SDL3/SDL_gamepad.h>
int wt_required_gamepad_type = SDL_GAMEPAD_TYPE_GAMECUBE;
EOF
    for lib in SDL3 avcodec avformat avutil swscale swresample c++_shared; do
        [[ -f "$prefix/lib/lib$lib.so" ]] || { echo "Missing dependency: lib$lib.so" >&2; return 1; }
        elf=$("$tc/bin/llvm-readelf" -h "$prefix/lib/lib$lib.so")
        [[ "$elf" == *AArch64* ]] || { echo "Not ARM64: lib$lib.so" >&2; return 1; }
        dynamic=$("$tc/bin/llvm-readelf" -d "$prefix/lib/lib$lib.so")
        [[ "$dynamic" == *"Library soname: [lib$lib.so]"* ]] || {
            echo "Unexpected Android SONAME: lib$lib.so" >&2; return 1;
        }
        while read -r alignment; do
            (( alignment >= 16384 )) || { echo "Not 16 KB aligned: lib$lib.so" >&2; return 1; }
        done < <("$tc/bin/llvm-readelf" -lW "$prefix/lib/lib$lib.so" | awk '$1 == "LOAD" {print $NF}')
    done
    echo "Verified Android ARM64 dependencies (unversioned SONAMEs, 16 KB alignment): $prefix"
}

if "$verify_only"; then verify; exit; fi
for tool in "$cmake" ninja curl tar make; do
    command -v "$tool" >/dev/null || { echo "Required tool missing: $tool" >&2; exit 1; }
done
mkdir -p "$prefix" "$work/downloads" "$work/sources" "$work/build"
prefix=$(cd "$prefix" && pwd)
work=$(cd "$work" && pwd)

sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}

fetch() {
    local archive=$1 url=$2 expected=$3 directory=$4
    if [[ ! -f "$work/downloads/$archive" ]]; then
        curl --fail --location --retry 3 "$url" -o "$work/downloads/$archive.part"
        mv "$work/downloads/$archive.part" "$work/downloads/$archive"
    fi
    if [[ $(sha256 "$work/downloads/$archive") != "$expected" ]]; then
        echo "SHA-256 mismatch: $work/downloads/$archive" >&2; exit 1
    fi
    if [[ ! -d "$work/sources/$directory" ]]; then
        tar -xf "$work/downloads/$archive" -C "$work/sources"
    fi
}

fetch SDL3-3.4.18.tar.gz https://www.libsdl.org/release/SDL3-3.4.18.tar.gz \
    9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 SDL3-3.4.18
fetch ffmpeg-n7.1.3.tar.gz https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n7.1.3 \
    e0b04c4b43d7e6d67cb6710334fb513adf13ac860532f30e1d0ac4c231f232fb FFmpeg-n7.1.3
fetch glslang-15.1.0.tar.gz https://codeload.github.com/KhronosGroup/glslang/tar.gz/refs/tags/15.1.0 \
    4bdcd8cdb330313f0d4deed7be527b0ac1c115ff272e492853a6e98add61b4bc glslang-15.1.0
fetch Vulkan-Headers-v1.4.304.tar.gz https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/v1.4.304 \
    5e1a06b3f27e7581b55d1dea176fd97ee0a2f299406db2f437c1d2f297ceba5b Vulkan-Headers-1.4.304

common=(-G Ninja "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$api" -DANDROID_STL=c++_shared
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$prefix" -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384)

"$cmake" -S "$work/sources/SDL3-3.4.18" -B "$work/build/sdl3-3.4.18-arm64" "${common[@]}" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
"$cmake" --build "$work/build/sdl3-3.4.18-arm64" -j "$jobs"
"$cmake" --install "$work/build/sdl3-3.4.18-arm64"

"$cmake" -S "$work/sources/glslang-15.1.0" -B "$work/build/glslang-arm64" "${common[@]}" \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF
"$cmake" --build "$work/build/glslang-arm64" -j "$jobs"
"$cmake" --install "$work/build/glslang-arm64"

mkdir -p "$work/build/ffmpeg-arm64"
(
    cd "$work/build/ffmpeg-arm64"
    "$work/sources/FFmpeg-n7.1.3/configure" --prefix="$prefix" --target-os=android --arch=aarch64 \
        --enable-cross-compile --sysroot="$tc/sysroot" --cc="$tc/bin/aarch64-linux-android${api}-clang" \
        --cxx="$tc/bin/aarch64-linux-android${api}-clang++" --ar="$tc/bin/llvm-ar" \
        --ranlib="$tc/bin/llvm-ranlib" --strip="$tc/bin/llvm-strip" --nm="$tc/bin/llvm-nm" \
        --enable-shared --disable-static --enable-pic --disable-autodetect --disable-programs \
        --disable-doc --disable-avdevice --disable-avfilter --disable-network --disable-encoders \
        --disable-muxers --disable-debug --extra-ldflags=-Wl,-z,max-page-size=16384
    make -j "$jobs"
    make install
)

mkdir -p "$prefix/include" "$prefix/share/SDL3/java" "$prefix/share/licenses/FFmpeg" \
    "$prefix/share/licenses/glslang" "$prefix/share/licenses/Vulkan-Headers" \
    "$prefix/share/licenses/libc++" "$prefix/share/sources"
cp -R "$work/sources/Vulkan-Headers-1.4.304/include/." "$prefix/include/"
cp -R "$work/sources/SDL3-3.4.18/android-project/app/src/main/java/." "$prefix/share/SDL3/java/"
cp "$work/sources/FFmpeg-n7.1.3/COPYING.LGPLv2.1" "$prefix/share/licenses/FFmpeg/"
cp "$work/sources/glslang-15.1.0/LICENSE.txt" "$prefix/share/licenses/glslang/"
cp "$work/sources/Vulkan-Headers-1.4.304/LICENSE.md" "$prefix/share/licenses/Vulkan-Headers/"
cp "$tc/NOTICE" "$prefix/share/licenses/libc++/NDK-LLVM-NOTICE.txt"
cp "$tc/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$prefix/lib/"
cp "$work/downloads/ffmpeg-n7.1.3.tar.gz" "$prefix/share/sources/"
cp "$work/build/ffmpeg-arm64/config.h" "$work/build/ffmpeg-arm64/ffbuild/config.mak" "$prefix/share/sources/"
cp "${BASH_SOURCE[0]}" "$prefix/share/sources/android-deps.sh"

{
    printf 'export WTBUILD_ANDROID_DEPS=%q\n' "$prefix"
    printf 'export WTBUILD_CFLAGS=%q\n' "-I$prefix/include"
    printf 'export WTBUILD_LDFLAGS=%q\n' "-L$prefix/lib"
    printf 'export WTBUILD_GLSLANG_LIBS=%q\n' "-Wl,--start-group $prefix/lib/libglslang.a $prefix/lib/libglslang-default-resource-limits.a -Wl,--end-group -lc++_shared -lm -Wl,--exclude-libs,ALL"
    printf 'export SDL3_JAVA_SOURCE_DIR=%q\n' "$prefix/share/SDL3/java"
    printf 'export WT_SDL_SOURCE=%q\n' "$work/sources/SDL3-3.4.18"
} > "$prefix/android-env.sh"
verify
echo "Source $prefix/android-env.sh before building the Android runtime."
