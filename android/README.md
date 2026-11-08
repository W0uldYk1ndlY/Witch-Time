# Experimental Android ARM64 port

Target for first device validation: Oppo Find N5 / ColorOS 17. This branch is a native NDK/SDL3/Vulkan port, not a Switch emulator. **No device compatibility or playable APK is claimed until the build and game-specific tests listed below pass.**

## What remains private

Use your own legally obtained, decrypted Bayonetta 3 dump, of the exact version required by `wtbuild setup`. This repository's setup validates that game's NSO hashes and layouts. A free/homebrew Switch game cannot stand in for it. The checked-in synthetic NSOs and lifter tests exercise conversion and translation without copyrighted game data.

Never publish the resulting `libgame.so`, converted ELFs, lifted C, RomFS, saves or an APK containing them. Follow the original repository's license and dependency notices when distributing runtime-only sources or binaries.

## Toolchain

- Linux x86-64 host, Rust >= 1.87 and ordinary C/C++ build tools.
- Android NDK r28c (`28.2.13676358`), API 29 minimum, ARM64 only.
- Android SDK platform 35 and Gradle 8.11.1 (AGP 8.9.2), Java 17 or 21.
- ARM64 SDL 3.4.18, FFmpeg and glslang built with the same NDK, with unversioned shared-library names. Build with `scripts/android-deps.sh --help` for the supported pinned dependency recipe.

The dependency build emits `deps/android-arm64/android-env.sh`; source it to set the include/library flags. Set `ANDROID_NDK_HOME` to the NDK, `WTBUILD_ANDROID_DEPS` to the dependency installation prefix, and `WT_SDL_SOURCE` to the exact SDL source checkout used to build `libSDL3.so`. Gradle uses the official SDL Android Java sources directly from that checkout. No Java sources from a mismatched SDL release should be used.

## Runtime and personal game build

Build the host `tools/wtbuild` release first. `wtbuild runtime --target android-arm64 --out prebuilt/android-arm64` builds the dump-free runtime. The Android target uses the NDK compiler, not the host compiler, and isolates its outputs from desktop builds.

The Android target applies to developer commands, not the desktop `setup` orchestrator: do not use `setup --target android-arm64`. Run the existing verified conversion/lift/tables stages on the host with your own dump, keeping their documented paths. Then run `wtbuild compile --target android-arm64` and `wtbuild link --target android-arm64`. Inspect `wtbuild --help` and `tools/wtbuild/README.md` for the host setup stages. Do not run the desktop launcher on Android.

Run `scripts/android-package.sh` only after the complete Android runtime and personal `libgame.so` exist in `build/android-arm64`. It deliberately fails instead of producing a misleading launcher-only APK. It stages native libraries and runs `gradle :app:assembleDebug`. Install the resulting debug APK locally using `adb install` (no device is connected by this repository).

## Local game files

The application uses its own external files directory, normally:

```
/sdcard/Android/data/org.witchtime.android/files/
  targets/exefs/main.elf
  targets/exefs/sdk.elf
  targets/romfs/...
  config/graphics.ini
  config/controllers.ini
  userdata/            # generated saves; back up before uninstalling
  cache/               # generated shader cache
```

Use `adb shell mkdir -p` and `adb push` on your own device to copy these files. If ColorOS restricts access, use an authorized device-local file transfer method. No all-files, network, or broad storage permission is requested. Uninstalling removes the app's data; back up saves yourself first. A missing dump displays an error, not a simulated game screen.

## Device validation checklist

1. Confirm ABI, Android API, actual host page size and Vulkan features via device diagnostics. ColorOS version alone does not establish these.
2. Check Vulkan 1.3, dynamic rendering, synchronization2 and required shader/texture features. BC compression is required by the current renderer; an unsupported GPU must produce an explicit error. Mobile Vulkan availability alone is not sufficient.
3. Run platform memory/fiber tests on ARM64. 16 KiB ELF alignment does not prove guest memory semantics; test 4 KiB guest allocations/protection/decommit on the actual host page size. Partial-host-page protection/decommit on a 16 KiB host are rejected rather than corrupting neighboring guest pages. Full 4 KiB protection emulation is not implemented.
4. Start the personal game build, capture `adb logcat` and the app-files `runtime.log` (native stdout/stderr and GPU rejection details), verify rendering/audio/input, and use a controller for first tests. No touch-control UI has been implemented.
5. Test fold/unfold, resize, rotation, background/foreground and surface loss. Never infer lifecycle correctness from a desktop test.
6. Back up a test save and verify save/restore after a clean exit and process termination. Do not use a valuable save for first tests.

## Dump-free checks

- `cargo test --release --manifest-path tools/wtbuild/Cargo.toml`
- `python3 tests/android_package_test.py`
- `python3 tests/vulkan_only_test.py`
- Platform tests and NDK compile checks as documented in their source files.
- `wtbuild check-freestanding --words 200000 --seed 1 --jobs 4`
- `wtbuild check-freestanding --target android-arm64 --words 60000 --seed 1 --jobs 4`
- `wtbuild smoke --target android-arm64 --link-only --prebuilt prebuilt/android-arm64` (synthetic link only; never runs or packages the stand-in as a game)
- `tools/lifter-check --synthetic 2000 --seed 1` with its documented dependencies.

Record actual outputs and failures separately. Passing source/unit tests does not establish that Bayonetta 3 runs on the target phone.

## Dependency redistribution

Keep the dependency recipe, pinned upstream source archives, build configuration and license notices alongside any runtime binary distribution. The dependency build saves these under its prefix in `share/sources` and `share/licenses`. FFmpeg is built without GPL/nonfree components and linked dynamically; users must remain able to replace the LGPL libraries. APKs in this workflow contain private game-derived code and are for local use, not publication.
