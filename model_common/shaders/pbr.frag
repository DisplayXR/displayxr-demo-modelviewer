// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Metallic-roughness PBR fragment shader (Cook-Torrance GGX) with the full
// glTF material texture set (base-color, metallic-roughness, normal, occlusion,
// emissive), sRGB-correct sampling, tangent-free normal mapping (Schüler's
// cotangent frame from screen-space derivatives), one directional light,
// image-based lighting (irradiance + prefiltered specular + BRDF LUT, baked
// from the active environment), the tier-1 KHR_materials_* layers, and an
// explicit exposure + named tone curve (tonemap.glsl). See ../../PORTING.md.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "tonemap.glsl"
#define SHEEN_PI 3.14159265359
#include "sheen.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in float inViewZ;
layout(location = 4) in vec4  inTangent;   // world-space TANGENT + handedness (0 = absent)

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 view;
    vec4 cameraPos;
    vec4 lightDir;     // .xyz = light dir, .w = clipFar (view-space; 0=off)
    mat4 invViewProj;  // (skybox only)
    vec4 tone;         // x=exposure (2^EV), y=curve id, z=directional-light scale,
                       // w=transmission probe (issue #75; 0 = normal shading)
    vec4 viewport;     // xy = this eye's viewport as a fraction of the colour target
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
    vec4 p3;   // anisotropyStrength, anisotropyRotation, iridescenceFactor, iridescenceIor
    vec4 p4;   // iridescenceThicknessMin, iridescenceThicknessMax, transmissionFactor, thicknessFactor
    vec4 p5;   // attenuationColor.rgb, attenuationDistance
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
// Texture-driven variants of the KHR_materials_* factors. Each samples the
// channel its extension specifies and MULTIPLIES the factor, per glTF. Absent
// maps are bound to 1×1 white, which is the multiplicative identity — so the
// factor-only path costs one extra tap and behaves identically.
layout(set = 1, binding = 5)  uniform sampler2D clearcoatTex;        // R
layout(set = 1, binding = 6)  uniform sampler2D clearcoatRoughTex;   // G
layout(set = 1, binding = 7)  uniform sampler2D sheenColorTex;       // RGB, sRGB
layout(set = 1, binding = 8)  uniform sampler2D sheenRoughTex;       // A
layout(set = 1, binding = 9)  uniform sampler2D specularTex;         // A
layout(set = 1, binding = 10) uniform sampler2D specularColorTex;    // RGB, sRGB
layout(set = 1, binding = 11) uniform sampler2D transmissionTex;     // R
layout(set = 1, binding = 12) uniform sampler2D thicknessTex;        // G

// Set 2: image-based lighting (generated from the analytic sky).
layout(set = 2, binding = 0) uniform samplerCube irradianceMap;   // diffuse
layout(set = 2, binding = 1) uniform samplerCube prefilteredMap;  // specular (roughness mips)
layout(set = 2, binding = 2) uniform sampler2D   brdfLUT;         // split-sum scale/bias
// Mipped copy of the opaque pass's SCENE-LINEAR colour — what transmissive
// surfaces refract against. Mip level is chosen from roughness, so a rough
// transmissive surface scatters what's behind it (KHR_materials_transmission,
// issue #70 tier 2).
//
// This holds pre-tone-map, pre-encode radiance (issue #75). It is a copy of the
// second colour attachment this shader writes below, NOT of the displayed
// image: sampling the displayed image put an already-tone-mapped (and, on a
// UNORM swapchain, already-sRGB-encoded) value into a linear `color` that then
// ran the whole tail again, which is what made glass render washed out.
layout(set = 2, binding = 3) uniform sampler2D   sceneColor;
// Sheen directional albedo E(N·V, sheenRoughness) — see sheen_lut.frag. Lets
// sheen redistribute energy rather than add it.
layout(set = 2, binding = 4) uniform sampler2D   sheenLUT;

layout(location = 0) out vec4 outColor;
// Scene-linear twin of outColor: the same shaded radiance BEFORE exposure, the
// tone curve and the sRGB encode. Transmission samples a mipped copy of this
// attachment (issue #75) so the transmitted light is composited in the space it
// was captured in and reaches the display transform exactly once.
layout(location = 1) out vec4 outSceneLinear;

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

// Tangent-free normal mapping (Christian Schüler). Builds a cotangent frame
// from screen-space derivatives of position + uv, so no TANGENT attribute is
// needed. N is the (viewer-facing) geometric normal.
// Builds the frame; `valid` is false when there's no usable UV gradient (a mesh
// without TEXCOORD_0, e.g. AnimatedMorphCube) — T/B collapse to 0 there and
// inversesqrt(0) would poison everything downstream with NaNs.
void cotangentFrame(vec3 N, vec2 uv, out vec3 T, out vec3 B, out bool valid) {
    vec3 dp1 = dFdx(inWorldPos), dp2 = dFdy(inWorldPos);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    T = dp2perp * duv1.x + dp1perp * duv2.x;
    B = dp2perp * duv1.y + dp1perp * duv2.y;
    float maxlen = max(dot(T, T), dot(B, B));
    valid = maxlen >= 1e-12;
    if (!valid) { T = vec3(1.0, 0.0, 0.0); B = vec3(0.0, 1.0, 0.0); return; }
    float invmax = inversesqrt(maxlen);
    T *= invmax;
    B *= invmax;
}

vec3 perturbNormal(vec3 N, vec2 uv, vec3 T, vec3 B, bool valid) {
    if (!valid) return N;   // a flat normal map is identity here anyway
    vec3 mapN = texture(normalTex, uv).xyz * 2.0 - 1.0;
    return normalize(mat3(T, B, N) * mapN);
}

// ── KHR_materials_anisotropy ────────────────────────────────────────────────
// Reference formulation from the extension spec (D and V verbatim).
float D_GGX_anisotropic(float ndoth, float tdoth, float bdoth, float at, float ab) {
    float a2 = at * ab;
    vec3 f = vec3(ab * tdoth, at * bdoth, a2 * ndoth);
    float w2 = a2 / dot(f, f);
    return a2 * w2 * w2 / PI;
}
float V_GGX_anisotropic(float ndotl, float ndotv, float bdotv, float tdotv,
                        float tdotl, float bdotl, float at, float ab) {
    float ggxV = ndotl * length(vec3(at * tdotv, ab * bdotv, ndotv));
    float ggxL = ndotv * length(vec3(at * tdotl, ab * bdotl, ndotl));
    return clamp(0.5 / (ggxV + ggxL), 0.0, 1.0);
}

// ── KHR_materials_iridescence ───────────────────────────────────────────────
// Thin-film interference, following the extension's reference implementation.
// The colour does not come from a pigment: it's the wavelength-dependent
// interference between light reflected off the top of a film and off the
// substrate under it, so it swings with both view angle and film thickness.
float sq(float x) { return x * x; }
vec3  sq(vec3 x)  { return x * x; }

float iorToFresnel0(float transmittedIor, float incidentIor) {
    return sq((transmittedIor - incidentIor) / (transmittedIor + incidentIor));
}
vec3 iorToFresnel0(vec3 transmittedIor, float incidentIor) {
    return sq((transmittedIor - vec3(incidentIor)) / (transmittedIor + vec3(incidentIor)));
}
vec3 fresnel0ToIor(vec3 f0) {
    vec3 s = sqrt(clamp(f0, vec3(0.0), vec3(0.9999)));
    return (vec3(1.0) + s) / (vec3(1.0) - s);
}

// Maps an optical path difference to an RGB response by integrating against
// Gaussian fits of the CIE colour matching functions, then converting XYZ→sRGB.
const mat3 XYZ_TO_REC709 = mat3(
     3.2404542, -0.9692660,  0.0556434,
    -1.5371385,  1.8760108, -0.2040259,
    -0.4985314,  0.0415560,  1.0572252);

vec3 evalSensitivity(float opd, vec3 shift) {
    float phase = 2.0 * PI * opd * 1.0e-9;
    vec3 val = vec3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    vec3 pos = vec3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    vec3 var = vec3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    vec3 xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift)
             * exp(-sq(phase) * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09)
           * cos(2.2399e+06 * phase + shift[0]) * exp(-4.5282e+09 * sq(phase));
    xyz /= 1.0685e-7;
    return XYZ_TO_REC709 * xyz;
}

vec3 evalIridescence(float outsideIor, float filmIor, float cosTheta1,
                     float thickness, vec3 baseF0) {
    // Thin film thinner than a wavelength behaves as if absent.
    if (thickness < 1.0) return F_Schlick(cosTheta1, baseF0);

    // Snell into the film. Total internal reflection → pure mirror.
    float iridIor = mix(outsideIor, filmIor, smoothstep(0.0, 0.03, thickness));
    float sinTheta2Sq = sq(outsideIor / iridIor) * (1.0 - sq(cosTheta1));
    float cosTheta2Sq = 1.0 - sinTheta2Sq;
    if (cosTheta2Sq < 0.0) return vec3(1.0);
    float cosTheta2 = sqrt(cosTheta2Sq);

    float r0 = iorToFresnel0(iridIor, outsideIor);
    float r12 = F_Schlick(cosTheta1, vec3(r0)).x;
    float t121 = 1.0 - r12;

    vec3 baseIor = fresnel0ToIor(clamp(baseF0, vec3(0.0), vec3(0.9999)) + vec3(0.0001));
    vec3 r1 = iorToFresnel0(baseIor, iridIor);
    vec3 r23 = F_Schlick(cosTheta2, r1);

    float opd = 2.0 * iridIor * thickness * cosTheta2;

    // Half-wave phase shifts wherever light reflects off a denser medium.
    float phi12 = iridIor < outsideIor ? PI : 0.0;
    float phi21 = PI - phi12;
    vec3 phi23 = vec3(baseIor.x < iridIor ? PI : 0.0,
                      baseIor.y < iridIor ? PI : 0.0,
                      baseIor.z < iridIor ? PI : 0.0);
    vec3 phi = vec3(phi21) + phi23;

    vec3 r123 = clamp(vec3(r12) * r23, vec3(1e-5), vec3(0.9999));
    vec3 sqrtR123 = sqrt(r123);
    vec3 rs = sq(t121) * r23 / (vec3(1.0) - r123);

    vec3 I = vec3(r12) + rs;      // m = 0 term
    vec3 cm = rs - vec3(t121);
    for (int m = 1; m <= 2; ++m) {
        cm *= sqrtR123;
        vec3 sm = 2.0 * evalSensitivity(float(m) * opd, float(m) * phi);
        I += cm * sm;
    }
    return max(I, vec3(0.0));
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
    vec3 frameT, frameB; bool frameValid;
    // An authored TANGENT is continuous across UV seams and well-defined at
    // poles, so prefer it. The screen-space-derivative frame stays as the
    // fallback for assets that ship none. `authoredTangent` also gates
    // anisotropic IBL, which is only trustworthy on a real frame.
    bool authoredTangent = dot(inTangent.xyz, inTangent.xyz) > 1e-8;
    if (authoredTangent) {
        frameT = normalize(inTangent.xyz - Ng * dot(Ng, inTangent.xyz));  // Gram-Schmidt
        frameB = cross(Ng, frameT) * (inTangent.w < 0.0 ? -1.0 : 1.0);
        frameValid = true;
    } else {
        cotangentFrame(Ng, inUV, frameT, frameB, frameValid);
    }
    vec3 N = perturbNormal(Ng, inUV, frameT, frameB, frameValid);

    vec3 L = normalize(ubo.lightDir.xyz);
    vec3 H = normalize(V + L);
    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, H), 0.0);

    MatExt me = matExt.materials[int(pc.emissive.w + 0.5)];
    float ior                = me.p0.x;
    float specularFactor     = me.p0.y;
    float clearcoatFactor    = me.p0.z;
    float clearcoatRoughness = me.p0.w;
    vec3  specularColor      = me.p1.rgb;
    float sheenRoughness     = me.p1.w;
    vec3  sheenColor         = me.p2.rgb;
    float emissiveStrength   = me.p2.w;
    float anisoStrength      = clamp(me.p3.x, 0.0, 1.0);
    float anisoRotation      = me.p3.y;
    float iridescenceFactor  = clamp(me.p3.z, 0.0, 1.0);
    float iridescenceIor     = me.p3.w;
    float iridThickness      = me.p4.y;   // no thickness texture → the maximum
    float transmissionFactor = clamp(me.p4.z, 0.0, 1.0);
    float volumeThickness    = me.p4.w;
    vec3  attenuationColor   = me.p5.rgb;
    float attenuationDist    = me.p5.w;

    // Fold the texture variants into the factors. sheenColor/specularColor are
    // sRGB-encoded per spec; the rest are linear single channels.
    clearcoatFactor    *= texture(clearcoatTex, inUV).r;
    clearcoatRoughness  = clamp(clearcoatRoughness * texture(clearcoatRoughTex, inUV).g, 0.03, 1.0);
    sheenColor         *= srgbToLinear(texture(sheenColorTex, inUV).rgb);
    sheenRoughness      = clamp(sheenRoughness * texture(sheenRoughTex, inUV).a, 0.05, 1.0);
    specularFactor     *= texture(specularTex, inUV).a;
    specularColor      *= srgbToLinear(texture(specularColorTex, inUV).rgb);
    transmissionFactor  = clamp(transmissionFactor * texture(transmissionTex, inUV).r, 0.0, 1.0);
    volumeThickness    *= texture(thicknessTex, inUV).g;


    // ── KHR_materials_ior + KHR_materials_specular ───────────────────────────
    // The dielectric reflectance is no longer hard-coded at 0.04. That constant
    // was only ever the value for ior 1.5; with the extensions present it is
    // derived, then tinted and scaled. Metals keep taking f0 from base colour —
    // both extensions only affect the dielectric lobe, per spec.
    float iorF0 = (ior - 1.0) / (ior + 1.0);
    vec3 dielF0 = min(vec3(iorF0 * iorF0) * specularColor, vec3(1.0)) * specularFactor;
    vec3 f0 = mix(dielF0, albedo, metallic);
    vec3 f90 = vec3(mix(specularFactor, 1.0, metallic));

    // ── KHR_materials_iridescence ────────────────────────────────────────────
    // Replaces the specular Fresnel with the thin-film response, blended by
    // iridescenceFactor. Because it substitutes F rather than adding a lobe, it
    // costs no extra energy — it recolours the reflection that was already
    // there, which is exactly what a soap-film or fuel-slick does.
    float vdoth = max(dot(H, V), 0.0);
    vec3 F = f0 + (f90 - f0) * pow(clamp(1.0 - vdoth, 0.0, 1.0), 5.0);
    vec3 Fr_base = F_SchlickRoughness(ndotv, f0, roughness);
    if (iridescenceFactor > 0.0) {
        vec3 iriDirect = evalIridescence(1.0, iridescenceIor, vdoth, iridThickness, f0);
        vec3 iriView   = evalIridescence(1.0, iridescenceIor, ndotv, iridThickness, f0);
        F       = mix(F,       iriDirect, iridescenceFactor);
        Fr_base = mix(Fr_base, iriView,   iridescenceFactor);
    }

    // Direct directional light. Anisotropy swaps the isotropic GGX lobe for the
    // spec's anisotropic D and V; everything else is untouched.
    vec3 spec;
    if (anisoStrength > 0.0 && frameValid) {
        // Rotate the tangent frame in the tangent plane, then re-derive the
        // bitangent so T/B/N stay orthonormal after the rotation.
        vec2 dir = vec2(cos(anisoRotation), sin(anisoRotation));
        vec3 aT = normalize(frameT * dir.x + frameB * dir.y);
        vec3 aB = normalize(cross(N, aT));
        // Per spec: the tangent direction gets rougher, the bitangent keeps the
        // material roughness — so the highlight stretches ALONG the tangent.
        float at = mix(a, 1.0, anisoStrength * anisoStrength);
        float ab = a;
        float tdoth = dot(aT, H), bdoth = dot(aB, H);
        float tdotv = dot(aT, V), bdotv = dot(aB, V);
        float tdotl = dot(aT, L), bdotl = dot(aB, L);
        float Da = D_GGX_anisotropic(ndoth, tdoth, bdoth, at, ab);
        float Va = V_GGX_anisotropic(ndotl, ndotv, bdotv, tdotv, tdotl, bdotl, at, ab);
        spec = Da * Va * F;
    } else {
        float D = D_GGX(ndoth, a);
        float G = G_SchlickSmith(ndotv, ndotl, a);
        spec = (D * G) * F / max(4.0 * ndotv * ndotl, 1e-4);
    }
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    // Direct light is scaled by ubo.tone.z, which the renderer drops to 0 when
    // an HDRI environment is active: a real capture already contains its own
    // sun, so keeping the analytic key light would double-count the dominant
    // light source and quietly break any authoring-tool comparison.
    vec3 direct = (kd * albedo / PI + spec) * vec3(3.0 * ubo.tone.z) * ndotl;

    // Ambient = image-based lighting (split-sum): irradiance cube for diffuse,
    // prefiltered cube + BRDF LUT for specular.
    vec3 Fr = Fr_base;
    vec3 diffuseIBL = texture(irradianceMap, N).rgb * albedo * (1.0 - metallic);
    float maxLod = float(textureQueryLevels(prefilteredMap) - 1);
    // Anisotropic reflections: bend the reflection vector towards the direction
    // the highlight is stretched in. This is the glTF sample-viewer approach —
    // the extension specifies the direct-light D and V but leaves prefiltered
    // IBL to the implementation, so this is an approximation, not spec text.
    // Anisotropy affects DIRECT light only; the IBL reflection is NOT bent.
    //
    // The standard trick (glTF sample viewer) bends the reflection vector toward
    // the stretch direction so the environment term elongates too. It is
    // implemented correctly here and was then removed on the evidence: even with
    // an AUTHORED tangent frame it produces a hard vertical pinch on a sphere,
    // and the effect is non-monotonic — strength 0.17 and 0.33 look far more
    // distorted than 1.0. The cause is not the tangent frame (that was my first
    // guess, and authoring TANGENT disproved it): bending the reflection swings
    // it across the analytic sky's hard sky/ground horizon, and the two-tone
    // environment turns a smooth stretch into a visible seam.
    //
    // Anisotropic IBL is not spec text — the extension defines the direct-light
    // D and V and leaves prefiltered IBL to the implementation. Given the choice
    // between a visible artifact and an under-stated effect, a material-fidelity
    // demo takes the under-stated one. Worth revisiting under a real HDRI, where
    // there is no hard horizon for the bend to cross.
    vec3 reflDir = reflect(-V, N);
    vec3 prefiltered = textureLod(prefilteredMap, reflDir, roughness * maxLod).rgb;
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
    float sheenMax = max(sheenColor.r, max(sheenColor.g, sheenColor.b));
    if (sheenMax > 0.0) {
        // Energy compensation, per spec: the base layer is scaled by
        // 1 - max3(sheenColor)·E before sheen is added, so a sheened fabric
        // never reflects more light than falls on it. E is the hemispherical
        // integral of the very same D_Charlie/V_Ashikhmin pair evaluated below
        // (shared via sheen.glsl), baked into sheenLUT at startup.
        float E = texture(sheenLUT, vec2(ndotv, sheenRoughness)).r;
        color *= (1.0 - sheenMax * E);

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

    // ── KHR_materials_transmission + KHR_materials_volume ────────────────────
    // Refract through the surface, find where the ray leaves the volume, project
    // that exit point to screen space and read the already-rendered scene there.
    // This is why transmission needed its own milestone: it consumes a copy of
    // the frame, and that copy is per view tile in the atlas.
    //
    // The sample is scene-linear radiance (see the sceneColor declaration), so
    // everything below — Beer-Lambert absorption, the baseColor tint, and the
    // diffuse-lobe replacement — operates on radiance, which is the only space
    // in which any of them is physically meaningful. That is the substantive
    // argument for capturing a linear copy rather than decoding the displayed
    // one: an inverse tone curve would hand this block an approximation of
    // radiance that is unrecoverably clipped wherever the scene exceeded
    // display white.
    //
    // ubo.tone.w is the issue #75 acceptance probe. It forces thickness 0 (so
    // the ray degenerates to the fragment's own screen position) and mip 0, and
    // makes the surface output its raw sample instead of shading it — the
    // display transform at the end of main() is still applied, unchanged and
    // exactly once, so a correct sample makes every transmissive surface
    // reproduce the pixels behind it and visually vanish. Enable with
    // DXR_MODELVIEWER_TRANSMISSION_PROBE=1.
    bool probe = ubo.tone.w > 0.5;
    if (transmissionFactor > 0.0) {
        // Ray through the volume. thickness 0 (a thin surface) degenerates to
        // sampling straight behind the fragment, which is the correct
        // "infinitely thin" behaviour the transmission spec describes.
        // thicknessFactor is expressed in the material's LOCAL space, so the ray
        // has to be scaled by the model matrix before it is walked in world
        // space — Khronos' getVolumeTransmissionRay does the same. Without it a
        // scaled model refracts by the wrong distance, and this viewer auto-fits
        // every scene it loads, so scaled models are the norm rather than the
        // exception. Scale is the length of the model matrix's basis columns.
        vec3 modelScale = vec3(length(pc.model[0].xyz),
                               length(pc.model[1].xyz),
                               length(pc.model[2].xyz));
        vec3 refracted = refract(-V, N, 1.0 / max(ior, 1.0001));
        vec3 exitPos = probe ? inWorldPos
                             : inWorldPos + normalize(refracted) * volumeThickness
                                          * max(max(modelScale.x, modelScale.y), modelScale.z);

        vec4 clip = ubo.viewProj * vec4(exitPos, 1.0);
        vec2 ndc = clip.xy / max(clip.w, 1e-5);
        // Y is inverted because the renderer rasterises through a NEGATIVE-height
        // viewport (+Y-up world without a view-matrix flip), so NDC +Y is image
        // row 0. Then scale into the viewport's sub-rect of the colour target —
        // sampling the full [0,1] would read past what was actually rendered.
        vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * ubo.viewport.xy;
        uv = clamp(uv, vec2(0.001), ubo.viewport.xy - vec2(0.001));

        // Roughness picks the mip: a rough transmissive surface blurs what is
        // behind it. Same idea as the prefiltered IBL chain, applied to scene
        // colour instead of the environment.
        float sceneLod = probe ? 0.0 : float(textureQueryLevels(sceneColor) - 1) * roughness;
        vec3 transmitted = textureLod(sceneColor, uv, sceneLod).rgb;

        if (probe) {
            // Acceptance probe: hand the raw sample to the display transform.
            color = transmitted;
        } else {
            // Volume absorption, Beer-Lambert: T(x) = attenuationColor^(x/distance).
            // An infinite attenuation distance (the glTF default) means no
            // absorption at all, so guard rather than divide by it.
            if (attenuationDist > 0.0 && !isinf(attenuationDist)) {
                vec3 sigma = -log(clamp(attenuationColor, vec3(1e-4), vec3(1.0))) / attenuationDist;
                transmitted *= exp(-sigma * max(volumeThickness, 0.0));
            }

            // baseColor tints what passes through; metals absorb it entirely.
            transmitted *= albedo;

            // Transmission replaces the DIFFUSE lobe — the specular reflection
            // off the surface stays. That is what makes glass read as glass
            // rather than as a hole in the image.
            vec3 diffuseTerm = (kd * albedo / PI) * vec3(3.0 * ubo.tone.z) * ndotl
                             + diffuseIBL * ao;
            vec3 transmissionTerm = transmitted * (1.0 - metallic);
            color += (transmissionTerm - diffuseTerm) * transmissionFactor;
        }
    }

    // ── KHR_materials_emissive_strength ──────────────────────────────────────
    // A plain multiplier on the emissive term, which is exactly what lets an
    // emissive surface exceed 1.0 and actually reach the tone curve's shoulder.
    if (!probe || transmissionFactor <= 0.0) color += emissive * emissiveStrength;

    // Scene-linear radiance, captured for transmission BEFORE the display
    // transform below (issue #75).
    vec3 sceneLinear = color;

    // Exposure + tone curve, THEN the transfer function. Tone mapping runs on
    // both swapchain paths — only the sRGB *encode* is conditional (an sRGB
    // swapchain format gets that from the blit's hardware write). This runs
    // exactly once per pixel per pass, and the transmitted contribution folded
    // into `color` above has not been through it before.
    color = applyToneMapping(color, ubo.tone);
    if (ubo.cameraPos.w > 0.5) color = linearToSrgb(color);
    outColor = vec4(color, baseSample.a * pc.baseColorFactor.a);
    outSceneLinear = vec4(sceneLinear, baseSample.a * pc.baseColorFactor.a);
}
