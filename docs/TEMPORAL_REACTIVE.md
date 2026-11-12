# Reactive contract groundwork (2026-11-12)

Task 6.1 remains **UNPROVED**. Task 6.2 and the FSR/FS part of 6.3 remain
**BLOCKED for production**: no verified opaque capture, scene pair validation,
render-size mask images, or backend mask consumption is implemented here.
`runtime/upscale_reactive.h` is a tested GLSL helper, not production support.
Existing backend observers and pinned upstream shaders are unchanged.

## Narrow contract

`wtReactiveMasks` operates once per render-size pixel, with no temporal state,
neighborhood sampling or blend-state heuristics. Its caller must validate that
opaque and final are from the **same current frame**, resource generation,
render extent/viewport, raster jitter, color encoding and exposure. Opaque must
come from an independently verified opaque-to-transparent boundary. Never pass
current final as opaque or use last-frame color. The `pairValid` boolean reports
that validation; the helper does not establish it.

For finite nonnegative linear RGB, normalized max-channel absolute difference
is clamped to [0,1]. Explicit transparent/hair coverage augments composition;
explicit invalid-motion coverage additionally augments reactive. An unchanged
opaque pixel stays zero. An unchanged hair pixel requires classification input:
RGB differences cannot identify it. Motion validity must not use zero velocity
as a missing-data sentinel. Classification and validity must match this frame
and render extent too; no scene producer is supplied by this helper.

RG output is `(reactive, transparency/composition)`, bounded [0,1]. Reset,
invalid pair or nonfinite input rejects history with `(1,1)`. Disabling masks
returns `(0,0)` only for a valid non-reset pair; it cannot bypass pair/reset
safety. Generate fresh outputs every frame; never reuse masks across resize,
surface/resource generation changes or failed pair acquisition. Full rejection
on an invalid pair is a safety contract, **not a fabricated reactive mask**.

## Exact integration handoff, only after boundary proof

1. Own two render-size external single-channel sampled images, or an RG image
   with explicit channel-selecting adapter. Allocate/retire and synchronize them
   with their frame inputs. Write every in-bounds pixel before sampling, with
   explicit write-to-sampled-read barriers. No allocation or binding is added here.
2. In `runtime/nvn_vk_fsr.c`, prepare-reactivity currently binds `FI_DEFR` at
   SRV 3 and 4. Replace **only those external placeholder inputs** with reactive
   and transparency images respectively. Upstream callbacks read `.r` for both:
   binding the same RG view twice loses the G channel unless the source adapter
   explicitly selects it. Do not alias or replace `FI_REACT`: it is the internal
   UAV 9 dilated output, subsequently consumed by luma-instability/accumulate.
   Preserve the upstream prepare/dilate/accumulate algorithm and scale contract.
3. In the runtime-generated FS upscale shader adapter, add one sampled RG mask
   binding. Sample at matching current render coordinates (with explicit clamped
   render-pixel mapping), not reprojected history UV. Immediately before the final
   `mix(HistoryColor, Upsampledcw.xyz, alpha)`, set
   `alpha = 1.0 - wtReactiveHistoryWeight(1.0 - alpha, masks);`.
   Thus zero masks preserve existing alpha, one masks select current, and reset
   remains effective. Wire descriptor counts/writes/layout and helper emission
   together. Do not edit the pinned `third_party` shader. CS remains out of scope.

Neither adaptation is authorized/implemented in this independent groundwork;
coordinate exact backend changes with the lead and observer-hook owner first.

## Verification

Run `python tests/upscale_reactive_test.py <glslc> <spirv-dis>`.
The new test emits the actual C-string GLSL contract and reuses the existing
SGSR Vulkan GPU harness without changing it. Synthetic bright/additive,
unchanged opaque, explicitly classified hair, invalid-motion coverage, reset,
invalid-pair, bounds/nonfinite inputs and mask-on/off history-mix trail/detail
assertions execute in GLSL across an odd-size 17x13 -> 25x19 draw. Two mutation
controls must fail readback: omitted masks and omitted history rejection.

Passed on local NVIDIA GeForce RTX 5070 Ti Vulkan. These are **helper numerical
tests**, not temporal scene quality tests: synthetic trail/detail mixes do not
prove backend consumption, real-scene hair classification, opaque boundary,
Android behavior or complete task acceptance. `--compile-only` runs no numerical
GPU checks. Lead review and scene/backend integration remain required.
