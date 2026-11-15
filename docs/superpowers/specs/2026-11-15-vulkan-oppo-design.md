# Vulkan backend efficiency on Oppo Find N5

## Intent and constraints

Implement the user's requested Vulkan backend optimizations on top of
`android/oppo-find-n5` at `579ab5d307ed2bf5f6dd500d0a125521eae1591b`.
Reduce CPU recording, CPU/GPU round trips and upload traffic while preserving
image data, rendering order and all game-visible synchronization.

The user authorizes implementation and asks for maximum performance without
image-quality loss. Resolution, texture precision, shader effects, draw order,
occlusion workarounds, saves and existing graphics preferences must remain
unchanged. Unsupported or ambiguous fast paths use the existing correct path.
Host tests and cross-compilation cannot establish phone FPS or visual parity.

## Existing implementation

The renderer already has a mapped coherent stream ring, retained compute
buffers, GPU indirect draws/counts, texture descriptor caching, dynamic-state
shadowing, ASTC hardware format selection, SPIR-V files, graphics pipeline
libraries, warmup and a driver pipeline cache. These are baseline behavior,
not new features of this change.

Remaining concrete costs include CPU decoding of indirect dispatch arguments,
all-command image barriers, intermediate texture deswizzle buffers followed by
another copy into the upload ring, repeated buffer descriptor updates and
unchanged draw bindings. Cache readers/writers also need complete-file and
compiler-identity checks to avoid unusable artifacts and needless rebuilds.

## Approach

Use bounded improvements to the existing renderer with behavior tests and an
independent final review. Each fast path preserves original operands and falls
back when eligibility cannot be established. This is the implementation choice
made for the user's stated goal.

Alternatives considered:

- A GPU-driven scene renderer that reorders or merges arbitrary game draws:
  needs semantic knowledge beyond this NVN translation layer; storage effects,
  blending and draw IDs prevent a blanket transformation.
- Zero-copy imports of arbitrary guest memory: shared physical RAM alone does
  not establish Vulkan import, alignment, lifetime or host-cache guarantees.
  Direct deswizzle into an already mapped coherent Vulkan allocation removes
  a known redundant copy without assuming such guarantees.

## Components

1. **GPU command consumption:** Keep eligible live compute-written indirect
   dispatch arguments on the GPU. Separate ordinary indirect-draw capability
   from draw-count capability. Preserve preparation-time relocation and the
   CPU diagnostic path. Cover read/write dependencies explicitly.
2. **Image synchronization and memory:** Scope known image layouts to their
   actual producing/consuming stages; keep conservative scope for GENERAL and
   unknown layouts. Preserve same-layout write-after-write dependencies and
   reused-image execution ordering, including transitions from UNDEFINED.
   Audit allocator correctness and selection on coherent device-local memory.
3. **Texture uploads:** Permit the front end to deswizzle directly into a
   backend-reserved mapped destination. Native ASTC blocks remain identical;
   conversions and unsupported destinations retain the existing path. Preserve
   mip, layer, 3D, stride and ring-wrap behavior.
4. **Draw recording:** Reuse buffer descriptor sets only for exact keys within
   their command-buffer/pool lifetime. Suppress identical bindings and push
   constants only while the shadow remains valid. Invalidate across internal
   rendering paths and command-buffer reuse. Do not drop or reorder draws.
5. **Shader and pipeline caches:** Validate bounded complete cache files,
   atomically replace them, include compiler settings in identity and validate
   Vulkan pipeline headers against device/driver identity. A failed save remains
   dirty. Cache misses compile the same shaders; no placeholder rendering.

## Validation

Add executable tests of production functions and recorded Vulkan calls for the
changed branches. Compare direct-upload bytes with the original deswizzle path
on synthetic textures. Run existing coherency, synchronization, clear, lifecycle
and renderer regressions. Cross-compile affected runtime sources for Android
ARM64 with NDK r28c and build the runtime when dependencies permit. Record
physical-device visual parity, validation layers and frame-time comparison as
open gates until measured on an authorized connected phone.
