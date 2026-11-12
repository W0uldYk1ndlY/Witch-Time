# Motion interface (independent adapter contract)

`runtime/upscale_motion.h` defines shader helpers for FSR unit-scale and SGSR
`(-2*x, 2*y)` adapters. Canonical RG16F is **previousUV - currentUV** in
top-left image UV, excluding raster jitter. It contains total camera + object
displacement; a valid vector replaces camera reconstruction, never adds to it.
Separate R8 validity is 0 for missing correspondence and 1 for valid motion,
including exact zero. Never test vector magnitude or stock encoded X as validity.

Camera matrices are unjittered `previousVP * inverse(currentVP)`, uploaded
column-major. Reconstruction uses Y-up clip coordinates, depth 0 near / 1 far,
and homogeneous division. Remove the actual current raster jitter before applying
the matrix. Jitter here is top-left pixel displacement: NDC correction is
`(2*jx/width, -2*jy/height)`. The previous coordinate is unjittered as well.
FSR samples previous UV by adding its vector; SGSR uses currentNDC minus previousNDC.
Reverse-Z and a predecessor with nonpositive/invalid W require separate qualification.

Production SGSR uses the shared unjitter helper without changing its upstream
algorithm, descriptors or UBO. No pipeline interface or translator cache key changes.
There is **no owned raw+validity producer** yet: production remains camera-only,
including FSR; the raw selection helpers are independently GPU-tested contracts,
not evidence of dynamic motion support. Task 5.6 integration and geometry/pose
tasks remain blocked by the 5.2 evidence gate. No geometry family, identity,
previous transform, bone palette or deformed pose is inferred here.

## Verification

`python tests/upscale_motion_test.py <glslc> <spirv-dis>` compiles the actual
production FS adapter, asserts the existing C UBO layouts and runs a disposable
test fragment through the unchanged headless `sgsr_gpu_test.c` harness. It checks
near/middle/far standard depth, nonsymmetric projective matrices with homogeneous
division, changing raster jitter, camera/object/combined displacement, per-pixel
valid zero versus invalid fallback, and identical FSR/SGSR previous sample UVs.
The actual production Convert camera output is separately read back against the
harness's analytic reference. Missing-final-pass, wrong-SGSR-X-sign and zero-vector
sentinel mutations must fail readback. This does not exercise an owned raw texture
producer or claim FSR/SGSR dynamic binding, game geometry, mobile quality or speed.
