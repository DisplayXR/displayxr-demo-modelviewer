# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# The single list of GLSL sources and their #include dependencies.
#
# Two separate CMake projects compile these: model_common/CMakeLists.txt (the
# desktop targets) and android/src/main/cpp/CMakeLists.txt (gradle's
# externalNativeBuild, which does NOT include the former). They used to keep
# private copies of the list, which meant adding a shader built everywhere
# except Android — and the failure surfaced as a missing generated header
# (`fatal error: 'sheen_lut.frag.h' file not found`) rather than as anything
# resembling "you forgot to update a list". One list, included by both.
#
# Names are bare filenames; each consumer prepends its own shaders/ path.

set(MV_SHADER_NAMES
    pbr.vert
    pbr.frag
    skybox.frag
    fullscreen.vert
    brdf_lut.frag
    irradiance.frag
    prefilter.frag
    sheen_lut.frag
)

# Headers pulled in via #include. Listed so a change to one retriggers the
# compile of every shader that includes it — glslangValidator resolves them via
# -I regardless, so omitting one here causes stale SPIR-V, not a build error.
set(MV_SHADER_INCLUDE_NAMES
    sky.glsl
    env.glsl
    ibl_common.glsl
    tonemap.glsl
    sheen.glsl
    # Also #included by model_renderer.h (it is only #defines). Both sides of the
    # material SSBO's layout come from it, so a stale SPIR-V here would be a
    # stride mismatch — see #81 — not merely an out-of-date constant.
    material_slots.glsl
)
