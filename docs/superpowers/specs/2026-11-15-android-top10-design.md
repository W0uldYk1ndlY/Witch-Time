# Android runtime: ten source-backed improvements

Date: 2026-11-15. Base: `579ab5d307ed2bf5f6dd500d0a125521eae1591b`.
Branch: `improve/android-top10-20261115`.

## Intent and constraints

The requested deliverable is an updated Android port branch, informed by open
ports/recompilers and containing ten useful implemented improvements. Existing
FSR/SGSR, render-thread execution, shader prewarming, graphics pipeline libraries,
write-watch experiments and load-clear folding are already present and do not
count as new work. Select actual defects and avoidable work in this source.

Target remains ARM64 / NDK r28c / API 29+. No game dump, game-derived library,
assets, saves or private run artifacts may be committed. Do not change resolution,
frame cap, audio, draw-skipping policy, user settings, GPU requirements or the
default status of experimental renderer treatments. Preserve desktop behavior
unless fixing a shared correctness defect. Host checks are not device acceptance;
physical sound/fold and performance remain unverified without the target device.

## Approaches considered

1. Add large features (new upscaler, register-cache lifter, guest FP environment,
   direct NVN replacement). These require game evidence and broad semantic work;
   existing upscalers also make several of these duplicates.
2. Enable aggressive settings. This may trade quality/correctness for speed and
   does not establish a sound implementation improvement.
3. Repair confirmed defects and optimize bounded, testable hot paths. **Selected
   by the implementer** for this branch: each change has an observable contract,
   a conservative fallback and an independent regression.

## Ten changes

1. Repair the temporal capture v2 bind/emit ABI, including epoch isolation.
   The baseline cannot compile: the public callback takes five arguments but
   the recorder calls it with four. Retain bounded recording and distinguish
   stale callbacks from current-session evidence.
2. Fix signed overflow in SQDMULH/SQRDMULH by-element helpers. The product of
   two `INT32_MIN` values, doubled, is 2^63; the current int64 intermediate is
   undefined. Use exact wider arithmetic and preserve saturation/rounding.
3. Add narrow native integer SIMD paths only where ARM64 code generation improves.
   Preserve source/destination aliasing, inactive-lane zeroing and scalar fallback.
   Floating-point approximation/NaN behavior is outside scope. If the compiler
   already generates equivalent code, record the evidence and choose another
   source-confirmed optimization rather than adding redundant intrinsics.
4. Prevent GPU allocator range loss when its 512-entry free list fills. Returning
   a live allocation must retain its entire range; metadata failure must fail
   safely rather than silently discard reusable capacity.
5. Reclaim unused ordinary Vulkan blocks with a bounded warm-block policy.
   Existing retirement/fence ownership must continue to decide when freeing is
   safe; no live allocations or in-flight resource memory may be freed.
6. React to UMA memory pressure before allocating another 64 MiB block. Query
   available budget even without a second heap, trim only empty blocks and use
   a smaller adequate allocation/retry where appropriate. Driver budget is an
   estimate, not a reason to reject an otherwise valid required allocation.
7. Validate cache inputs before handing them to Vulkan: driver cache identity
   (header/vendor/device/UUID), bounded lengths and complete reads; SPIR-V
   header/alignment validation. Invalid caches become misses and are regenerated.
8. Make pipeline-cache writes transactional: failed close/rename/readback must
   preserve the previous cache and dirty checkpoint; overlapping writer activity
   must not falsely mark additions as saved or lose pending preparation markers.
9. Bound and validate pipeline prewarm record loading before allocating/sorting.
   Reject partial/oversized input, handle allocation/read failure as an empty
   prewarm list and preserve existing record deduplication/feature filtering.
10. Build the Android runtime with `-O2` while retaining debug information,
    `-fno-strict-aliasing` and normal floating-point semantics. Keep existing
    desktop flags; ensure source/build identity captures this policy.

## Validation

Use production functions in host harnesses where Vulkan needs a GPU/device.
Exercise fragmentation, empty-block ownership, simulated allocation pressure,
cache corruption and failed writes. Use exact arithmetic oracles/UBSan and
ARM64 cross-compilation for CPU helpers. Run the existing affected tests and
Rust suite. Build the dump-free Android runtime if dependencies are available;
never substitute a synthetic game library into a distributable APK.

External reference evidence and actual verification results will be recorded in
`docs/ANDROID_TOP10.md`. No numeric FPS claim will be made from source analysis.
