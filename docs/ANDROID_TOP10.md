# Android runtime: ten improvements and verification

Date: 2026-11-15. Source branch: `android/oppo-find-n5`.
Exact base: `579ab5d307ed2bf5f6dd500d0a125521eae1591b`.
Delivery branch: `improve/android-top10-20261115`.

This branch repairs observed defects and reduces avoidable work in the existing
ARM64 runtime. It includes source changes, regression tests and CI coverage.
The Android runtime was cross-built with NDK r28c; no game dump or physical Oppo
device was available. This is not a playable APK or a measured FPS result.

## Implemented changes

| # | Change and production location | Why it matters and what was verified |
|---|---|---|
| 1 | Repair the existing temporal-capture v2 interface in `runtime/temporal_source_capture.c` and `runtime/temporal_source_guest.h`. | The base called a five-argument bind interface with four arguments and could not compile in isolation. Binding now carries a nonzero session epoch; teardown clears the binding, and callbacks recheck their epoch after admission. Delayed callbacks from an old run cannot contaminate or invalidate a new run. Tests exercise concurrent publication, restart races, permissions and lifecycle boundaries. |
| 2 | Remove signed overflow in SQDMULH/SQRDMULH by-element helpers in `runtime/rt_ext.h`. | Doubling `INT32_MIN * INT32_MIN` forms 2^63 before saturation. The intermediate product and rounding increment now use `__int128`, narrowing only after the high-half shift. An independent integer oracle checks 362,952 cases; the old calculation fails UBSan. |
| 3 | Add native integer SIMD for selected Android AArch64 forms in `runtime/neon.h`. | ADD/SUB/MUL, equality, signed/unsigned comparisons and CMTST use compiler vectors for 8/16/32-bit lanes in 64/128-bit vectors. All 54 accelerated forms produce smaller packed AArch64 code. Operand aliases, eight-byte alignment and inactive-lane zeroing are checked. `WT_NATIVE_INT_SIMD=0` keeps the scalar fallback testable. Desktop defaults, scalar forms, 64-bit lanes and floating-point behavior are unchanged. |
| 4 | Preserve allocator free ranges under fragmentation in `runtime/nvn_vk_draw.c`. | The old fixed 512-entry free list silently discarded ranges when full. Metadata now grows before handing out an allocation and reserves enough capacity to free every live owner without allocating metadata during free. A 1,200-allocation regression previously lost 22,784 bytes; the corrected allocator loses zero and retains all 601 free intervals. |
| 5 | Bound the warm Vulkan block pool in `runtime/nvn_vk_draw.c`. | Empty ordinary backing is limited to one reusable block per memory type and allocation class; dedicated allocations are released. Three empty 64 MiB blocks reduce to one. Repeated primary-to-fallback transitions reuse eligible ranges before allocating backing, preventing ownerless empty blocks from accumulating. Existing retirement/fence call sites are preserved. |
| 6 | Make Vulkan allocation respond to UMA budget pressure and actual OOM in `runtime/nvn_vk_draw.c`. | Budget checks now apply even when no alternate heap exists. The allocator trims only empty blocks and retries smaller adequate backing. Driver budget remains advisory: a required-size real allocation can still succeed beyond the estimate. Cached fallback preference cannot suppress a final primary attempt. The explicit synthetic VRAM cap keeps its diagnostic behavior. |
| 7 | Validate optional driver and SPIR-V caches in `runtime/vk_cache.h` and active code in `runtime/nvn_vk_m4.c`. | Bounded complete reads reject oversize, partial and malformed input. Driver cache header/version/vendor/device/UUID must match. SPIR-V checks cover magic, version, alignment, bound/schema fields and instruction boundaries, including offline preparation. Invalid input becomes a cache miss. This is structural validation, not a replacement for full SPIR-V semantic validation. |
| 8 | Make pipeline-cache persistence transactional in `runtime/nvn_vk_m4.c`. | Saved dirty generations advance only after successful write, close and atomic replacement. Failed writes preserve the previous cache and queue pending preparation markers again. A busy writer cannot lose a marker request; shutdown waits for the actual writer to finish. If deletion of a stale completion marker fails, partial cache persistence is suppressed until a full preparation completes, preventing a false completed state on the next startup. |
| 9 | Bound and validate prewarm record loading in `runtime/nvn_vk_m4.c`. | Record files must be within a fixed limit, record-aligned and completely read. Allocation/read failures safely produce an empty prewarm list. Sorting is guarded for empty input; existing deduplication, record feature filtering and compute-kind checks remain active. |
| 10 | Use `-O2` for Android native runtime and generated tables in `tools/wtbuild/src/link.rs`. | Debug information, `-fno-strict-aliasing` and ordinary floating-point semantics remain enabled. Desktop stays on its existing optimization policy. Prebuilt manifests record the runtime flags; an old source-identical `-O1` Android prebuilt is rejected and rebuilt. The Rust regression covers this cache-provenance case. |

Cache size limits are 1 GiB for the driver cache, 16 MiB for one SPIR-V module
and 64 MiB for prewarm records. The driver limit accommodates the existing large
desktop cache. These are rejection limits, not allocations made at startup.

Existing FSR/SGSR, shader prewarming, graphics pipeline libraries and render-thread
execution are already in the base and are not counted as new improvements.
Resolution, frame cap, audio, user configuration, saved games and experimental
renderer defaults are not changed by this branch.

## Measured results and their limits

The following are reproducible helper/allocator measurements, not whole-game
performance measurements.

| Measurement | Before | After |
|---|---:|---:|
| Fragmentation fixture: lost reusable bytes | 22,784 B | 0 B |
| Backing after retiring three ordinary 64 MiB blocks in one pool | 192 MiB | 64 MiB |
| Byte-vector ADD, 16 lanes, AArch64 wrapper instructions including RET | 73 | 5 |
| Byte-vector signed greater-than, 16 lanes | 92 | 5 |
| Halfword-vector MUL, 8 lanes | 36 | 5 |
| Word-vector ADD, 4 lanes | 12 | 5 |
| Word-vector CMTST, 2 lanes with cleared upper half | 11 | 6 |

Instruction counts use identical wrappers and NDK r28c Clang 19.0.1 with
`-O2 -ffp-contract=off -fno-strict-aliasing`, the production freestanding headers
and always-inline helper policy. They demonstrate reduced helper code, without
claiming a corresponding FPS multiplier. The untouched 64-bit-lane helpers
already compiled efficiently.

OOM fixtures verify a 64 → 32 → 16 → 8 MiB retry sequence, with success at 8 MiB;
a cached alternate-heap preference followed by alternate OOM still makes one
required-size primary attempt. Repeated fallback/reset cycles retain 128 MiB
while owners remain live and one 64 MiB warm block after full retirement.

## Verification record

Toolchain: Linux x86-64, Rust 1.100, Android NDK r28c (`28.2.13676358`), API 29,
Clang 19.0.1 for Android. Pinned Android SDL 3.4.18, FFmpeg 7.1.3 and glslang 15.1
dependencies were built; their shared-library names and 16 KiB alignment passed
the dependency recipe's checks.

| Check | Observed result |
|---|---|
| Rust `wtbuild` unit/regression suite | 145 passed, zero failed. |
| Full dump-free Android runtime build after allocator/cache corrections | 54 translation units compiled, zero failures; `libwitchtime.so` and `libwt_glslang.so` linked with exit 0. Runtime source identity `8400ecf374ad1c8b`. |
| Android freestanding comparison, 60,000 input words, seed 1 | 18,337 lifted instructions; 14 files compiled with both header environments; zero differing objects. |
| Android synthetic link smoke | Five generated C files and three table units compiled; stand-in `libgame.so` linked successfully as AArch64 ELF64 against the final prebuilt runtime. It was not run or packaged. |
| Saturating multiply, GCC `-O2` and UBSan | 362,952 exact cases passed. |
| Integer SIMD, GCC `-O2` and UBSan | 1,083,600 exact cases per backend, both scalar and native passed. Clang-instrumented checks also passed using GCC's sanitizer link runtime. |
| NDK AArch64 code-generation check | All 54 accelerated forms use packed instructions and shrink; Android default matches explicit enable. |
| Actual NDK-built AArch64 ELF under Unicorn 2.1.4 | 73,805 cases per backend; 147,610 total passed against independent Python integer oracles, including source/destination aliasing and guard/inactive bytes. |
| Extracted production Vulkan allocator | 15 scenarios passed; GCC ASan/UBSan run also passed. LeakSanitizer was disabled because this executor uses ptrace. |
| Extracted production cache handling with temporary files and injected failures | 25 tests passed, including corrupt headers, failed allocation/read/write/close/rename, writer overlap and stale-marker reload. |
| Temporal capture | Capture, lifecycle, concurrency, permissions, bootstrap and instrumentation checks passed. The stale-epoch race regression fails when the post-admission check is removed. |
| Repaired baseline host fixtures | Android bootstrap (5), Vulkan-only build (2) and native-driver restart (5) passed. Repairs add missing host declarations/mocks and preserve the existing assertions. |
| Selected existing regression suites | All 17 commands passed: Android package/bootstrap, flash policy, Vulkan-only, compute coherency, ordered helpers, producer wait, soft write-watch consumer, upscale retirement/reset, renderer/native-driver restart, temporal capture/lifecycle/wiring, treatment controls and load-clear commands. The ordered-helper command required explicitly selecting NDK Clang and llvm-objdump on this host. |

The allocator and cache harnesses execute extracted production code with a
deterministic mock driver; they are not tests of a phone's Vulkan implementation.
The existing renderer-restart entry-coverage test passes, while its Windows-only
production-gate test is skipped on this Linux host. Physical sound, fold/unfold,
gameplay, thermal behavior and frame pacing remain unverified in this session.

CI adds host arithmetic/allocator/cache checks and a pinned-NDK job that checks
packed code generation and executes AArch64 helper binaries. Local equivalents
have run; a hosted GitHub Actions run is not claimed.

## Research basis

These projects were studied for relevant implementation and verification
patterns. Their code was not copied into this branch and no new production
dependency was added. The implementations fit the existing C/Vulkan runtime.

| Project / source | Revision or primary reference | Applicable lesson |
|---|---|---|
| XenonRecomp | [`ddd128bcca99fe8bfbb99bea583c972351fa6ace`](https://github.com/hedge-dev/XenonRecomp/tree/ddd128bcca99fe8bfbb99bea583c972351fa6ace), especially `XenonRecomp/recompiler.cpp`, `test_recompiler.cpp` and `XenonUtils/ppc_context.h` | Explicit SIMD emission and independently executable instruction fixtures informed changes 2–3 and their tests. |
| UnleashedRecomp | [`cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c`](https://github.com/hedge-dev/UnleashedRecomp/tree/cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c), `UnleashedRecompLib/config/SWA.toml` and build configuration | Target-specific optimization must preserve guest semantic constraints; broad register-cache/FP-mode changes were not enabled without game evidence. |
| N64Recomp | [`598ce2080004a5672d604dceadbda436045307b7`](https://github.com/N64Recomp/N64Recomp/tree/598ce2080004a5672d604dceadbda436045307b7), `LiveRecomp/live_recompiler_test.cpp` | Compare emitted instructions against independent expected behavior, including machine-code execution rather than host-only compilation. |
| Zelda64Recomp | [`b65c482ed672258eb684ab94a4e8c8aa662615ee`](https://github.com/Zelda64Recomp/Zelda64Recomp/tree/b65c482ed672258eb684ab94a4e8c8aa662615ee), `patches.toml` | Scope target-specific patches explicitly; a working port's game-specific fixes are not universal fixes for another game. |
| Ship of Harkinian / libultraship | [Shipwright `d4bf6528a3bb242bc5d862e4f1ab2b6d385c157b`](https://github.com/HarbourMasters/Shipwright/tree/d4bf6528a3bb242bc5d862e4f1ab2b6d385c157b), `soh/soh/Extractor/Extract.cpp`; [libultraship `da6daf1a50a5e46d6641408d52227e31e85e46df`](https://github.com/HarbourMasters/libultraship/tree/da6daf1a50a5e46d6641408d52227e31e85e46df), `src/ship/resource/ResourceLoader.cpp` | Validate binary inputs at their boundary and keep source-only verification independent of private game data. |
| Dolphin Vulkan backend | [`ObjectCache.cpp`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/Vulkan/ObjectCache.cpp) | Validate pipeline-cache identity before passing optional cached data to Vulkan. |
| Vulkan Memory Allocator | [`vk_mem_alloc.h`](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/include/vk_mem_alloc.h), `VmaBlockVector::Free`; [budget guidance](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/staying_within_budget.html) | Retain a bounded warm block, shrink backing under pressure and treat budget as an estimate. This is a memory allocator reference, not a game port. |

Format rules additionally follow Khronos's
[`VkPipelineCacheHeaderVersionOne`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheHeaderVersionOne.html)
definition and the [SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html).
Android memory choices were checked against the
[NDK Vulkan design notes](https://developer.android.com/ndk/guides/graphics/design-notes).

## Reproduce the checks and rebuild

Use the toolchain and pinned dependency recipe in [`android/README.md`](../android/README.md).
Set `ANDROID_NDK_HOME` to r28c and source
`deps/android-arm64/android-env.sh` before the Android commands. Host-only `CPATH`
entries must not leak into the Android compiler's include search.
The host cache harness requires Vulkan development headers (for example,
`libvulkan-dev` on Ubuntu, as installed by CI); the Android dependency environment
alone does not configure the host compiler's header search.

```sh
cargo test --locked --manifest-path tools/wtbuild/Cargo.toml
python3 tests/vulkan_allocator_test.py
python3 tests/vulkan_cache_test.py
python3 tests/temporal_source_capture_test.py
python3 tests/temporal_source_lifecycle_test.py
```

The exact arithmetic compiler commands are in `.github/workflows/wtbuild.yml`.
The following tests require NDK r28c; the machine-code test also requires the
test-only packages `unicorn==2.1.4` and `pyelftools==0.33`:

```sh
python3 tests/native_integer_simd_codegen_test.py
python3 tests/native_integer_simd_arm64_test.py
cargo run --locked --manifest-path tools/wtbuild/Cargo.toml -- \
  runtime --target android-arm64 --out prebuilt/android-arm64
tools/wtbuild/target/debug/wtbuild check-freestanding \
  --target android-arm64 --words 60000 --seed 1 --jobs 4
tools/wtbuild/target/debug/wtbuild smoke \
  --target android-arm64 --link-only --prebuilt prebuilt/android-arm64
```

The smoke command links a synthetic stand-in only. Never package its output as a
game. A personal playable build requires recompiling and linking the real game
objects using the owner's own dump and the normal Android build instructions.
Header changes affect guest helpers, so refreshing only `libwitchtime.so` is
insufficient to obtain all ten changes; rebuild the generated game objects too.

Actual device acceptance should compare the same scene, resolution, driver,
thermal state and preparation state, recording frame-time distributions, memory
and startup behavior. No device benchmark or Android lifecycle acceptance gate
is closed by the host results above.
