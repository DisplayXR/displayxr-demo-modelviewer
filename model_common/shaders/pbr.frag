// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Metallic-roughness PBR fragment shader (Cook-Torrance GGX) with the full
// glTF material texture set (base-color, metallic-roughness, normal, occlusion,
// emissive), sRGB-correct sampling, tangent-free normal mapping (Schüler's
// cotangent frame from screen-space derivatives), one directional light, and a
// image-based lighting (irradiance + prefiltered specular + BRDF LUT, baked
// from the active environment), and an explicit exposure + named tone curve
// (tonemap.glsl). See ../../PORTING.md.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "tonemap.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in float inViewZ;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 view;
    vec4 cameraPos;
    vec4 lightDir;     // .xyz = light dir, .w = clipFar (view-space; 0=off)
    mat4 invViewProj;  // (skybox only)
    vec4 tone;         // x=exposure (2^EV), y=curve id, z=directional-light scale
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;  // linear (glTF factor)
    vec4 mrParams;         // x=metallic factor, y=roughness factor
    vec4 emissive;         // rgb emissive factor (linear), w = material index
} pc;

// Per-material KHR_materials_* factors (issue #70 phase 2). An SSBO rather than
// push constants because the push block is already 112 of the guaranteed 128
// bytes. Indexed by pc.emissive.w. See ModelRenderer::MaterialExtGpu.
struct MatExt {
    vec4 p0;   // ior, specularFactor, clearcoatFactor, clearcoatRoughness
    vec4 p1;   // specularColorFactor.rgb, sheenRoughness
    vec4 p2;   // sheenColorFactor.rgb, emissiveStrength
    vec4 p3;   // reserved (anisotropy)
    vec4 p4;   // reserved (iridescence)
};
layout(set = 0, binding = 1, std430) readonly buffer MatExtBuf {
    MatExt materials[];
} matExt;

// Set 1: per-material textures. Absent maps default to white / flat normal.
layout(set = 1, binding = 0) uniform sampler2D baseColorTex;
layout(set = 1, binding = 1) uniform sampler2D mrTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;
layout(set = 1, binding = 4) uniform sampler2D emissiveTex;

// Set 2: image-based lighting (generated from the analytic sky).
layout(set = 2, binding = 0) uniform samplerCube irradianceMap;   // diffuse
layout(set = 2, binding = 1) uniform samplerCube prefilteredMap;  // specular (roughness mips)
layout(set = 2, binding = 2) uniform sampler2D   brdfLUT;         // split-sum scale/bias

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float D_GGX(float ndoth, float a) {
    float a2 = a * a;
    float d = (ndoth * ndoth) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}
float G_SchlickSmith(float ndotv, float ndotl, float a) {
    float k = (a * a) * 0.5;
    float gv = ndotv / (ndotv * (1.0 - k) + k);
    float gl = ndotl / (ndotl * (1.0 - k) + k);
    return gv * gl;
}
vec3 F_Schlick(float cosT, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}
// sRGB EOTF (accurate piecewise). MUST be the exact inverse of linearToSrgb()
// below, or the pipeline is not transfer-function-neutral: a texel that should
// survive a round trip unchanged comes back shifted. The old pow(c, 2.2)
// approximation crushed shadows badly while leaving midtones alone — 0.05
// returned as 0.018 (-64%), 0.10 as 0.073 (-27%), 0.35 as 0.348 (-0.6%) — which
// reads as "the scene is a bit dark" and would have been silently charged to
// the material rather than to the decode (issue #70).
vec3 srgbToLinear(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, vec3(lessThanEqual(c, vec3(0.04045))));
}
// Inverse sRGB EOTF (accurate piecewise), for encoding the final linear color
// into a UNORM swapchain. Gated by ubo.cameraPos.w (1 = encode, 0 = skip).
vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThan(c, vec3(0.0031308))));
}
// Fresnel with a roughness-aware ceiling (for ambient specular).
vec3 F_SchlickRoughness(float cosT, vec3 f0, float rough) {
    return f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

// ── KHR_materials_sheen ─────────────────────────────────────────────────────
// Charlie distribution + Ashikhmin visibility, as specified by the extension.
// Models retroreflective fabric fuzz, which GGX cannot: the lobe peaks at
// grazing angles rather than around the mirror direction.
float D_Charlie(float ndoth, float sheenRough) {
    float alpha = max(sheenRough * sheenRough, 1e-4);
    float invAlpha = 1.0 / alpha;
    float cos2h = ndoth * ndoth;
    float sin2h = max(1.0 - cos2h, 1e-7);
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * PI);
}
float V_Ashikhmin(float ndotl, float ndotv) {
    return clamp(1.0 / (4.0 * (ndotl + ndotv - ndotl * ndotv)), 0.0, 1.0);
}

// Tangent-free normal mapping (Christian Schüler). Builds a cotangent frame
// from screen-space derivatives of position + uv, so no TANGENT attribute is
// needed. N is the (viewer-facing) geometric normal.
vec3 perturbNormal(vec3 N, vec2 uv) {
    vec3 mapN = texture(normalTex, uv).xyz * 2.0 - 1.0;
    vec3 dp1 = dFdx(inWorldPos), dp2 = dFdy(inWorldPos);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    // No usable UV gradient (mesh without TEXCOORD_0, e.g. AnimatedMorphCube) →
    // T/B collapse to 0 and inversesqrt(0) would poison N with NaNs. Keep the
    // geometric normal in that case (a flat normal map is identity here anyway).
    float maxlen = max(dot(T, T), dot(B, B));
    if (maxlen < 1e-12) return N;
    float invmax = inversesqrt(maxlen);
    return normalize(mat3(T * invmax, B * invmax, N) * mapN);
}

void main() {
    // Foreground-only clip (transparent mode): drop geometry behind the
    // display plane. 0 = disabled (opaque path unaffected).
    if (ubo.lightDir.w > 0.0 && inViewZ > ubo.lightDir.w) discard;

    vec4 baseSample = texture(baseColorTex, inUV);
    vec3 albedo = srgbToLinear(baseSample.rgb) * pc.baseColorFactor.rgb;

    vec3 mr = texture(mrTex, inUV).rgb;        // g=roughness, b=metallic (linear)
    float metallic  = clamp(mr.b * pc.mrParams.x, 0.0, 1.0);
    float roughness = clamp(mr.g * pc.mrParams.y, 0.04, 1.0);
    float a = roughness * roughness;
    float ao = texture(occlusionTex, inUV).r;
    vec3 emissive = srgbToLinear(texture(emissiveTex, inUV).rgb) * pc.emissive.rgb;

    vec3 V = normalize(ubo.cameraPos.xyz - inWorldPos);
    vec3 Ng = normalize(inNormal);
    // Two-sided: flip the normal for genuinely back-facing triangles (cull is
    // NONE) using the rasterizer's winding, NOT dot(N,V). The view test wrongly
    // flips large flat *front* faces seen near edge-on, sending their normal to
    // the dark lower hemisphere of the IBL irradiance cube (the dark-torso
    // artifact on low-poly skinned meshes like Fox). gl_FrontFacing stays
    // geometric under the renderer's Y-flipped projection, so flip only true
    // back-faces — visible front faces keep their authored outward normal.
    if (!gl_FrontFacing) Ng = -Ng;
    vec3 N = perturbNormal(Ng, inUV);

    vec3 L = normalize(ubo.lightDir.xyz);
    vec3 H = normalize(V + L);
    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, H), 0.0);

    MatExt me = matExt.materials[int(pc.emissive.w + 0.5)];
    float ior                = me.p0.x;
    float specularFactor     = me.p0.y;
    float clearcoatFactor    = me.p0.z;
    float clearcoatRoughness = clamp(me.p0.w, 0.03, 1.0);
    vec3  specularColor      = me.p1.rgb;
    float sheenRoughness     = clamp(me.p1.w, 0.05, 1.0);
    vec3  sheenColor         = me.p2.rgb;
    float emissiveStrength   = me.p2.w;

    // ── KHR_materials_ior + KHR_materials_specular ───────────────────────────
    // The dielectric reflectance is no longer hard-coded at 0.04. That constant
    // was only ever the value for ior 1.5; with the extensions present it is
    // derived, then tinted and scaled. Metals keep taking f0 from base colour —
    // both extensions only affect the dielectric lobe, per spec.
    float iorF0 = (ior - 1.0) / (ior + 1.0);
    vec3 dielF0 = min(vec3(iorF0 * iorF0) * specularColor, vec3(1.0)) * specularFactor;
    vec3 f0 = mix(dielF0, albedo, metallic);
    vec3 f90 = vec3(mix(specularFactor, 1.0, metallic));

    // Direct directional light.
    float D = D_GGX(ndoth, a);
    float G = G_SchlickSmith(ndotv, ndotl, a);
    float vdoth = max(dot(H, V), 0.0);
    vec3  F = f0 + (f90 - f0) * pow(clamp(1.0 - vdoth, 0.0, 1.0), 5.0);
    vec3 spec = (D * G) * F / max(4.0 * ndotv * ndotl, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    // Direct light is scaled by ubo.tone.z, which the renderer drops to 0 when
    // an HDRI environment is active: a real capture already contains its own
    // sun, so keeping the analytic key light would double-count the dominant
    // light source and quietly break any authoring-tool comparison.
    vec3 direct = (kd * albedo / PI + spec) * vec3(3.0 * ubo.tone.z) * ndotl;

    // Ambient = image-based lighting (split-sum): irradiance cube for diffuse,
    // prefiltered cube + BRDF LUT for specular.
    vec3 Fr = F_SchlickRoughness(ndotv, f0, roughness);
    vec3 diffuseIBL = texture(irradianceMap, N).rgb * albedo * (1.0 - metallic);
    float maxLod = float(textureQueryLevels(prefilteredMap) - 1);
    vec3 prefiltered = textureLod(prefilteredMap, reflect(-V, N), roughness * maxLod).rgb;
    vec2 ab = texture(brdfLUT, vec2(ndotv, roughness)).rg;
    vec3 specularIBL = prefiltered * (Fr * ab.x + ab.y);
    vec3 ambient = (diffuseIBL + specularIBL) * ao;

    vec3 color = direct + ambient;

    // ── KHR_materials_sheen ──────────────────────────────────────────────────
    // Added on top of the base layer, direct + a cheap ambient term. NOTE: the
    // spec's energy compensation (scaling the base by 1 - max(sheenColor)*E,
    // where E is the sheen directional albedo) needs a lookup table we don't
    // generate, so it is omitted — sheen here adds energy rather than
    // redistributing it. Documented as an approximation in the README; it shows
    // up as a fabric that is slightly too bright at grazing angles.
    if (max(sheenColor.r, max(sheenColor.g, sheenColor.b)) > 0.0) {
        float sheenD = D_Charlie(ndoth, sheenRoughness);
        float sheenV = V_Ashikhmin(ndotl, ndotv);
        color += sheenColor * sheenD * sheenV * 3.0 * ubo.tone.z * ndotl;
        // Ambient sheen: the irradiance cube stands in for the full integral.
        color += sheenColor * texture(irradianceMap, N).rgb
               * pow(1.0 - ndotv, 3.0) * ao;
    }

    // ── KHR_materials_clearcoat ──────────────────────────────────────────────
    // A second, always-dielectric GGX lobe (ior 1.5 → f0 0.04) layered over
    // everything above. The base is attenuated by the coat's Fresnel so the
    // layering conserves energy: what the coat reflects, the base doesn't get.
    // The coat uses the same normal as the base — KHR_materials_clearcoat also
    // allows a separate clearcoatNormalTexture, which we don't read.
    if (clearcoatFactor > 0.0) {
        float ca = clearcoatRoughness * clearcoatRoughness;
        float ccF = (0.04 + 0.96 * pow(clamp(1.0 - ndotv, 0.0, 1.0), 5.0)) * clearcoatFactor;

        float ccD = D_GGX(ndoth, ca);
        float ccG = G_SchlickSmith(ndotv, ndotl, ca);
        float ccFd = (0.04 + 0.96 * pow(clamp(1.0 - vdoth, 0.0, 1.0), 5.0));
        float ccSpec = (ccD * ccG) * ccFd / max(4.0 * ndotv * ndotl, 1e-4);

        vec2 ccAb = texture(brdfLUT, vec2(ndotv, clearcoatRoughness)).rg;
        vec3 ccIbl = textureLod(prefilteredMap, reflect(-V, N), clearcoatRoughness * maxLod).rgb
                   * (0.04 * ccAb.x + ccAb.y);

        color = color * (1.0 - ccF)
              + (vec3(ccSpec) * 3.0 * ubo.tone.z * ndotl + ccIbl * ao) * clearcoatFactor;
    }

    // ── KHR_materials_emissive_strength ──────────────────────────────────────
    // A plain multiplier on the emissive term, which is exactly what lets an
    // emissive surface exceed 1.0 and actually reach the tone curve's shoulder.
    color += emissive * emissiveStrength;

    // Exposure + tone curve, THEN the transfer function. Tone mapping runs on
    // both swapchain paths — only the sRGB *encode* is conditional (an sRGB
    // swapchain format gets that from the blit's hardware write).
    color = applyToneMapping(color, ubo.tone);
    if (ubo.cameraPos.w > 0.5) color = linearToSrgb(color);
    outColor = vec4(color, baseSample.a * pc.baseColorFactor.a);
}
