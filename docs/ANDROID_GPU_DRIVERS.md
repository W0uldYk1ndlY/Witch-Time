# Android app-local Vulkan drivers

## Scope and safety

The in-game PC Settings menu offers System default and imported driver packages.
No driver binary is bundled, generated or downloaded by the build. Ordinary startup skips the chooser.
Changes apply through a fresh-process restart, never to the live Vulkan loader.
This changes only the application's Vulkan loading path, not the system driver.

Imported drivers execute native code with Witch Time's privileges, including
access to its saves. Only import packages from a trusted source. ZIP/ELF checks
reject malformed input; they do not establish that executable code is safe.
Compatibility and performance remain specific to the phone, OS and driver.
Turnip is not a universal solution for Mali or Samsung Xclipse devices.

Existing resolution, 16× anisotropy, LOD −1, upscaling and pacing settings are
unchanged. Driver selection is not a promise of higher FPS or lower power use.
Texture-quality reductions and application-side driver downloads are outside
this change. Only an already-installed, verified legacy profile applies its reference
`TU_DEBUG=sysmem,nobin` preset; System, imported drivers and fallback do not.

## Selecting a driver

1. Open **Graphics driver** in the in-game PC Settings menu. Choose System,
   or a trusted imported ZIP. Import alone does not
   change the selection.
2. Confirm the restart warning. Unsaved gameplay progress is not automatically
   saved. The chooser returns to the resumed game Activity before draining work.
3. Failed custom startups remain quarantined and fall back to System; retry
   requires explicit consent. An orphaned sealed restart request has a pre-native
   recovery screen with an explicit **Cancel pending driver change** action.
   Ordinary startup never silently consumes or deletes such a request.

## Restart safety boundary

The active selection stays process-frozen. A separate session-bound nonce stages
the next selection. Reentrant renderer admission closes first; admitted work and
a nondestructive GPU-idle barrier complete while filesystem access remains open.
Then the outer NNFS mutation fence closes and drains admitted operations. No
incomplete transaction is forcibly committed.

A non-exported `:restart` Activity observes the old process through Binder. Its
one-shot foreground grant authorizes durable sealing, not successful completion.
Native exit requires both live fences and successful sealing. Home before the
grant revokes it; Home afterward postpones relaunch until the helper resumes.
Only observed parent death permits dispatch. Launcher consumes the matching
sealed nonce once before native driver preparation.

Cancellation is asynchronous: durable removal of the request precedes reopening
FS admission, accounting for the GPU barrier, and releasing the menu hold. Failed
durable rollback or a permanently hung GPU remains fail-closed; no timeout kills
the game. The helper retires only its own verified, finished process after its
protocol resolves. No SDL destruction, alarm, delayed kill, or live loader swap
serves as a restart acknowledgement.

## Integration contract

1. A plain Java launcher dispatches ordinary startup and recovery without loading SDL or the game.
   Explicit `WitchTimeActivity` diagnostic launches and `SaveExportActivity`
   remain separate entry points.
2. Ordinary bootstrap validates the game files, then asks
   `WitchTimeActivity.prepareGpuDriver()` for a frozen selection. System and
   diagnostic launches must not load the custom-driver bridge or its dependency
   constructors. Diagnostics must not change ordinary driver preferences or
   startup markers.
3. Java returns either no custom selection or five strings: native-library
   directory, private package directory, entry-library basename and the owned
   startup-marker path, plus the verified bundled profile ID (empty otherwise).
   The marker is made durable **before** loading native
   custom-driver code.
4. Bootstrap lazily loads `libwt_gpu_driver.so`, which opens the selected loader
   through libadrenotools. Its exported `vkGetInstanceProcAddr` forwards to that
   exact loader. `SDL_HINT_VULKAN_LIBRARY` points SDL to this proxy before window
   creation; `nvn_vk_android_set_driver()` gives the runtime the same underlying
   loader handle. The runtime's existing symbol table remains in use.
5. An interrupted custom startup remains quarantined until explicit retry or
   selection by the user. Only an actual successful/suboptimal
   `vkQueuePresentKHR` clears the current attempt's marker. Swapchain creation
   and deferred presentation's synthetic `VK_SUCCESS` are insufficient.

The custom-only bridge must never become an unconditional dependency of
`libmain.so` or a member of SDLActivity's startup library list. The pinned
linker-bypass implementation has a constructor that inspects and modifies
linker code permissions; even loading it can fail on an unsupported OS.

A non-null adrenotools loader does **not** prove that the requested ICD became
active: its hook can fall back to the stock driver. Logs distinguish the
requested path from Vulkan's observed device/driver properties. Those
properties alone may also be insufficient to identify a custom build uniquely.

## Package boundary

Packages are imported through Android's document picker into private internal
storage. Install into a new directory and publish only after validation; never
overwrite a package that a process may have loaded. Preserve binary bytes.
Reject unsafe paths, duplicate destinations, invalid metadata, incompatible
ELF libraries and archives exceeding bounded resource limits. Failure must
leave the previous selected package intact.

The importer accepts a flat ZIP containing `meta.json` with string
`name`, `version` (or `packageVersion` when `version` is absent) and `libraryName` fields; optional `minApi` must be compatible
with the running Android version. The entry library must be little-endian
AArch64 ELF64 `ET_DYN`. Nested/directory entries, links, encrypted/split archives
and ZIP64 are not supported. Limits are 64 MiB archive input, 128 files,
128 MiB per extracted file, 256 MiB total extracted data and 16 KiB metadata.
Raw `.so` import and package deletion are not part of this first implementation.

The existing pipeline-cache partition by vendor/device/driver version and
Vulkan's cache-validation fallback remain unchanged. Do not delete unrelated
shader caches or user configuration when selecting a driver.

## Sources and attribution

Design reference, inspected at commit
[`f65c40ba2788ebd7318361670bcbb82cfdef6ae8`](https://github.com/SansNope/UnleashedRecomp-Android/tree/f65c40ba2788ebd7318361670bcbb82cfdef6ae8):

- `UnleashedRecomp/os/android/vulkan_driver_android.cpp`: app-local loading,
  startup recovery and imported-package selection.
- `android-apk/app/src/main/java/org/libsdl/app/LauncherActivity.java`:
  pre-launch driver controls and document picker.
- `docs/GRAPHICS_DRIVERS.md`: driver/SoC and GMEM/Sysmem limitations. These are
  upstream observations, not Witch Time hardware acceptance.

Do not copy that project's unrestricted archive extraction or unrelated presets.
Dependency pins, source acquisition and BSD-2-Clause notices are maintained in
`android/third_party/gpu_driver/`. SDL is consumed unchanged.

### Legacy Adreno 830 upgrade compatibility

The former built-in profile is no longer packaged, provisioned or offered in the
chooser. Driver releases are user-managed through ZIP import. Stale generated
Turnip assets are not a Gradle asset source.

An existing selected private `turnip-adreno830-unleashed-mesa26.3-gralloc` package
continues to launch silently only after the original metadata, API 35, size and
SHA-256 checks pass. Invalid or quarantined packages fall back to System. Opening
the chooser does not change that selection; confirming System or an imported
driver applies the usual next-process restart. No existing package, startup
marker or pending restart request is automatically deleted by this removal.
The historical verification records below describe earlier APKs with different
packaging, not the current build.

## Verification boundary

### In-game selection and restart offline acceptance — 2026-11-14

Independent source review closed the identified fence, lifecycle, cancellation
and durability findings. A fresh current-source snapshot passed all 14 focused
scripts, including 19 lifecycle scenarios, 10 chooser tests (with executable
ownership harness), and 16 store tests. The 9 POSIX-only save-export cases remain
skipped on Windows. The bootstrap harness needed a recorded test-only header/JNI
fixture overlay; production sources were unchanged by that correction.

Fresh ARM64 runtime linking and isolated offline Gradle `:app:assembleDebug`
both exited 0. Inspection verified 18 JNI libraries, the exact bundled binary,
LOAD alignment at least 16 KiB, dependencies, matching fresh-runtime allocated
sections after stripping, 53 real-game imports, helper `:restart` isolation and
`exported=false`, and the Android Debug v2 signature. Workspace code hashes
matched the snapshot; the documentation update was recorded separately.

Evidence: `C:/Users/HomePC/AppData/Local/Temp/opencode/wt-gpu-final-restart-20261114/final-report.json`.
APK: the same directory's `snapshot/android/app/build/outputs/apk/debug/app-debug.apk`.
APK SHA-256: `00581fbc1b9685a9e90a1170b5d77e63dc03f2ca6ac461fd51d28a38f64494e6`.
This new APK was not installed. Real Binder lifecycle, filesystem durability,
GPU hangs, bundled-driver compatibility, visuals and performance remain
hardware-unverified. Host tests use mocked Android/Binder/GPU boundaries; the
private game library was preserved, not regenerated. No publication occurred.

Subsequently, the user authorized installation. `adb install -r` completed with
exit 0 on verified PKH110; the installed APK hash matches
`00581fbc1b9685a9e90a1170b5d77e63dc03f2ca6ac461fd51d28a38f64494e6`
and its signer matches the previous package. The previous installed APK was
backed up before replacement. Home/absence of app processes were checked before
and after; no game launch or input was performed. Evidence:
`C:/Users/HomePC/AppData/Local/Temp/opencode/wt-gpu-device-20261114-133848-41facc11/`.
The earlier no-install statement describes offline acceptance only. In-game
selection, actual driver activation and restart remain device-unverified;
ordinary user-operated launch is the next check, not uncalibrated UI automation.

The user then confirmed the in-game chooser, selected bundled Turnip, and
confirmed restart to a normal-looking main menu. Read-only runtime collection
subsequently observed `turnip Mesa driver, Mesa 26.3.0-devel (id 18)` on Adreno
830v1, not merely a requested custom path. The bundled selection had a consumed
nonce and no remaining restart/quarantine marker. Exit history records the old
main process exiting status 0, helper departure and later main processes, but
does not bind the exact nonce to a new PID. Present activity was logged; the
explicit real-present return was not captured.

Evidence is in the installation directory's
`manual-restart-connected-135100/read-only-report.json` and `runtime.txt`.
At collection no game process remained: later OEM exit records say
`OTHER KILLS BY SYSTEM` (`o-kill(22)` / `o-stop(40)`), with cause unknown. These
are not established renderer crashes or clean shutdowns. No host stop occurred.
Gameplay stability, FPS, power, physical sound and fold remain unverified.

Focused tests must cover real importer/recovery logic, shared loader dispatch,
system/diagnostic isolation, and all real versus deferred presentation paths.
Android compilation and APK inspection additionally establish packaging,
required hook libraries and ELF alignment; they do not establish OEM linker
compatibility, actual custom-driver activation, visual correctness or speed.

This implementation batch does not authorize installing an APK or running a
phone benchmark. Any later device check follows `AGENT_DIAGNOSTICS.md`, keeps
normal saves/configuration untouched and reports the actually observed driver.

### Initial launcher-only offline acceptance — 2026-11-14

An isolated source snapshot passed these focused Python scripts (exit 0):
`android_gpu_driver_store_test.py`, `android_gpu_driver_native_test.py`,
`android_gpu_driver_runtime_test.py`, `launcher_driver_ui_test.py`,
`android_bootstrap_test.py`, `android_package_test.py`, `android_surface_test.py`,
`save_export_test.py`, `runtime_power_bootstrap_test.py` and
`diag_boot_config_test.py`. The save-export script skipped its 9 POSIX-only cases
on Windows. Store tests execute production Java with host Android/JSON stubs;
runtime/bridge tests use mocks, while launcher tests check source structure.

Fresh ARM64 runtime linking, Gradle `:app:externalNativeBuildDebug` and
`:app:assembleDebug` exited 0. APK inspection verified 18 AArch64 libraries with
LOAD alignment at least 16 KiB, hook and license presence, no bundled driver,
the exported runtime setter, matching allocated runtime sections, and an
Android Debug v2 signature. The copied real game library supplies all 53 checked
game imports; the link-only stand-in was not packaged. The private game was not
regenerated and no gameplay was executed.

Independent review closed the import-monitor ANR: document-provider reads and
extraction run outside the store monitor, and publication rechecks the startup
freeze. A blocked-provider concurrency test covers the race. Android SDK
compilation also caught a nonpublic `OsConstants.O_DIRECTORY` reference that
host stubs had missed; directory sync now uses public APIs.

The local evidence directory is
`C:/Users/HomePC/AppData/Local/Temp/opencode/wt-gpu-final-20261114/`, containing
`final-report.json`, immutable initial and explicit overlay manifests, build
logs and `apk-inspection.json`. APK SHA-256:
`2f1ecdea1964247401ff3468a0aed0e4e5444965f2f793afc94428a46fb2faa5`.
The APK was not installed or published. Device lifecycle, OEM linker support,
actual custom-driver activation, filesystem durability, visuals and performance
remain unverified; physical sound/fold acceptance is unchanged.

### Authorized installation — 2026-11-14

The user subsequently authorized installation on PKH110. The above APK was
installed with `adb install -r` (exit 0, `Success`) after idle/Home checks,
preservation of crash/runtime evidence and backup of the existing APK. The
pulled installed APK matches the candidate hash exactly; its signer matches the
previous package. Android resolves the launcher to `.LauncherActivity`.
Evidence and the conditional rollback recipe are in
`C:/Users/HomePC/AppData/Local/Temp/opencode/wt-gpu-install-20261114-3f08b970/`.
No game launch, input, driver import, power/posture change, uninstall or data
clear was performed. Private app data was not backed up; the saved APK permits
binary rollback, not a guarantee of private-data recovery. UI/gameplay/custom
driver acceptance remains open.

The user subsequently confirmed that the launcher displays **System default**,
**Import driver ZIP** and **Start game**. This accepts basic launcher visibility
only; game startup and actual custom-driver activation remain unverified.

The user then confirmed reaching the game's main menu after **Start game** with
**System default**. This is user-observed system-driver startup acceptance, not
gameplay, imported-driver, recovery-path or performance validation.
