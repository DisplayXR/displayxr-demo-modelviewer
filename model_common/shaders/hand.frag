#version 450
// Translucent hand: a soft diffuse tint with a fresnel rim so the silhouette
// reads at alpha ~0.35 over any model. Straight (unpremultiplied) alpha; the
// pipeline blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA on attachment 0 and masks
// attachment 1 (the scene-linear transmission copy) off entirely.
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

    vec3 N = normalize(vNormal);
    vec3 V = normalize(vToEye);
    float ndv = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = pow(1.0 - ndv, 3.0);

    // One fixed key light from above-front plus a hemisphere-ish fill.
    vec3 L = normalize(vec3(0.35, 0.8, 0.5));
    float diffuse = 0.45 + 0.55 * max(dot(N, L), 0.0);
    vec3 rgb = pc.color.rgb * diffuse + mix(pc.color.rgb, vec3(1.0), 0.5) * fresnel * 0.45;

    // Base alpha from the push block (~0.35 * fade); the rim is more opaque.
    float a = clamp(pc.color.a * (1.0 + 1.4 * fresnel), 0.0, 0.9);

    if (pc.eye.w > 0.5) rgb = encodeSrgb(clamp(rgb, 0.0, 1.0));
    outColor = vec4(rgb, a);
    outLinear = vec4(0.0);
}
