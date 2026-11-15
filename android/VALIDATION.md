# Android validation record


## Ten runtime/build improvements — 2026-11-15

Branch `improve/android-top10-20261115`, based on
`579ab5d307ed2bf5f6dd500d0a125521eae1591b`, repairs the temporal v2 bind/emit
compile mismatch recorded below, isolates stale callback epochs, fixes integer
multiply overflow, adds tested Android integer SIMD, improves Vulkan allocation
and cache handling, and applies a provenance-checked Android `-O2` runtime policy.
The full report, exact scope and primary sources are in
[`docs/ANDROID_TOP10.md`](../docs/ANDROID_TOP10.md).

The final dump-free NDK r28c runtime build compiled all 54 units and linked both
libraries with zero failures; source identity `8400ecf374ad1c8b`. Rust tests passed
145/145. Android freestanding comparison compiled 14 files from 18,337 lifted
instructions in both header environments with zero differences. Allocator
production-unit scenarios passed 15/15 and cache failure-injection tests 25/25.
Actual NDK-built AArch64 helpers passed 147,610 emulator cases across native and
scalar backends; host arithmetic and temporal concurrency checks also passed.
These are host/NDK results. No device was connected, no game dump or playable APK
was produced, and no gameplay/FPS, physical-sound or fold acceptance is claimed.
Historical device records below retain their original scope.
