// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Exposure + a NAMED tone curve, shared by the model pass (pbr.frag) and the
// background pass (skybox.frag) so the two always agree.
//
// Why this exists (issue #70, phase 0): the viewer previously did no tone
// mapping at all — linear radiance went straight into an sRGB encode and
// clipped. That makes "this material matches the authoring tool" untestable,
// because every highlight difference is attributable to the missing curve
// rather than to the material translation. Comparisons need the curve, the
// exposure and the environment all pinned and written down.
//
// Curves (tone.y selects; see ModelRenderer::ToneCurve):
//   0 CLAMP    no curve, just clamp. The old behaviour — kept so captures can
//              be reproduced against pre-#70 reference images.
//   1 NEUTRAL  Khronos PBR Neutral. The default. Purpose-built for glTF
//              material fidelity: hue and saturation are preserved up to the
//              compression knee, so an authored albedo reads as itself instead
//              of drifting the way a filmic curve makes it drift. This is the
//              curve to use for authoring-tool comparisons.
//   2 ACES     ACES RRT/ODT fit (Stephen Hill). Filmic, punchier, and what many
//              DCC viewports show by default — offered so a comparison can be
//              made against a tool whose viewport is ACES rather than neutral.

// ── Khronos PBR Neutral ──────────────────────────────────────────────────────
// Reference implementation from the Khronos glTF sample viewer.
vec3 tonemapPbrNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

// ── ACES (Stephen Hill's RRT+ODT fit) ────────────────────────────────────────
vec3 rrtAndOdtFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
vec3 tonemapAces(vec3 color) {
    const mat3 inMat = mat3(
        vec3(0.59719, 0.07600, 0.02840),
        vec3(0.35458, 0.90834, 0.13383),
        vec3(0.04823, 0.01566, 0.83777));
    const mat3 outMat = mat3(
        vec3( 1.60475, -0.10208, -0.00327),
        vec3(-0.53108,  1.10813, -0.07276),
        vec3(-0.07367, -0.00605,  1.07602));
    return clamp(outMat * rrtAndOdtFit(inMat * color), 0.0, 1.0);
}

// Apply exposure (a linear multiplier, = 2^EV) then the selected curve.
// `params` is the renderer's UBO `tone` vector: .x = exposure, .y = curve id.
vec3 applyToneMapping(vec3 linearColor, vec4 params) {
    vec3 c = linearColor * params.x;
    int curve = int(params.y + 0.5);
    if (curve == 1) return tonemapPbrNeutral(c);
    if (curve == 2) return tonemapAces(c);
    return clamp(c, 0.0, 1.0);
}
