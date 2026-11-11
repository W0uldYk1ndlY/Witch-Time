# AI Agent Guidelines & Repository Knowledge

This document guides AI coding agents working on the Witch Time project.

---

## 1. Android Diagnostics Workflow

When investigating performance regressions, running benchmarks, analyzing crashes, or executing real-device verification on Android (e.g. Oppo Find N5 / Snapdragon 8 Elite):

1. **Required Reading Before Execution:**
   - **Operational Guide for Agents:** Read [`docs/AGENT_DIAGNOSTICS.md`](docs/AGENT_DIAGNOSTICS.md) **BEFORE** constructing `adb` commands or launching test runs. It provides the exact step-by-step procedures, request JSON templates, permission setups, and artifact collection scripts.
   - **Artifact Schema & Contract:** Consult [`android/DIAGNOSTICS.md`](android/DIAGNOSTICS.md) when modifying or validating diagnostic schemas, ring buffer formats, or `wtbuild` parsers.
   - **Validation Records & Open Gates:** Consult [`android/VALIDATION.md`](android/VALIDATION.md) for hardware acceptance history, baseline measurements, and strictly preserved open gates (`physical_sound: not_checked`, `physical_fold_validated: false`).

2. **Core Operational Invariants (Non-Negotiable):**
   - **NEVER** run `adb uninstall org.witchtime.android` or `pm clear org.witchtime.android`. Normal user saves in `/sdcard/Android/data/org.witchtime.android/files/userdata/save` must remain 100% untouched.
   - **ALWAYS** use fresh, unique `run_id` names for each run (format: `run_YYYYMMDD_<device>_<scenario>_<tag>`).
   - **ALWAYS** keep the device screen on (`svc power stayon true`) and reset posture overrides after testing (`cmd device_state state reset`).
   - **ALWAYS** isolate runs in `/sdcard/Android/data/org.witchtime.android/files/diagnostics/<run_id>/`.
   - **NEVER** commit session run artifacts (`artifacts/runs/`) or private game dumps to Git.

---

## 2. Architecture & Tooling Map

- **Runtime C Core:** Located in `runtime/`. Entry point `runtime/boot.c`. Diagnostic flight recorder in `runtime/diag.c`, `runtime/diag_writer.c`, and `runtime/diag_replay.c`.
- **Android Bootstrap:** Located in `android/app/src/main/cpp/`. Initializes SDL3, reads diagnostic intent extras, sets sandboxed environments in `diag_bootstrap.c`.
- **Host Tooling & Analysis (`wtbuild`):** Located in `tools/wtbuild/`.
  - Android runner: `tools/wtbuild/src/diag/android_runner.rs`.
  - Benchmarks: `tools/wtbuild/src/perf_bench.rs`.
  - Run tests with: `cargo test --manifest-path tools/wtbuild/Cargo.toml`.
- **Python Regression Harnesses:** Located in `tests/` (`android_bootstrap_test.py`, `android_package_test.py`, etc.). Run with host Python 3.

---

## 3. Communication Style for Pair Programming

- The user has ADHD. Keep responses action-first, concise, and structured.
- Skip preambles, apologies, and filler text.
- Limit multi-step lists to 5 items or fewer.
- Give concrete units (ms, MB, KB, seconds) for all measurements and time estimates.
