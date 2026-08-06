// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// The environment radiance the IBL generation passes integrate (irradiance.frag,
// prefilter.frag). Two sources behind one function:
//
//   * a loaded equirectangular HDRI (issue #70 phase 0) — required for any
//     "matches the authoring tool" claim, since the comparison is only
//     meaningful when both renderers are lit by the SAME environment;
//   * the procedural analytic sky (sky.glsl) — the fallback when no HDRI is
//     set, so the viewer still works with zero assets.
//
// Included ONLY by the generation passes. The main pass never samples this: it
// reads the cubes these passes bake, and the skybox reads the prefiltered cube
// at a high mip (deliberately blurred — see skybox.frag).
//
// The HDRI is sampled from a sampler2D at set 0 / binding 0. When no HDRI is
// loaded the renderer still binds a 1x1 dummy there (descriptors must be
// valid), and pc.envIsHdri selects the analytic sky instead.

#include "sky.glsl"

layout(set = 0, binding = 0) uniform sampler2D equirectMap;

// Fireflies: a real HDRI puts the sun at 10^4-ish, and importance-sampling that
// with a few thousand taps leaves bright speckle in the prefiltered mips. Clamp
// the integrand — standard practice, and it costs only the very top of the sun's
// specular highlight, which the blurred background hides anyway. Applied ONLY
// here (the generation integrand), never to displayed pixels.
const float ENV_RADIANCE_CLAMP = 100.0;

// Direction → equirectangular UV. Y-up, matching glTF/USD; u wraps at -Z.
// Literal reciprocals rather than PI so this header doesn't depend on being
// included after ibl_common.glsl.
vec2 equirectUv(vec3 d) {
    return vec2(atan(d.z, d.x) * 0.15915494309 + 0.5,
                acos(clamp(d.y, -1.0, 1.0)) * 0.31830988618);
}

vec3 envRadiance(vec3 dir, float isHdri) {
    vec3 r = (isHdri > 0.5) ? texture(equirectMap, equirectUv(normalize(dir))).rgb
                            : skyRadiance(dir);
    return min(r, vec3(ENV_RADIANCE_CLAMP));
}
