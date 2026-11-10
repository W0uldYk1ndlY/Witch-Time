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

### Regenerating lifted code after lifter changes

When lifter or runtime changes (such as memory ordering) are made, both the host SDK tables and main game lift must be regenerated through existing `wtbuild` commands:

1. Regenerate host tables and lifted SDK:
   `tools/wtbuild/target/release/wtbuild tables .`
2. Regenerate main game lifted C sources:
   `tools/wtbuild/target/release/wtbuild lift targets/exefs/main.elf build/gen_eh`
3. Compile and link the personal game library for Android:
   `tools/wtbuild/target/release/wtbuild compile --target android-arm64`
   `tools/wtbuild/target/release/wtbuild link --target android-arm64`

Do not use nonexistent legacy Python commands (such as `recomp.py`, `sdklift.py`, or `setup.py`) or `setup --target android-arm64`.

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
- `python3 tests/android_bootstrap_test.py`
- `python3 tests/flash_detector_policy_test.py`
- `python3 tests/vulkan_only_test.py`
- `python3 tests/compute_buffer_coherency_test.py`
- `python3 tests/ordered_helpers_codegen_test.py`
- Platform tests and NDK compile checks as documented in their source files.
- `wtbuild check-freestanding --words 200000 --seed 1 --jobs 4`
- `wtbuild check-freestanding --target android-arm64 --words 60000 --seed 1 --jobs 4`
- `wtbuild smoke --target android-arm64 --link-only --prebuilt prebuilt/android-arm64` (synthetic link only; never runs or packages the stand-in as a game)
- `tools/lifter-check --synthetic 2000 --seed 1` with its documented dependencies.

## Compute buffer coherency and fallback

On hosts where kernel memory-write tracking is unavailable (such as 16 KiB page kernels or Android environments where userfaultfd async write-protect is not exposed) or when a query fails, kept compute-buffer reuse relies on a complete snapshot comparison of the current fill. If a valid snapshot is absent or invalid, the range is conservatively treated as possibly changed.

This compute-buffer fallback is strictly distinct from sampled texture hashing (`texhash` in `nvn_gl.c` / `nvn_vk.c`):
- Compute buffer coherency ensures GPU-retained compute dispatch inputs remain coherent with preceding CPU writes without requiring page-fault signal handlers or altering page protections.
- Texture hashing tracks guest CPU modifications to sampled texture memory.
- Although complete snapshot comparison on unwatched compute buffers can be slower than kernel write-watch queries, it guarantees correctness without masking hazards or discarding valid tracked fast paths.

## Android runtime efficiency and diagnostics

To trim avoidable overhead without affecting rendering or lifecycle correctness:

- **Idle event waiting (SDL poll sentinel):** Android bootstrap in `android/app/src/main/cpp/main.c` sets `SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0")` before runtime loading and SDL initialization. This prevents SDL's event loop from spinning CPU cores on internal poll-cycle markers when idle, allowing the window thread to sleep until actionable input, lifecycle, resize, or shutdown events arrive.
- **Flash detector default policy:** The per-present flash detector (`runtime/nvn_vk_flash.c`) defaults to **off** on Android (`0`) and remains **on** by default on desktop (`1`). When off, the detector performs no diagnostic image allocations, staging copies, or brightness analyses during frame presentation.
- **Diagnostic re-enable:** To re-enable flash detection for diagnosis on Android, set the existing environment variable before initialization:
  - `WT_VK_FLASHLOG=1`: Report white flashes (1-3 frames near white and back).
  - `WT_VK_FLASHLOG=2`: Also report brightness jumps that do not recover within 3 frames.
  - `WT_VK_FLASHLOG=0`: Force detector off on desktop or Android.
- **Unchanged graphics baseline:** The default resolution scale on Android remains 1.0, existing `config/graphics.ini` overrides remain active, and the non-blocking occlusion-query bypass remains unchanged. No draws are dropped and no Vulkan capability checks are relaxed.

## Experimental optimization controls and diagnostic metrics

The runtime supports three candidate optimization treatments and one separate scheduling treatment, all disabled by default:

- **`WT_WW_SOFT=0|1` (Default: `0` / off):** Software memory write-watch backend.
  - *Eligibility:* Owned `MEM_WRITE_WATCH` allocations on supported 4 KiB and 16 KiB host page granules.
  - *Writer boundary:* Leases wrap direct kernel writes in `ReadFile` (`runtime/nnfs.c:736,879-903`) and `SystemFunction036` (`runtime/os_threads.c:590,615`). Native stacks, fiber stacks, signal handlers, and fixed mappings remain unwatched.
  - *Fallback:* Prefers kernel tracking when available. If tracking is unavailable or fails, falls back conservatively to full compute snapshot comparison without failing guest allocations or leaking tracking protections as logical permissions.
- **`WT_VK_WRITERWAIT=0|1` (Default: `0` / off):** Producer-scoped GPU waiting.
  - *Behavior:* CPU indirect reads wait specifically for the submitting slot containing the producing draw/dispatch rather than awaiting all queued work globally.
  - *Fallback:* Ambiguous or overlapping ranges fall back to conservative `cmd_submit` / slot synchronization (`cpu_read_sync`).
- **`WT_VK_LOADCLEAR=0|1` (Default: `0` / off):** Leading full colour clear folding.
  - *Behavior:* An eligible single-sample 2D colour full-subresource clear at the beginning of an unopened pass initializes the attachment using `loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR` with the typed `VkClearValue`.
  - *Fallback:* Partial rectangles, scissor clipping, masked channels, depth/stencil clears, or already open passes fall back to `vkCmdClearAttachments`.
- **`WT_VK_SPLIT=<n>` (Default: `0`):** Separate submission-scheduling treatment specifying the draw threshold before mid-frame command buffer submission at pass boundaries.

### Diagnostic Metrics & Units
- `words`: Logical hash words visited (`hash_words_visited`), representing CPU sampled reads, strictly distinct from physical DRAM traffic.
- `bytes`: Requested compute snapshot comparison span (`snapshot_compare_requested_bytes`) and tracker allocation budgets (`ww_tracker_memory_bytes`, max 16 MiB).
- `count`: Event counts for faults (`ww_fault_count`), rearms (`ww_rearm_count`), leases (`ww_lease_count`), queries (`ww_query_count`), waits, and folded clears (`clear_folded_count`).
- **Baseline preservation:** Scale 1.0, non-blocking occlusion query bypass, audio output, pacing policies, and user graphics overrides remain strictly preserved. No benchmark or configuration launcher framework is introduced.

Record actual outputs and failures separately. Passing source/unit tests does not establish that Bayonetta 3 runs on the target phone.

## Dependency redistribution

Keep the dependency recipe, pinned upstream source archives, build configuration and license notices alongside any runtime binary distribution. The dependency build saves these under its prefix in `share/sources` and `share/licenses`. FFmpeg is built without GPL/nonfree components and linked dynamically; users must remain able to replace the LGPL libraries. APKs in this workflow contain private game-derived code and are for local use, not publication.
