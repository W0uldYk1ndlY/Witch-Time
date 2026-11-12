# Committed-save export only (implementation; not device-qualified)

This diagnostic-only entry point copies the app's fixed ordinary
`getExternalFilesDir(null)/userdata/save` into a **fresh**
`diagnostics/<run_id>/committed-save-export/save` tree. Never publish the export,
manifest, filenames, hashes or save contents. The manifest is private evidence.
There is no source-path extra, environment bridge, game launch, journal recovery,
ordinary-save chmod or write. Ordinary launcher behavior is unchanged except that
it refuses concurrent game startup while export is active.

## Explicit authority

`SaveExportActivity` is separate from SDLActivity. It loads only `libsave_export`,
which has no SDL/game/runtime link dependency. The Activity requires the fixed
action `org.witchtime.android.EXPORT_COMMITTED_SAVE`, diagnostic BuildConfig,
`diag_run_id` and lowercase `request_sha256` intent extras. The latter is the
SHA-256 of the exact already-staged `request.json` bytes. Native code opens every
descendant with no-follow descriptor-relative operations and reuses bootstrap
validation on the bytes read from the pinned request descriptor.

The request is deliberately export-only, not a gameplay benchmark request:

```json
{
  "schema_version": "1.0",
  "run_id": "run_20261112_oppo_n5_save_export_UNIQUE",
  "build": {
    "variant": "diagnostic",
    "target": "android-arm64",
    "native_optimization": "release"
  },
  "profile": {"profile_name": "committed_save_export", "collectors": []}
}
```

Only these top-level keys plus optional `created_utc` are accepted for export;
no treatment, source or destination selector is accepted. A valid export request
sent to the ordinary SDL entry is refused before session setup/runtime dlopen.
An invalid request to the export Activity never falls back to game startup.

After review and **separate device authorization**, with the app stopped and no
other save writer: stage the request in a fresh run directory, compute its hash,
then explicitly start `.SaveExportActivity` with the fixed action and both extras.
Do not start `.WitchTimeActivity`, set stay-awake, change source permissions or
request broad storage access. This document does not authorize installation or
device execution. No export UI picker, arbitrary output path or share intent is
provided. Readable files remain inside app-scoped storage; platform denial of
pulling those artifacts must be reported, not bypassed.

## Tree policy, bounds and evidence

The policy is pinned to `runtime/nnfs.c`'s committed-tree contract: `save` is the
committed root; `save.commit`, `save.work.<pid>` and `save.journal.<pid>` are its
siblings. Their presence **rejects** export, including stale leftovers; export
never recovers/deletes them. Nothing inside `save` is omitted based on a guessed
filename. A runtime with a different persistence layout requires policy review.

Only regular single-link files and directories on the same filesystem are
accepted. Symlinks, special files, traversal/ambiguous backslash names, unknown
entry types, unreadable entries, changed identities/content and exceeded bounds
fail closed. Empty directories are preserved. Limits: 1,024 total file/directory
entries, depth 16 below root, relative paths under 1,024 bytes, 32 MiB per file,
128 MiB total file bytes. No truncation or partial-success fallback.

Each copied file uses exclusive creation, source hash, destination readback hash,
source reread and metadata comparisons. A second full source traversal verifies
all entries, identities, metadata and hashes, followed by sibling-marker and
source-root identity checks. The manifest records counts, total bytes, relative
paths, sizes, source identities/timestamps, hashes and request identity.
`path_encoding: filename-bytes-as-u00xx` means non-ASCII filename bytes are escaped
individually; restoring such names requires byte decoding, not UTF-8 re-encoding.

Exports get descriptor-checked `0666` file / `0755` directory modes, only on newly
created destination objects. `INCOMPLETE` remains as a warning even after success:
**require a fully parsed `manifest.json` with `complete=true` and independently
verify its file counts/hashes before use.** No manifest or `complete=false` means
unusable. Failed copies remain in the unique run for inspection; retries require
a new run ID. Existing exports are never overwritten.

## Lifecycle/security limits

Export rejects another same-UID process or already loaded SDL/game/runtime
mappings. A shared Java gate serializes export against the ordinary Activity's
SDL library loading; once game libraries have been requested, export refuses for
the remainder of that process. JNI serializes exports; the normal native entry
also refuses startup during export. This is not a filesystem snapshot or a lock against an unrelated
privileged writer: metadata/double-read detection cannot prove absence of every
possible racing external write. Keep the app stopped before invocation and do
not use other save tools concurrently. Reads may update filesystem access times;
source content, modes and modification times are never intentionally changed.

Package/game/update identity and gameplay checkpoint are **not inferred** from
save filenames. A successful export supplies a private immutable fixture
candidate only; compatibility, route and animated/skinned gameplay still require
separate evidence. Android Activity lifecycle, FUSE mode behavior and transfer
permissions require later device qualification.

## Checks

`python tests/save_export_test.py`: source lifecycle regressions on every host;
native typed-authority fixtures with a host C compiler, and the actual export
Activity's gate/rejection path with mocked Android classes when a JDK is present.
POSIX `cc` hosts additionally execute isolated synthetic fixtures covering full
trees/hashes, original content/mode/mtime preservation, bad authority, links,
special files, pending journals, bounds, partial failure and source mutation.
These tests never access a phone or normal save directory.
The test is wired into the existing Ubuntu CI job; a Windows-only pass does not
qualify the POSIX filesystem cases.
