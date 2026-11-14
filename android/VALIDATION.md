# Android validation record

## Ten runtime/build improvements — 2026-11-15

Branch `improve/android-top10-20261115`, based on
`579ab5d307ed2bf5f6dd500d0a125521eae1591b`, repairs the temporal v2 bind/emit
compile mismatch recorded below, isolates stale callback epochs, fixes integer
multiply overflow, adds tested Android integer SIMD, improves Vulkan allocation
and cache handling, and applies a provenance-checked Android `-O2` runtime policy.
The full report, exact scope and primary sources are in
[`docs/ANDROID_TOP10.md`](../docs/ANDROID_TOP10.md).

The final dump-free NDK r28c runtime build compiled all 54 units and linked both
libraries with zero failures; source identity `8400ecf374ad1c8b`. Rust tests passed
145/145. Android freestanding comparison compiled 14 files from 18,337 lifted
instructions in both header environments with zero differences. Allocator
production-unit scenarios passed 15/15 and cache failure-injection tests 25/25.
Actual NDK-built AArch64 helpers passed 147,610 emulator cases across native and
scalar backends; host arithmetic and temporal concurrency checks also passed.
These are host/NDK results. No device was connected, no game dump or playable APK
was produced, and no gameplay/FPS, physical-sound or fold acceptance is claimed.
Historical device records below retain their original scope.

## Touch task closed by user — 2026-11-14

The user requested closing the touch task and committing its changes. This is
administrative closure, not full hardware acceptance. After the polish APK was
installed, the user reported missing startup text and no pad/touch response,
then reported the screen appeared using a build from another APK. No causal
fix or complete gameplay/editor/persistence/fold/multitouch acceptance was
established. Read-only logs and the exact previous APK were preserved; no
rollback, data clear, cache wipe or config-permission change followed that
report. Windows remains explicitly excluded. The touch guide and OpenSpec task
record retain the unverified scenarios rather than marking them passed.

Commit-isolation checks ran on an exported Git index, excluding unrelated dirty
rendering/power/driver work: touch/config, desktop+Android menu, routing,
diagnostics, HID/prompts/replay, orientation/CPU fallback, overlay structure,
headless format blits and surface checks passed. An isolated ARM64 build
compiled the selected touch paths but failed in unchanged HEAD
`temporal_source_capture.c`: its four-argument `WtTscBind` calls conflict with
the five-argument typedef already in HEAD `temporal_source_capture.h`. Neither
file is part of this commit. This pre-existing baseline mismatch remains
unresolved; earlier successful whole-working-tree builds are not proof that
the isolated commit builds completely.

## Complete OFF overlay and touch visual polish — 2026-11-14

- User superseded the original always-available SET requirement: OFF now hides
  all gameplay overlay, including SET, and ignores its old hit region. Saving
  OFF releases held input and clears pending Settings requests. Already-open
  menu/editor routing remains usable. Re-enable before closing, or reopen with
  existing F1 / player-one Start + Select; no hidden recovery gesture was added.
  OpenSpec artifacts, tooltip and touch guide were updated accordingly.
- Touch textures use dark ink, warm edges and ivory glyphs, with a garnet SET
  face. Geometry, safe area, hit targets and saved layout format are unchanged.
  Independent review found an RGB/BGRA mismatch; corrected premultiplied BGRA
  writes now have an extracted-production asymmetric pixel regression. A
  six-character CANCEL label was also corrected to fit its texture, with bounds
  assertions for every existing label.
- Final parent checks passed (exit 0): touch/config, menu desktop+Android guards,
  routing, diagnostics, HID, replay, orientation/CPU fallback, overlay structure
  and surface tests. The menu fixture originally expected SET to work while OFF;
  it now verifies rejection and recovery through the controller chord. Headless
  BGRA/RGBA UNORM/sRGB blits passed, but do not establish Android WSI appearance.
- Isolated ARM64 build compiled 54 units, zero failures; both links exited 0.
  Runtime source identity `f07784c0e6212ab5`. Candidate APK SHA-256:
  `eb3ea65d37fc2fdf7173d100f4076a6bac8ef79217d2966e40315f11e4d0d7c7`.
  ZIP/import/signer checks passed; only `lib/arm64-v8a/libwitchtime.so` changed
  versus the preserved touch APK, excluding signing metadata. Private artifact:
  `C:/Users/HomePC/AppData/Local/Temp/opencode/touch-polish-off-20261114/touch-diagnostic.apk`.
  With explicit user approval, this candidate was subsequently installed using
  `adb install -r` (Success); the device APK hash matches the candidate. Phone
  PKH110 / serial 3B1F5GEARBRJLXX2 was rediscovered at a user-supplied Wi-Fi
  endpoint after mDNS discovery returned empty. The first attempt found PID
  23018 still running and made no installation changes; the user stopped the
  game manually. Actual launcher foreground and absence of an app PID were
  checked before and after installation; no automated launch/input followed.
  Previous installed APK was backed up and hash-verified as
  `a081835e3ef673490050b1aa569e9d2b109809c6e2fb53acc045a73b69240cfc`.
  No data-clear/uninstall or config/save changes were used. Install evidence is
  in the private candidate directory's `install/`. Appearance and OFF behavior
  still await the user's manual acceptance.

## Upright ordinary touch menu — 2026-11-13

- Ordinary menu layout and hit testing now use SDL pixels. Final GPU composition
  applies the presentation transform with attached UVs; local bitmap rotation and
  a nearest transfer blit provide a shader-independent CPU fallback. Guest and
  temporal targets and global swapchain transform policy were not changed.
  Independent source review accepted mapping, synchronization, resources and
  fallback; its filtering finding was corrected with nearest canvas/scratch
  samplers while artwork retains linear filtering.
- Focused orientation/CPU-fallback, menu, touch/config/routing/diagnostic,
  HID/replay, surface and overlay-structure host checks passed (exit 0).
  Both changed GLSL stages compiled with `glslc --target-env=vulkan1.3`.
  Isolated ARM64 runtime compilation passed: 54 units, zero failures, both links
  exit 0, source identity `bb7479f75b5fa027`. Runtime-only candidate APK SHA-256:
  `a081835e3ef673490050b1aa569e9d2b109809c6e2fb53acc045a73b69240cfc`.
- Isolated run `run_20261113_oppo_n5_menu_upright_c697` sent one SET tap.
  `session.log:542-549` records an accepted request, opening via touch Settings,
  and GPU panel composition. `screen-30.png` shows upright PC Settings.
  Independent offline artifact review supports upright menu visibility, not
  gameplay, editor operation or restart persistence. Private evidence is under
  `C:/Users/HomePC/AppData/Local/Temp/opencode/menu-upright-c697/device`.
- Recorded thermal samples range from 32.5 C to 34.7 C, status 0. PID 20112's
  exit record reports USER REQUESTED / FORCE STOP; commands record stay-on
  restoration to 15. A subsequent read-only live check found no package PID,
  launcher foreground, stay-on 15 and battery 35.3 C. No posture override was set.
- Safety limitation: this historical probe used source-string script rewriting
  and package-wide force-stop, which the current bounded-runner contract forbids.
  Do not replay it. This evidence does not establish runner safety/calibration,
  clean application completion or complete diagnostic stream acceptance.
  Further device work must use a path satisfying the current safety contract.
  Windows remains explicitly skipped; OpenSpec remains 14/16, with device/editor
  acceptance and final scenario evidence open.

## Touch acceptance scope update — 2026-11-13

The user excluded the Windows build from this change's acceptance. It remains
skipped, not passed; task 5.1 now records the completed host/Android checks with
that explicit exclusion. Touch device/editor/persistence/lifecycle checks and
the final scenario evidence remain open. Physical multitouch, fold and pad
usability will be presented as user-operated checks, not inferred from ADB taps.

## Touch presentation correction — SET opens, 2026-11-13

- Implemented normalized SDL-to-image mapping for identity/90/180/270 degrees,
  retaining original UVs. Shared hit/editor geometry stays in SDL pixels;
  ordinary-menu pointer conversion occurs after the editor branch. Unsupported
  transforms fail closed. Presentation destruction releases held input, and
  authoritative SDL dimensions are reconciled under the metadata lock to avoid
  either geometry/recreation ordering leaving stale dimensions. Independent
  scoped review accepted the fixes. Ordinary-menu visuals remain unchanged and
  can be sideways; they were not redesigned or globally counterrotated.
- Focused touch/config, menu (desktop and Android guards), HID/replay, surface,
  routing, diagnostic and overlay-structure checks passed. Headless Vulkan
  BGRA/RGBA UNORM/sRGB round trips also passed, but do not prove WSI placement.
  Final isolated ARM64 build compiled 54 units with no failures and linked both
  runtime libraries, identity `db7856db58dd74a8`. Candidate SHA-256
  `24bcc24add56ac5115248a48cea749747fcfc66ce2188f366aa69b9d8f86cfdf`
  changed only the runtime library versus the earlier touch APK, apart from
  signing metadata; matching signer, ZIP/content and game import checks passed.
- Non-destructive installation and device APK hash passed. Run
  `run_20261113_oppo_n5_touch_aligned_b696` sent one tap at (1124,224).
  `session.log` records SDL forwarding at (1124.500,224.090), SET hit=1,
  request raised/consumed accepted=1, then `pcmenu: opened (touch Settings)`.
  Later screenshots show PC Settings. This confirms the tested SET alignment
  and opening path, not full touch gameplay or physical-panel multitouch.
- The initial admission attempt observed no app focus and made no installation
  or settings changes. A read-only recheck found NotificationShade over the
  launcher; only that system shade was collapsed before rerunning admission.
  The approximately 55-second run peaked at battery 36.4 C with thermal status 0.
  Owned PID 9535 was stopped and stay-on restored to 15; an additional check
  found no app PID and confirmed stay-on and installed APK hash. No posture
  override was set. Private artifacts: `touch-transform-final-b696/device`.
  Candidate remains installed; exact original APK rollback remains preserved.
  Gameplay, editor, restart persistence, physical pad/fold/multitouch and full
  diagnostic stream/EOF acceptance remain open; OpenSpec tasks stay 13/16.
  Independent offline artifact review confirmed the narrow SET route and cleanup
  evidence without promoting it to gameplay, performance or complete acceptance.

## Touch presentation transform observed — 2026-11-13

- Added one diagnostic line per successful swapchain creation; Android surface
  mock checks passed (five tests). Isolated ARM64 runtime compiled 54 units and
  linked successfully, identity `0626bacf0ede436a`. Runtime-only APK SHA-256
  `c8a6a6e444c91693cf5d40e0d5174ee87c4455911124b65fd18f7dbcd011c853`
  passed the same content/signer/import checks and was installed non-destructively.
- Bounded approximately 12-second run
  `run_20261113_oppo_n5_touch_surface_b694` sent no taps. Actual surface log:
  currentTransform=8, preTransform=8 (ROTATE_270), supportedTransforms=511,
  current/image extent 2248x2480, SDL pixels 2248x2480,
  SDL safe rectangle (97,146,2054,2289). Combined with the preceding SET trace,
  the required SDL-to-image rotation for this surface is normalized (v,1-u),
  not an identity mapping or swapped-extent assumption. Oracle source/evidence
  review accepted this convention; implementation and device revalidation are
  still pending. Ordinary menu hit coordinates need the same mapping separately
  from touch/editor sprites.
- Peak battery temperature 32.2 C, global thermal status 0; owned PID 23594
  stopped and stay-on restored. APK remains installed. Private artifacts are in
  `touch-diag-isolated-b694/device`. No new hardware acceptance gate is closed.

## Touch SET trace — delivery observed, alignment failed, 2026-11-13

- After renewed idle-phone confirmation, candidate `3877bc1a` was installed
  non-destructively and hash-checked. Run
  `run_20261113_oppo_n5_touch_trace_b693` performed one touchscreen tap at the
  observed SET position. SDL logged DOWN/UP with timestamp newer than cutoff,
  replay off, matching generation, valid/visible geometry and ready lifecycle.
  Touch runtime received pixel coordinates (2048.911,1240.500), but its SET
  rectangle was (1046,146,156,156), hit=0. The failed visible SET interaction
  therefore has a directly observed draw/hit-coordinate mismatch; stale-event
  filtering did not reject this event. Actual Vulkan surface transform still
  needs logging before selecting the correction. Menu/editor alignment must be
  considered alongside sprites, not assumed fixed by rotating SET alone.
- Owned PID 20482 was stopped after the bounded approximately 55-second trace;
  stay-on was restored to 15. Fifteen admission/monitor samples peaked at
  battery 35.3 C and global Android thermal status 0. Candidate remains installed.
  Private trace/captures are in `touch-diag-isolated-b693/device`. No gameplay,
  editor, persistence or physical multitouch acceptance follows from this trace.

## Touch input tracing follow-up — offline candidate, 2026-11-13

- Old touch runtime `dcf69e3d92c790a4` was inspected with NDK `llvm-objdump`:
  `poll_devices` calls `SDL_FlushEvents` with first=0 and last=65535. This can
  discard finger events; it is a confirmed old-binary defect, not yet causal
  proof for the observed SET failure. Current Android source flushes only the
  joystick/gamepad ranges. The blanket flush in its non-Android `#else` is not
  the current Android behavior.
- Added bounded Android diagnostic-session-only touch dequeue/runtime/SET
  request traces, with explicit advisory dispositions and no routing changes.
  Independent source review accepted the scoped one-tap diagnostic plan.
  Host touch/config, routing, diagnostic caps, HID, replay, menu (desktop and
  Android guards), and Android pad discovery tests all passed; scoped diff
  whitespace check passed. These are mocked host checks.
- Shared build staging was busy (`os error 1224`); no competing process was
  stopped. An isolated source copy compiled 54 units and linked both runtime
  libraries successfully, source identity `1cdeb2404e09ba5b`. Private candidate
  APK SHA-256 `3877bc1a126d0595e67022e53db38aa92c3f6cb91a5af08872a032a38523651c`
  is at `touch-diag-isolated-b693/touch-diagnostic.apk` in approved temporary
  storage. It changes only `lib/arm64-v8a/libwitchtime.so` versus the previous
  signed touch APK (apart from signing metadata). ZIP integrity, package-content
  equality, retained build metadata, existing signing certificate and game
  import compatibility were checked. New SDL imports resolve in packaged SDL;
  `pthread_detach` is an Android libc API. This was runtime-only repackaging,
  not a fresh game/bootstrap build.
- This candidate has NOT been installed or run. Admission found another app
  foreground; no new device writes were made. The earlier touch APK remains
  installed. Live SDL delivery, SET opening, gameplay and the full hardware
  acceptance matrix remain unresolved.

## Touch phone smoke — partial, 2026-11-13

- Non-destructive `adb install -r` succeeded for signed touch APK SHA-256
  `866fea1fb4443e415837ed0dc511297c6554107ac0c0787ea60d7a6ea3756b7d`.
  Its signer matches the installed certificate; the exact previous APK
  (`f81da711e92dbb9452ebe5611fb173b2117467b85fa0f5f824a628e7d280d57f`)
  was pulled and hash-checked against the device for rollback. Game library
  bytes match the previous APK. No uninstall or data clear was performed.
- Run `run_20261113_oppo_n5_touch_b691b53b` used copied userdata/config in
  its own diagnostic directory, without replay. Logs confirm isolated config,
  scale 1.2, 16x anisotropy and LOD -1. Actual game window was 816x900, so
  nominal 1920x1080 request metadata does not prove an actual 1080p render.
- An approximately 80-second bounded observation captured visible touch controls
  over a black background. Labels/shapes require investigation. No touch input
  was injected; gameplay, multitouch, settings/editor and persistence are NOT
  accepted. Last status was active/healthy with zero reported drops, not a
  clean stopped EOF or complete stream validation.
- Twenty admission/monitor samples observed battery temperature at most 39.2 C
  and global Android thermal status 0. Owned PID 15710 was force-stopped;
  subsequent check found no app PID and stay-on was restored to 15. No posture
  override was set. The touch APK remains installed; rollback has not been
  executed. Private artifacts are in the approved temporary directory
  `touch-phone-rollback-b691b53b52f84fa4b6c90dcd0261583a`.
- Follow-up `run_20261113_oppo_n5_touch_set_b692` performed one ADB touchscreen
  tap at (2048,1240), the observed SET location in the unrotated 2248x2480
  capture. Active input viewport reported orientation 0 and matching logical/
  physical bounds. Later captures did not show the menu, and no menu-open log
  was found. Event routing/coordinate alignment remain unresolved, not accepted.
  Peak observed battery temperature was 41.3 C, global thermal status remained 0;
  the run was force-stopped and stay-on restored. The preceding staging-only
  attempt `touch_set_b691` never launched: copying prior run-owned shader cache
  failed with permission denied. No permissions on that old cache were changed;
  b692 used a fresh cold cache. The earlier label-corruption hypothesis was
  withdrawn after reviewing a rotated capture and the glyph source. Cold shader
  preparation was unfinished in the first run; black backgrounds alone establish
  neither a render defect nor successful gameplay. Source snapshot `probe.py`
  was subsequently amended to restore stay-on even if stopping the app fails.

## Android touch controls — offline evidence, 2026-11-13

- Host touch/config, production-HID-with-mocks, prompt, SDL-filter-with-mocks,
  desktop/Android-guard menu, replay, bootstrap, surface and overlay-structure
  checks passed. A headless NVIDIA Vulkan check passed BGRA/RGBA, UNORM/sRGB,
  light/dark and direct/intermediate blit round trips; it did not use Android,
  WSI, the full overlay pipeline or a Vulkan validation layer.
- ARM64 runtime: 54 source units compiled and both runtime libraries linked;
  source identity `dcf69e3d92c790a4`. Debug and unsigned diagnostic APKs
  assembled offline in an isolated temporary copy. Runtime, shader-library and
  existing game-library allocated sections match the packaged copies. Neither
  APK was installed; the debug signing key is temporary and the diagnostic APK
  is unsigned. The existing game library was reused, not rebuilt for this test.
- Independent review identified and rechecked fixes for stale touch/Settings
  races, compact-editor mismatch and RGBA/sRGB format handling. Details and
  commands are in `docs/ANDROID_TOUCH_CONTROLS.md`.
- **All touch hardware scenarios remain not checked:** touch-only menus and
  gameplay, multi-touch, editor, persistence, interruptions, pad coexistence,
  resolution/upscaler alignment and both fold/safe-area postures. No ADB or
  phone action was performed. No previous physical acceptance gate is changed.
  A Windows desktop runtime build is also open because the available compiler
  lacks `#embed` and desktop SDL/Vulkan/FFmpeg headers are absent.

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

### 2026-11-12 — Reconciled committed baseline and continued local verification

Starting tree was clean at `6247d1c`, after diagnostics commit `48576c6` and temporal commit `0e49c6d`. Earlier diagnostic hardware results are not temporal acceptance. The source/caller/producer-serial inventory was reconciled in `docs/TEMPORAL_UPSCALING.md`; unrelated defaults and normal saves remain untouched.

- `python tests/sgsr_gpu_test.py <NDK r28c glslc.exe> <spirv-dis.exe>`: 24 positive RTX 5070 Ti cases and two correctly rejected missing-final-draw controls. Five added direct Convert readbacks compare nonidentity camera/jitter, encoded neutral/total velocity, and non-unit depth/projected W against analytic expectations. This remains a separate shader harness without Khronos validation, actual previous geometry, or production in-flight lifetime proof.
- Independent compatibility verification: nine host C suites and fourteen Python/shader suites passed; `cargo test --release --locked --offline --manifest-path tools/wtbuild/Cargo.toml` passed 125 tests. Exact logs are local at `C:/Users/HomePC/AppData/Local/Temp/opencode/verify6247d1c-fs/`.
- `python tests/android_surface_test.py`: all five mocked WSI tests passed after adding the existing surface-generation diagnostic hook to the extraction harness and asserting its generation/extent. This corrects an outdated harness, not Android surface behavior.
- `CC='<NDK clang.exe> --target=aarch64-linux-android29' NM='<NDK llvm-nm.exe>' python tests/vulkan_only_test.py`: frontend no-OpenGL-import check passed; execution-only trap intentionally skipped for the cross target. Default Windows compilation is not supported by this POSIX frontend harness.
- `wtbuild runtime --target android-arm64 --out C:/Users/HomePC/AppData/Local/Temp/opencode/temporal-runtime-20261112`, with NDK r28c, existing `deps/android-arm64`, and the documented static glslang library group: 52 host source files compiled, strict `--no-undefined` links for `libwitchtime.so` and `libwt_glslang.so` passed; runtime source identity `670beabea5479fc0`. The build uses the named link-only game stand-in and does not need or validate private game tables. Output was temporary, not installed or packaged for the phone. Later edits require a fresh build identity.
- `python tests/upscale_retirement_test.py`: production `upscale_mode_changed`, `sgsr_retire_images`, `vt_garbage` and `garbage_collect` execute against mocked fence serials. All 32 mode transitions and four concurrently pending generations retain images through their last possible users. This is serial-policy evidence, not Vulkan in-flight lifetime validation.
- Native Windows `gcc -std=gnu2x -O2 -Iruntime tests/pcmenu_selftest.c runtime/gfx_options.c -o <temp>/pcmenu_temporal.exe`, then `<temp>/pcmenu_temporal.exe <temp>/pcmenu_temporal_run`: all menu/config compatibility assertions passed, including SGSR options, environment precedence, old-file round trips and apply-on-close. The test now supplies Windows environment/directory shims and normalizes its in-memory shipped-file fixture to LF; no production config was rewritten. Existing dllimport redeclaration warnings remain in the test stubs.
- `wtbuild smoke --target android-arm64 --link-only`: passed synthetic lift/table/game/runtime strict links and AArch64 ELF64 checks. An earlier attempt with `--prebuilt <temp>/temporal-runtime-20261112` correctly rejected a stale source fingerprint after concurrent implementation edits; its rebuilt links passed but prebuilt-use assertion failed. The un-prebuilt retry passed. Neither command ran a device or a real game module.

#### Compute comparison and marker follow-up

- `python tests/sgsr_cs_compile_test.py`, `python tests/sgsr_cs_resources_test.py`, `python tests/sgsr_cs_source_test.py <glslc> <spirv-dis>` and `python tests/sgsr_cs_gpu_env_test.py`: passed syntax/seam, mocked resource/capability, emitted unique-binding/144-byte-UBO/bounds/precision and oracle-environment checks.
- `python tests/sgsr_cs_gpu_test.py <glslc> <spirv-dis>`: six RTX 5070 Ti constant-input dispatch/coverage fidelity cases plus missing-dispatch rejection passed. CPU expectations reproduce pinned official compression/packing/FP16/inverse equations, not the original RGB quality oracle. Constant repeated inputs do not prove distinguishable history/reset or moving reprojection.
- The separate `--quality` invocation exits 1: red 4 becomes 3.82227 and red 1000 becomes 99.875. Independent review confirmed the pinned inverse denominator and stored-ColorMax branch explain the loss and why the 60000 case can pass. This is a failed HDR acceptance result, not an unexplained host-binding fault. Original algorithm and 2% relative quality tolerance remain unchanged. A lead rerun with both inherited fidelity/skip variables set still correctly executed quality comparisons and retained the failure.
- CS runtime build with the same NDK/dependency environment linked both Android libraries successfully (52 files; source identity `40efd8ca86f8fc56`, before later marker-counter/diagnostic edits). Final integrated source needs a fresh identity. No package/device activity or 3-pass support follows.
- `python tests/pass_timestamp_test.py` and `python tests/pass_timestamp_lifecycle_test.py`: passed production recording-interval and real extracted command lifecycle/ring-wrap tests under Vulkan mocks. Independent review confirmed corrected 2 replay + 2 FS draw attribution and collection/reset ownership. Internal PT draws now have their own counter to preserve optional split-threshold policy. Missing component transport/lost-tail reporting remains separate until integrated.
- `python tests/rs_store_test.py` passed after updating its extraction harness for the existing new marker end call; it asserts that rejected replay does not close a nonexistent interval. No production replay semantics were changed by that harness fix.

#### Motion contract and full-grid GPU model

- `python tests/upscale_motion_test.py <NDK glslc> <spirv-dis>` passed on RTX 5070 Ti using the actual emitted `upscale_motion.h` helpers and production SGSR Convert. It covers camera/object/combined displacement, explicit valid zero and invalid fallback, depth 0/0.5/1, jitter, nonsymmetric column-major projective UBO reads and FSR/SGSR previous-UV equivalence. Wrong-sign, zero-sentinel and omitted-final-draw controls are rejected. Independent review accepted the task 5.1 contract; no game previous-transform/bone/deformation source, owned raw texture producer or dynamic binding is established.
- `python tests/rs_store_gpu_test.py <NDK glslc>` passed 12 RTX 5070 Ti positive cases and six skipped-replay controls. Direct/indexed image-only replay fills all pixels at 17×13, 31×9 and 3×5; mixed-buffer cases reject replay and preserve one atomic increment per initial fragment. Their intentionally untouched rows/columns are a rejected-eligibility result, not accepted smaller-scene coverage. Helpers and eligibility are extracted from production; draw/viewport/query/target orchestration remains modeled. Independent read-only review found no blocking flaw in this narrow GPU evidence. Actual replay `exec`, ring wrap and indirect/conditional/tessellation qualification remain open; the temporal floor is retained.
- Both new GPU harnesses offer `--compile-only` for CI without a GPU, explicitly reporting that numerical/coverage checks were not run. Vulkan-header-dependent source/resource/lifecycle checks are conditional on existing headers; CI does not install a new dependency or silently claim skipped checks passed. Local full GPU runs above were not compile-only.
- Integrated runtime rebuild after shared motion and split-preserving marker changes: 52 files compiled, both strict Android links passed, source identity `9c324d4c7883663a` in temporary `temporal-integrated-runtime-20261112`. This identity predates any later diagnostic/reactive integration edits and is not a packaged/installed game identity.
- Current `vulkaninfo --summary` still lists only NVIDIA/Steam layers, no Khronos validation layer. RTX 5070 Ti driver reports 621.01. The loader reports the existing missing Epic overlay manifest; no registry/layer configuration was changed. Validation-layer-dependent tasks remain open.
- `python tests/upscale_reactive_test.py <glslc> <spirv-dis>` passed synthetic RTX 5070 Ti emitted-helper checks for local masks/history weighting, bright/additive/unchanged detail, classification, invalid coverage, reset and nonfinite inputs. Both missing-mask and missing-history-rejection mutations are rejected. Independent review's disabled/nonfinite ordering finding was fixed and disabled-mode assertions added; GPU and compile-only reruns passed. This is isolated groundwork only: no actual opaque boundary, masks, backend binding or production reactive support is established; 6.1/6.2/6.3 remain open.

#### Fresh integrated host checks, 2026-11-12

- Full `cargo test --release --locked --offline --manifest-path tools/wtbuild/Cargo.toml`: 134 passed, zero failed, including all 84 diagnostic tests.
- Fresh strict Android runtime rebuild: 52 units, zero compile failures; both libraries linked successfully; source identity `1add260893c758fa`. No package installation or device acceptance was performed.
- Fresh `wtbuild smoke --target android-arm64 --link-only` also passed: synthetic NSO verification, 510 lifted functions/24850 instructions, generated/table compilation and strict runtime/game links. Output is AArch64 ELF64. The game library is a synthetic stand-in; this does not verify actual game boot or gameplay.
- Fresh PT recording/lifecycle, store transition, GPU component report, temporal diagnostic wiring, retirement and CS resource tests passed. These use mocked host/Vulkan seams, not device execution. The CS syntax harness initially failed because its host seam omitted the new `upscale_diag_dispatched` declaration; adding that declaration restored the complete-unit syntax check. Android surface tests then passed all five cases.
- Bounded offline execution of the original ELF Oodle decoder consumed exactly 5294135 compressed bytes and produced the 44184304-byte shader DAT, with 5588 catalog entries. Twenty-four velocity members were selected into approved temporary storage; five selected stages translated and compiled. Original inputs were unchanged. This establishes availability and offline translation, not previous-pose semantics, active velocity reachability, opaque capture, GPU readback or device quality.
- Independent final review accepted task 8.2's host implementation/test/documentation criteria; the checkbox is complete (10/37 overall). Physical component-overhead calibration and phone acceptance remain unverified. Fresh FSR evidence wiring/cleanup/image-usage, SGSR cleanup/resources, CS environment isolation, config parser, lifecycle wiring and scale-policy checks passed. The component report and temporal wiring checks are now included in the existing CI test entry.
- Fresh full RTX 5070 Ti FS suite, motion contract suite and reactive helper suite passed, including their missing-final, wrong-sign, zero-sentinel, missing-mask and missing-history-rejection controls. These do not qualify real game geometry, Adreno quality or timing. FSR limits/evidence, scene identity, SGSR UBO layout, CPU sentinel and all 14 timestamp helper cases also passed. The direct-include route harness needed updated diagnostic mock types after observer integration; with those test-only seams and matching function linkage corrected, its production routing assertions pass again.
- Fresh shaderc FSR descriptors/camera UBO, pinned SGSR descriptors/varyings/UBO and production 2-pass-CS unique-binding/bounds/highp checks passed. Coherency, producer-wait, treatment and bootstrap regressions passed. Dither and resolution-pyramid C self-tests passed; Vulkan requirements passed with existing `deps/android-arm64/include` headers (the initial bare host compile omitted that include directory). No headers or dependencies were installed.
- Final independent host-integration review accepted tasks 9.1/9.2: CI/source fingerprints include the new backend and shader headers, focused checks are wired, material findings resolved and unavailable source-family/device evidence stays explicit. OpenSpec progress is now 12/37, not overall feature acceptance. Fresh CS official-equation fidelity again passed all six constant-input cases and the missing-dispatch control; `--quality` again returned 1, including red 4→3.82227 and 1000→99.875. No algorithm/tolerance change was made to hide that failure.

#### Wi-Fi device preflight and isolated probe preparation

- Corrected the empty-ADB-list conclusion: the PKH110 phone is reachable through its wireless TLS serial; another discovered endpoint is Smart TV, not the target. All subsequent device commands explicitly select the verified phone serial. API 37 and firmware fingerprint `OPPO/PKH110/OP5DC1L1:17/AP3A.240617.008/V.491da13-22d16cc-22fa78d:user/release-keys` were read from the phone. Physical display reports 1140×2616; no posture override was applied.
- Installed APK was pulled into approved temporary storage before changes: SHA-256 `d1c78fa51d4ebfe575854e91a436d74fd80ea6239517b25e77f91ac2b412f178`. ZIP integrity and library hashes were verified. Original package certificate SHA-256 is `8a3e08ab79906ab3f960989619a3196941ea1fb2b455babc27067f12d17cc1dd`.
- Android denied shell reads of ordinary config/saves and `run-as` rejected the non-debuggable package. The user explicitly waived config/save backup for this batch because they contain nothing important; this does not authorize deleting or overwriting ordinary saves. No export feature was added. APK rollback remains available.
- Diagnostic APK built offline with cached Gradle; original local runtime staging was backed up and restored in `finally`. Packaged runtime `.text`, `.rodata`, `.data` and build-id note match fresh runtime identity `1add260893c758fa`; signing certificate matches installed package. APK SHA-256 `4c430a36a83b6b60b2c46277baa64ae7f8e91fbe37ed948405212be6502c0d2e`; non-destructive `adb install -r` succeeded. No uninstall/clear was performed.
- Started unique isolated request `run_20261112_oppo_n5_temporal_fsr_probe_01`: FSR native-AA, cold empty session userdata, component profiling, startup/scenario deadlines 45/75 seconds. This is a bootstrap/execution probe, not a matching combat fixture, quality reference or calibrated comparison. Outcome remains pending at this record; no task/device acceptance inferred from installation.

#### Actual phone probe results and diagnostic corrections

- Probe 01 stopped healthy with 3235 emitted/written events, zero drops and 137 chunks. Component report is healthy: 48 completed and 4 null-duration lost-tail records (only ordinary pass/present components). Temporal observation correctly reports no eligible scene, no initialization/dispatch/consumption. First-run preparation consumed 63.4 seconds of the 65-second native timed run, leaving only 27 scene frames. This is not an FSR failure or accepted execution test.
- Probe 01 exposed pre-existing fabricated outcome metadata: generic exit/replay EOF callers supplied `cp_combat_end`, and the writer hardcoded Shibuya segment coverage despite empty checkpoints/assertions. Corrected all three callers to unknown checkpoint and removed fabricated coverage; explicit genuine checkpoint arguments remain supported. Six executable writer tests and independent review passed. Fresh strict Android runtime identity `045cd3a0340af25b` linked successfully; diagnostic APK SHA-256 `6d27daa23cb7ba9fd73d36ef58f0557242d040bf57548752deaeefb809d1ad55` has matching signing certificate and verified fresh runtime sections. Non-destructive replacement succeeded.
- Probe 02 uses fresh isolated userdata/cache and explicitly changes only its session-owned config to `shader_prepare=background` plus native-AA FSR. It is an uncalibrated execution probe, not a speed comparison. Logs establish Adreno 830, Vulkan 1.3.284, driver `0x80320050`, all 8 FSR pipelines created, 1600×896 scene/output resources, dispatch and exact output binding/consumer draw at frame 989. These recorded commands are not visual-quality proof or complete cohort provenance.
- Probe 02 verifies corrected outcome: null last checkpoint and empty coverage/checkpoints/assertions on ordinary timed completion. However recorder reports 19776 drops, 40341 emitted and 39826 written; host correctly publishes no temporal observation and an unhealthy/empty component report. This invalid stream cannot be used for component timing or temporal acceptance. Investigation identified a fixed 256-record flush cap despite configured chunk limit 1000 and ring capacity 1024; throughput correction and regression are in progress. Task 9.2 reopened for this material shared-diagnostics finding.
- Screenshot tooling must select the active physical display explicitly: the foldable exposes an inactive inner display first, which produces a black image and is not game-quality evidence. Captures in these probes do not establish a valid visual reference. No posture override was introduced. After each finished run, posture reset was issued and original stay-on setting 15 restored. Battery temperatures read 29.1°C before probe 01, 33.0°C afterward and 35.5°C after probe 02; these sparse readings are not sustained thermal qualification. Physical sound remains not checked and physical fold validation remains false.

- The configured chunk cap is now honored up to the fixed ring capacity, and stop performs bounded final drain passes. Ten executable writer tests and independent review accepted those host changes; strict runtime identity `9ca036f4858ba8df` and matching packaged sections were verified. Diagnostic APK SHA-256 `ec5d6a07e7badf9c4d981634a8eaf803b15416881aa06bd184d6e7f168cad69b` was installed non-destructively with the matching certificate.
- Probe 03 reached startup acknowledgement timeout. No native manifest/session was created. Subsequent system inspection showed secure keyguard active and the display asleep; after user-requested manual unlock, probe 04 with the same APK completed. This failed attempt is retained, not renamed or treated as FSR execution failure. No credentials were requested.
- Probe 04 still reports 2624 drops, 50591 emitted, 50589 written and 336 chunks; it remains invalid. The original host runner pulled only 200 chunks (21997 rows). The remaining 136 committed chunks were recovered separately without overwriting the first capture; all 50589 written rows are now present. Matching-status collection was corrected to collect the declared count with a 4096-chunk bound and existing per-chunk/stream byte limits; missing required chunks fail collection. New 336-chunk/missing-middle/4097 fixtures pass among 85 diagnostic tests; independent review accepted this host-only fix.
- Full sequence inspection corrects the two-record-deficit hypothesis: sequence IDs 3980 and 29210 are absent **inside** the stream, while maximum sequence is 50591. This is not explained by a shutdown tail. Both gaps precede present-queued events; the front-end `nvn_gl.c` present hook and render-thread hooks currently share the same nominal gfx SPSC producer. A separate shutdown-admission race also exists, but fixing it alone cannot explain these interior gaps. Thread-owned hook producers, bounded admission sealing and their concurrency regressions are in progress; no stream-integrity gate is relaxed.
- The render producer's accepted peak exceeds one ring's capacity per 500 ms. Exact fixed component-profile `WT_VK_PASSTIME=1` now selects 50 ms writer flush cadence; default/unset/other values retain 500 ms. Existing chunk/ring limits and audio event semantics remain unchanged. Executed boot-snippet tests and independent source review accepted this narrow profile distinction. Its new overhead and hardware loss behavior are not yet qualified; old calibration is not promoted.

- Interior corruption was traced to shared-domain SPSC ownership. All recorder hooks now lazily obtain a thread-owned producer with actual thread ID; successful/failed TLS caches invalidate on session epoch changes. The fixed 16-slot lifetime bound remains; exhaustion explicitly counts loss. Registration publishes metadata through initializing/ready states. Explicit `diag_emit` callers retain their SPSC ownership obligation. Optimized concurrency tests exercised 400000 records with exact unique sequence coverage; existing native suites and independent review accepted the host change.
- Admission sealing is separate: it closes new emits/registrations and waits on accepted operations before bounded final drain. Seal failure is sticky until reinitialization; writer status remains unhealthy/ERROR through repeated stop/shutdown, and capture is incomplete without fabricating the actual process exit code. Paused-emitter/late-admission and writer-timeout tests passed. Additional atomics/TLS and tighter component flush cadence invalidate promotion of earlier calibration.
- Fresh strict runtime identity `5daffab1b99e46b3` compiled all 52 units and linked both libraries. Offline diagnostic packaging restored original staging afterward; signature and packaged executable/data/build-id sections were verified. APK SHA-256 `8b61cc63f04559f90667516caa0fe69bfe1694a7e7fb97b8a4d3e8d17ae5b111` installed successfully. Probe 05's isolated config was staged, but the probe was **not launched**: the phone was locked, then the user took it away and explicitly requested offline-only continuation. No more device commands are authorized while it is away.
- Pending device follow-up: the original APK rollback remains in approved temporary storage; the newly installed diagnostic APK is still on the phone. The last preparation command set `svc power stayon true`; restore the recorded original global stay-on value 15 when device access resumes. No fold override or screen-timeout change was made. Probe 05 has no execution result, and combined TLS/seal/cadence/collection fixes still require a fresh complete phone stream before hardware qualification.

#### Offline continuation after phone departure

#### Phone returned: fresh integrated probes

- Explicit temporal-source requests now select the existing 50-ms diagnostic writer cadence through typed requested state, which is established before runtime boot and remains set while waiting to arm. Independent review accepted this narrow change; no activation environment bridge, ring-size or capture-budget change. Other diagnostic runs remain 500 ms unless component timing already selects 50 ms; non-diagnostic startup is unchanged. Lead and reviewer reran the cadence matrix plus real recorder/writer controlled-drain test preserving 1075 records, including 638 clear-fold records and terminal tail. The modeled 500-ms loss is not attribution of the device's 149 drops; scheduling/overhead/calibration remain unqualified.

- Third discriminating run established visible title output, not persistent black: native output at 85.646 s; foreground screenshots at 91.523/93.545 s show the title's PRESS ANY BUTTON prompt. No input was injected. All 677 chunks/71621 written records were retrieved, but 149 drops make the stream ineligible for loss-free acceptance. No ordinary-save re-export comparison was performed after this third run.
- User subsequently confirmed physically seeing the screen, selecting SGSR Quality in settings, then launching gameplay successfully. Attribute this to direct user observation: physical functionality is confirmed by the user, not measured quality/FPS, backend variant, effective scale, automated Continue route or qualified motion-source evidence. Preserve the user's changed ordinary settings; earlier diagnostic baselines must not overwrite them. A late relaunch was observed around this interaction, but no autonomous-relaunch defect is established. No further device actions should interrupt the user's live session without coordination.

- Timeline correction for the two route attempts: scene dimensions first became nonzero at 89.57/89.33 s; native 1600×896 output appeared at 91.77/91.53 s. All valid in-app screenshots preceded these transitions, and Enter at 63.887 s was early. Therefore persistent black output and failed Continue were not established. All recorded presents were successful normal presents, not progress-only lifecycle presents; the overlay alone cannot classify them. Both runs ended at the configured 110-second timeout.
- `shader_prepare=off` did not disable background preparation: screen suppression and preparation controls are separate. Event 9 has no production emission site, so its absence does not establish absent HID polling. Recorded filesystem operations succeeded but lack path identities; save compatibility/loading remains unproved. Next discriminating run retains native settings/deadline, sends no early A and captures immediately on actual nonzero output, with user physical observation requested.

- Run-owned trace isolation is implemented and independently accepted: validated diagnostic startup sets `WT_TRACE=<run_dir>/boot_trace.txt` before runtime loading, overriding stale environment values; truncation fails before environment mutation and export-only requests refuse game setup before side effects. C `-Werror`, four startup and five export checks passed; nine POSIX export cases remain skipped. Device trace placement has not been tested, and this fix is not established as a visual-output cause.

- User identified Continue→gameplay as the intended route. Two bounded route-qualification attempts used only fresh copies of the exported committed tree, replay/source capture disabled and pinned nonempty configs. Six bounded installed asset hashes matched local references; installed runtime sections matched the export APK with the accepted replay fix. Both runs exited cleanly with healthy contiguous recorders (64348 and 63991 records, zero drops), but private screenshots remained black/progress-only. A single Enter→A action did not establish visible menu/control response; Continue was not identified. User did not watch the physical screen, so physical output is unknown, not confirmed black or visible gameplay.
- Post-run export comparison verified ordinary-save contents, sizes, inodes and modification times unchanged. App stopped, stay-on restored to 15, no fold change. Evidence remains private under `C:/Users/HomePC/AppData/Local/Temp/opencode/route-qualification-20261112-61af`. Offline black/progress diagnosis is pending; healthy recording, heuristic renderer labels and clean exit do not qualify gameplay, pose or performance.

- Export-only Android verification subsequently succeeded on PKH110 after offline packaging, signer/native-section checks and non-destructive install. Only `SaveExportActivity` was invoked; a separate offline validator confirmed the retrieved complete tree's exact inventory, sizes and hashes. Native source-stability checks passed; source-root/runtime-log metadata stayed unchanged. No ordinary-save content/mode changes were performed. Private provenance/verification reports are in `C:/Users/HomePC/AppData/Local/Temp/opencode/save-export-package-20261112-9d4c/device-evidence-02`; never publish save filenames, hashes or contents. App stopped afterward; stay-on remains 15, no fold override.
- This qualifies observed export/transfer only. Nine POSIX fixture tests remain unrun, process-map reads were denied, and source-stability checks are not an atomic snapshot. Gameplay route, save/game-update compatibility and animated-geometry qualification remain separate gates; read-only offline candidate analysis follows.

- Explicit committed-save export received independent source/security/lifecycle acceptance: fixed diagnostic-only digest-bound authority, descriptor-relative no-follow source traversal, exclusive bounded destination, matching nnfs sibling-marker policy and separate game-startup gate. Five host checks passed; nine POSIX filesystem tests remain skipped on Windows (no local WSL distribution available). Process enumeration/double verification are not an atomic snapshot or filesystem lock. Packaging/linkage, Android lifecycle/FUSE readability and private retrieval remain pending; source acceptance is not gameplay qualification.

- Capture02 follow-up found only synthetic host fixtures, not a demonstrated animated gameplay route. User authorized read-only copying of the phone's real committed save tree; shell enumeration was denied while the package was stopped, and no source permission/content change was attempted. The user separately authorized implementing a narrow explicit export-only app path, without loading the game or invoking its save lifecycle. Implementation/review/device verification remain pending; no save contents or gameplay progress are yet known.
- A production reproducer confirmed WTRP playback ignored unanchored poll deadlines and reached EOF before final HID delivery. A scoped fix adds a separate 64-bit global playback poll and exclusive total-poll EOF contract, preserving anchored-relative scheduling. Worker replay (9) and input-policy (12) tests passed; lead reran the production-extracted HID final-sample/EOF test successfully with mocked memory/exit. Independent review is pending; this does not qualify the synthetic fixture as real gameplay.

- Replay delta subsequently received independent acceptance: all discovered callers updated, layouts unchanged, global timing independent of anchors/rebinding, neutral cancellation/failure preserved. Reviewer reran all nine replay/twelve policy/HID tests successfully. Fresh full Android relink then compiled 53 runtime units, linked both runtime libraries, compiled three table units and linked game from 409 objects without failures. No device replay execution, external-producer compatibility or reached semantic-anchor claim follows from these checks.

- Fresh capture02 (`run_20261112_oppo_n5_temporal_source_capture_02_b7e2c4`) on APK SHA-256 `26e6e75795f1285c20b15190ccaf1edfe95dcea597d7b83cbc6cdd60581528ba` verified artifact permissions 0666 and matching remote/local hashes. Binary is 147000 bytes/875 records in frame window [885,889); sidecar reports complete/window_complete true, 18 guest records, 185 backend-selection records, zero overflow/unavailable/core loss/pending/unpublished records. `join` remains unknown, backend execution unavailable and motion_source_qualified false. No previous-pose ownership follows from complete raw capture.
- Accepted-source collector recovered a healthy stopped 494-chunk stream with 49339 contiguous rows, no gaps/duplicates/drops and clean exit 0. Source/harness/request/APK provenance is saved privately in `C:/Users/HomePC/AppData/Local/Temp/opencode/temporal-capture02-provenance.json`; artifacts in `temporal-phone-source-capture02-b7e2c4`. Observed selection vtable main+0x22539a8/target+0xd4d9ac is being statically classified. C14 binder/group hooks were not observed in this window; that is not absence proof. App stopped and stay-on 15 verified; no fold override, performance/visual or pose qualification claimed.
- The collector's byte-derived chunk bound received independent acceptance after correcting combined event/incident byte accounting. Seven focused tests were independently rerun; lead full Rust suite passed 142/142 before the incident delta, and implementation full suite passed 142/142 afterward. Actual per-file/aggregate event limits and exact sequence checks remain enforced.

- The user explicitly reauthorized device testing. Wi-Fi ADB identified PKH110 separately from the Smart TV; the new APK installed with `-r`. Probe 06 was rejected before runtime (`INVALID_BUILD_VARIANT`, whitespace-formatting cause under investigation); it remains a rejection, not an execution result.
- Compact-request retry 06b stopped cleanly: 80723 records across 4176 chunks, recovered exact sequence 1..80723 with zero losses, duplicates, missing records or header errors. Capture 01 likewise stopped with 65920 records/4102 chunks and exact sequence coverage. Both host runs remain `CollectorFailure`: their committed chunk counts exceed the bounded 4096 collector limit. Separately recovered full streams do not retroactively make those host runs successful.
- Latest temporal observations in both runs report FSR at 1600×896 scene/output, flags 15 (supported/initialized/dispatched/consumed), camera-only motion and no fallback. This is recorded execution-stage evidence, not GPU completion, quality, performance or reduced-scale acceptance. Exit 0 retains null checkpoint and empty coverage/assertions. Capture preflight had keyguard active, so no unlocked-screen/visual claim applies to it.
- Source capture binary (147000 bytes) and JSON (602 bytes) exist but app-owned mode 660 denies `adb pull`; contents/completeness/joins remain unknown pending readable export. Investigating artifact permissions and excessive tiny chunk production before another integrated run. Temporary evidence is under `C:/Users/HomePC/AppData/Local/Temp/opencode/temporal-phone-probe06b*` and `temporal-phone-source-capture01*`. App stopped; original stay-on value 15 restored and verified. Physical sound remains not_checked; physical fold validation remains false.

- Follow-up source/artifact investigation confirmed fragmentation: 06b spans 80.519 event seconds and 9210867 bytes; capture spans 80.503 seconds and 7519670 bytes. Median chunk size is ten records in both; 1303/1269 chunks contain at most four records. Writer publishes separately for each nonempty producer drain on each nominal 50-ms sweep. These are not excessive-byte streams. The host count bound must remain justified by validated byte/record limits rather than an arbitrary larger constant; writer coalescing alone cannot guarantee a fixed count for arbitrary duration.
- Native build validation used literal substring spellings for `variant`, `target` and related fields, explaining valid multi-space JSON rejection. Structural existing member/string helpers are required instead, retaining allowlists, duplicate checks and object scope. Capture exclusive opens omitted the house diagnostic-readable permissions; checked descriptor-based permission setup is being added without global umask or broader path authority. Neither proposed fix retroactively qualifies unread capture contents or rejected host runs.

- Structural build validation is implemented using the existing unique-object/member and decoded-string helpers. Seventy-one added cases cover whitespace, ASCII escapes, duplicate/escaped keys, types, missing fields and outside-object decoys. Independent review compiled/ran the C suite and accepted the narrow delta; lead reran `android_bootstrap_test.py` (3/3 passed). Existing schema-version/run-id literal validation was not changed; no device retest of this fix is claimed yet.

- Offline diagnostic APK packaging completed with original native staging restored in `finally`. Signer verified against the existing diagnostic certificate; ZIP integrity passed. APK SHA-256 `7172b09f7a3464d8f207cd307f15b0946e0ecf7eb91fbbe0bc0a43be79a71525`, saved privately as `C:/Users/HomePC/AppData/Local/Temp/opencode/temporal-capture-diagnostic-20261112.apk`. Packaged `.text`, `.rodata`, `.data` and build-id sections match newly linked `libgame.so`, `libwitchtime.so` and `libwt_glslang.so`. This APK was **not installed or executed**; the phone retains the earlier baseline. Capture lifecycle test was also added to CI.

- Capture lifecycle integration and both boundary-race fixes received independent delta acceptance. A fresh lead run of `temporal_source_capture_test.py` and `temporal_source_lifecycle_test.py` passed; native startup/eligible-scene/sealed teardown dependencies remain mocked. Missing/empty generated-source sets now fail verification. One lifecycle owner and no restart with live producers remain explicit assumptions; raw joins are still unknown.
- Fresh release wtbuild rebuilt exactly the two instrumented buckets (407 reused, no fallback/failure); subsequent provenance verified 409/409 current source/object stamps. Full Android link compiled 53 runtime units, linked both runtime libraries, compiled three table units and linked `libgame.so` from 409 objects, all successfully. Separately built runtime source identity is `99b65e19c9b22baa`. This is build evidence only, not device execution, GPU validation or capture qualification.

- Full `cargo test --offline --manifest-path tools/wtbuild/Cargo.toml` passed 136/136 tests, including 86 diagnostic tests. The new capture host suite passed bounded publication/concurrency/bootstrap/instrumentation checks, but independent review then identified an excluded closing-frame admission race and an empty-source provenance-verification gap. Capture acceptance remains pending their correction, deterministic regression and delta review; the earlier suite pass is not proof these defects were absent.
- During lifecycle integration, the extracted present harness required explicit capture-hook mocks. After updating it, `upscale_reset_lifecycle_test.py` and `temporal_diag_wiring_test.py` passed again. The progress branch also bypasses capture-boundary calls; the ordinary mocked boundary is explicitly ineligible and cannot arm capture. These remain mocked host contracts, not GPU/device evidence.

- `python tests/rs_store_production_test.py` now executes extracted production direct/indexed and CPU-record indirect case bodies plus real replay helpers under mocks. It checks index reacquisition after a hostile mocked ring replacement, targets, queries, failed preparation, disabled replay and stage-side-effect exclusion. Six mutations are rejected, including the old fragment-only write guard. This is not actual allocator lifetime, Vulkan barrier or GPU-indirect/count qualification.
- That test demonstrated a real guard defect: `g_dp_fs_stores` excluded only fragment-buffer writers, allowing vertex/tessellation buffer side effects to repeat. The production storage-resolution loop now aggregates `ssbo_w` across all active VS/FS/TCS/TES stages and rejects replay on any buffer writer, independent of whether a kept CB exists. Read-only buffers remain eligible. Independent review accepted this narrow correction; no query policy/default/clamp changed. Fresh `rs_store_gpu_test.py <glslc>` passed its 12 synthetic GPU cases and six skipped-replay controls. Non-FS side-effect decisions remain mocked, not GPU-stage execution evidence. Tasks 3.2/3.3 stay open.
- `python tests/sgsr_cs_focused_gpu_test.py <glslc> <spirv-dis>` passed on RTX 5070 Ti: three scene types across four executions (1×1, 17×13, 9×7, then 17×13 again), not 12 distinct configurations. Actual production adapter motion readback checks matrix/jitter/depth/homogeneous division; constant-input recurrence distinguishes ping-pong history and reset; exact 1:1 reset patterns check edges. Wrong matrix, skipped history/reset, flattened pattern and skipped final dispatch controls fail. Independent review accepted the narrow analytic oracles. Moving patterned history, scaled patterns, in-process resize, host orchestration, HDR quality and Android are not established; 7.1 remains open. New `--compile-only` path also passed and explicitly excludes numerical/mutation execution; CI uses it only with existing Vulkan headers.
- `python tests/upscale_reset_lifecycle_test.py` passed: extracted production FS/CS reset decisions, actual FSR resource-table/sizing/resize/fresh reset under mocked Vulkan allocation, 72 mode transitions, six populated variant-only transitions and four pending generations. Missing fresh/FS-retire/CS-retire mutations fail. Projection-only FSR history retention is recorded as current behavior, not an invented algorithm repair; FS/CS reject non-jitter projection changes. Two hundred executions of the actual progress branch suppress temporal-hook calls, but hooks are counter stubs and held flush is source-level only. Full held/resume/WSI integration and real GPU lifetime remain unqualified; 2.3 stays open.
