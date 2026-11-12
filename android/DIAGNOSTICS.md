# Android Diagnostic Artifact Contract

## Temporal extension (typed launch, renderer events and host-derived observation)

`treatments.temporal` is optional; omission retains all older request/default
semantics. Example:
```json
{"selection":{"mode":"sgsr2_quality","variant":"2_pass_cs"},"cohort":"equal_output"}
```
Modes are `native`, `fsr3`, `fsr3_quality`, `fsr3_balanced`,
`fsr3_performance`, `fsr3_ultra_performance`, `sgsr2`, `sgsr2_quality`,
`sgsr2_performance`. Only SGSR modes require/accept `variant`:
`2_pass_fs`, `2_pass_cs`, `3_pass_cs`. Unknown fields/values and numeric modes
are rejected. Cohorts are only `equal_output` and `equal_render`.

Full manifests may carry `temporal_observation`, containing actual effective
selection, `scene_dimensions`, `output_dimensions`, `output_grid`, optional
`surface_dimensions`, motion support
(`unavailable`, `camera_only`, `partial_dynamic`, `full_dynamic`),
`reactive_consumed`, separate supported/initialized/dispatched/consumed/skipped
booleans, nullable reset/fallback reasons, scene frame, surface generation, and
`unrelated_settings_hash`. Derive the exact per-axis ratio from output/scene
dimensions; never substitute the menu's requested ratio. `output_grid` must be
`reconstruction` for comparison: these are backend reconstruction/tonemap extents,
including odd game dynamic-resolution levels, **not** the Android surface or
requested display resolution. Omitted grid provenance defaults to `unknown` and
is ineligible. `surface_dimensions` comes only from an existing surface-generation
event matching the scene generation; missing surface evidence stays absent.
The settings hash must
cover non-temporal settings including LOD/mips, FXAA/grain and game dynamic
resolution. It must not hash the intentionally varied temporal selection.
Build provenance reuses the existing APK/runtime/shim/main/SDK identity fields;
all must be present for temporal comparisons, but candidate hashes may differ.

Both sides must declare the same cohort and acknowledge their exact treatments.
Actual reconstruction output extents and unrelated settings must match in either cohort;
equal-render additionally requires identical actual scene extents. Missing
observations, zero dimensions, unsupported execution, unconsumed temporal
output, skipped scenes and fallback are ineligible, not successful treatment.
One observation cannot certify an entire varying-resolution route: route-level
coverage must still be established from scene events and correctness gates.
Configured display resolutions remain matched and acknowledged separately by
the existing settings contract. They are not required or inferred to equal
reconstruction dimensions. Reported surface extent/availability must also match
across the pair; reconstruction equality cannot certify physical display output.

Recorder hook `diag_hook_temporal(const diag_temporal_observation_t *)` writes
four existing fixed-size scalar records on the graphics producer:

* 32: scene frame, selected mode, effective mode, variant.
* 33: scene frame, packed scene extent, packed output extent, surface generation.
  Extents pack unsigned width in the upper 32 bits, height in the lower 32 bits.
* 34: scene frame, flags, motion support, reset reason.
* 35: scene frame, fallback reason, failing pass, compiler/Vulkan error code.

Mode IDs preserve native=0/FSR=1..5/SGSR=6..8; variants are -1 unavailable,
0 fragment, 1 compute-two-pass, 2 compute-three-pass. Motion IDs are 0 unavailable,
1 camera-only, 2 partial dynamic, 3 full dynamic. Flags are bits 0 supported,
1 initialized, 2 dispatched, 3 consumed, 4 skipped, 5 reactive **consumed**.
Backend reset/fallback/pass IDs require a documented renderer mapping; 0 means
none and -1 unknown. Sequence gaps or missing members make that observation
incomplete; do not reconstruct a successful scene from records of other frames.

Native bootstrap validates the typed selectors after verifying the request
digest, including duplicate/unknown selector keys and invalid modes/variants.
Only its fixed mode/variant allowlist maps to existing `WT_UPSCALER` and
`WT_SGSR_VARIANT` selectors before `dlopen`; no arbitrary environment names or
values are accepted. `native` maps to the existing `off` selector. Omission
does not override older launch defaults. Invalid selections reject with
`INVALID_TEMPORAL_TREATMENT` before acknowledgement/loading.

The shared renderer now calls the hook at the frame-ending decision before its
scene evidence is cleared. FSR uses its existing matching output-bound and
recorded-tonemap-draw evidence. SGSR requires the exact routed temporal output
view plus a recorded tonemap consumer draw; its passes and scene-copy alone do
not establish consumption. Consequently a same-size SGSR copy-only route remains
explicitly unconsumed. Motion stays camera-only and reactive consumption false.
These observations establish recorded commands, **not GPU completion**.
Native scene dimensions come from the observed scene image; native output
dimensions require a matching scene sample and recorded tonemap draw into the
actual target. They are not inferred to equal scene dimensions, including under
game dynamic resolution. No such consumer leaves output dimensions unavailable.
Reset reason 1 means the backend's actual reset decision; individual cut/resize
causes are not inferred. Fallback reasons 1..9 are respectively no eligible
scene, unsupported capability/variant, setup failure, no dispatch, no recorded
matching temporal-output consumer, baseline fallback, camera conflict, input mismatch, unsupported output.
The skipped flag means the selected treatment lacks a complete proven output
chain; it does not negate a separately reported dispatch (for example SGSR's
copy-only route with unavailable direct-output consumption evidence).
Unavailable pass/error IDs remain -1; backend logs retain their precise errors.

After collecting committed chunks, the existing Android runner may add the
latest complete `temporal_observation` to its **local** manifest. It requires
all four events in order with matching scene/producer/thread/generation,
valid modes/flags, healthy matching-run **stopped** writer status, zero recorder
drops, emitted/written record equality and all reported chunks. Chunk
headers/counts/endpoints and complete rows are checked against final record totals.
Missing/malformed events, partial tail, collector gaps or lost samples leave
the observation absent. The app's receipt-only acknowledgement never supplies
effective temporal state. Existing manifest fields are preserved; no request
values are promoted into effective treatments, identities or unrelated settings.

**Remaining acceptance limitation:** this transport does not establish the
unrelated-settings hash or absent build/fixture identities. The derived
observation deliberately leaves `unrelated_settings_hash` empty, so such a run
remains comparison-ineligible until those independent facts are established.
It is a latest-scene snapshot, not a complete route-coverage certificate.

### Component GPU timing transport

Event 36 preserves legacy IDs and carries `frame` in p0, pass ID in p1's upper
32 bits, state in p1's lower 32 bits, submission serial in p2 and duration ns in
p3. `diag_stream_gpu_component(frame, pass_id, serial, gpu_ns, state)` emits only
valid completed/wrapped durations; unavailable time is -1, never zero. The host
`DiagnosticGpuComponent::from_payload` maps unavailable duration to JSON null,
retaining pass/frame/submission identity without summing it into utilization.

States: completed=0, pending=1, unsupported=2, failed=3, wrapped=4,
capacity-overflow=5, lost-tail=6, no-results=7. Invalid completed duration becomes
failed/unavailable. Production markers publish one pending record per unique
non-gap component at submission, keyed by `(frame, pass_id, submission_serial)`.
Collection publishes one aggregate terminal: every interval must be valid before
their durations can be summed; measured zero remains valid. Capacity overflow
invalidates the component instead of publishing a partial sum.

The runner validates complete global sequence coverage across all producer chunks
and a healthy stopped writer before enrichment. Finals replace pending once;
duplicate terminals or malformed states make the report unhealthy. Pending-only
records at that validated EOF become `lost_tail` with null duration offline. No
shutdown-thread GPU inspection, extra wait, or post-shutdown event is required.

The fixed opt-in `gpu_components` collector enables both `WT_VK_GPUTIME` and
`WT_VK_PASSTIME`; legacy `gpu_timing` retains whole-span timing with pass timing
disabled. Component profiling adds distinct overhead and requires its own A/A
calibration: lightweight recorder calibration does not qualify it. Summed marked
cost excludes gaps and is neither whole-frame duration nor GPU utilization.

This document specifies the versioned JSON artifact contract and validation rules for automated, reproducible Android diagnostic sessions, offline analysis, and performance/correctness comparisons in Witch Time.

The tooling implementation lives in `tools/wtbuild/src/diag/` and is verified by synthetic tests and deterministic round-trip fixtures in `tools/wtbuild/testdata/diagnostics/`.

---

## 1. Schema Versioning

All diagnostic artifacts declare their schema version using semantic `major.minor` numbering:
- Current version: `"1.0"` (`major: 1, minor: 0`).
- **Compatibility policy:**
  - Minor updates (`1.x` where `x > 0`) are backward-compatible and additive. Parsers accept unknown optional fields and preserve them.
  - Major updates (`major != 1`, e.g. `2.0` or `0.x`) represent breaking changes; parsers reject unsupported major versions with `DiagError::UnsupportedSchemaMajor`.

---

## 2. Artifact Overview

Each automated run produces an isolated set of artifacts located under `build/diagnostics/<run-id>/` (host) and `files/diagnostics/<run-id>/` (Android device):

| File | Purpose | Producer | Consumer |
|---

---

## 3. Core Contract Models

### 3.1 Request (`request.json`)
Specifies the requested parameters before runtime initialization:
- `schema_version`: String (`"1.0"`).
- `run_id`: Opaque unique identifier (e.g. `run_20261110_190000_a1b2`).
- `build`:
  - `variant`: Must be `"diagnostic"`. Ordinary release builds ignore diagnostic requests.
  - `target`: Execution target (e.g. `"android-arm64"`).
  - `native_optimization`: Compiler optimization mode (e.g. `"release"`). Must match candidate and baseline.
- `scenario`:
  - `scenario_id`: Semantic scenario name (e.g. `"shibuya_combat"`).
  - `route_id`: Sub-route or chapter identifier.
  - `fixture_id`: Identifier of the test fixture.
  - `save_fixture`: Relative path to clean initial save file.
  - `replay_fixture`: Relative path to validated input replay file.
  - `deadlines`: Explicit timeout limits in seconds:
    - `startup_deadline_secs`: Maximum time allowed until the first frame heartbeat.
    - `scenario_deadline_secs`: Maximum duration for completing the scenario.
    - `recovery_deadline_secs`: Deadline for foreground recovery after interruption.
- `profile`:
  - `profile_name`: Diagnostic profile (`"lightweight"`, `"deep_capture"`, `"calibration"`).
  - `memory_budget_bytes`: Bounded memory ceiling for flight recorder rings (default: 8 MiB).
  - `storage_budget_bytes`: Maximum disk budget for session artifacts (default: 128 MiB).
  - `incident_disk_reserve_bytes`: Dedicated disk reserve for the first incident capture (default: 16 MiB).
  - `max_incidents`: Maximum number of deep captures (default: 3).
  - `collectors`: Allowlist of active collectors (`"frame_timing"`, `"gpu_timing"`, `"live_status"`).
- `configuration`:
  - `resolution`: Requested display dimensions `[width, height]`.
  - `fps_target`: Target framerate (`30.0` or `60.0`).
  - `audio_mode`: `"output"` or `"muted"`.
  - `cache_mode`: `"app_cache_cold"`, `"process_cold"`, or `"warmed"`.
  - `input_policy`:
    - `source`: Controller origin policy (e.g. `"synthetic_fullkey_player1"`).
    - `mapping`: Rebinding policy (e.g. `"default_type_a"`).
    - `menu_confirm`: Menu confirmation binding (e.g. `"native_a"`).
- `treatments` (Optional typed treatment allowlist; unknown keys or invalid types are rejected):
  - `ww_soft`: Boolean (default: `false`). Experimental software write-watch.
  - `vk_writerwait`: Boolean (default: `false`). Producer-scoped GPU waiting.
  - `vk_loadclear`: Boolean (default: `false`). Leading full clear folding.
  - `vk_split`: Optional integer (default: `null`). Submission split threshold.
- `policy`: Predeclared evaluation policy ID, cryptographic hash, and required assertions.

### 3.2 Manifest (`manifest.json`)
Records acknowledged effective state and hardware/binary provenance:
- `diagnostic_version`: Version string of the diagnostic subsystem.
- `toolchain`: NDK, Clang, Rust compiler versions, and host OS.
- `source`:
  - `git_commit`: Working tree commit hash.
  - `is_dirty`: Boolean flag indicating if uncommitted changes were present.
  - `local_diff_digest`: SHA-256 hash of the git diff, or `null` if clean.
- `build_identity`:
  - `package_id`: Android application ID (`com.retro.witchtime`).
  - Module hashes for APK, `libwitchtime.so`, game shim, lifted main executable, and generated SDK identity.
- `device`:
  - Hardware model, manufacturer, Android OS version, API level, Vulkan driver version/name, OS page size (e.g. 16384 on 16 KiB systems like OPPO Find N5), and ABI (`arm64-v8a`).
- `fixtures`: Hashes of starting save file, replay input file, and scenario definition.
- `cache`: Acknowledged effective shader cache and Vulkan pipeline cache paths, seed hash, and OS driver cache state (`"unknown"`).
- `effective_paths`: Concrete session paths for `session_root`, `save_dir`, `config_file`, `shader_cache_dir`, `vk_cache_dir`, `artifacts_dir`.
- `effective_settings`: Actual resolution, framerate, audio mode, and controller policies applied by the runtime.
- `effective_treatments`: Acknowledged effective treatments applied (`ww_soft`, `vk_writerwait`, `vk_loadclear`, `vk_split`). Omitted in legacy manifests where treatment provenance remains unknown.
- `capabilities`: Map of subsystem name to capability status (`"supported"`, `"unavailable"`, `"disabled"`, `"permission_denied"`) and reason.
- `mismatches`: List of discrepancies between requested and effective parameters, including field name, requested value, effective value, and stable reason code.

### 3.3 Measurements: Null vs Measured Zero
Every measurement record strictly differentiates unavailable or pending data from measured zero:
- `name`: Metric name.
- `value`: `Option<f64>`.
  - If measured zero, serialized as `0.0` (`"value": 0.0`).
  - If unavailable, disabled, pending, or failed, serialized explicitly as `null` (`"value": null`).
- `units`: Standard unit identifier:
  - `"ms"`, `"us"`, `"ns"`: Timing distributions.
  - `"fps"`: Framerate measurements.
  - `"bytes"`: Requested comparison spans and memory allocations.
  - `"words"`: Logical words visited during texture hashing (`hash_words_visited`), strictly distinct from measured physical DRAM traffic.
  - `"celsius"`: Thermal temperatures.
  - `"count"`: Dispatched draws, faults, rearms, queries, waits, leases, and folded clears.
  - `"ratio"`, `"percent"`: Fractional and percentage ratios.
- `source`: Collector source (`"gpu_query_pool"`, `"clock_monotonic"`, `"thermal_zone"`, `"audio_ring_counter"`).
- `capability`: Capability status (`"supported"`, `"unavailable"`, `"disabled"`, `"failed"`, `"pending"`).
- `sample_count`: Count of observed samples.
- `coverage`: Fractional coverage over expected duration/frames (`0.0` to `1.0`).
- `validity`: `"valid"`, `"invalid"`, `"incomplete"`, `"unsupported"`.
- `reason_code`: Optional stable reason code when value is null.

> [!IMPORTANT]
> Missing measurements (such as absent GPU timer queries) must never be coerced to zero or used to deduce a CPU bottleneck.

### 3.4 Outcome (`outcome.json`)
Records how the execution completed:
- `execution_outcome`: `"completed"`, `"assertion_failed"`, `"diverged"`, `"timed_out"`, `"cancelled"`, `"crashed"`, `"lost_observation"`, `"storage_exhausted"`.
- `reason_code`: Stable machine-readable reason code (e.g. `CLEAN_COMPLETION`, `CHECKPOINT_TIMEOUT`, `ASSERTION_FAILED`, `NATIVE_CRASH`).
- `exit_status`: Exit code, signal, clean exit flag, and origin (`"process"`, `"runner_deadline"`, `"runner_cancel"`, `"external"`).
- `last_confirmed_checkpoint`: Name of the last passed checkpoint.
- `checkpoints`: Ordered list of checkpoint assertion results with elapsed host and game milliseconds.
- `correctness_assertions`: Visual, audio, save, lifecycle, and semantic gate evaluations (`"passed"`, `"failed"`, `"not_checked"`).

### 3.5 Summary (`summary.json`)
Offline aggregation of performance and telemetry:
- `segments`: Per-segment statistics with sample count, coverage, duration, `frame_time_ms` distribution (min, max, median, p95, p99, mean, stddev), `gpu_time_ms` distribution, and present classifications (held vs progress presents).
- `measurements`: Key named `DiagnosticMeasurement` records.
- `hypotheses`: Causal diagnoses (`CPU_BOUND`, `GPU_BOUND`, `WAIT_STALL`, `THERMAL_THROTTLED`) with evidence links.

### 3.6 Comparison (`comparison.json`)
Deterministic evaluation between baseline and candidate:
- `eligibility`: Boolean flag and list of stable ineligibility reason codes if matched criteria fail.
- `dimensions`:
  - `execution`: `"equivalent"`, `"baseline_failed"`, `"candidate_failed"`, `"both_failed"`, `"different_coverage"`.
  - `correctness`: `"passed"`, `"candidate_regression"`, `"baseline_regression"`, `"not_checked"`.
  - `performance`: `"improved"`, `"regressed"`, `"equivalent"`, `"trade_off"`, `"inconclusive"`.
  - `overall_verdict`: `"improvement"`, `"regression"`, `"equivalence"`, `"trade_off"`, `"inconclusive"`, `"not_comparable"`.
- `metric_comparisons`: Metric differences with delta, percent change, noise margin, and uncertainty bounds.
- `trade_offs`: Conflicting metric pairs (e.g. improved median frametime but regressed tail latency).

---

## 4. Stable Reason Codes

| Category | Reason Code | Meaning |
|---

---

## 5. Parser and Offline Validation Rules

1. **Size Limits:** Parsers enforce a hard payload limit (`MAX_DIAGNOSTIC_JSON_BYTES = 16 MiB`) via `parse_json_bounded` to prevent memory exhaustion.
2. **Numeric Rigor:** `validate_finite_numbers` guarantees that all parsed numbers are finite. Non-finite values (`NaN`, `Infinity`, `-Infinity`) are rejected.
3. **Sequence Ordering:** `validate_strict_sequence` ensures telemetry records have strictly monotonic IDs (`seq == prev + 1`), rejecting gaps, duplicates, or out-of-order events.
4. **Deterministic Round-Trip:** All models use ordered collections (`BTreeMap`) to ensure JSON serialization is byte-for-byte deterministic.

---

## 6. Acceptance Scope and Unsupported Metrics

### 6.1 Acceptance Scope and Evaluation Verdicts
The offline evaluation engine (`tools/wtbuild/src/diag/evaluation.rs`) computes separate dimensions:
- **Eligibility:** Verified before comparison. Mismatched resolution, differing scenarios, divergent mapping policies, mismatched cache classes, or crashed runs produce `is_eligible: false` and force `overall_verdict: "not_comparable"`. Intentionally different candidate binaries under matched conditions remain fully eligible.
- **Correctness vs Performance Decoupling:**
  - A required correctness failure (e.g. visual distortion or black screen) always produces `overall_verdict: "regression"`, even if frame and GPU timings improved.
  - If a required gate is `not_checked` (e.g. audio backend unverified due to mute), accepted improvement is withheld and `overall_verdict: "inconclusive"`.
  - An optional gate marked `not_checked` does not withhold an accepted performance improvement.
- **Trade-Off Classification:**
  - When the median frametime improves beyond the noise margin but p99 tail latency worsens beyond its threshold, the verdict is classified as `trade_off`, never hidden in a single aggregate score.

### 6.2 Phase Budgets and Present Classification
- **Phase-Specific Budgets:** Segments declaring 30 fps evaluate dropped frames against a 33.3 ms target budget (with 10% pacing tolerance), whereas 60 fps segments evaluate against 16.6 ms.
- **Presents:** Repeated held presents during menu pauses or shader pre-warming do not count as progress presents.

### 6.3 Unsupported Metrics and Non-Inference
- **No Physical Display Certification:** Presentation queue callbacks, fence completions, and framebuffer captures verify internal graphics pipeline execution, not photon emission on the physical OLED panel.
- **No Physical Audio Certification:** Guest PCM generation does not verify physical speaker sound. Muted runs record `AUDIO_MUTED` and leave the physical backend gate `not_checked`.
- **Absent GPU Timings:** When GPU timer queries are unavailable or pending, GPU statistics are reported as `None` with absence reasons. The analyser never deduces a `CPU_BOUND` diagnosis from missing GPU data.

---

## 7. Runtime Event Transport, Memory/Disk Ceilings, and Loss Semantics

### 7.1 Record Structure and Memory Ceilings
- **Fixed-Size 64-Byte Records:** Every diagnostic event is stored in a fixed-size `diag_record_t` structure (64 bytes). The record consists strictly of scalar fields (`seq`, `timestamp_ns`, `type`, `producer_id`, `thread_id`, `generation`, and a 32-byte bounded `payload[4]`). No transient pointers, variable-length buffers, or heap pointers are stored.
- **Bounded Bounded Producer Slots:** Up to 16 producers (`DIAG_MAX_PRODUCERS = 16`) can register simultaneously. Each producer owns an independent single-producer single-consumer (SPSC) ring buffer of 1024 slots (`DIAG_RING_CAPACITY = 1024`).
- **Static Memory Ceiling:** The total flight-recorder memory footprint is statically fixed at `16 * 1024 * 64 bytes = 1024 KiB` (1 MiB). Zero dynamic memory allocation (`malloc`) occurs during diagnostic logging.
- **No-Op Disabled Path:** When diagnostics are not active or initialized, `diag_is_enabled()` returns false, `diag_register_producer` returns -1, and `diag_emit` exits immediately via relaxed atomic load. No memory is touched, no locks are acquired, and no capture allocation occurs.

### 7.2 Non-Blocking Drop-New Loss Semantics
- **Drop-New on Full Buffer:** When a producer's ring buffer fills up (`(head + 1) & 1023 == tail`), the producer drops the new record immediately. Game threads and render threads never block, sleep, or wait on consumer processing or disk storage.
- **Loss Accounting:** Dropped events atomically increment the producer's `dropped_count` and the global `diag_total_dropped` counter. Offline analysis and live status snapshots consume these counters to detect data loss and report `SEQUENCE_GAP`.
- **Monotonic Sequencing:** Every emitted record is stamped with a 64-bit strictly monotonic sequence number (`seq`), ensuring total ordering and corruption detection across all producers.
- **Release/Acquire Publication:** Producers write record fields in full before updating `head` with release semantics. Consumers read `head` with acquire semantics and update `tail` with release semantics, guaranteeing that partially written records are never observable.

### 7.3 Disk Ceilings and Incident Reserve
- **Storage Budget:** Configurable per session in `DiagnosticProfile` (default 128 MiB).
- **First-Incident Disk Reserve:** A dedicated 16 MiB reserve ensures that the first crash, hang, or divergence incident can record uncompromised state even if ordinary telemetry is throttled by storage limits.
- **Thread-Lifetime Reuse:** When a transient thread terminates, its producer slot is unregistered and its generation counter incremented. When the slot is subsequently re-acquired by a new thread, counters reset cleanly and consumer drains can distinguish events across thread lifetimes without ambiguity.

### 7.4 Background Chunk Writer, Atomic Publication, and Artifact Recovery
- **Atomic Publication:** Telemetry chunks (`events-XXXXXX.jsonl`) and the live status snapshot (`status.json`) are written first to `.tmp` files and then committed via platform atomic renames (`MoveFileExA` with `MOVEFILE_REPLACE_EXISTING` on Windows, `rename(2)` on Linux/Android). External tools and background monitors never see half-written chunks.
- **Non-Destructive Directory Protection:** Diagnostic writer initialization verifies that target directories do not already hold prior session artifacts (`status.json`, `manifest.json`, `events-000001.jsonl`). Existing runs are never overwritten, protecting baseline and candidate data from accidental loss.
- **Artifact Recovery Under Interruption:**
  - In the event of process crash, timeout, or external kill, all committed chunks (`events-000001.jsonl` through the last committed chunk) remain intact on disk.
  - Uncommitted `.tmp` files are left isolated and safely ignored by offline analysers.
  - If an in-flight chunk experiences trailing truncation, the streaming JSONL reader extracts all complete records up to the truncation point and records `SEQUENCE_GAP` rather than discarding the entire session.
- **Opt-Out of Legacy Unbuffered Logging:** In opted diagnostic sessions, legacy unbuffered stdout logging is replaced with buffered output and captured via a bounded in-memory circular sink (`diag_text_sink_write`, 256 KiB), eliminating disk thrashing and frame pacing jitter caused by frequent synchronous `fwrite` calls.

### 7.5 Continuous Timing Stream, GPU Query Semantics, and Domain Anchors
- **Uncapped Continuous Streaming:** Frame and GPU timing samples are streamed incrementally across the entire session duration without the legacy 40,000-frame cap. Sessions extending to hundreds of thousands of frames write completed chunk files incrementally, removing the shutdown-only bottleneck.
- **Timing Metrics Meanings:**
  - `cpu_duration_ns`: Exact CPU time spent recording and submitting the frame's passes and dispatches.
  - `wall_interval_ns`: Interval between consecutive present calls (`t_now - t_prev`) in monotonic nanoseconds.
  - `gpu_duration_ns`: GPU hardware execution duration obtained from command buffer timestamps (`vkCmdWriteTimestamp`), calibrated by device `timestampPeriod`.
- **Query Completion States:**
  - `COMPLETED` (0): GPU query completed cleanly with valid timestamps.
  - `PENDING` (1): GPU execution still in flight; query results pending.
  - `UNSUPPORTED` (2): Platform or driver lacks Vulkan timestamp query support.
  - `QUERY_FAILED` (3): Fence timeout or query pool retrieval failure.
  - `TIMESTAMP_WRAPPED` (4): Sub-64-bit hardware counter overflow resolved via wrap arithmetic.
- **Non-Blocking Query Policy:** The flight recorder never stalls the render loop, presentation thread, or shutdown path waiting for delayed GPU queries. Pending queries at shutdown remain marked `PENDING`, and all completed samples are committed before the clean-completion marker.
- **Domain Anchors:** Monotonic domain anchors bind `CLOCK_MONOTONIC` nanoseconds to `CLOCK_REALTIME` Unix timestamps at startup, Android suspend, resume, and periodic sync points. External NTP adjustments or timezone jumps do not distort monotonic frame interval analysis.

---

## 8. Validated Replay Format, First-Profile Restrictions, and Legacy Selection

### 8.1 Binary Replay Specification
Validated input replay uses a strict binary format designed for deterministic reproduction:
- **Header (68 bytes):** Magic `WTRP` (`0x50525457`), version `1.0`, declared `source_policy`, `mapping_policy`, `confirm_policy`, `record_count`, `total_polls`, and reserved padding.
- **Records (36 bytes per change):**
  - `poll`: Strictly monotonic poll index (`poll > prev_poll`).
  - `anchor`: Screen/draw class anchor ID (`-1` if unanchored).
  - `anchor_poll`: Polls elapsed since reaching the anchor.
  - `buttons`: Bitmask of standard Switch Pro Controller buttons (`DIAG_REPLAY_VALID_BUTTON_MASK`).
  - `lx, ly, rx, ry`: Left and right analog stick axes in standard signed range `[-32768..32767]`.
  - `attributes`: Bitmask of connection flags (`NPAD_ATTR_CONNECTED`, `NPAD_ATTR_WIRED`).
- **Footer (16 bytes):** EOF marker `_END` (`0x444E455F`), `record_count_check`, and checksum.
- **Playback timing:** `total_polls` is an exclusive duration: samples are delivered at zero-based 64-bit global polls `[0, total_polls)`. Loading a replay resets its clock to zero; rebinding an existing session or changing semantic anchors does not reset it. Unanchored changes become due when `record.poll <= global_poll`; unanchored records at or beyond `total_polls` are rejected. Anchored changes retain anchor-ID/anchor-relative timing. Between changes, including the tail after the final change, the last state is held. EOF releases input on the first query at `global_poll >= total_polls`, not when the last change is consumed, so the final in-duration HID sample is delivered before completion. Empty replays return false/neutral samples and complete at their declared duration; zero duration completes immediately.
- **Safety Ceilings:** Max file size capped at 16 MiB; max record count capped at 500,000.

### 8.2 Fixed First-Profile Restrictions
To guarantee reproducible benchmarking and prevent divergent routes, Profile 1 enforces:
1. **Source Policy:** Strictly `DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1` (1). Keyboard or physical controller origins are rejected.
2. **Mapping Policy:** Strictly `DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A` (1). Custom user rebindings are rejected.
3. **Confirm Policy:** Strictly `DIAG_REPLAY_CONFIRM_A` (1). B-confirm menu settings are rejected.

### 8.3 Strict Failure Guarantee (Zero Fallback)
If validation encounters a missing file, bad header, poll order violation, invalid button bits, or truncated footer:
- The parser returns an explicit error code and marks the session failed.
- The input subsystem locks to neutral release (0 buttons, 0 stick deflection).
- **No live fallback:** The session never falls back to live keyboard, mouse, or touch input.
- **No legacy fallback:** The session never silently drops to legacy text replay.
- **No file overwrite:** Failed playback never touches, corrupts, or overwrites existing recordings.

### 8.4 Explicit Legacy Selection
- **Legacy Replay:** Selected explicitly via environment variables `WT_PLAY=<file>` and `WT_REC=<file>` using legacy text format (`a <anchor> <since> <buttons> <lx> <ly>`).
- **Validated Replay:** Selected through diagnostic scenario requests (`replay_fixture`) or `WT_VALIDATED_REPLAY=<file>`.

### 8.5 Synthetic Player 1 Discovery and Controller Decoupling
- **Pre-Query Discovery:** When synthetic Player 1 is enabled (`diag_input_enable_synthetic_player1(true)` or via active validated replay), `nn::hid` discovers Player 1 as a connected Switch Pro Controller (`NpadStyleFullKey`) before the guest issues its first input query.
- **Physical Decoupling:** Replaying remains pure and decoupled from the host environment:
  - Rapid physical gamepad hotplugging, connect, or disconnect events do not alter Player 1's connection status.
  - Keyboard origin or mouse clicks cannot contaminate or merge with synthetic Player 1 inputs.
  - Controller button prompts (`nnpad_prompts_used(0)`) are strictly preserved, preventing spurious keyboard prompt switches.
- **Supported Style and ID Constraints:** If guest software specifies non-FullKey styles (e.g., Handheld only) or whitelists IDs excluding Player 1 (`id == 0`), synthetic Player 1 connection is withheld in conformance with guest HID requirements.
- **Policy Verification Reason Codes:** Environment and replay policy mismatch produces stable rejection codes:
  - `DIFFERENT_MAPPING_POLICY`: Triggered on keyboard origin, custom button rebindings, or swapped confirm (`swap_confirm = on`).
  - `INCOMPATIBLE_PROFILES`: Triggered on unsupported source policy or invalid policy structure.

### 8.6 Canonical Composition Boundary, Settings-Menu Gate, and Unsupported Modes
- **Injection Boundary:** Injection occurs post-source-composition directly into `nn::hid` FullKey state:
  - Canonical 16-bit face/shoulder/d-pad buttons are composed with analog stick deflection `[-32768..32767]`.
  - Stick-as-button bits (bits 16..19 for left stick, bits 20..23 for right stick) are derived deterministically when deflection exceeds half travel (`|axis| > 16383`).
  - Connection attributes preserve wired (`0x3`) and wireless (`0x1`) state.
  - Sampling counters strictly increment per guest poll.
- **Settings-Menu Gate (`pad_gate`):**
  - The PC settings menu gate (`pcmenu_gate`) is retained and authoritative.
  - While the settings menu is open, delivered guest pad state is completely zeroed out (`buttons = 0`, `lx = ly = rx = ry = 0`), preventing background guest actions while configuring settings.
- **Neutral Release on EOF, Cancellation, or Failure:**
  - On reaching end-of-file, explicit replay cancellation (`diag_replay_cancel`), or validation/session failure, the system automatically releases all buttons and sticks to neutral (0), holding attributes connected.
- **Unsupported Modes:**
  - Motion / 6-axis gyro & accelerometer replay (handled via neutral zero motion; not recorded in replay v1.0).
  - Touchscreen emulation (console is treated as docked: 0 active touches).
  - Joy-Con horizontal single-controller mode (only FullKey / Pro Controller supported in Profile 1).
  - Players 2..8 synthetic injection (only Player 1 is driven synthetically; players 2..8 retain standard live hardware input).

---

## 9. Semantic Observations, Diagnostic Shim Binding, and Freshness Evidence

### 9.1 Game-Thread Observation Publication
Semantic observations are published from the owning game thread using existing runtime boundaries:
- **Presence & Progress:** Chapter ID, active verse number, difficulty level, and area index are sampled from `game_presence` during `presence_tick` on the game thread.
- **Movie Creation:** Extractor and container events in `nnmovie_mf.c` signal active cutscene playback (`input_mark("movie")`).
- **Render/Draw Count:** Presented draw calls per frame are updated via `input_frame_draws()`.
- **Thread-Safe Mark Publication:** Mark slots are reserved and filled before atomic publication (`InterlockedIncrement(&g_mark_count)` after memory barrier), completely eliminating publish-before-fill races across concurrent producer and query threads.

### 9.2 Optional Versioned Shim Diagnostic Binding (`game_diag_bind`)
The host interacts with `game.dll` / `libgame.so` through an optional versioned shim handshake:
- **No Layout Modification:** Preserves `Ctx` and `GameHost` structures intact without binary layout breakage.
- **Graceful Backward Compatibility:** If `game_diag_bind` is not exported by older game libraries, the host continues normal launch without failing.
- **Capability Rejection:** Strictly enforces supported scopes:
  - **Program-1 Rejection:** Requests for `program_id == 1` are rejected with `DIAG_SHIM_ERR_UNSUPPORTED_PROGRAM` (-3).
  - **Game Hash Verification:** Mismatched game build hashes return `DIAG_SHIM_ERR_UNSUPPORTED_HASH` (-4).
  - **Version Guard:** Incompatible major versions return `DIAG_SHIM_ERR_UNSUPPORTED_VERSION` (-2).

### 9.3 Observation Freshness vs. Transition Guarantees
- Semantic state reports strictly reflect the most recent confirmed game-thread observation with a monotonic nanosecond timestamp.
- Evaluation checkpoints must verify evidence freshness and occurrence IDs rather than assuming every single intermediate visual transition triggers a distinct event.

---

## 10. Scenario Checkpoint State Machine, Clocks, and Divergence Retention

### 10.1 Checkpoint Sequence and Occurrence Identification
A validated diagnostic scenario defines an ordered sequence of required and optional milestones:
- **Occurrence IDs:** Distinguish repeated visits to the same state (e.g. `occurrence: 1` vs `occurrence: 2` when revisiting a chapter or verse). Stale observations from earlier visits cannot satisfy later occurrences.
- **Semantic Matchers:** Verify exact chapter, verse, difficulty, area, and cutscene presence.
- **Draw Counts Insufficiency:** Changes in draw call counts alone cannot advance or satisfy semantic checkpoints.

### 10.2 Independent Timing Clocks
Each checkpoint declares an explicit `clock_policy` and bounded `deadline_ms`:
1. `host_time`: Monotonic wall-clock time elapsed on the host. Used for boot and network transport bounds.
2. `game_active_time`: Active gameplay time. Paused during lifecycle backgrounding or system suspend, ensuring background intervals do not trigger spurious scenario timeouts.
3. `input_poll_count`: Logical controller polling ticks. Guarantees deterministic input synchronization across varying render frame rates.

### 10.3 Failure, Replay Exhaustion, and Divergence Retention
- **Replay Exhaustion:** If an input replay finishes before required scenario checkpoints (such as combat completion) are reached, the scenario halts with outcome `DIVERTED` (`REPLAY_EXHAUSTED`).
- **Partial Outcome Retention:** All partial, timed-out, or diverted runs strictly preserve:
  - `last_confirmed_checkpoint`: The ID of the highest successfully confirmed milestone.
  - `covered_segments`: The complete list of validated segments (e.g. `["boot", "loading", "prologue"]`).
  - Recorded intermediate checkpoint results and timestamps.
- **Comparability Constraint:** A candidate run that diverges or fails to complete all required checkpoints cannot establish regression eligibility or performance improvement against a complete baseline.

---

## 11. Private-Route Provisioning Instructions (`boot_continue_combat`)

### 11.1 Authorized Save State Provisioning
To run reproducible diagnostics against commercial game saves while maintaining copyright safety:
- **Private Route Workspace:** Authorized save binaries must reside outside source control or within git-ignored paths (e.g., `userdata/save_private.bin` or per-session directories under `userdata/runs/<run-id>/save.bin`).
- **Strict Git Hygiene:** **Never commit copyrighted game saves, decrypted Switch binaries, ROM dumps, or proprietary recordings to the repository.** Synthetic mock fixtures (`synthetic_save.bin`, `synthetic_scenario.json`) are provided in `tools/wtbuild/testdata/diagnostics/` for CI and offline unit testing.
- **Fixture Provisioning Flow:**
  1. The orchestrator copies the authorized baseline save into the isolated session workspace:
     `cp <authorized-save-path> userdata/runs/<run-id>/save.bin`
  2. The run manifest records `save_fixture_hash` (SHA-256) and `save_fixture_id` to ensure baseline and candidate compare identical save states.
  3. The runtime mounts the session-isolated directory as the effective `save_dir`.

### 11.2 Control Scheme and Button Mapping Configuration
The standardized `boot_continue_combat` scenario depends on deterministic input sequences matched to guest input handlers:
- **Fixed Type-A Controls:** The in-game controller configuration must be set to the default **Type-A** layout. Custom button remappings in guest options alter combat action codes and trigger scenario divergence.
- **Native A Confirm Policy:** The Nintendo Switch native button policy requires `A` (East button, `0x0001`) for confirmation and `B` (South button, `0x0002`) for cancellation (`swap_confirm = off`). The orchestrator validates `diag_verify_input_policy()` before replay injection starts; runs with swapped confirm buttons or non-native layouts are rejected upfront with `DIAG_POLICY_ERR_DIFFERENT_MAPPING`.
- **Synthetic Player 1 Injection:** Synthetic Player 1 operates in `NpadStyleFullKey` (Pro Controller) mode. Physical controller inputs, keyboard events, and mouse clicks are decoupled during replay playback.

### 11.3 Scenario Execution Bounds and Timeout Budgets
- **Boot Phase:** Wall-clock (`HostTime`) timeout of 15,000 ms to achieve initial presence and title screen presentation.
- **Title to Continue:** Transition through title prompt with `A` button pulse (minimum 3 frames held, 2 frames neutral).
- **Loading Phase:** Active gameplay time (`GameActiveTime`) timeout of 20,000 ms during save loading and asset streaming.
- **Combat Sequence:** `GameActiveTime` bounded to 45,000 ms; verified via occurrences of `chapter: 0`, `verse: 1`, and active combat marks.

---

## 12. Android Launch / Acknowledgement Protocol and Capabilities

### 12.1 Explicit Diagnostic Build Gate (`WT_DIAGNOSTIC_ENABLED`)
- **Variant Separation:** Only the explicitly enabled `diagnostic` build variant accepts automated launch configuration.
- **Release and Debug Safety:** Ordinary `release` and `debug` builds compile with `WT_DIAGNOSTIC_ENABLED = false`. Any supplied diagnostic intent extras (`diag_run_id`, `run_id`) or command-line flags (`--diag-run-id`) are ignored, ensuring ordinary player launches never expose unrestricted environment manipulation.
- **Optimization Parity:** The `diagnostic` variant inherits directly from `release`, ensuring optimization levels, inline thresholds, and code generation match the production candidate. It includes table-level symbol tables (`ndk { debugSymbolLevel 'SYMBOL_TABLE' }`) and enables shell profiling (`<profileable android:shell="true" />`).

### 12.2 Launch Protocol and Staging Boundary
Automated execution follows a strict multi-stage staging and validation protocol:
1. **Host-Side Staging:**
   - The host orchestrator generates a unique, bounded run identifier (`run_id` matching `^[a-zA-Z0-9_-]{1,64}$`).
   - The orchestrator stages `request.json` under the session directory: `<files-root>/diagnostics/<run-id>/request.json`.
2. **Intent Dispatch:**
   - The orchestrator dispatches the Android Activity intent:
     `adb shell am start -n org.witchtime.android/.WitchTimeActivity --es diag_run_id <run-id>`
   - The intent carries only the opaque `run_id`, never raw JSON or arbitrary shell commands.

### 12.3 Pinned SDL Argument Hook Bridge
`WitchTimeActivity` bridges launch intent extras into native runtime arguments via SDL's supported extension point without modifying SDL sources:
- **Hook Method:** Overrides `protected String[] getArguments()` on `SDLActivity`.
- **Pre-Initialization Verification:**
  - Validates `run_id` against allowlist regex (`^[a-zA-Z0-9_-]{1,64}$`).
  - Verifies canonical directory paths to prevent path traversal (`..`) or symlink escapes outside the app's `diagnostics/` root.
  - Enforces payload bounds: `request.json` must be non-empty and <= 65,536 bytes (64 KB).
  - Calculates SHA-256 digest of the raw `request.json` bytes.
  - Validates JSON structure using Android's built-in parser: enforces schema version major (`"1.x"`), matching `run_id`, allowlisted build settings (`variant: "diagnostic"`, `target: "android-arm64"`, `native_optimization: "release"`), safe scenario identifiers, and positive finite deadlines.
- **Argument Delivery:** If all checks pass, returns:
  `["--diag-run-id", "<run-id>", "--diag-digest", "<sha256-hex>"]`.

### 12.4 Native Pre-dlopen Bootstrap Bridge and Startup Ordering
In native `SDL_main(int argc, char **argv)` before `dlopen("libwitchtime.so")`:
- **Argument Extraction:** `diag_bootstrap_parse_args()` inspects `argv` for `--diag-run-id` and `--diag-digest`.
- **Digest Verification:** Computes the SHA-256 digest of `diagnostics/<run-id>/request.json` and verifies exact byte equality against the `--diag-digest` passed by the Activity. Tampered or mismatched requests are rejected immediately.
- **Rejection Outcome:** On validation failure, writes a machine-readable rejection report to `diagnostics/<run-id>/outcome.json` (status: `"rejected"`, reason code) and terminates before `dlopen`.
- **Pre-dlopen Startup Acknowledgement:** On validation success, writes `diagnostics/<run-id>/manifest.json` with status `"acknowledged"` and startup phase `"bootstrap_pre_dlopen"`. This guarantees that an acknowledged session record exists before any native game library or constructor runs.
- **Session Environment Isolation:**
  - Creates session-owned directories: `<run-dir>/userdata`, `<run-dir>/cache/shaders`, `<run-dir>/cache/vk`, `<run-dir>/config`, `<run-dir>/artifacts`.
  - Sets isolated environment variables: `WT_USERDATA`, `WT_SHADER_CACHE`, `WT_VK_CACHE`, `WT_CONFIG`, `WT_GRAPHICS_CONFIG`, `WT_DIAG_RUN_ID`, `WT_DIAG_DIR`.
  - Redirects `stdout` and `stderr` to `<run-dir>/session.log`, preventing contamination of shared `runtime.log`.

---

## 13. Session-Owned Test State and Cache Isolation

### 13.1 Session-Owned Directory Isolation
To guarantee complete determinism and prevent side-effects between consecutive runs:
- **Root Segregation:** Every automated run executes exclusively within `<files-root>/diagnostics/<run-id>/`.
- **Dedicated Environment Pointers:**
  - `WT_USERDATA`: Set to `<session-root>/userdata`. Never falls back to shared `userdata/`.
  - `WT_CONFIG`: Set to `<session-root>/config/controllers.ini`.
  - `WT_GRAPHICS_CONFIG`: Set to `<session-root>/config/graphics.ini`.
  - `WT_SHADER_CACHE`: Explicitly pointed to `<session-root>/cache/shaders`.
  - `WT_VK_CACHE`: Explicitly pointed to `<session-root>/cache/vk`. Never defaults to shared `../cache/vk`.
  - `WT_DIAG_DIR`: Points to `<session-root>`.
  - `session.log`: Replaces shared `runtime.log` for stdout/stderr logging.

### 13.2 Fixture Immutability and Content Verification
- **Read-Only Golden Fixtures:** Test saves and replay streams are staged from immutable reference sources.
- **Session-Owned Copy:** The orchestrator copies `save_fixture` into `<session-root>/userdata/save.bin` and records its SHA-256 digest in `DiagnosticFixtureIdentity.save_hash`.
- **Integrity Guarantee:** Post-execution checks verify that the original reference fixture's hash remains unchanged regardless of in-game guest saves or modifications.

### 13.3 Cache Provenance and Seeding Rules
- **A/B Cold Cache Isolation:** Each cold run receives a fresh, initially empty `cache/` directory. Shader compilation or pipeline creation during Run A cannot warm or accelerate Run B.
- **Seeded Warm Caches:** When `cache_mode = "seeded_warm"` is requested, the cache is populated exclusively from a recorded seed directory with its SHA-256 digest committed in `DiagnosticCacheProvenance.seed_hash`. Spontaneous or unrecorded cache warming is rejected.

### 13.4 App-Cache-Cold vs. Unknown OS/Driver Cache Boundary
- **Application Cache Boundary:** The diagnostic runner has full authoritative control over application-level shader caches (`WT_SHADER_CACHE`, `WT_VK_CACHE`), compiled pipeline caches, and in-memory caches.
- **Driver/OS Kernel Boundary:** Proprietary Android GPU drivers (Qualcomm Adreno, ARM Mali) maintain internal kernel/HAL shader and binary pipeline caches outside the app's sandboxed storage.
- **Honest Provenance:** In conformance with the artifact contract, the manifest records `os_driver_cache: "unknown"` for all standard runs. The runner never claims OS/driver cache coldness unless explicit OS-level cache invalidation (requiring elevated host permissions) has occurred and been verified.

---

## 14. Host Orchestration and ADB Runner (`perf-bench android`)

### 14.1 Cross-Platform Portability (Windows and Linux)
The Android test orchestrator is integrated directly into `wtbuild` (`perf-bench android`):
- **Universal Host Execution:** Uses `std::process::Command` with direct argument vector arrays on both Windows (`adb.exe`) and Linux (`adb`).
- **No Host Shell Dependencies:** Never spawns Unix shell processes (`/bin/sh`, `bash`), `timeout`, `/proc`, or `pgrep` on the host, guaranteeing identical behavior across developer desktops and CI runners.

### 14.2 Device Ownership and Lease Management
- **Explicit Serial Targeting:** Automated runs target an explicit device serial via `--serial <serial>`. If multiple devices are attached and no serial is passed, the runner stops immediately with `RunnerExitStatus::AmbiguousDevice`.
- **Create-New Application Lease:**
  - Before staging or starting, the runner queries `pidof <package>`.
  - If an active instance exists without a diagnostic lease, the runner refuses to start (`ExistingAppRunningConflict`), protecting active interactive gameplay sessions from disruption.
  - If an active diagnostic lease is held by another session, the runner rejects the collision (`ConcurrentRunConflict`).

### 14.3 Watchdogs, Deadlines, and Transport Recovery
- **Independent Deadlines:**
  - `startup_deadline_secs` (default 15.0s): Limits the time allowed for the app to acknowledge launch and write `manifest.json`.
  - `scenario_deadline_secs` (default 60.0s): Limits the total duration of the scenario until `outcome.json` is generated.
  - `recovery_deadline_secs` (default 5.0s): Bounded window to recover from intermittent USB or ADB transport disconnections.
- **Transport Disconnect vs. Crash:** Loss of the ADB transport is tracked distinctly from guest process crashes. If the transport disconnects while the process might still be running, it is reported as `TransportLost` rather than falsifying an application crash or success.

### 14.4 Safe Termination Boundary
- **Scoped Cleanup:** At session completion, timeout, or cancellation, the orchestrator invokes `am force-stop <package>` strictly targeting the specified package ID (default `org.witchtime.android`).
- **No Collateral Damage:** Unrelated packages, system services, and background daemons are never signaled or terminated.
- **Non-Destructive Operations:** The orchestrator never uninstalls packages, replaces certificates, or clears global application data as an automatic fallback.

### 14.5 Host CLI Syntax
```bash
# Basic run on single connected device:
wtbuild perf-bench android --request build/diagnostics/run_01/request.json

# Explicit device and package targeting with custom deadlines:
wtbuild perf-bench android \
  --serial 1234567890ABCDEF \
  --package org.witchtime.android \
  --request requests/shibuya_prologue.json \
  --out build/diagnostics/run_01 \
  --startup-deadline 15.0 \
  --scenario-deadline 45.0
```

---

## 15. Module Identity, Packaging Verification, and Rollback Safety

### 15.1 Module Identity Tying
To guarantee that performance and correctness comparisons evaluate identical binaries:
- **Build Identity Manifest:** Each session manifest commits `DiagnosticBuildIdentity` tying:
  - `runtime_lib_hash`: SHA-256 digest of `libwitchtime.so`.
  - `game_shim_hash`: SHA-256 digest of `libgame.so`.
  - `main_executable_hash`: SHA-256 digest of `targets/exefs/main.elf`.
  - `generated_sdk_identity`: SHA-256 digest of `targets/exefs/sdk.elf`.
  - `apk_hash`: SHA-256 digest of the packaged APK file.
  - `build_variant`: `"diagnostic"` (matching native optimization `"release"`).
- **Source Revision vs. Binary Reality:** Matching Git commit hashes alone do NOT establish comparable runs. Only matching module hashes and build IDs certify identical binaries.

### 15.2 Stand-in Game Rejection Policy
- **Link Stand-in Stubs:** Development link stand-ins (generated by `wtbuild link` to verify linker symbols without game data) are strictly excluded from packaging.
- **Packaging Refusal:** `scripts/android-package.sh` and `verify_module_identities()` reject any build where `libgame.so` contains the link stand-in marker or is smaller than 16 KiB.

### 15.3 Architecture and Mixed-Library Protection
- **AArch64 Architecture Guard:** All packaged native libraries must declare ELF machine `EM_AARCH64` (183) and 64-bit class. Mixed architectures (e.g. host x86_64 binaries accidentally placed into `jniLibs/arm64-v8a`) are rejected immediately.
- **Page Size Alignment:** In conformance with Android 15 requirements, all native load segments must be aligned to 16 KiB (`-Wl,-z,max-page-size=16384`).

### 15.4 Non-Destructive Stop on Signing / Package Mismatch
- **Certificate Guard:** When staging an APK to a device where an existing installation exists with a different signing key or package certificate, Android ADB returns `INSTALL_FAILED_UPDATE_INCOMPATIBLE`.
- **Strict Prohibition Against Uninstall:** The orchestrator halts immediately with an actionable warning. **Under no circumstances does the runner execute `adb uninstall` or `pm clear`.**
- **User Save Protection:** The user's original saves, configuration, and app data are never deleted as an automatic installation repair.

### 15.5 Compatible Rollback and Local-Only Output Policy
- **Rollback Procedure:** If a candidate diagnostic build exhibits regressions or crashes, reverting simply entails reinstalling the previously authorized baseline package (`adb install -r <baseline-apk>`) or launching the ordinary non-diagnostic variant.
- **Local Artifact Isolation:** All telemetry chunks, metrics, and incident reports remain private on the local workstation under `build/diagnostics/<run-id>/`. No automated export or remote transmission is ever initiated without explicit developer command.

---

## 16. Stage A Supported Scope, Capabilities, and Verification Status

### 16.1 Stage A Deliverables and Architecture
Stage A (Groups 1 through 4) establishes complete host-side orchestration and offline analysis infrastructure alongside native Android bootstrap and replay validation:
1. **Contract and Evaluation:** Full JSON schema suite (`DiagnosticRequest`, `DiagnosticManifest`, `DiagnosticOutcome`, `DiagnosticSummary`, `ComparisonEligibility`, `DiagnosticComparisonReport`) with strict null-vs-zero semantics, deterministic segment statistics (phase budgets, present drops, median/tail percentiles), and Markdown reporting.
2. **Replay and Route Validation:** Strict versioned replay format parser with CRC32 integrity, Type-A controller mapping guard, native A confirmation policy verification, FullKey player 1 synthetic injection, and occurrence-based scenario checkpoint tracking.
3. **Android Diagnostic Bootstrap:** Explicitly gated `WT_DIAGNOSTIC_ENABLED` build variant, pinned SDL argument bridge passing `--diag-run-id` and `--diag-digest`, native pre-dlopen validation, and session-isolated workspaces (`userdata`, `cache/shaders`, `cache/vk`, `config`, `artifacts`).
4. **Host Orchestration:** Windows and Linux compatible ADB runner (`wtbuild perf-bench android`), create-new application lease locking, independent timeouts (startup, scenario, recovery), and non-destructive termination.
5. **Module Identity Verification:** Cryptographic tying of APK, runtime library, game shim, main executable, and SDK identities, preventing stale mixed libraries or stand-in game binaries from executing.

### 16.2 Gate Status Distinction and Honest Evaluation
- **Implemented Stage A Gates:** Replay integrity, input source and mapping policy compliance, checkpoint progression, and frame/GPU timing statistics are fully evaluated.
- **Unimplemented Stage B/C Correctness Gates:** Continuous hardware counters, system thermal metrics, and native tombstone crash reporting remain explicitly marked as `not_checked`.
- **Strict Evidence Rule:** Unchecked or missing required evidence never defaults to `passed` and cannot yield a green verdict. Incomplete runs report `inconclusive` or `ineligible`.

### 16.3 Legacy Invocation and Hardware Status
- **Desktop Workflow Intact:** Existing desktop test, setup, link, and report invocations in `wtbuild` remain 100% operational and backwards-compatible.
- **Hardware Pending Notice:** All physical device validation and real-hardware execution are strictly deferred to **Group 10**, requiring separate explicit developer authorization. Stage A is verified in CI via synthetic fixtures and fake transport mocks.

---

## 17. Continuous Subsystem State, Counters, and Units

### 17.1 Extended Bounded Event Types
Stage B extends the production 64-byte SPSC flight recorder with dedicated continuous subsystem observations:
| Event Type | ID | Payloads (p0, p1, p2, p3) | Description |
|---

---

## 18. Live Status, Android Lifecycle, and Actual vs. Inferred State

### 18.1 Atomic Live Status Snapshot (`status.json`)
The diagnostic writer emits a compact, machine-readable snapshot to `status.json` atomically via `status.json.tmp` rename:
- **Refresh Frequency:** Published approximately once per second by the background flusher or explicitly upon key lifecycle milestones.
- **Payload Schema:**
  - `lifecycle`: Concrete Activity lifecycle state (`created`, `started`, `resumed`, `paused`, `stopped`, `destroyed`) and `is_foreground` flag.
  - `surface_generation`: Integer counter tracking native EGL/Vulkan surface recreation events (e.g. folding/unfolding display transitions, orientation changes).
  - `thermal`: `headroom` (0.0 to 1.0+ or `null` if unsupported) and Android `thermal_status` level (`none`, `light`, `moderate`, `severe`, `critical`, `emergency`, `shutdown`).
  - `progress`: `render_age_ms` (time elapsed since last frame present), `semantic_freshness_ms` (time since last confirmed chapter/verse transition), `is_active_stall`, and `expected_inactivity`.
  - `recorder`: Live diagnostics health, active producers, records written, total bytes, and remaining disk budget.

### 18.2 Actual vs. Inferred State Boundary
To ensure honesty in automated agent reasoning:
| State Aspect | Actual Evidence (Measured) | Inferred Assessment (Heuristic) |
|---

---

## 19. Incident Retention, Prehistory Ring, and First-Occurrence Preservation

### 19.1 Incident Storage Budgets and Ceilings
To prevent runaway diagnostic storage consumption during failure storms:
- **Maximum Incident Captures:** Bounded strictly to at most **3** deep incident bundles per session (`DIAG_MAX_INCIDENTS = 3`).
- **Disk Budget Reserve:** 16 MiB of the total 128 MiB session budget is reserved exclusively for incident captures (`incident_reserve_bytes`). Regular telemetry chunk writers stop when the unreserved 112 MiB is reached, ensuring incident captures always have disk space to persist critical fault evidence.
- **Storm Protection:** If more than 3 incidents trigger during a session, additional triggers increment `incident_storms` in `status.json`. Triggers beyond the 3rd are dropped, strictly preserving the first 3 incidents without overwriting or disk exhaustion.

### 19.2 Writer-Owned Prehistory Ring Buffer
The diagnostic writer maintains an in-memory ring buffer of the most recent **1024** committed records:
- **Zero-Copy Hot Path:** Producers emit records into their SPSC queues. As the background writer flushes chunks, records are copied into the prehistory ring.
- **Incident Prehistory Persistence:** Upon triggering an incident, the writer immediately flushes all pending producer buffers and writes the prehistory ring to `<session-dir>/incidents/incident-%03u/prehistory.jsonl`.
- **Short History Honesty:** If an incident occurs early in execution when fewer than 1024 events have elapsed, the prehistory manifest records the exact available event count (e.g. `event_count: 5`) and actual earliest sequence number without padding or falsification.

### 19.3 First-Occurrence Preservation and Linked Recoveries
- **First-Occurrence Invariance:** The root failure (`incident-001`) captures the initial causal anomaly and is permanently preserved for the remainder of the session. Subsequent cascading stalls or faults cannot overwrite the original failure evidence.
- **Non-Destructive Linked Recoveries:** If the engine successfully recovers from an incident (e.g. swapchain recreation resumes frame presentation), the recovery is recorded as a linked event in the existing `incident.json`:
  ```json
  "recovery": {
    "recovered": true,
    "reason": "surface_recreated_frames_resumed",
    "resume_timestamp_ns": 1234567890
  }
  ```
  The recovery does NOT erase the incident, alter the original failure reason, or mark the scenario completed.

### 19.4 Honest Capture Coverage and Artifact Links
- **No Nonexistent Pre-Trigger Claims:** In conformance with the artifact contract, incident manifests declare explicit coverage:
  `pre_trigger_screenshot: "none"`, `deep_trace: "unavailable"`. If a deep collector (such as Perfetto or simpleperf) is started reactively after a trigger, the manifest records `post_trigger_coverage: "started"` with its genuine `collector_start_ns` timestamp.
- **Deterministic Replay and Trace Links:** Incident bundles record the exact input replay frame offset, active scenario checkpoint occurrence ID, and last confirmed semantic state (`last_chapter`, `last_verse`), allowing developers and automated agents to correlate incident telemetry with the deterministic input trace.

---

## 20. Android Crash Preservation, Async-Signal-Safe Channel, and Fatal Hook

### 20.1 Deadlock Avoidance and Platform Reporting Preservation
- **Problem with Legacy Report Path on Android:** The legacy crash handler executed `printf()`, `dump_guest_registers()`, `plat_backtrace()`, and `report_and_exit(4)`. On Android Bionic:
  1. `printf()` and libc unwinding allocate memory or acquire stdio locks. If a crash occurs while a logging or heap lock is held, the handler deadlocks indefinitely.
  2. Calling `report_and_exit()` invokes `exit()`, cleanly terminating the process and suppressing Android's `debuggerd` from generating native tombstones and system `ApplicationExitInfo` records (`REASON_CRASH_NATIVE`).
- **Preserving Platform Crash Reporting:** In diagnostic sessions (`diag_is_enabled()`), the fatal signal handler intercepts `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, and `SIGABRT` solely to persist an async-signal-safe 64-byte breadcrumb, resets the signal handler to `SIG_DFL`, and re-raises the signal. This guarantees that Android `debuggerd` generates the official native tombstone and OS crash log without interference.

### 20.2 Async-Signal-Safe Breadcrumb Channel
- **Zero-Allocation Fixed 64-Byte Record:**
  ```c
  typedef struct diag_crash_breadcrumb {
      uint32_t magic;              /* DIAG_CRASH_MAGIC: 0x43525348 ('CRSH') */
      uint32_t signal_num;         /* Fatal signal / exception code */
      uint64_t fault_address;      /* Faulting memory address (si_addr) */
      uint32_t thread_id;          /* OS thread ID (gettid / GetCurrentThreadId) */
      uint32_t exit_reason;        /* 0 = crash */
      uint64_t monotonic_ns;       /* Monotonic timestamp in nanoseconds */
      uint64_t global_seq;         /* Last diagnostic sequence number */
      uint64_t reserved[3];        /* Padding to exactly 64 bytes */
  } diag_crash_breadcrumb_t;
  ```
- **Pre-Opened File Descriptor:** `diag_init_crash_channel(session_dir)` pre-opens `<session_dir>/crash_breadcrumb.bin` during session initialization. In the fatal signal handler, the breadcrumb is written via a single raw `write(2)` syscall followed by `fsync(2)`, requiring no memory allocation, no mutexes, and no path resolution.
- **Alternate Signal Stack:** `diag_setup_altstack()` configures a dedicated 64 KiB alternate signal stack via `sigaltstack(2)` (`SA_ONSTACK`), ensuring that stack-overflow crashes can still execute the breadcrumb recording.

### 20.3 Platform Chaining and Limits
- **Linux/Android:** After committing `crash_breadcrumb.bin`, the fatal hook invokes `signal(sig, SIG_DFL)` and `raise(sig)`, cleanly transferring control to the kernel and platform crash handlers.
- **Windows:** In desktop/Windows runs with diagnostics enabled, the breadcrumb is written and `crash_filter` returns `EXCEPTION_CONTINUE_SEARCH` so Windows Error Reporting or attached debuggers handle the crash.
- **API and Platform Limits:** Crash breadcrumbs capture process-internal state up to the fatal moment. Full stack unwinding and register context collection are deferred to platform tombstone generators and offline symbolization (Section 21) rather than risking in-process unwinder deadlocks.

---

## 21. ApplicationExitInfo, Native Tombstones, Vulkan Faults, and Offline Symbolization

### 21.1 ApplicationExitInfo Parsing and Limitations
- **Android Platform Support:** `dumpsys activity exit-info <pkg>` is available on Android 11+ (API level 30+). On older versions (API 29), the status is recorded as `ApiLevelUnsupported(29)` without treating it as a failure.
- **Overwritten / Absent Records:** Under heavy process turnover or rapid restarts, Android's circular buffer may overwrite exit records. If no record matches the target PID, `ExitInfoStatus::AbsentOrOverwritten` is reported.
- **Non-Fabrication Invariant:** A missing `ApplicationExitInfo` record is NEVER turned into a confirmed crash or ANR cause. Root cause classification requires genuine platform tombstone, exit-info, or crash breadcrumb evidence.

### 21.2 Native Tombstone and Logcat Crash Ingestion
- **Root-Free Collection:** Tombstones from `/data/tombstones/` typically require root privileges. The offline analyzer ingests crashes via standard user-accessible streams:
  1. `logcat -b crash -d` or `logcat -d -s DEBUG`
  2. `ApplicationExitInfo.getTraceInputStream()` dumps where supported
  3. Pre-committed runtime `crash_breadcrumb.bin`
- **Parsed Context:** The parser extracts the fatal signal number, signal mnemonic (`SIGSEGV`, `SIGBUS`, etc.), faulting memory address, faulting thread ID, and backtrace frames with their load modules and ELF Build-IDs.

### 21.3 Offline Symbolization and Build-ID Verification
- **Build-ID Matching:** Each backtrace frame includes the module Build-ID when present. The local symbolizer matches local unstripped libraries against this exact Build-ID:
  - If Build-IDs match: symbol resolution proceeds against local ELF `.symtab` / `.strtab`.
  - If Build-IDs mismatch: the symbolizer strictly refuses to use mismatched symbols (which would produce deceptive function names) and reports `BuildIdMismatch`, falling back to the raw module offset (`module+0xoffset`).
  - If local symbol files are unavailable: reports `SymbolsNotFound` and preserves the raw offset.
- **Guest Function Address Mapping:** Recompiled functions (`f_<guest_hex>`) are resolved to their original guest addresses (e.g. `0x710004a000`) using the local guest address map, without disclosing private game code or uploading binaries to cloud symbol servers.
- **Privacy and Local Symbol Retention:** All crash symbolization is performed 100% offline on the developer host machine. No symbol files, dumps, or private game assets are transmitted to external services.

---

## 22. External Observer State Model, Stop Rules, and Bounded Recovery

### 22.1 Observer State Model
The host-side runner (`AndroidRunner`) maintains an explicit phase state machine tracking process and transport transitions:
- `Init`: Verifying lease, establishing ADB communication, validating device connectivity, staging `request.json` and `active_lease.json`.
- `Startup`: Process initiated via `am start`; awaiting `manifest.json` acknowledgement with verified digest and first frame rendering (`surface_generation > 0` or `render_age_ms < 5000`).
- `ActiveGame`: Game process alive, in foreground, continuously presenting frames and advancing scenario checkpoints.
- `BackgroundInactivity`: Application transitions to stopped/paused lifecycle state (`is_foreground: false`, `expected_inactivity: true`). Frame presentation pauses intentionally; observer suppresses stall detection.
- `LiveStall`: Application is foregrounded, but render presentation age exceeds the active stall threshold (`is_active_stall: true`).
- `ConfirmedDeath`: `pidof` reports empty; process terminated unexpectedly or via fatal crash.
- `AdbTransportLoss`: Transport dropped (USB disconnect or ADB daemon restart); observer enters recovery window.
- `Completed`: `outcome.json` successfully published with clean completion.

### 22.2 Presentation Failures vs. ANR
- **No-Presentation Invariant:** If the startup deadline expires after acknowledgement but no frame was ever presented, the runner terminates with `RunnerExitStatus::NoFirstFrame` (or `StartupTimeout`).
- **No Automatic ANR Classification:** Missing frame presentation is NEVER labeled an Android OS ANR unless an official `ApplicationExitInfo` record with `REASON_ANR` or an OS ANR trace is confirmed by the system.

### 22.3 First-Failure Preservation Across Bounded Recoveries
- **First-Occurrence Invariance:** When an active stall or unexpected death occurs, the runner generates a `FirstFailureRecord` capturing the initial causal phase, reason code, and timestamp.
- **Bounded Recovery Limits:** If bounded recovery is authorized (`max_recovery_attempts > 0`), the runner executes at most the declared number of recovery attempts (e.g. surface refresh or process restart).
- **Evidence Integrity:** Subsequent recovery attempts or secondary failures NEVER overwrite or erase the `FirstFailureRecord`. The root cause remains permanently pinned to the initial failure.

### 22.4 Reconnect Without Duplicate Launch
- **Transport Dropout Recovery:** When an ADB command fails, the runner records the disconnect timestamp and polls for transport restoration up to `recovery_deadline_secs`.
- **Duplicate Launch Prevention:** When ADB connection is restored, the runner inspects `pidof`. If the original process PID is still alive and running, the runner resumes observation directly (`reconnected_without_relaunch = true`) and strictly avoids issuing a second `am start` command.

### 22.5 Stop Rules
The observer halts execution and pulls artifacts immediately without recovery under the following unrecoverable conditions:
1. **Device / Lease Conflict:** Another diagnostic run or active interactive game session is running (`ConcurrentRunConflict`, `ExistingAppRunningConflict`).
2. **Digest Mismatch:** The target package acknowledged a mismatched request digest or run ID (`AcknowledgeMismatch`).
3. **Confirmed Death with Crash Breadcrumb:** The target process crashed and deposited a native signal breadcrumb (`ConfirmedDeath`).
4. **Transport Timeout Exceeded:** The ADB transport remained unavailable for longer than `recovery_deadline_secs` (`TransportLost`).
5. **Recovery Attempts Exceeded:** The maximum allowed recovery attempts have been exhausted (`LiveStall`).

---

## 23. Targeted Deep Capture, Native ATrace Scopes, and Root-Free Collection

### 23.1 Native ATrace Scopes
- **Dynamic Linkage:** `diag_atrace_init(bool enabled)` dynamically binds `ATrace_beginSection`, `ATrace_endSection`, and `ATrace_setCounter` from `libandroid.so` when opted-in. On desktop platforms or when disabled, all ATrace macros safely compile into no-ops.
- **Instrumented Critical Sections:** Scopes are placed around high-overhead render submit boundaries, pipeline compilations, and audio packet queues.
- **Overhead Containment:** ATrace markers are active only when the session profile declares `deep_trace: true` or specifies an ATrace collector.

### 23.2 Root-Free Perfetto and Simpleperf Collection
- **App-Level User Permissions:** Deep capture commands (`perfetto`, `simpleperf`) are constructed strictly using application-level filtering (`--app <package>`).
- **Security Invariant:** Deep capture NEVER invokes `su`, never attempts `setenforce 0`, and requires no system or rooting modifications.
- **Capability Verification:** `check_collector_capabilities` validates tool availability on device PATH and enforces Android API requirements (Perfetto requires Android 9+ / API 28+; simpleperf requires API 24+).

### 23.3 Bounded Time and Storage Budgets
- **Execution Limits:** Duration is bounded to a declared maximum (`max_duration_secs <= 120s`) and terminates automatically via collector CLI timeouts.
- **Storage Limits:** Trace sizes are constrained to `max_size_bytes` (e.g. 32 MiB). Ingestion verifies file bounds and flags `budget_exceeded` if trace buffers overran.
- **Lost Sample Accounting:** Trace parsers inspect headers for drop records or circular buffer wraps, exposing `lost_samples` directly to the report rather than silently ignoring gaps.

### 23.4 Strict Non-Pooling Policy
- **Independent Diagnostic Treatment:** Deep trace runs introduce measurable sampling and syscall overhead.
- **Comparison Disqualification:** Under the diagnostic schema, runs with `deep_capture: true` are strictly disqualified from being pooled or compared against clean lightweight baseline/candidate performance runs. Dedicated deep capture is reserved for targeted root-cause investigation recipes.

---

## 24. Checkpoint Visual Assertions, Tolerances, and Provenance Approval

### 24.1 Bounded Checkpoint Render-Image Assertions
- **Assertion Trigger:** Checkpoints in the scenario state machine trigger capture of the presented swapchain texture at designated occurrence IDs.
- **Error Metrics:** The comparator computes Mean Absolute Error (MAE) and mismatched pixel fraction across all unmasked pixels.
- **Bounded Region Masking:** Volatile screen regions (dynamic HUD text, mini-maps, timestamps, nondeterministic particle overlays) are excluded from comparison via declared bounding boxes (`Rect { x, y, width, height }`).
- **Temporal Tolerances:** Frame offsets (+/- 1-2 frames) accommodate subtle sub-frame animation phase differences without false positives.

### 24.2 Heuristic Flash Detection vs. Assertion Failures
- **Suspected Anomaly Classification:** The 16x9 runtime white-flash detector logs suspected brightness jumps as heuristic anomalies (`PassWithSuspectedFlashAnomaly`).
- **Separation of Heuristics from Correctness Gates:** A heuristic flash detection NEVER automatically fails a correctness gate. A visual gate failure requires an approved checkpoint image assertion to exceed defined MAE or pixel mismatch limits.
- **Texture vs. Physical Display:** Captured Vulkan swapchain textures explicitly record `physical_display_verified: false` to distinguish texture-level buffer validation from unverified physical panel display.

### 24.3 Rejection of Candidate Self-Promotion
- **Provenance Requirement:** Visual references must carry explicit human approval metadata (`approved_by`, `sha256`, `approved_timestamp_ms`, `settings_hash`).
- **Self-Promotion Prohibition:** Candidates are strictly forbidden from promoting their own rendered frames to baseline references (`SelfPromotionRejected`).
- **Settings Matching:** Assertions verify that rendering resolution and graphics configuration match the reference provenance exactly (`SettingsMismatch`).

---

## 25. Audio Observation Boundaries, Backend Gates, and A/V Synchronization

### 25.1 Five Audio Observation Boundaries
1. **Guest PCM Generation:** Tracks guest engine audio output via `AppendAudioOutBuffer` (`audio_appended_frames`).
2. **Host Output Backend:** Identifies the active audio sink (`AudioBackendKind`):
   - `RealDevice`: Actual hardware device backend (AAudio, Oboe, ALSA, DirectSound).
   - `ClockFallback`: Software clock consumption without hardware playback (triggered by `WT_AUDIO_MUTE=1` or host audio device open failure).
   - `DisabledOrMuted`: Host audio device opted out.
3. **Starvation and Buffer Underruns:** Monitors `audio_starved` (guest engine failed to supply audio chunks in time) and `audio_underruns` (host output buffer depleted before next submit).
4. **Audio/Video Synchronization:** Computes clock drift between cumulative audio samples released (`(released_frames * 1e9) / sample_rate`) and elapsed presentation timestamps (`|audio_ns - video_ns|`).
5. **Physical Sound Emission:** Acknowledged as `physical_sound: "not_checked"`. Software assertions strictly observe data buffers up to the output backend and cannot verify actual acoustic transducer vibration without external acoustic sensors.

### 25.2 Output / Speaker Gate Invariants
- **Guest PCM Alone Is Insufficient:** Producing valid PCM buffers in the guest mixer does NOT prove that sound was emitted to the device speaker. If a real backend is requested, running under clock-fallback or mute fails the audio gate with `FailedRealBackendRequired`.
- **Non-Blocking Audio Callbacks:** Diagnostic counters in `runtime/nnaudio.c` update atomic state variables (`DIAG_ATOMIC_STORE`) on the audio thread. Audio processing never executes file I/O, string serialization, or mutex locking, completely eliminating diagnostic audio hitching or buffer starvation.

---

## 26. Isolated-Save Progression, Lifecycle Surface Recovery, and Fold Validation Limits

### 26.1 Isolated-Save Progression Assertions
- **Progression Mutability:** Checkpoint progression in scenarios legitimately mutates the session save file (`slot0.sav`). A hash difference from the session's starting save is verified as valid progression (`ValidProgression`), not a correctness regression.
- **Reload Integrity:** Following save emission, the engine asserts reloadability and checksum verification (`fs_OpenFile`, `fs_ReadFile`). Write refusals (`WriteFailed`) or corrupt state loads (`ReloadCorrupted`) trigger immediate correctness gate failure.
- **Original User Save Invariance:** The user's baseline save files in shared storage are verified before and after each session (`original_save_start_hash == original_save_end_hash`). Any byte delta on the original user save is flagged as `OriginalUserSaveTampered`.

### 26.2 Lifecycle Surface Loss and Recovery
- **Surface Recreation:** Window resizing, orientation changes, or app foreground restoration increment `surface_generation`.
- **Presentation Resumption:** Recovery requires that frame presentation resumes on the new surface generation (`RecoveredCleanly`). If `surface_generation` increments but frames cease presenting, `SurfaceRecoveryFailed` is raised.

### 26.3 Modeled WSI vs. Physical Fold Validation
- **Synthetic Limitation:** Modeled WSI swapchain tests assert surface loss and recreation within driver memory, but strictly record `physical_fold_validated: false`.
- **Human / Physical Device Evidence:** Full validation of foldable display behavior (e.g. Oppo Find N5 inner/outer panel transition, hinge posture changes, aspect ratio resizing under game load) requires real physical hardware execution and direct human/device observation (documented under Group 10). Modeled harnesses never claim physical fold validation.

---

## 27. Paired-Run Comparison Policies, Run-Level Uncertainty, and Stop Rules

### 27.1 Predeclared and Frozen Comparison Policy
- **Policy Freezing:** Comparison thresholds, noise floors, practical margins, and equivalence boundaries must be declared and frozen (`is_frozen: true`) prior to testing candidate binaries (`PolicyNotFrozen`).
- **Elimination of Significance Peeking:** Evaluating significance on individual frames or peeking repeatedly after each frame inflates statistical confidence due to temporal autocorrelation. Uncertainty is computed strictly across independent paired runs ($n = \text{number of pairs}$).

### 27.2 Classification Spectrum
1. **Improvement:** Candidate demonstrates median frame-time reduction exceeding `practical_margin_pct` (e.g. > 2.0%) without regressing p99 tail latency.
2. **Regression:** Median or tail frame times worsen beyond `practical_margin_pct`.
3. **Equivalence:** Mean differences remain constrained within `equivalence_threshold_pct` (e.g. +/- 1.5%), confirming identical performance within noise limits (A/A calibration).
4. **Trade-Off:** Candidate improves median performance while degrading tail latency (p99/p95), or vice versa.
5. **Insufficient Evidence:** Fewer than `min_pairs` completed, requiring additional paired executions.
6. **Inconclusive Noise:** Observed deltas fall within the noise floor (`noise_margin_pct`).

### 27.3 Bounded Repetition and Stopping Rules
- Execution evaluates at least `min_pairs` (default 3) and at most `max_pairs` (default 5).
- If statistical convergence is established at `min_pairs`, the trial terminates cleanly to save device battery and compute budget. If noise remains high at `max_pairs`, the outcome is recorded as `InconclusiveNoise`.

---

## 28. Matched Thermal/Cache Eligibility and Sustained-Play Degradation

### 28.1 Short-Run Thermal and Cache Eligibility
- **Initial Thermal Baseline:** Short-run comparisons enforce matched starting thermal conditions (`|baseline_headroom - candidate_headroom| <= 0.10`). Starting with a pre-heated device invalidates comparative claims.
- **Cache Class Equivalence:** Cold cache runs must be evaluated against cold cache baselines. Warm cache runs must share identical pre-warmed pipeline cache digests (`CacheClassMismatch`). A warm candidate compared to a cold baseline is rejected to prevent caching artifacts masquerading as code optimizations.
- **Handling Unsupported Devices:** On devices or Android versions lacking thermal headroom APIs, thermal status is preserved as `unsupported`, and the report qualifies that power/thermal conclusions remain unverified.

### 28.2 Sustained-Play Trajectory and Throttling Detection
- **Time-to-Degradation (`time_to_degradation_secs`):** For sustained runs (10-30 minutes), the evaluator records median FPS across consecutive time windows (e.g. 60s windows). When sustained frame rate drops by >= 10% concurrent with thermal headroom depletion, the onset timestamp is logged.
- **Candidate-Caused Throttling:** If the candidate binary causes earlier or severe thermal throttling absent in the baseline, the outcome is flagged as `CandidateCausedThrottling`. Throttled late-game performance is retained and reported rather than discarded.
- **Excluded Interval Accounting:** Asset loading screens and scene transition pauses are explicitly declared as excluded windows (`is_excluded: true`), ensuring loading latency does not distort steady-state gaming trajectories.

---

---
---

## 29. Recorder Overhead Calibration and Capped-FPS Cost Accounting

### 29.1 Design Overhead Budget
- **Frame Latency Budget:** <= 0.25 ms median frame latency overhead on target 60 FPS gameplay (< 1.5% overhead).
- **Memory Footprint Budget:** <= 16 MiB additional resident memory overhead across all ring buffers, incident prehistory, text sink, and chunk queues.

### 29.2 Capped-FPS Cost Accounting
- **Vsync Masking:** Games operating with Vsync or capped frame rates (30 / 60 FPS) exhibit identical 16.66 ms frame-to-frame intervals whether recorder is enabled or disabled.
- **Mandatory CPU / Memory Tracking:** The evaluation enforces that capped-FPS comparisons report CPU execution time (cpu_overhead_ms) and resident memory growth (memory_overhead_bytes). A zero-millisecond frame time delta under Vsync is never claimed as zero overhead.

### 29.3 Hardware Calibration Status and Limits
- **Host vs. Physical Device Evidence:** Until physical device trials are conducted on authorized hardware, overhead metrics are categorized strictly as unverified_hardware_overhead (hardware_calibrated: false).
- **Device Provenance:** Real device calibration values require hardware execution under explicit authorization (Group 10), preserving clear separation between verified host models and unexecuted physical benchmarks.
