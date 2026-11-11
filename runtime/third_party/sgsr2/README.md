# Snapdragon GSR 2 shader provenance

The fragment two-pass shaders in `glsl_2_pass_fs/` are from Qualcomm's
[snapdragon-gsr](https://github.com/SnapdragonGameStudios/snapdragon-gsr)
commit `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e`, directory
`sgsr/v2/include/glsl_2_pass_fs`. They are licensed under BSD-3-Clause; see
`LICENSE`. The source files and license have a final newline added relative to
that revision; their other bytes match upstream. They are not yet integrated
or qualified for HDR or device rendering.

Upstream Git blob IDs: vertex `93382cf655e4a7a0f9a574ac988e0540195a1bf1`,
convert `d104c97abb2fc69bf962e384c3910996800dcdad`, upscale
`e0b492a955d6dba9f26f59505918cb0e1bdc75c7`, license
`d9d513c75858739e0446ded1e48ff6c4d932518f`.

The 2-pass compute Convert and 3-pass compute Convert/Activate sources are
also pinned to this commit, with only a final newline added. Their upstream
blob IDs are respectively `38ba12370273c7fdc1a56bedc0e14f1ebe1e6d76`,
`0e48f203d91218c7b1e2726113acebde21235757` and
`8c33d69e7a5a14d64c1ba502ea05319b2b470255`. The 2-pass and 3-pass compute
Upscale sources have upstream blob IDs `3994222c97a47bb3689938ff91f5ee5b5b8af539`
and `6a4b0c565815ac110b60ce03ef080379332f6baf` respectively, also with only a
final newline added. None of the compute stages is integrated or qualified.
