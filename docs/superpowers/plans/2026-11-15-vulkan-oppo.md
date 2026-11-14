# Vulkan Oppo Efficiency Implementation Plan

> **For agentic workers:** Use the assigned isolated task scopes and the
> superpowers execution, TDD and verification workflows. Changes share one
> worktree; do not rewrite another worker's functions.

**Goal:** Reduce Vulkan translation overhead without changing image quality.

**Architecture:** Extend the existing renderer with exact-key reuse, direct
mapped uploads and GPU-side indirect command consumption. Preserve the old
path whenever the fast path cannot prove its preconditions.

**Tech Stack:** C, Vulkan 1.3, glslang, Python/C host harnesses, Android NDK r28c.

**Spec:** `docs/superpowers/specs/2026-11-15-vulkan-oppo-design.md`

## Global Constraints

- Start from upstream `579ab5d307ed2bf5f6dd500d0a125521eae1591b`.
- No image-quality, rendering-order, save-data or default-resolution changes.
- Keep coherent memory and GPU lifetime requirements explicit.
- Never claim phone performance or visual parity from mocks or host tests.
- Do not modify the separate, dirty `improve/android-top10-20261115` worktree.

## Review Focus

- A ring wrap or draw preparation relocates indirect or descriptor buffers.
- A new command buffer/pool reset invalidates recorded state and cached sets.
- Internal upscalers, stencil clears or compute operations disturb bindings.
- ASTC mips/layers and 3D uploads preserve exact bytes and subresource metadata.
- Corrupt/truncated/stale cache files rebuild safely and failed writes retry.

### Task 1: GPU indirect dispatch and capability separation

**Files:** `runtime/nvn_vk_draw.c` indirect/compute sections; dedicated tests.
**Interfaces:** Existing `CB`, `BufRef`, `indirect_src`, `dispatch` and command
execution. Preserve exported front-end command formats.

- [x] Write and run failing command tests for GPU dispatch argument selection,
  preparation relocation, capability fallback and compute visibility.
- [x] Add `vkCmdDispatchIndirect` routing with exact dependency handling;
  decouple record-only draws from optional count commands.
- [x] Run focused tests plus existing compute coherency/producer wait tests.

### Task 2: Memory, image synchronization and native texture uploads

**Files:** `runtime/nvn_vk_draw.c`, `runtime/nvn_gl.c`, `runtime/nvn_front.h`;
new allocator/barrier/upload behavior tests.
**Interfaces:** Optional texture destination reservation consumed by
`nvn_front_tex_read_into`; keep `nvn_front_tex_read` as the compatibility path.

- [x] Test allocator fallback, direct texture output and exact image barriers
  against the existing implementation before changing each path.
- [x] Deswizzle eligible unchanged-format textures directly into coherent
  mapped upload memory; keep conversion paths and ring-wrap safety.
- [x] Scope image barrier stages from layouts, retaining safe GENERAL and
  reused-UNDEFINED ordering; fix demonstrated allocator defects.
- [x] Compare bytes, mip/layer metadata and recorded upload commands; compile
  affected front and back ends for Android ARM64.

### Task 3: Descriptor and draw-state reuse

**Files:** `runtime/nvn_vk_draw.c` descriptors/draw sections; focused tests.
**Interfaces:** Exact descriptor keys tied to the command-buffer generation;
existing `dsh_forget`/command lifecycle invalidation.

- [x] Test key changes, hash collisions, capacity, generation reset and custom
  rendering invalidation before introducing reuse.
- [x] Cache eligible buffer sets and suppress only redundant Vulkan state.
- [x] Assert unchanged draw counts/operands and reduced actual API calls.

### Task 4: Shader/pipeline cache reuse

**Files:** `runtime/nvn_vk_m4.c`, `runtime/vk_glslang_shim.c`, cache helper/tests.
**Interfaces:** Complete bounded cache reads and atomic replacement; compiler
fingerprint export with an older-shim compatibility path.

- [x] Add real-filesystem tests for truncation, malformed SPIR-V, pipeline
  identity mismatch and failed save acknowledgement.
- [x] Implement safe reuse and compiler-aware invalidation in active m4 paths.
- [x] Compile the shim and renderer; run cache behavior tests.

### Task 5: Integration and review

**Files:** This plan and a concise Vulkan validation/change record.

- [x] Run relevant regression suite, Android runtime build and diff checks.
- [x] Obtain a fresh independent review of GPU lifetime and synchronization.
- [x] Fix material findings with regression tests, commit verified changes
  and provide the branch plus a portable source patch/bundle if push is absent.
- [x] Document exactly what passed and the remaining device measurements.

## Completion record

Implementation and independent review are complete. The Android ARM64 runtime
builds all 54 units and both shared libraries. All 145 wtbuild tests pass; the
77-script host corpus has 67 passes, 5 failures reproduced on the base, and
5 skips. Detailed scope, controls, artifact identities and remaining phone
measurements are recorded in `docs/VULKAN_OPPO_OPTIMIZATION.md`.

The build also required two minimal baseline repairs: the POSIX signal-header
include and the temporal-source v2 epoch callback ABI. A deterministic
old-callback/rebind race test and the existing collector suites pass. The
changes are isolated in a prerequisite commit. No private game data, phone
state or files from the separate dirty worktree were used or changed.
