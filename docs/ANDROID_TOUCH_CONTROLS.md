# Android touch controls

Task closed at the user's direction on 2026-11-14. Implementation is retained
with the acceptance limits below; closure is not a claim that the outstanding
device matrix passed. The user reported a visible screen with a build based on
another APK after reporting missing text/input on the tested candidate; the
cause and full gameplay/editor acceptance remain unconfirmed.

Acceptance scope: the user explicitly excluded the Windows/desktop build from
this change's acceptance. It remains skipped, not verified. Real simultaneous
physical touches, fold/posture and physical-controller usability require the
user's hands-on checks; automated phone observations must be listed separately.

## Phone follow-up, 2026-11-13

An isolated device trace confirmed that SET now opens PC Settings after the
Android presentation-transform correction. The observed surface uses
`ROTATE_270`; touch/editor quad vertices are transformed into image coordinates
while UVs remain attached and shared touch geometry stays in SDL pixels.
Presentation invalidation releases contacts; SDL dimensions are taken
under the metadata lock and refreshed when geometry changes. Focused host tests,
ARM64 build and independent scoped source review passed.

The subsequent `menu_upright_c697` run confirmed upright ordinary PC Settings on
the GPU after one SET tap. Ordinary menu layout and hits now share SDL pixels;
final composition handles orientation, with a shader-independent CPU fallback.
Focused host checks, GLSL compilation, ARM64 build and independent source/artifact
review passed. This does not establish physical multitouch, gameplay, editor
operation, persistence, pad coexistence or fold acceptance. Those checks remain
open. The historical probe must not be reused; see
`android/VALIDATION.md` for evidence and execution limitations.

Implementation is available in the tested candidate APK; the device acceptance
matrix below is not complete.

## Settings file

`touch_controls.ini` lives beside the resolved `WT_CONFIG` file, normally
`config/controllers.ini`. Diagnostic runs use their own config directory. It
is separate from game saves and does not modify `controllers.ini`.

```ini
version = 1
enabled = 1
left_stick = 0.13, 0.70, 1, 0.65
a = 0.91, 0.58, 1, 0.65
```

Each control value is `x, y, scale, opacity`. Positions are normalized to the
safe area, from 0 to 1; scale ranges from 0.5 to 2 and opacity from 0.2 to 1.
The minimum hit target is 48 dp per axis. Known IDs are `left_stick`,
`right_stick`, `dpad`, `a`, `b`, `x`, `y`, `l`, `r`, `zl`, `zr`, `plus`,
`minus`, `l3`, and `r3`. Missing controls use the defaults. Unknown keys are
ignored. Duplicate or invalid control values reset the layout to defaults;
an invalid or unsupported version uses all defaults without rewriting the file.
The default is enabled.

Default centers, in ID order above, are `(0.13, 0.70)`, `(0.74, 0.76)`,
`(0.28, 0.68)`, `(0.91, 0.58)`, `(0.86, 0.69)`, `(0.86, 0.47)`,
`(0.81, 0.58)`, `(0.08, 0.25)`, `(0.92, 0.25)`, `(0.18, 0.25)`,
`(0.82, 0.25)`, `(0.60, 0.25)`, `(0.40, 0.25)`, `(0.24, 0.88)`, and
`(0.65, 0.88)`. Default scale is 1 and opacity is 0.65. Bounds are clamped
without rewriting those centers; if the normal arrangement is inaccessible,
a compact in-memory arrangement is used where the safe area permits it.

The editor writes only on an explicit Save or Enabled toggle; Cancel does not
write, and Reset changes the draft layout without changing Enabled or
controller bindings. A failed write leaves the active and saved settings
unchanged and retains the draft.

## Intended operation

Tap **SET** to open PC Settings. Swipe the list to reach **Touch controls**;
tap its row, then **Enabled** or **Edit layout**. Per the updated user requirement,
OFF hides the entire overlay, including SET, and disables its hit regions. The
already-open settings panel remains usable. After closing it, use **F1** on a
keyboard or **Start + Select** on player one's controller to reopen PC Settings
and re-enable touch; no hidden touch recovery gesture is provided. In the editor,
tap and drag a control; **SZ−/SZ+**
adjust size and **OP−/OP+** adjust opacity. **SAVE** applies the draft,
**CANCEL** discards it and **RESET** resets the draft only. **ERROR** means
the draft could not be saved; adjust it or retry, or cancel. The game is held
while the panel is open. These controls are not yet device-validated.

Touch and physical buttons combine; a held touch stick takes precedence over
the corresponding physical stick until released. Touch uses logical Switch
buttons and the existing gameplay rebind and confirm-swap rules. Automatic
prompts follow touch as Switch/Pro; an explicit prompt family is preserved.
Live touch and touch Settings requests are excluded during diagnostic replay
and `WT_PLAY`. Changing the layout does not change bindings.

## Device acceptance matrix (not yet checked)

| Check | Required observation |
| --- | --- |
| Touch-only | Menus, both sticks with attack, all buttons, editor and toggle work without hardware. |
| Persistence | Save, cancel, reset and restart retain expected settings; OFF hides SET and rejects its former hit area; re-enable in the open panel or through the existing keyboard/controller entry. |
| Display | Keep/stretch, internal resolution and each upscaler leave screen geometry and hit testing aligned. |
| Lifecycle | Background/foreground, focus and resize release held touches. |
| Fold and insets | Safe-area and layout remain usable in both folded and unfolded postures. |
| Coexistence | Physical pad buttons and uncaptured axes continue to work. |

Host tests cannot establish that the overlay is visible or usable on a device.

## Offline verification, 2026-11-13

- `python tests/touch_controls_test.py`, `touch_hid_test.py`,
  `touch_prompts_test.py`, `touch_routing_test.py`, `pcmenu_host_test.py`,
  `diag_replay_hid_test.py`, `android_bootstrap_test.py`,
  `android_surface_test.py`, `android_package_test.py`, and
  `touch_overlay_structure_test.py` passed. The packaging checks were updated
  to identify the actual game activity and distinguish the export-only
  `RTLD_NOLOAD` probe from loading the game runtime.
  The Android menu and HID checks execute host builds with mocked devices and
  Android guards; they are not Android-device tests.
- `python tests/touch_overlay_gpu_test.py` passed BGRA/RGBA UNORM/sRGB,
  light/dark and direct/intermediate headless blit round trips on an NVIDIA
  RTX 5070 Ti. This does not exercise Android, WSI, the full overlay pipeline,
  or a Vulkan validation layer; no Khronos validation layer was available.
- `cargo test --offline --manifest-path tools/wtbuild/Cargo.toml` passed
  147 tests. An Android ARM64 runtime compiled 54 units and linked both
  libraries (runtime source identity `dcf69e3d92c790a4`). Debug and unsigned
  diagnostic APKs assembled offline from an isolated copy; allocated sections
  of the runtime, shader library and existing local game library matched the
  APKs. These private APKs were not installed or published. The debug APK was
  signed with a temporary key, not the device's existing key; the diagnostic
  APK is unsigned.
- The Windows desktop runtime build remains **unverified**: the available NDK
  Clang lacks `#embed`, and desktop SDL/Vulkan/FFmpeg headers are not installed.
  The desktop PC-menu host self-test passed separately; this is not a desktop
  runtime build.
- An independent read-only review covered lifecycle interleavings, replay,
  persistence, editor behavior and Vulkan resource/format risks. It identified
  stale-touch/SET races, compact-editor mismatch and RGBA/sRGB rendering risks;
  the fixes were re-reviewed and the identified races and color-format issue
  were accepted in source. The tests above exercise the corrected cases, but
  do not replace the device matrix or Android Vulkan validation.
- The editor's skip of panel uploads was independently rechecked: no permanent
  stuck-menu path was found; a transient cached panel at a transition remains
  possible. Existing `upscale_reset_lifecycle_test.py`,
  `upscale_host_lifecycle_test.py`, `upscale_retirement_test.py`,
  `upscale_scale_test.py` and `temporal_diag_wiring_test.py` also passed with
  their documented mocks; they do not establish Android GPU correctness.
- A further independent review found that overlay readiness loss could leave an
  invisible editor active. The editor now cancels safely, reports an error and
  returns to the touch-navigable CPU panel; the focused host regression passed
  and the correction was re-reviewed. The cancelled draft is not applied.
