# Archive integration — 2026-11-15

Integrated into `android/oppo-find-n5` from the user-provided archives:

- `witch-time-vulkan-oppo-20261115.zip`: branch `perf/vulkan-oppo-20261115`,
  tip `968f5b6a9d8347804f487a2fcd8b3e4be8aa76c2`, merge `38567f3`.
- `witchtime-android-top10.zip`: branch `improve/android-top10-20261115`,
  tip `a284fe5303683a116cef4d7618613e8381a4c6e4`, merge `e401bd5`.
- Follow-up integration/fixture corrections: `33b429e`.

Both bundles passed `git bundle verify`; every delivered SHA256SUMS entry
matched. Archive base `579ab5d` and original local HEAD `6e1db93` have identical
trees despite different commit histories. The first merge's tree was verified
identical to the Vulkan branch; the second combines actual overlapping changes.
Bundled native binaries were not installed, packaged or copied into the repo.

## Resolution invariants

- Keep top10 allocation metadata growth, reclamation, fallback and UMA retries;
  retain Vulkan branch's `cls != 0` mapping guard, including dedicated images.
- Retain compiler/UUID cache identity, atomic unique temporary publication,
  successful-write dirty checkpoints, strict prewarm records and top10's stale
  completion-marker protection and shutdown writer join.
- Retain both cache suites as `vulkan_cache_test.py` and
  `vulkan_cache_recovery_test.py`. Recovery tests inject reported oversized file
  lengths instead of creating gigabyte files; pipeline input keeps a compatible
  header so the size rejection is actually exercised.
- Original uncommitted work was saved in retained stash
  `e365133dc73d427ac68f3972dfa61c623b767b2a`, restored, and left uncommitted.
  Its positive temporal triggers, sampler evidence, power controls, diagnostic
  runner, storage snapshots and velocity replay lifetimes were preserved.
  Admission now protects guest validation/copying and rejects stale malformed
  callbacks before dereferencing them; a callback contributes one writer count.

## Verification performed here

These are fresh local checks, separate from the archives' historical reports.

- Committed source export after `33b429e`: NDK r28c Android runtime compiled all
  54 files, zero failures; `libwitchtime.so` and `libwt_glslang.so` linked.
  Source identity: `ee7367dcbc122ca0`.
- Working tree including restored local work: the same Android build compiled
  54 files, zero failures, both libraries linked; identity `cbc5f58119859b5b`.
- Working-tree `cargo test --offline --manifest-path tools/wtbuild/Cargo.toml`:
  174 passed, zero failures.
- Whole working-tree `runtime/nvn_vk.c` GCC syntax check passed.
- Production allocator: 15 scenarios passed. Memory barrier, draw submission,
  indirect dispatch (15 cases), temporal capture/concurrency/lifecycle/sampler,
  temporal diagnostic wiring, package, native restart, RS store production,
  timestamp lifecycle, upscale retirement/reset, runtime power workers/pacing
  and storage capture host checks passed.
- Integer SIMD: 1083600 exact host cases passed for each of scalar and native
  backends; saturation test passed 362952 cases. NDK codegen checked 54 forms.
- Independent read-only review accepted the resolved runtime integration.

## Explicit limits

Cache fixture execution is not qualified on this Windows host: the imported
POSIX-oriented fixtures need platform support. Pipeline cache fixture compilation
was attempted and failed at missing Windows stub declarations, not production
runtime compilation. Texture-upload and Android-bootstrap host fixtures also
encountered POSIX/MinGW harness incompatibilities. Remaining cache/concurrency/
shader-identity and upload/bootstrap fixture execution should run on Linux.
Shader-source CLI checks without the required shader-tool arguments and the
Windows TLS CLI check without its required arguments are not passing evidence.

No game-library rebuild, playable APK, device installation, gameplay, image
equivalence, FPS, physical sound or fold validation was performed. Git changes
remain local; no push was made. Do not mix these newly built runtimes with old
game libraries as evidence of pair compatibility.
