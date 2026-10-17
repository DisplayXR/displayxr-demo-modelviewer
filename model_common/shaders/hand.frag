#version 450
// Ghost hand body / outline fragment. Straight (unpremultiplied) alpha; the
// pipeline blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA on attachment 0 and masks
// attachment 1 (the scene-linear transmission copy) off entirely. The depth
// prime pass runs this same shader with every colour channel masked off, so
// gl_FragDepth below is what it primes.
//
// Look (Ultraleap Ghost Hand, LeiaViewer GenericHandShader.shader + "Ghost
// Hand.mat"): unlit dark translucent body whose colour is the fresnel term
// (there: 22x white * fresnel^0.629 * alpha 0.098), i.e. nearly black facing
// the viewer and glowing at the silhouette, plus a hairline inverted-hull
// outline. Here the rim carries the left/right tint so the hands stay
// colour-coded.
//
// Depth: the hand is rasterised with its OWN projection (same frustum, a far
// nearer near plane — hands sit between the glass and the viewer, where the
// model's near plane `ez - vH` would slice them; a hand straddling that plane
// also hit a MoltenVK resolve artefact). To keep the depth test against the
// model exact, the fragment depth is recomputed in the MODEL projection's
// [0,1] mapping from the view-space z (= -1 / gl_FragCoord.w); anything
// nearer than the model's near plane clamps to 0 and simply wins.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vToEye;

layout(push_constant) uniform PushBlock {
    mat4 viewProj;
    vec4 color;
    vec4 eye;
    vec4 depth;
    vec4 params;
} pc;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outLinear;   // write-masked off; declared to match the subpass

vec3 encodeSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    // Model-projection depth for this fragment: z_ndc = (m10 * z_v + m14) / -z_v
    // with z_v = -1 / gl_FragCoord.w  ->  -m10 + m14 * gl_FragCoord.w.
    gl_FragDepth = clamp(-pc.depth.x + pc.depth.y * gl_FragCoord.w, 0.0, 1.0);

    const float fade = pc.color.a;
    vec3 rgb;
    float a;
    if (pc.params.y > 0.5) {
        // Outline: the extruded back faces that peek past the primed silhouette.
        rgb = vec3(0.0);
        a = 0.55 * fade;
    } else {
        vec3 N = normalize(vNormal);
        vec3 V = normalize(vToEye);
        float ndv = clamp(dot(N, V), 0.0, 1.0);
        // The reference's broad fresnel (_FresnelPower 0.629) relies on an
        // HDR rim (22x) plus bloom; without bloom the same curve reads as a
        // grey haze, so the falloff is tightened to keep a dark glass body
        // with a bright tinted silhouette.
        float f = 1.0 - ndv;
        vec3 rim = mix(pc.color.rgb, vec3(1.0), 0.35);   // tinted white rim
        rgb = pc.color.rgb * 0.05 + rim * min(1.0, 1.5 * pow(f, 1.8));
        a = (0.10 + 0.60 * pow(f, 2.2)) * fade;
    }

    rgb = clamp(rgb, 0.0, 1.0);
    if (pc.eye.w > 0.5) rgb = encodeSrgb(rgb);
    outColor = vec4(rgb, clamp(a, 0.0, 1.0));
    outLinear = vec4(0.0);
}
