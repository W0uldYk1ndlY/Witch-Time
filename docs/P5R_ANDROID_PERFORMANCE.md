# P5R-inspired Vulkan hot paths: current-branch adaptation

Adapted from the supplied `witch-time-p5r-performance.patch` to
`android/oppo-find-n5` at `b44aa6a`, preserving existing local changes.
The donor series ends at
[`c6595ff`](https://github.com/Chakeyy/p5r-android/commit/c6595ff49206fa3eba09f030170350a3070f228d).
This is not a wholesale cherry-pick of the donor's C++ runtime.

## Existing functionality retained

The current branch already has exact per-command-buffer descriptor reuse,
vertex-layout/buffer shadows, layout-aware push suppression, descriptor-bind
batching and push-constant shadows in `runtime/nvn_vk_draw_submit.h`.
Those helpers remain the single implementation. Their cache capacities,
full-field equality, pool-generation ownership, fallback paths, invalidation
sites and `WT_VK_BUFFERSETS` / `WT_VK_DRAWSHADOW` switches are retained.
The patch's duplicate descriptor-cache header and obsolete plan/spec are not
imported. Its `WT_VK_BSETCACHE` switch is not introduced.

## Missing optimizations added

- Changed adjacent vertex-buffer slots are bound as groups; unchanged slots,
  including slots retained across a smaller binding list, remain bound.
- Index handle/offset/type are shadowed for direct, full-store replay and
  indexed indirect draws. Existing `dsh_forget()` sites invalidate this state.
  `WT_VK_DRAWSHADOW=0` disables both binding optimizations.
- Exact buffer descriptors have a last-resolved-set shortcut before hashing.
  Both ordinary table hits and saved entries promote it; generation changes,
  including wrap and buffer destruction, forget it.
- Graphics pipeline lookup keeps four recent ready, used `Pipe` objects,
  comparing the entire key. Compilation, first-use and worker slow paths remain.
- Samplers use a 4,096-slot collision-checked index into the existing 2,048-entry
  array. Settings flushes clear the index; old handles keep their existing lifetime.

No guest memory sampling, coherency/barrier changes, resource lifetime changes,
quality reductions, new dependency or Android UI setting is introduced.

## Focused validation

Run with a host C compiler and Vulkan headers:

```sh
python tests/pipeline_mru_test.py
python tests/vulkan_binding_cache_test.py
python tests/vulkan_descriptor_cache_test.py
python tests/vulkan_sampler_cache_test.py
python tests/pass_timestamp_lifecycle_test.py
python tests/rs_store_production_test.py
python tests/vulkan_draw_submission_test.py
```

The adapted binding/descriptor tests reuse the existing production-helper
fixture, not a duplicate implementation. They add sparse-slot/index trace
checks, 2,000 varied draws, and last-resolved-set promotion/generation checks
to existing identity, collision, saturation, template and rollback assertions.
Set `CC`, `VULKAN_INCLUDE_DIR`, and the sampler harness's `CPPFLAGS` as needed.

The supplied patch's historical validation, sanitizer, full translation-unit
compilation and operation-count claims are **not** validation of this adaptation.
Current command results are recorded in the applying session's verification log.
No Android build, actual Vulkan execution, image comparison, device FPS or
hardware acceptance gate is established by these host checks.
