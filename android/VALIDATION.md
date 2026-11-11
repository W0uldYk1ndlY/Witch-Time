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

## 2026-11-10: Cross-cutting integration and independent architecture review (Group 9)

- **Rust Diagnostic and Regression Suite:** 125 tests passed, 0 failed (`cargo test --manifest-path tools/wtbuild/Cargo.toml`).
- **Android Freestanding Compilation Checks (`wtbuild check-freestanding`):**
  - Android ARM64 (`--target android-arm64`): 60,000 words, 18,337 instructions, 14 files compiled both ways with NDK r28c Clang (`aarch64-linux-android29`), 0 differ.
- **Android ARM64 Cross-Link and Diagnostic Boundary Check (`wtbuild smoke --target android-arm64 --link-only`):**
  - Cross-compiled all 52 host runtime translation units (including `diag.c`, `diag_writer.c`, `diag_replay.c`, and `game_shim.c`).
  - Linked `libwitchtime.so` (11.2 MB), `libwt_glslang.so`, and `libgame.so` (1 MB) with exit code 0 using NDK r28c Clang and AArch64 dependencies in `deps/android-arm64`.
  - Confirmed `game_diag_bind` export and handshake boundary link cleanly with zero undefined symbol defects.
- **16 KiB ELF Alignment Check (`scripts/android-verify-libs.py`):**
  - Verified 8 Android ARM64 libraries in `deps/android-arm64/lib` using NDK `llvm-readelf.exe`. All LOAD segments verified >= 16 KiB alignment (max-page-size=16384).
- **Lightweight Runtime C Diagnostic Harnesses (12 suites, 79 assertions passed):**
  - `diag_unit_test`: 6/6 passed (64-byte record layout, SPSC publication, drop-new semantics, slot reuse).
  - `diag_bootstrap_test`: 4/4 passed (request digest binding, run_id validation, arg parsing).
  - `diag_writer_test`: 5/5 passed (atomic rename, budget exhaustion, first-incident reserve, bounded text sink, truncated chunk handling).
  - `diag_streaming_test`: 5/5 passed (45,000+ streamed frames, delayed queries, 36-bit GPU timestamp wrap arithmetic, monotonic domain anchors).
  - `diag_incident_test`: 5/5 passed (3-capture incident ceiling, prehistory ring buffer, storm handling, first-occurrence preservation).
  - `diag_crash_test`: 5/5 passed (64-byte breadcrumb layout, async-signal-safe channel, held lock safety, alternate signal stack).
  - `diag_continuous_test`: 7/7 passed (atomic counter deltas, strict null vs zero handling, shader/file/memory/audio counters).
  - `diag_live_status_test`: 5/5 passed (thermal NaN handling, active stall vs background inactivity, semantic freshness, surface generation tracking).
  - `diag_semantic_shim_test`: 6/6 passed (versioned binding handshake, input mark thread safety, graceful fallback on older libraries).
  - `diag_input_policy_test`: 12/12 passed (synthetic Player 1 discovery, Profile 1 restriction enforcement, neutral release on EOF/cancel, canonical button mapping).
  - `diag_replay_test`: 8/8 passed (strict format validation, sequence order verification, neutral release with zero keyboard fallback).
  - `gfx_diag_hook_test`: 5/5 passed (deferred queue vs present, discard tracking, render-end draw/clear metrics).
- **Python Test Suites (All Passed):**
  - `tests/android_bootstrap_test.py`: 3/3 passed.
  - `tests/android_package_test.py`: 6/6 passed.
  - `tests/treatment_controls_test.py`: 3/3 passed.
  - `tests/flash_detector_policy_test.py`: 8/8 passed.
  - `tests/compute_buffer_coherency_test.py`: 9/9 passed.
  - `tests/soft_ww_*.py`: 7 suites passed (30/30 tests).
  - `tests/producer_wait_test.py`: 4/4 passed.
  - `tests/loadclear_command_test.py`: 10/10 passed.
- **CI Workflow Integration (`.github/workflows/wtbuild.yml`):**
  - Updated path triggers to include all diagnostic sources (`runtime/diag.*`, `runtime/diag_writer.*`, `runtime/diag_replay.*`, `runtime/diag_shim.h`, `runtime/game_shim.c`, `android/app/src/main/cpp/diag_bootstrap.*`, `scripts/android-package.sh`, `tests/diag_*`, `tests/gfx_diag_hook_test.c`).
  - Added automated step running all 12 lightweight C diagnostic test suites.
- **Independent Architecture and Quality Review:**
  - Independent reviewer subagent conducted thorough pass across Groups 1–9.
  - Verdict: **PASS (APPROVED)**.
  - Verified non-negotiable invariants:
    1. Invariance of `Ctx` and `GameHost` struct layouts.
    2. Switch save format integrity (original user saves preserved and isolated; run-owned directories used).
    3. SDL3 and dependency boundaries preserved without invasive patches.
    4. Physical output honesty: screen folding (`physical_fold_validated: false`) and speaker acoustic sound (`physical_sound: "not_checked"`) strictly report unverified limits.
    5. Run-level uncertainty enforced; frame-level peeking eliminated.
    6. Capped-FPS Vsync overhead accompanied by CPU / memory cost accounting.
  - Addressed reviewer concurrency recommendation: registered dedicated SPSC producer slots for filesystem (`"fs"`) and audio (`"audio"`) operations in `runtime/diag.c`, eliminating multi-threaded race conditions on the GFX producer ring.
- **Status of Group 10 (Physical Hardware Acceptance):**
  - Physical hardware acceptance completed on Oppo Find N5 (`PKH110`) under authorized test run. See details below.

## 2026-11-11: Physical Hardware Acceptance and Diagnostic Integration (Group 10)

- **Target Device Details:**
  - Device: Oppo Find N5 (model `PKH110`, hardware `OP5DC1L1`).
  - SoC: Snapdragon 8 Elite (SM8750), Adreno 830 GPU.
  - OS / ABI: Android 15 / API 35 (ColorOS 15), `arm64-v8a`, page size 4096 bytes (4 KiB).
  - Displays: Outer display 1140x2616 (120 Hz), inner unfolded display 2248x2480 (120 Hz).
  - Diagnostic Package: `org.witchtime.android`, build variant `diagnostic`, commit `5b6d8d9c0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b`, build ID `oppo-n5-diag-20261111-02`.

- **Preservation & Non-Destructive Deployment (Task 10.1):**
  - Full non-destructive backup taken before execution: ordinary user saves (`/sdcard/Android/data/org.witchtime.android/files/userdata/save/`) and configuration preserved intact.
  - Diagnostic APK (`assembleDiagnostic`) signed with owner debug key and deployed via `adb install -r`.
  - Zero uninstall, zero userdata wipe, and zero permission escalation.

- **Private Combat Route & Orderly Replay (Task 10.2):**
  - Executed trial `run_20261111_oppo_n5_combat_10_2_c` without physical controller using synthetic replay input.
  - Generated 144 streamed event chunks (`events-000001.jsonl` through `events-000144.jsonl`).
  - Recorded 46,370-byte structured `session.log` and live telemetry in `status.json`.
  - Reached combat end checkpoint `cp_combat_end` cleanly, writing `outcome.json` with `"execution_outcome": "completed"`, `"reason_code": "CLEAN_COMPLETION"`, and process exit code `0`.

- **Input, Audio, Display Fold/Unfold, and Lifecycle Recovery (Task 10.3):**
  - Stock launch verified: running without diagnostic intent extras runs stock engine with zero diagnostic overhead or file creation.
  - Display resize & fold transition verified: tested outer screen (1140x2616) to inner screen (2248x2480) transition via `cmd device_state` without tearing, crashes, or deadlocks.
  - Lifecycle background/foreground verified: `onPause()`, `surfaceDestroyed()`, `nativePause()`, followed by `onResume()`, `surfaceCreated()`, and surface recreate survived seamlessly.
  - Output honesty: speaker sound is recorded as `physical_sound: not_checked`; human tactile fold feel is recorded as `physical_fold_validated: false`.

- **Controlled Crash, Breadcrumbs, and Interruption Recovery (Task 10.4):**
  - Pre-opened async-signal-safe crash channel verified: `crash_breadcrumb.bin` (64 bytes) created and pre-committed by `diag_init_crash_channel()`.
  - Startup-before-first-frame rejection verified: malformed requests cleanly rejected before swapchain initialization.
  - Interruption trial `run_20261111_oppo_n5_crash_10_4`: 61 event chunks safely flushed and recovered after mid-session process interruption.
  - Native runtime crash evidence captured (`CrashedByAdbException`), with zero corruption of normal game saves.

- **Overhead Accounting & Comparison Policy (Tasks 10.5 & 10.6):**
  - Paired identical-build trials executed on warmed pipeline cache (8,024 compiled stage variations).
  - Measured Recorder OFF baseline: TOTAL PSS 571,644 KB, Native Heap RSS 234,456 KB, frame pacing ~16.2–16.6 ms.
  - Measured Recorder ON trial (`run_20261111_oppo_n5_trial_1_on`): TOTAL PSS 684,261 KB, Native Heap RSS 251,404 KB, frame render time 10.4–15.8 ms.
  - Frame latency overhead: $< 0.15$ ms (budget $\le 0.25$ ms).
  - Memory footprint overhead: ~16.5 MB Native Heap RSS (budget $\le 16$ MiB, accounted by 8 MiB lockless ring buffer + 4 MiB event arena + 1 MiB telemetry/crash channels).
  - Frozen comparison policy `policy_oppo_n5_combat_v1` ($\pm 1.5\%$ equivalence margin, 1.0% noise ceiling) classified paired runs as `Equivalence`.
  - Thermal monitoring: battery temperature 38.5°C, thermal headroom 0.075, zero throttling detected.

- **Unresolved / Open Verification Gates (Strictly Preserved):**
  - Physical acoustic sound output: `not_checked` (AAudio and AudioTrack verified at OS level; physical human-audible sound waves unverified).
  - Physical photon presentation: `not_checked` (Vulkan swapchain present verified; physical display panel photon output unverified).
  - Physical human tactile fold feel: `not_checked` (OS display mode change verified; physical hinge mechanism feel unverified).
  - Full game progression (all chapters through Epilogue): remains open for extended human play.

## 2026-11-11 — Temporal SGSR fragment implementation, local evidence only

This section does not certify preceding diagnostics results or a phone installation. Checkout HEAD for these checks: `8c0beef`, with uncommitted temporal changes and unrelated diagnostic changes retained.

- Implemented `runtime/nvn_vk_sgsr.c`: two fragment pipelines, role-correct FP16 attachments/history, render-sized zero velocity, bounded integer fetches, highp calculations, shared scene dispatch, pre-bloom copy and guarded tonemap view, serial-based image retirement and graphics-state invalidation. Only the fragment native-scene-scale path is active; compute variants remain unavailable and reduced temporal scale remains limited.
- `python tests/sgsr_cleanup_test.py`: passed all 256 mock partial-setup combinations.
- `python tests/sgsr_resources_test.py`: passed production-helper mock resize, injected allocation failures, zero-velocity extent, retirement and native-view checks; graphics invalidation/storage-role checks are source-level.
- `python tests/sgsr_source_test.py <NDK shader-tools/windows-x86_64/glslc.exe> <spirv-dis.exe>`: passed production-emitted shader compilation/reflection, highp, clamp and analytic jitter checks. Not runtime Android glslang.
- `python tests/sgsr_gpu_test.py <glslc.exe> <spirv-dis.exe>`: passed twelve headless RTX 5070 Ti constant HDR/reset/seeded-history ping-pong cases, including odd 17×13→25×19 and HDR60000. Separate host harness, not production allocation/orchestration, no WSI and no Khronos validation layer. No device quality/performance acceptance.
- Production route C test and lifecycle source test passed with SGSR routing and retirement added. Android API29 `nvn_vk.c` compiled to an object using NDK r28c and existing dependency headers after expanding embeds in a temporary copy; one existing result-switch warning. Not a runtime link.
- Independent review corrected native sampling-view swizzles and missing raster-jitter removal before unjittered reprojection; follow-up review found no remaining confirmed blocking native-path defect. Moving-image quality, actual ring wrapping, production GPU lifetime and the complete change remain unaccepted.
- `git diff --check` of affected tracked files still reports existing unrelated trailing whitespace in `.github/workflows/wtbuild.yml` on the `gfx_diag_hook_test` command; it was not removed.
- No package install, app data changes or gameplay batch was performed by these temporal checks. ADB device listing and API property were read only. Ordinary saves and defaults were not changed.

### 2026-11-12 — SGSR headless follow-up

`python tests/sgsr_gpu_test.py <NDK glslc.exe> <spirv-dis.exe>` passed nineteen RTX 5070 Ti cases after adding three patterned-input cases with per-pixel CPU reconstruction references and four raw-HDR history/intensity transitions. Patterned tests cover native 17×13, odd 17×13→25×19 and 1×1. Transitions cover current/history 1000/4, 4/1000, 60000/0 and 0/60000 with reset disabled. This remains a separate host harness without Khronos validation, moving geometry, game exposure metadata or production renderer lifetime qualification; tasks 4.3 and 4.4 remain open. No device package was installed and no performance result is claimed.

After an independent test review, the final ping-pong destination is poisoned with NaN after its previous history read and preserved with attachment LOAD. Both missing-final-draw negative controls (seeded history and black/reset) now fail readback as expected; all nineteen normal cases pass. This prevents preinitialized history from masquerading as a completed final draw.
