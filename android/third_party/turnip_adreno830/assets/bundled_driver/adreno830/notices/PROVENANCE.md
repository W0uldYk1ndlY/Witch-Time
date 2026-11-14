# Exact reference Turnip binary — private local APK

The sibling bundle-manifest.json pins the binary URL, SHA-256, byte count,
profile ID, Mesa source revision and reference-host revision. The generated
binary is not committed. It is fetched only by the build, never by the app.
An offline build may use a verified cached generated asset, or supply
`-PwtTurnipDriverFile=<local file>` / `WT_TURNIP_DRIVER_FILE`; every input is
size/hash checked. The generated asset is published atomically only after
verification. No driver is loaded or executed by this build task.

Reference driver/README.md identifies Mesa main
6b9cd5ab9cc3bfd0c79d9500a07a5e74351c72d1 (26.3.0-devel), VARIANT=gralloc,
with only patches-unleashed/0010-gralloc-qcom-ubwc-detect.patch. The reference
recipe's builder dependency refs are not all pinned. Exact binary identity is
verified; a reproducible rebuild is NOT established. The reference README is
preserved verbatim and discusses other profiles that are NOT included here.

The supported profile floor is explicitly Android API 35. It is not a claim
about the binary's inferred minimum API; versioned imports include LIBC_R.
The reference profile's TU_DEBUG=sysmem,nobin belongs only to this bundled
profile. This packaging lane does not apply any runtime environment or change
the system/default graphics selection.

Inspected binary: ELF64 little-endian AArch64 ET_DYN, SONAME vulkan.adreno.so,
four LOAD segments with 16KiB alignment. DT_NEEDED: libhardware.so, liblog.so,
libnativewindow.so, libsync.so, libm.so, libz.so, libdl.so, libc.so.
These are inspection facts, not on-device compatibility evidence.

Mesa's license documentation says the core is MIT and individual files may
have other licenses. Available upstream license documentation, MIT text, the
patched source's copyright/license header and the exact patch are preserved
here with source URLs. The reference repository's GPL license does not by
itself relicense this separately built Mesa binary. A complete per-component
binary redistribution inventory is NOT proven. Public redistribution is NOT
approved; this artifact is for the user's private local APK only.

Sources (all retrieved at the pinned revisions):
- https://raw.githubusercontent.com/SansNope/UnleashedRecomp-Android/f65c40ba2788ebd7318361670bcbb82cfdef6ae8/driver/README.md
- https://raw.githubusercontent.com/SansNope/UnleashedRecomp-Android/f65c40ba2788ebd7318361670bcbb82cfdef6ae8/turnip-driver-ci/patches-unleashed/0010-gralloc-qcom-ubwc-detect.patch
- https://gitlab.freedesktop.org/mesa/mesa/-/raw/6b9cd5ab9cc3bfd0c79d9500a07a5e74351c72d1/docs/license.rst
- https://gitlab.freedesktop.org/mesa/mesa/-/raw/6b9cd5ab9cc3bfd0c79d9500a07a5e74351c72d1/licenses/MIT
- https://gitlab.freedesktop.org/mesa/mesa/-/raw/6b9cd5ab9cc3bfd0c79d9500a07a5e74351c72d1/src/util/u_gralloc/u_gralloc_fallback.c
