# Agent Guide: Android Diagnostics & Performance Profiling

### Temporal comparison status

The optional `treatments.temporal` host contract and renderer recorder hook are
documented in `android/DIAGNOSTICS.md`. Native bootstrap now applies only the
validated fixed mode/variant selectors before loading the renderer. Shared
renderer events now report actual stage/consumer decisions; the host runner can
derive a latest-scene temporal manifest observation from complete healthy chunks.
A pre-dlopen
acknowledgement is **not** an effective mode or execution acknowledgement.
Do not use an ad-hoc environment bridge to work around missing observations.
Missing chunks/events or recorder loss leave observations unknown. The adapter
does not fabricate missing build/fixture identities or unrelated-settings hashes;
those missing facts still block comparison and all device acceptance.

For offline fixtures, declare `equal_output` or `equal_render` on both sides.
Both require matching actual output, pacing and unrelated settings; equal-render
also requires matching actual scene dimensions. Require real effective mode,
variant, dimensions, execution stages, support/fallback and build identities.
Missing or unsupported evidence is ineligible, never measured zero or success.

This guide provides operational instructions for AI coding agents running, monitoring, and evaluating diagnostic sessions on Android devices (e.g. Oppo Find N5 / Snapdragon 8 Elite).

---

## 1. Safety Rules & Operational Invariants

Before executing any commands on an attached device, agents **MUST** obey these rules:

1. **Zero Data Loss:**
   - **NEVER** run `adb uninstall org.witchtime.android`.
   - **NEVER** run `pm clear org.witchtime.android` or wipe `/sdcard/Android/data/org.witchtime.android/files/userdata/`.
   - All diagnostic runs use isolated run-owned directories under `files/diagnostics/<run_id>/`.
2. **Unique Run Identifiers:**
   - Each trial **must** have a fresh `run_id` (e.g. `run_YYYYMMDD_<device>_<scenario>_<tag>`).
   - The diagnostic engine strictly rejects duplicate or pre-existing run directories to prevent artifact clobbering.
3. **Screen & Power Management:**
   - Keep screen awake before running:
     ```powershell
     adb shell "svc power stayon true; input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard"
     ```
   - If fold state is altered via `cmd device_state state <N>`, always reset it after testing:
     ```powershell
     adb shell "cmd device_state state reset"
     ```
4. **Honesty on Physical Gates:**
   - Never assume physical sound or tactile hinge feel was verified. Always record:
     - `physical_sound: "not_checked"`
     - `physical_fold_validated: false`

---

## 2. Environment Setup & Build

### 2.1 Toolchain Variables (Windows Host)
Ensure Java 21 and the ARM64 dependency prefix are defined in the environment:
```powershell
$env:JAVA_HOME = "D:\jdk-21.0.6+7"
$env:WTBUILD_ANDROID_DEPS = "D:/Projects/Bayonetta_android_port/Witch-Time/deps/android-arm64"
```

### 2.2 Building the Diagnostic APK
The diagnostic build enables the flight recorder, memory rings, and async-safe crash channel with release-tier compiler optimizations (`debuggable false`):
```powershell
cd Witch-Time
.\gradlew.bat assembleDiagnostic
```
Install non-destructively:
```powershell
adb install -r android\app\build\outputs\apk\diagnostic\app-diagnostic.apk
```

---

## 3. Running a Diagnostic Session

### 3.1 Session Directory and Permissions
Create the run directory on the device and configure permissions:
```powershell
$runId = "run_20261112_oppo_n5_trial_1"
$remoteDir = "/sdcard/Android/data/org.witchtime.android/files/diagnostics/$runId"
adb shell "mkdir -p '$remoteDir'; chmod 777 '$remoteDir'"
```

### 3.2 Authoring `request.json`
Every diagnostic run requires a valid `request.json` adhering to Schema 1.0. Unknown top-level keys are rejected.

Template:
```json
{
  "schema_version": "1.0",
  "run_id": "run_20261112_oppo_n5_trial_1",
  "created_utc": "2026-11-12T00:00:00Z",
  "build": {
    "variant": "diagnostic",
    "target": "android-arm64",
    "native_optimization": "release",
    "git_commit": "5b6d8d9c0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b",
    "build_id": "oppo-n5-diag-20261111-02"
  },
  "scenario": {
    "scenario_id": "combat_benchmark",
    "route_id": "chapter0_verse1_fight",
    "fixture_id": "fix_shibuya_v1",
    "deadlines": {
      "startup_deadline_secs": 45.0,
      "scenario_deadline_secs": 80.0,
      "recovery_deadline_secs": 15.0
    }
  },
  "profile": {
    "profile_name": "lightweight",
    "memory_budget_bytes": 8388608,
    "storage_budget_bytes": 134217728,
    "incident_disk_reserve_bytes": 16777216,
    "max_incidents": 3,
    "collectors": [
      "frame_timing",
      "gpu_timing",
      "live_status"
    ]
  },
  "configuration": {
    "resolution": [1600, 900],
    "fps_target": 60.0,
    "audio_mode": "output",
    "cache_mode": "app_cache_cold",
    "input_policy": {
      "source": "synthetic_fullkey_player1",
      "mapping": "default_type_a",
      "menu_confirm": "native_a"
    }
  },
  "policy": {
    "policy_id": "policy_oppo_n5_combat_v1",
    "policy_hash": "sha256_e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "required_assertions": [
      "visual_no_black_screen",
      "audio_output_active"
    ]
  }
}
```

Push and grant read permissions:
```powershell
adb push request.json "$remoteDir/request.json"
adb shell "chmod 666 '$remoteDir/request.json'"
```

### 3.3 Launching with Diagnostic Intent

For separately budgeted component GPU timing, add the fixed `gpu_components`
collector to the request. It enables whole-submission and per-component markers;
the existing `gpu_timing` collector alone remains whole-span-only. Do not inject
arbitrary environment settings or reuse lightweight recorder A/A calibration:
component markers add separate overhead requiring separate calibration. The
runner publishes event-36 aggregates by frame/pass/submission and converts
pending-only records to null-duration `lost_tail` only after validating complete
sequence coverage and healthy stopped-writer EOF. Missing time is not zero;
summed marked costs are not whole-frame time or utilization.

Launch `WitchTimeActivity` with the `diag_run_id` extra:
```powershell
adb shell "am force-stop org.witchtime.android; logcat -c; am start -n org.witchtime.android/.WitchTimeActivity -a android.intent.action.MAIN -c android.intent.category.LAUNCHER --es diag_run_id $runId"
```

---

## 4. Monitoring & Artifact Collection

### 4.1 Live Telemetry
While running, inspect `status.json` on the device:
```powershell
adb shell "cat $remoteDir/status.json"
```
Key fields to verify:
- `surface_generation`: Must increment to $\ge 1$ after swapchain creation.
- `render_age_ms`: Increments with active rendering.
- `heartbeat_age_ms`: Freshness heartbeat ($\le 1000$ ms).

### 4.2 Detecting Completion
Wait until `outcome.json` appears and the process terminates cleanly:
```powershell
adb shell "cat $remoteDir/outcome.json"
```
Expected success output:
```json
{
  "execution_outcome": "completed",
  "reason_code": "CLEAN_COMPLETION",
  "exit_status": {
    "clean_exit": true,
    "exit_code": 0
  },
  "last_confirmed_checkpoint": "cp_combat_end"
}
```

### 4.3 Pulling Artifacts to Host
Pull the generated session artifacts to the host:
```powershell
$hostDir = "artifacts/runs/$runId"
mkdir -Force $hostDir
adb pull "$remoteDir/outcome.json" "$hostDir/outcome.json"
adb pull "$remoteDir/session.log" "$hostDir/session.log"
adb pull "$remoteDir/status.json" "$hostDir/status.json"
adb pull "$remoteDir/crash_breadcrumb.bin" "$hostDir/crash_breadcrumb.bin"
# Pull event chunks
for ($i = 1; $i -le 200; $i++) {
    $c = "{0:D6}" -f $i
    adb pull "$remoteDir/events-$c.jsonl" "$hostDir/events-$c.jsonl" 2>$null
}
```

---

## 5. Automated Host Runner (`wtbuild`)

Instead of manual adb commands, agents can use the integrated host runner:
```powershell
cargo run --manifest-path tools/wtbuild/Cargo.toml -- perf-bench android --request <path_to_request.json> --out <output_dir>
```
The runner automates:
1. Ownership lease check on device.
2. Pushing request and launching intent.
3. Live status polling and heartbeat watchdog.
4. Clean process termination and artifact retrieval.
5. Exit status classification (`Completed`, `NoFirstFrame`, `ConfirmedDeath`, `SessionInterrupted`).

---

## 6. Overhead Budget & Comparison Policy

When evaluating changes (A/B testing):
1. **Frame latency budget:** Recorder overhead must be $\le 0.25$ ms/frame.
2. **Memory budget:** Maximum resident memory overhead $\le 16.0$ MiB.
3. **Equivalence threshold:** $\pm 1.5\%$ median frame time difference under policy `policy_oppo_n5_combat_v1`.
4. **Thermal guard:** Stop trial if battery temperature exceeds 42.0°C or thermal headroom drops below 0.02.
