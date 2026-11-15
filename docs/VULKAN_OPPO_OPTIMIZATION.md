# Vulkan backend efficiency on Oppo Find N5

## Scope and result

Implemented on `perf/vulkan-oppo-20261115`, based on
`android/oppo-find-n5` commit `579ab5d307ed2bf5f6dd500d0a125521eae1591b`.
The complete Android ARM64 runtime builds. All eight new Vulkan regression
suites pass, and an independent review found no remaining material regression
in the changed synchronization, resource lifetime or draw-recording paths.

These are source-level and host-test results. No phone, Vulkan device or game
capture was used for this change. Frame-time improvement and visual parity on
Adreno remain to be measured. The settings, resolution, texture precision,
shader effects and ordering of guest draws are preserved.

## Implemented changes

| Area | Change | Eligibility and correctness boundary |
| --- | --- | --- |
| GPU indirect dispatch | Use `vkCmdDispatchIndirect` for a complete, aligned argument record already held in a live compute buffer. | Resolve the source after draw/dispatch preparation and retry when the command-buffer epoch changes. Ambiguous, landed or unaligned records use the existing CPU path; compute comparison diagnostics retain their CPU path. |
| Indirect draws | Separate ordinary indirect-record support from optional indirect-count entry points. | Preserve device `maxDrawIndirectCount`; oversized batches use the existing direct loop. Single-record count commands use the required 16/20-byte stride. |
| Buffer synchronization | Expose compute writes to indirect-command reads independently of draw-count support, and order an indirect argument read before later shader writes. | Keep the existing buffer producer/consumer and readback dependencies. No blanket fence or barrier deletion. |
| Image synchronization | Select pipeline stages from transfer, attachment and shader-read layouts. | Preserve same-layout dependencies, access masks and all subresources. GENERAL, unknown and reused-UNDEFINED paths stay conservative. Tessellation and geometry stages are included only when enabled. |
| Native texture upload | Deswizzle eligible native-format tiled textures directly into coherent mapped Vulkan upload memory. | Reserve the ring region before writing, including any wrap/wait. Consume that same reservation without a second allocation or scratch-to-ring memcpy. Conversion and large/ineligible uploads retain the original path. |
| Memory allocation | Avoid unused host mapping of optimal images. Release surplus completely empty normal blocks, keeping one warm block per memory type/class. | Buffer mappings remain intact. Reclamation follows existing GPU-completion lifetime rules; live blocks are retained and dedicated allocations keep their existing release behavior. |
| Descriptor reuse | Reuse immutable buffer descriptor sets for identical layouts, bindings, buffer handles, offsets and ranges. | Confirm exact equality after hashing. Reset on command-buffer/pool reuse and buffer destruction; bounded saturation falls back to allocation. |
| Draw-state recording | Omit repeated vertex input, vertex buffers, descriptor binds, pushed buffer descriptors and push constants. Bind two adjacent changed descriptor sets together. | Existing internal render paths invalidate the shadow. Draw count, operands, preparation and barriers are unchanged. |
| Shader and pipeline cache | Add complete bounded reads, structural SPIR-V validation, pipeline-header identity checks, compiler-aware keys and atomic file publication. | Invalid entries miss and rebuild. Failed saves retain dirty state and retry; old shims without the optional identity export use a stable legacy identity. |

### ASTC and shared memory

The base branch already selects native ASTC when the Vulkan device exposes the
required format support. This change preserves that hardware path; it does not
force unsupported formats or add a lossy conversion. The direct-upload tests
compare bytes for all 28 ASTC LDR UNORM/sRGB formats, as well as mipmaps, arrays,
cubes, 3D textures, BC and uncompressed data. Native inputs do not enter the
software decoder.

The concrete saving is one scratch-to-upload-ring CPU copy for each eligible
native tiled upload. Deswizzling and the Vulkan buffer-to-image transfer still
exist. Shared physical RAM does not by itself make arbitrary guest memory a
valid Vulkan image or imported allocation.

### Draw-call scope

The translator cannot safely combine arbitrary guest draws without knowing
their blending, storage effects and draw-ID semantics. This implementation
reduces their recording and descriptor overhead and broadens the existing
indirect path. It does not claim to merge general game draws.

In the production-helper mock test, 1,001 preparations with identical buffer
bindings require one descriptor-set allocation/update. This is an API-call
count result, not an FPS measurement. The cache has 2,048 slots, a bounded probe
length and 32,768 stored descriptor values (approximately 1 MiB of host state).

### Cache behavior

SPIR-V/stage identities include the actual glslang build version and target
policy. Stage records use a versioned checksum. Preparation markers also
include the compiler identity and driver UUID and cannot suppress preparation
when the driver cache was rejected. The new identity causes a one-time rebuild
of affected old entries; subsequent unchanged runs reuse the new entries.

Cache paths are thread-local, and POSIX publication applies the same path
normalization as reads. Unique temporary files are published only after a
successful complete write and buffered close. Interrupted writers preserve the
previous published file. No per-shader `fsync` is added.

Validation is deliberately described precisely: SPIR-V checks instruction
framing, not full semantic validity; driver pipeline payloads remain opaque
and are checked through their Vulkan identity header. Arbitrary corruption
with a valid header can still reach the driver. A completely cold cache can
still require shader/pipeline compilation; no effects are skipped to hide it.

## Controls and observations

All new eligible fast paths are enabled by default. These existing-style
environment switches support controlled comparison before runtime startup:

| Variable | Default | Meaning |
| --- | --- | --- |
| `WT_VK_GPUDISPATCH` | `1` | Set `0` to use CPU-resolved indirect dispatch arguments. |
| `WT_VK_DIRECT_UPLOAD` | `1` | Set `0` to use the previous intermediate texture upload path. |
| `WT_VK_BUFFERSETS` | `1` | Set `0` to allocate buffer descriptor sets per draw. |
| `WT_VK_DRAWSHADOW` | `1` | Set `0` to record the original repeated draw-state commands. |
| `WT_VK_PREPTIME` | Existing opt-in | Set `1` to include `nvn_vk_submit` reuse and omitted-command counters. |

Shutdown reports `nvn_vk_upload` with native levels uploaded directly and bytes
of scratch-to-ring copying avoided. Submission counters explicitly retain the
guest draw count. These variables are runtime environment controls, not new
`graphics.ini` keys or Android UI options.

## Verification record

| Check | Result |
| --- | --- |
| Android NDK r28c / API 29 / ARM64 runtime build | 54 source files compiled, zero failures; both runtime and glslang shared libraries linked successfully with `--no-undefined`. |
| ELF inspection | Both shared libraries are AArch64 ELF64; every LOAD segment has at least 16 KiB alignment. The compiler identity export is present. |
| `wtbuild` release tests | 145 passed, zero failed. |
| Python host corpus | 77 scripts inventoried: 67 passed, 5 baseline failures, 5 whole-script skips, zero timeouts. |
| Eight new Vulkan suites | Passed: cache, cache concurrency, pipeline cache, compiler identity, indirect dispatch, draw submission, memory/barriers and texture upload. |
| Temporal-source compatibility tests | Collector, lifecycle, permissions, publication and deterministic stale-callback/rebind cases passed. |
| Independent review | Vulkan lifetime/synchronization and the separate diagnostic ABI repair accepted. The discovered custom POSIX cache-path regression was fixed and retested. |
| Physical GPU / phone | Not run. Four GPU-oriented scripts ran in compile-only mode; they made no Vulkan device calls. |

Five script failures were reproduced on the unchanged base commit:

| Script | Baseline issue in this Linux host environment |
| --- | --- |
| `android_bootstrap_test.py` | Extracted fixture omits required integer/POSIX declarations. |
| `native_driver_restart_test.py` | Strict-C fixture lacks required POSIX types/declarations. |
| `rs_store_production_test.py` with Clang 19 | A deliberately mutated negative control leaves a variable unused under `-Werror`. A separate GCC run passes production and all six negative controls. |
| `save_export_test.py` | Strict-C fixture lacks declarations for `setenv` and `strcasecmp`. |
| `touch_hid_test.py` | Host fixture does not link required platform helper implementations. |

Whole-script skips: `sgsr_gpu_test.py`, `sgsr_cs_gpu_test.py` and
`touch_overlay_gpu_test.py` require real Vulkan execution; Android GPU-driver
lifecycle/store tests require unavailable `javac`. Individual Java launcher
and save-export checks and a Windows-only renderer-restart harness were also
recorded as skipped. The host corpus is not presented as fully green.

Two pre-existing runtime build blockers were repaired separately: `plat.h`
now includes the POSIX signal declarations it uses, and temporal-source
callbacks/binding match the already-declared v2 epoch ABI. The latter rejects
stale callbacks both before and after admission, so an old run cannot write
into or close a later capture. Its binary header/schema and capture window
were not redesigned.

Build source identity:
`de619a01d7f4919949b22f2dd053196f58930e729c2e6e48196e01133cef7964`.

| Runtime artifact | SHA-256 |
| --- | --- |
| `libwitchtime.so` | `8e3d4655192d03c08c62185530899057ee1eb2d355325f9035cc6a6a9686e423` |
| `libwt_glslang.so` | `37b64873a225bbbf33202a2e304c3015a03e6b7bcf8d507026495338ff7cc448` |

## Build and remaining device comparison

With the pinned dependencies and NDK described in `android/README.md` installed,
build from this repository root:

```sh
source deps/android-arm64/android-env.sh
cargo build --release --locked --manifest-path tools/wtbuild/Cargo.toml
tools/wtbuild/target/release/wtbuild runtime --target android-arm64 --out prebuilt/android-arm64
```

`ANDROID_NDK_HOME` must point to NDK r28c. The runtime-only output contains no
personal game library and is not an installable APK. Use the project's existing
personal game and Android packaging workflow for a device build.

The next acceptance gate is the same scene/replay on the same phone, driver,
resolution and graphics settings, comparing warm-cache CPU/GPU frame time,
p50/p95/p99 and the new submission/upload counters. Check native ASTC and
compute-driven effects with Vulkan synchronization validation and compare
images. Measure cold preparation separately from warm gameplay. No device
configuration, application data or saves were modified by this work.

Specification references: [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html),
[descriptor-set rules](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html),
[push constants](https://docs.vulkan.org/guide/latest/push_constants.html), and
[pipeline-cache identity header](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheHeaderVersionOne.html).
