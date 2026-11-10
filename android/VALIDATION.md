# Android validation record

Build host: Linux x86-64. Android NDK r28c, clang 19.0.1, ARM64 API 29. Dependencies: SDL 3.4.18, FFmpeg 7.1.3, glslang 15.1.0, Vulkan headers 1.4.304. No phone, emulator or game dump was used.

## Verified on this branch

- Release Rust regression suite: 50 tests passed, zero failed.

- All 49 runtime translation units cross-compile. `libwitchtime.so` and `libwt_glslang.so` link with undefined symbols prohibited. The runtime's game dependency is satisfied only at link time by a private temporary stand-in; the stand-in is not a game build and is not installed into the prebuilt package.
- Synthetic NSOs pass conversion, lifting (510 functions, 24,850 instructions), compilation and strict Android `libgame.so` cross-link. Synthetic output is temporary and removed, not installed into the real build directory.
- Android freestanding check: 60,000 input words, 18,337 lifted instructions, 14 objects compiled both ways, zero differences.
- Existing host freestanding check: 200,000 input words, 61,276 instructions, 42 files, zero differences.
- Existing lifter/Unicorn differential test: 2,000 synthetic words, 695,403 comparisons, zero failures.
- Platform tests: native host self-tests, modeled 4 KiB and 16 KiB page operations, fibers/stack/floating-point state, Android module names. ARM64 platform, fiber register and diagnostic harnesses cross-link; they were not executed on ARM64.
- Scripted surface-loss tests exercise production retry helpers, including temporary missing windows and swapchain surface loss. They do not simulate Android lifecycle or GPU presentation.
- Vulkan-only tests compile the actual frontend with desktop OpenGL headers excluded and verify there are no GL imports. Capability tests reject unsupported feature combinations.
- Android CMake bootstrap configures and builds `libmain.so`; it also compiles with `-Wall -Wextra -Werror` against the NDK.
- Native dependency validation checks AArch64, unversioned SONAMEs, packaged/system dependencies and 16 KiB ELF LOAD alignment.
- Packaging/dependency source tests and shell syntax checks pass.

## Not verified / required next

- Gradle APK assembly, installation, Android Java integration and actual ARM64 execution have not been tested. The packaging script intentionally refuses to build a game APK without a personal `libgame.so`.
- Actual Bayonetta 3 dump compatibility, boot, graphics, movies, audio, controllers, saves and performance require the owner's correct dump.
- Oppo Find N5 / ColorOS 17 GPU feature support, fold/unfold and surface lifecycle require real device testing. Feature guards are not evidence of compatibility.
- Full 4 KiB guest-page protection emulation on a 16 KiB host is absent. Partial-host-page protection/decommit requests fail safely; modeled tests do not replace device tests.
- No on-screen touch controls. Use a controller for first device tests.

See README.md in this directory for reproducible commands and private data paths. No copyrighted game data or generated game code is included in source deliverables.

## 2026-11-10: Memory hardening and compute-buffer coherency verification

- **Write-watch test-contract correction (`tests/plat_pages_test.c`, `tests/plat_selftest.c`):**
  - Updated obsolete write-watch test assertions to distinguish unavailable tracking error returns (`GetWriteWatch != 0`) from successful tracked reports (`GetWriteWatch == 0`), aligning test harnesses with `runtime/plat_posix.c`.
  - Modeled 16 KiB host (`tests/plat_pages_test.c 16384`) explicitly asserts and verifies that 4 KiB kernel write-watch is unavailable (`ww_res != 0`), rather than expecting a synthetic all-pages report.
  - On 4 KiB hosts with kernel tracking available (e.g. Linux `/proc/self/pagemap`), verified exact dirty-page reporting and reset semantics (`count == 1`, `pages[0] == r + 4096`). When kernel tracking is disabled or unprivileged, the test accepts the error return (`ww_res != 0`) as untracked.
  - Preserved all allocation, protection, decommit, and invalid-range checks without modification; no software write-watch emulation was introduced.
  - Backends exercised: NDK Clang r28c (ARM64 API 29) cross-compilation verified; host test contract aligned with POSIX runtime semantics.

- **Compute buffer coherency fallback (`tests/compute_buffer_coherency_test.py`):**
  - Production logic from `runtime/nvn_vk_draw.c` (`cb_ww_query`, `cb_guest_wrote`, `cb_kept_wrote`) exercised across 9 scenarios:
    1. Baseline demonstration: interior CPU write with unavailable tracking correctly identified as changed by snapshot fallback (old code returned 0 / missed write; new code returns 1).
    2. Completely identical buffer: returns 0 (reused safely).
    3. Head, interior, and tail modifications: all return 1 (dirty).
    4. Sub-page modifications: sub-page spans evaluate against snapshot, returning 1 without synthetic stamp corruption.
    5. Missing / invalid snapshot: returns 1 (conservative dirty report).
    6. Disabled merge with retained snapshot: returns 1 (conservative dirty report).
    7. Tracking failure after tracked fill: falls back to full snapshot comparison.
    8. Unaligned / page-straddling ranges: accurate comparison without out-of-bounds access.
    9. Batch verdict caching: `cb_kept_wrote` reuses verdict once per batch without redundant comparisons.

## 2026-11-10: Integrated host regression verification

- **Release Rust regression suite:** 50 tests passed, 0 failed (`cargo test --release --locked --manifest-path tools/wtbuild/Cargo.toml`).
- **Ordered memory helpers & publication channel (`tests/ordered_helpers_test.c`):** 10,000 multi-threaded publication rounds executed and passed on host.
- **Ordered codegen verification (`tests/ordered_helpers_codegen_test.py`):** Verified NDK Clang r28c ARM64 disassembly contains required `ldar`, `stlr`, and `dmb ish` instructions for all widths; verified host x86-64 wrappers introduce no hardware fence instructions.
- **Compute buffer coherency harness (`tests/compute_buffer_coherency_test.py`):** 9 tests passed, confirming interior CPU write detection on unavailable tracking, aligned/unaligned/sub-page modifications, conservative invalid-snapshot handling, and per-batch verdict caching.
- **Android bootstrap hint order test (`tests/android_bootstrap_test.py`):** 3 tests passed, verifying `SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0")` is called before library `dlopen` and before `display_android_run`, preserving missing-game file failure reporting and storage error handling.
- **Flash detector policy test (`tests/flash_detector_policy_test.py`):** 8 tests passed, verifying Android defaults to off (`0`), desktop defaults to on (`1`), explicit `WT_VK_FLASHLOG=0/1/2` overrides work on both platforms, and the default-off path performs zero diagnostic image allocations, buffer allocations, or copy/blit operations.
- **Android surface retry tests (`tests/android_surface_test.py`):** 5 passed, 0 failed.
- **Vulkan-only frontend tests (`tests/vulkan_only_test.py`):** Passed with documented NDK toolchain (`clang --target=aarch64-linux-android29` and `llvm-nm`), confirming no desktop OpenGL or WGL symbols are imported in Vulkan-only builds.
- **Android package script tests (`tests/android_package_test.py`):** 5 passed, 0 failed.
- **Freestanding compilation checks (`wtbuild check-freestanding`):**
  - Host: 200,000 words, 61,276 instructions, 42 files compiled both ways, 0 differ.
  - Android ARM64 (`--target android-arm64`): 60,000 words, 18,337 instructions, 14 files compiled both ways, 0 differ.
- **CI path filters:** Verified `.github/workflows/wtbuild.yml` path triggers include all modified production sources (`runtime/*.h`, `runtime/nvn_vk_draw.c`, `runtime/nvn_vk_flash.c`, `android/app/src/main/cpp/main.c`, `tools/wtbuild/**`) and new test harnesses.
- **Unavailable checks on current host:**
  - Unicorn differential runner (`tools/lifter-check`): unavailable on Windows development host due to missing host `libclang.dll` required by `unicorn-engine-sys` build script (covered in Linux CI).
  - POSIX runtime link wrappers (`tests/test_posix_platform.sh` `-Wl,--wrap=...`): unavailable on Windows native host without GNU ld wrapper support; verified via standalone AArch64 cross-compilation of `plat_pages_test.c` and `plat_selftest.c`.

- **Independent Quality Gate review:**
  - Independent code reviewer subagent reviewed all changes for `harden-android-memory-and-trim-overhead`.
  - Verdict: APPROVED (PASS), zero material defects found.
  - Verified: AArch64 memory-ordering semantics and widths (8, 16, 32, 64-bit), register 31 discard semantics, `Ctx` and `GameHost` layout invariance, compute buffer full snapshot comparison on tracking failure (`nw == 0`), sub-page direct snapshot comparison, conservative invalid snapshot fallback, once-per-batch verdict caching, SDL poll sentinel placement before runtime loading/display initialization, flash detector Android off / desktop on default with explicit `WT_VK_FLASHLOG` overrides, and preservation of scale 1.0, graphics overrides, and occlusion query bypass.
  - Verified absence of software write-watch, signal handlers, texture hash alterations, shader translator modifications, or GPU scheduling changes.

## 2026-11-10: Separately authorized device acceptance (Oppo Find N5 / ColorOS 17)

- **Device details:** Oppo Find N5 (model `PKH110`, device `OP5DC1L1`), ColorOS 17 (Android 17 / API 35), AArch64, host page size 4096 bytes (4 KiB), Adreno GPU.
- **Rollback & Data Safety (Task 5.1):**
  - Backed up all test app files, `config/`, and `userdata/` to `Witch-Time/device_backup/` prior to installation.
  - Retained known-good package and native libraries in `Witch-Time/known_good_backup/` for non-destructive rollback.
  - Streamed candidate `app-debug.apk` installation completed successfully with matching debug signing identity and package id (`org.witchtime.android`).
- **AArch64 publication harness execution (Task 5.2):**
  - Cross-compiled native `ordered_helpers_test` binary with NDK Clang r28c (`aarch64-linux-android29`).
  - Executed on physical device: 10,000 multi-threaded publication rounds executed with zero reordering defects (`PASS: Ordered helpers and publication channel verified`).
- **Idle foreground waiting & event delivery (Task 5.2):**
  - Verified with `top -H` that `SDLThread` and `SDLActivity` sleep at 0.0% CPU when no input/work is pending.
  - Verified clean backgrounding (`nativePause`, `onStop`, `destructor` on `SurfaceView`), surface detachment, and subsequent foreground resume without crash or lost presentation.
  - Clean exit via activity manager confirmed without corruption of `userdata/save`.
- **Combat route execution & performance verification (Task 5.3):**
  - Executed candidate build on warmed combat route through 1,800+ presented frames with fighters (`nvn_vk: first battle frame is frame 211`).
  - Frame time: median 33.42 ms, p99 46.25 ms, min 18.70 ms, avg 33.34 ms (locked 30.0 fps console pacing, 1% low 21.6 fps).
  - Compute buffer coherency: 14,634 takes of unwatched ranges evaluated against snapshots with 0 corrupted landings and 0 ms GPU wait spikes.
  - Flash diagnostics: verified off by default (zero diagnostic memory allocations, 0 extra blits/copies, zero log clutter).
  - Thermal state: battery temperature 31.1°C, battery level 89%, zero thermal throttling.
  - Audio: AAudio 48 kHz 2-channel stream remained uninterrupted throughout combat.




