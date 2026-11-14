# Bounded temporal source validation capture

This collector records private local scalar evidence. It is **not** a motion-source
qualification, previous-pose claim, actor-ownership claim, or performance sample.
No generated game source belongs in Git.

## Activation and lifecycle (implemented; full relink/device evidence pending)

Request `profile.profile_name: "validation"` and explicitly include
`"temporal_source_capture"` in `profile.collectors`. Bootstrap rejects other
profiles, duplicates and unknown collectors. Ordinary builds expose a false typed
flag. No environment variable enables this collector.

Three lifecycle sites are wired. Core `diag.c`, `diag.h` and writer/seal protocol
are not modified by this change:

1. In `android/app/src/main/cpp/main.c`, **after** successful request validation
   and runtime `dlopen`, and **before** calling `display_android_run`, resolve
   `wt_tsc_start`/`wt_tsc_finish` from the runtime and optional `game_temporal_source_bind` from
   its loaded dependency scope. Only if
   `diag_bootstrap_temporal_source_requested()` is true, call
   `wt_tsc_start(validated_run_dir, 1, optional_bind)`. Check its return code;
   missing game export produces an unavailable sidecar when finalized. Missing
   runtime export or file-open failure must be reported as collector unavailable,
   not silently accepted. The native entry retains the already validated run
   directory, writes a rejection outcome and stops before runtime execution on
   initialization failure. Its post-run checked finish covers early runtime
   returns; it never replaces an existing nonzero runtime return code.
2. `nvn_vk_draw.c` calls `wt_tsc_frame(U.frame, scene_eligible)` after the real
   `upscale_draw` scene observation. Arming requires a non-preflight draw,
   no camera conflict, and `upscale_scene_matches` for the current color/depth
   textures and image identities, current scene/depth/camera frames and matching
   nonzero generations. It does not use generic present eligibility. Following
   `fsr_present`'s scene-frame increment, a second call with `eligible=0` advances
   the window even on frames with no eligible scene; it can never arm capture.
   Progress/held presents bypass that existing boundary and do not advance it.
   These are renderer scene labels, not a proven guest submission-frame join.
3. In both `boot.c`'s normal return and `hostrt.c`'s `report_and_exit`, before module unload and after closing producer
   admission, call `wt_tsc_core_loss(diag_get_total_dropped())`, then check
   `wt_tsc_finish()`. A failed core seal must contribute at least one loss even
   if the drop snapshot is zero. This includes the existing TLS 16-slot
   exhaustion/drop count: ignoring it leaves `core_loss_checked: false` and
   capture incomplete. A failed finish fails an otherwise clean exit; an
   existing nonzero guest exit and core non-quiescent reason retain priority.
   Core shutdown still occurs only after a successful seal. Do not reinitialize a run with live
   producers. An abnormal process termination can leave an incomplete binary;
   absent/empty final sidecar means unavailable/incomplete, never success.

The module is in `tools/wtbuild/src/link.rs`'s runtime source list. The guest
binding export lives in `runtime/game_shim_eh.c` through
`temporal_source_guest.h`; no `Ctx` or `GameHost` layout changes are made.

## Bounds and honest coverage

The first eligible scene arms four frame identities `[first, first+4)`, with at
most 1024 records and a 256 KiB binary budget. The fixed-size record is 168 bytes
on the checked little-endian Android/host ABIs. The current record cap therefore
limits binary size to 172032 bytes, below the independent byte cap. Existing
files are opened exclusively, never truncated. There are 32 bounded recording
identity slots; exhaustion stops capture with an unavailable count. Guest and
backend producers atomically reserve fixed artifact slots, write their own
record, and release-publish that slot. They do no file I/O, allocate no memory
and register no global or TLS SPSC diagnostic producer. Only the bounded
frontend recording-identity map is serialized. This is a separate fixed heavy
capture artifact, not a core timing ring or a new tracing framework.

On Android/POSIX, startup applies the existing diagnostic export mode `0666`
using `fchmod(fileno(file), 0666)` on each exclusively created file and verifies
the resulting descriptor mode with `fstat`. A permission syscall failure or
silent mode clamping rejects initialization, closes both streams and removes
the newly created files before any guest binding or capture activation. A
pre-existing file is never chmodded, truncated or removed. No process umask,
directory mode, validated run path or fixed filename is changed; Windows keeps
its existing ACL/CRT behavior. Local/private handling requirements still apply
to both artifacts. Descriptor chmod does not bypass Android mount, scoped-storage
or SELinux policy: retrieval still needs verification on a fresh rebuilt run.
If that storage rejects/ignores the mode or still denies export, the lead must
use an already authorized diagnostic export route rather than weakening this
collector's initialization checks or requesting arbitrary paths.

Outside activation there is no hashing, file I/O, allocation or callback emission.
Frontend/backend hooks have a disabled atomic gate; an instrumented guest lift
also has gated site calls. This is not a zero-instruction-cost production claim.

`temporal_source.json` reports counts, limits, overflow, unavailable observations,
I/O error, publication completion/pending writers/unpublished slots, core loss
accounting, whether an eligible scene was seen, whether the guest export bound,
guest record count and backend selection record count. `guest_sites` is
`unavailable` when no guest records arrived, even if an uninstrumented shim bound.
It always reports `join: "unknown"`, `backend_execution: "unavailable"` and
`motion_source_qualified: false`.

Finalization stops admission and never waits for a paused writer. It copies only
release-published slots; an unfinished reservation is omitted and explicitly
reported incomplete. No slot is recycled or overwritten. `wt_tsc_finish()`
returns failure for incomplete capture, overflow, losses, missing guest evidence,
missing core-loss accounting, an unfinished window, or I/O failure. A successful
return means a complete bounded **raw artifact**, never a qualified motion join.

Frame labels are frozen before admission and rechecked immediately after atomic
admission. An interior-frame change across that admission is explicitly counted
as loss, not silently assigned to another frame. Closing the window atomically
closes admission without publishing its excluded `first+4` label. Even writers
admitted before that boundary keep their immutable included label; they never
reread a later frame when publishing. First arm and terminal closure use one
atomic gate word (OPEN/CLOSED/TERMINAL plus admitted writer count). Once terminal
closure begins, an already-entering frame hook cannot reopen admission. Frame
metadata is atomic, and finalization reports a busy frame/tracking operation as
incomplete without waiting for its lock. Initialization/finalization require one
lifecycle owner, and a new run requires old producers to be joined; renderer
frame advancement may race finalization, and callbacks may run concurrently on
multiple guest threads.

Recording generations and ordinals are real frontend observations. **Submission,
command-list replay, actual draw execution, guest-object association and actor
ownership are not joined.** A recording already in progress at arming has unknown
generation until its next begin; observations are counted unavailable rather
than assigning an invented generation. Backend evidence is standalone pipeline
selection, not proof that a Vulkan draw or GPU work executed. Absence at a hooked
site is not proof that the broader route is unreachable.

## Binary schema, version 2

The binary record and bind/emit ABI use `WT_TSC_VERSION == 2`; the JSON
sidecar's own schema version remains 1. Bindings carry a nonzero capture epoch,
which is checked again after callback admission to isolate successive runs.

Little-endian `WtTscRecord` contains four u32 fields (`version`, `size`, `kind`,
`reserved`), then u64 `frame`, `sequence`, `site`, and sixteen u64 `word` entries.
`frame` is the collector frame-boundary label; sequence is serialized capture
  reservation order, not a guest execution/submission causality proof. Private records can
contain guest and host pointers: keep the files and backups local.

- **Guest (kind 1):** explicit instruction site, then original registers in order
  `x0,x1,x2,x3,x4,x8,x19,x20,x21,x22,x24,x30,x23,x25,x28`, followed by Ctx identity.
  The hook reads no guest memory and modifies no guest register, flag, monitor or
  guest memory. At selection `0x7100803178`, x8 is the actual vtable already
  loaded from the selected shader object; at `0x7100803184`, x8 is its actual
  `+56` target. The entry and branch records retain original draw arguments.
  Explicit tail-call sites are recorded separately because LR is not their
  immediate-caller identity.
- **Begin (kind 2):** command-buffer guest identity, new capture recording
  generation; ordinal starts at zero. Reuse of the same address receives a new
  generation. Ordinals advance for every command appended by `rec`; ordinary
  commands do not consume evidence records.
- **Program (kind 3):** command buffer, generation, ordinal, program guest
  identity, host program serial, stage mask.
- **Range (kind 4):** command buffer, generation, ordinal, NVN stage, slot,
  address, size. Only slots 9 and 11 are emitted, including selected slots inside
  a batched bind. Hardware CB12/CB14 correspond to these NVN slots, not slots
  12/14. This does not establish the semantic resource behind slot 9.
- **Backend selection (kind 5):** vertex guest program/serial, fragment guest
  program/serial, raw vertex/fragment code FNV64 identities, selected vertex/
  fragment `VStage.hb` identities, selected vertex/fragment `VStage.h` variant
  identities, tessellation-present flag, pipeline-ready flag, renderer frontend
  frame label, control/evaluation variant identities, host thread identity.
  The ordered vertex/fragment fields are the observed pair. Zero absent fields
  are not a fabricated stage. Raw hashes cover the code bytes used by the
  current source record; `hb` includes backend constants/version/options, and
  `h` identifies the selected variant. These identities are deliberately not
  conflated. Pipeline-ready is not executed.

Known guest records include the binder's live bank load and NVN11 wrapper
selection, and uploader entry/group/type/copy-endpoint scalars. The bank value is
render setup state, **not temporal history**. No palette, shader bytecode, frame
image or arbitrary memory is dumped. The render-setup bank-assignment instruction
and the NVN9 guest wrapper producer remain outside the present exact-site map;
NVN9 has frontend range evidence only.

## Reproducible private lift instrumentation

`tools/temporal_source_instrument.py` defaults to read-only verification. It
requires the exact pinned SHA256 of `c_00aa.c` and `c_00ab.c` and checks every
bounded site's original instruction. It inserts hooks **after** instruction
labels, so a branch to a label cannot bypass the hook. It leaves shared headers
and original guest instructions untouched. `--apply --backup <fresh-private-dir>`
backs up both original buckets and writes a hash manifest before changing either
file, restoring originals on a partial write failure. Re-instrumentation or a
different lift is rejected; re-lift/restore the backed-up original inputs before
applying again. All generated originals, modified buckets and backups stay private.

The script is a validation-only post-lift pass, not a broad lifter override or a
new tracing framework. Indirect calls remain indirect and original tail calls
remain tail calls. No generated private contents are embedded in its public
source: only derived sites, instruction checks and source hashes.

`tools/temporal_source_provenance.py` independently reproduces `compile.rs`'s
Android config/header hash, checks every stamped source SHA, object stamp and
AArch64 ELF relocatable target. Missing or empty generated source sets fail
closed; zero objects are never accepted as verified reuse. It does not update
stamps or build anything.
Verified before instrumentation: 409/409 original sources and objects match;
config `f2d2edb0d0f8e43916fdf7ca61a5d8a8da3549ebe29d8e119c4c5c345bf01022`.
The matching compiler spelling is
`\\?\D:\Android SDK\ndk\android-ndk-r28c\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe`,
with repo spelling `D:\Projects\Bayonetta_android_port\Witch-Time`.
Changing the compiler spelling/metadata or tracked shared headers invalidates
reuse. The new guest header is included by the shim only, not `runtime.h`.
After instrumentation only the two target buckets need lifted recompilation;
the shim and host capture/frontend/backend objects also need rebuilding and
both game/runtime libraries need relinking. Do not claim a full incremental
build solely from the object count.

## Offline checks

- `python tests/temporal_source_capture_test.py`: mocked immutable Ctx, binding
  version/size/null checks, missing export, disabled/eligible window, exclusive
  output, record cap/overflow, missing generation, bounded tracking, batched
  NVN9/11, generation reuse, distinct code/stage identities, concurrent guest
  callbacks with unique publication slots, paused/unpublished writer finalization,
  blocked reset with a live writer, deterministic post-admission closing/interior
  frame races, terminal finish-versus-first-arm interleaving, core-loss
  invalidation, missing/empty source provenance, bootstrap profile
  gates in ordinary/diagnostic builds and synthetic backup/instrumentation checks.
  Includes descriptor export/error tests: POSIX runs use restrictive umask
  `0777` and check both actual file modes; Windows injects POSIX syscall results
  and checks descriptors/modes, both permission-failure positions, stat failure,
  silently clamped mode, cleanup/retry, exclusive refusal and sentinel preservation.
  Windows results do not establish Android filesystem permission behavior.
- `cargo test --offline --manifest-path tools/wtbuild/Cargo.toml temporal_source_collector_tests`:
  Rust allowlist, validation-only helper and duplicate rejection.
- `python tests/temporal_source_lifecycle_test.py`: compiles the actual native
  startup/fallback, scene eligibility/advance and boot/host sealed-teardown calls
  with mocks. Covers missing exports, absent guest bind, init/finish failure,
  ineligible scenes, non-arming present advance, core drops/failed seal, sticky
  non-quiescent reason and preservation of nonzero guest exit codes.
- `python tests/temporal_source_crosscheck.py --clang <existing-NDK-clang>`:
  Android object compilation of module, shim, frontend, native entry, boot/host
  teardown and current draw source using
  existing expanded Android assets, and privately instrumented bucket copies in
  temporary storage (private buckets at O0; host/shim at O1). No generated build
  input or existing build object is overwritten. Resulting objects are checked
  as little-endian AArch64 ELF and then deleted with the disposable directory.

These checks execute lifecycle callsites with mocks, not a fully relinked runtime,
device capture or hardware qualification. No device operation is needed for them.
