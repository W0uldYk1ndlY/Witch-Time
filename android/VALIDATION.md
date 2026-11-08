# Android validation record

Build host: Linux x86-64. Android NDK r28c, clang 19.0.1, ARM64 API 29. Dependencies: SDL 3.4.18, FFmpeg 7.1.3, glslang 15.1.0, Vulkan headers 1.4.304. No phone, emulator or game dump was used.

## Verified on this branch

- Release Rust regression suite: 50 tests passed, zero failed.

- All 49 runtime translation units cross-compile. `libwitchtime.so` and `libwt_glslang.so` link with undefined symbols prohibited. The runtime's game dependency is satisfied only at link time by a private temporary stand-in; the stand-in is not a game build and is not installed into the prebuilt package.
- Synthetic NSOs pass conversion, lifting (510 functions, 24,850 instructions), compilation and strict Android `libgame.so` cross-link. Synthetic output is temporary and removed, not installed into the real build directory.
- Android freestanding check: 60,000 input words, 18,337 lifted instructions, 14 objects compiled both ways, zero differences.
- Existing host freestanding check: 200,000 input words, 61,276 instructions, 42 files, zero differences.
- Existing lifter/Unicorn differential test: 2,000 synthetic words, 695,403 comparisons, zero failures.
- Platform tests: native host self-tests, modeled 4 KiB and 16 KiB page operations, fibers/stack/floating-point state, Android module names. ARM64 platform, fiber register and diagnostic harnesses cross-link; they were not executed on ARM64.
- Scripted surface-loss tests exercise production retry helpers, including temporary missing windows and swapchain surface loss. They do not simulate Android lifecycle or GPU presentation.
- Vulkan-only tests compile the actual frontend with desktop OpenGL headers excluded and verify there are no GL imports. Capability tests reject unsupported feature combinations.
- Android CMake bootstrap configures and builds `libmain.so`; it also compiles with `-Wall -Wextra -Werror` against the NDK.
- Native dependency validation checks AArch64, unversioned SONAMEs, packaged/system dependencies and 16 KiB ELF LOAD alignment.
- Packaging/dependency source tests and shell syntax checks pass.

## Not verified / required next

- Gradle APK assembly, installation, Android Java integration and actual ARM64 execution have not been tested. The packaging script intentionally refuses to build a game APK without a personal `libgame.so`.
- Actual Bayonetta 3 dump compatibility, boot, graphics, movies, audio, controllers, saves and performance require the owner's correct dump.
- Oppo Find N5 / ColorOS 17 GPU feature support, fold/unfold and surface lifecycle require real device testing. Feature guards are not evidence of compatibility.
- Full 4 KiB guest-page protection emulation on a 16 KiB host is absent. Partial-host-page protection/decommit requests fail safely; modeled tests do not replace device tests.
- No on-screen touch controls. Use a controller for first device tests.

See README.md in this directory for reproducible commands and private data paths. No copyrighted game data or generated game code is included in source deliverables.
