#!/usr/bin/env bash
# Stage a personal ARM64 runtime + game into the APK. Never upload this output.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
: "${WTBUILD_ANDROID_DEPS:?Set WTBUILD_ANDROID_DEPS to the Android dependency prefix}"
: "${WT_SDL_SOURCE:?Set WT_SDL_SOURCE to the matching SDL3 source checkout}"
out="$repo/android/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$out"
# Refuse to emit a launcher-only APK as a game build.
for lib in libwitchtime.so libwt_glslang.so libgame.so; do
    test -f "$repo/build/android-arm64/$lib" || { echo "Missing $lib: finish wtbuild link --target android-arm64 with your own game dump first" >&2; exit 1; }
done
for lib in libwitchtime.so libwt_glslang.so libgame.so; do cp "$repo/build/android-arm64/$lib" "$out/"; done
for lib in SDL3 avformat avcodec swscale swresample avutil; do
    test -f "$WTBUILD_ANDROID_DEPS/lib/lib$lib.so" || { echo "Missing Android lib$lib.so" >&2; exit 1; }
    cp -L "$WTBUILD_ANDROID_DEPS/lib/lib$lib.so" "$out/lib$lib.so"
done
# NDK libc++ is required by the shader compiler shim.
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
test -n "$ndk" || { echo 'Set ANDROID_NDK_HOME' >&2; exit 1; }
cp "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$out/"
python3 "$repo/scripts/android-verify-libs.py" "$out" "$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
variant="Debug"
gradle_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --diagnostic)
            variant="Diagnostic"
            shift
            ;;
        --debug)
            variant="Debug"
            shift
            ;;
        *)
            gradle_args+=("$1")
            shift
            ;;
    esac
done
if [[ "${WT_BUILD_VARIANT:-}" == "diagnostic" ]]; then
    variant="Diagnostic"
fi

cd "$repo/android"
exec gradle --no-daemon ":app:assemble${variant}" "${gradle_args[@]}"
