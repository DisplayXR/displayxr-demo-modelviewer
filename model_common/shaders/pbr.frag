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
// MV_TEX_SLOTS / MV_MAT_VEC4S. Shared verbatim with model_renderer.h — the
// material SSBO's stride depends on both, and #81 is what a divergence looks
// like. Do not re-spell either number here.
#include "material_slots.glsl"

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
    vec4 p6;   // scatterStrength, scatterAnisotropy, -, -   (KHR_materials_scatter)
    vec4 p7;   // multiscatterColor.rgb, -
    vec4 p8;   // coatIor, coatDarkening, coatAnisoStrength, coatAnisoRotation
    vec4 p9;   // coatColor.rgb, hasCoat   (KHR_materials_coat)
    // KHR_texture_transform, one entry per texture slot (binding order).
    // MV_TEX_SLOTS, never a literal: this length IS the struct's stride, and
    // C++ derives its own from the same define (#81).
    vec4 uvXf[MV_TEX_SLOTS];   // offset.xy, scale.xy
    vec4 uvRot[MV_TEX_SLOTS];  // .x = rotation (radians)
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
layout(set = 1, binding = 13) uniform sampler2D scatterStrengthTex;  // A
layout(set = 1, binding = 14) uniform sampler2D multiscatterColorTex;// RGB, sRGB
layout(set = 1, binding = 15) uniform sampler2D coatColorTex;        // RGB, sRGB
layout(set = 1, binding = 16) uniform sampler2D coatAnisotropyTex;   // B = strength, RG = rotation
layout(set = 1, binding = 17) uniform sampler2D diffuseRoughTex;     // R
layout(set = 1, binding = 18) uniform sampler2D fuzzTex;             // R

// KHR_texture_transform. Slot indices match the set-1 binding order above, which
// ModelTexSlot / MaterialTexSlot also mirror (there is a static_assert on that).
const int XF_BASE_COLOR = 0, XF_MR = 1, XF_NORMAL = 2, XF_OCCLUSION = 3, XF_EMISSIVE = 4,
          XF_CLEARCOAT = 5, XF_CLEARCOAT_ROUGH = 6, XF_SHEEN_COLOR = 7, XF_SHEEN_ROUGH = 8,
          XF_SPECULAR = 9, XF_SPECULAR_COLOR = 10, XF_TRANSMISSION = 11, XF_THICKNESS = 12,
          XF_SCATTER_STRENGTH = 13, XF_MULTISCATTER_COLOR = 14,
          XF_COAT_COLOR = 15, XF_COAT_ANISOTROPY = 16,
          XF_DIFFUSE_ROUGHNESS = 17, XF_FUZZ = 18;

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
// KHR_materials_diffuse_roughness (draft) — a microfacet diffuse term to replace
// the Lambertian 1/PI when the diffuse substrate is rough. V-shaped cavities add
// masking, shadowing and interreflection, so the surface brightens where view
// and light align and darkens where they are perpendicular: the back-scattering
// that makes sandstone and unglazed clay read as rough rather than as matte
// plastic.
//
// This is Fujii's energy-preserving qualitative Oren-Nayar, NOT the EON model
// (arXiv 2410.18026) the spec points at as OpenPBR's choice. The spec explicitly
// allows the substitution — "Implementations of the BRDF itself can vary based
// on device performance and resource constraints. There is no single
// micro-facet model that we can use as a ground truth reference" — and this one
// is a handful of ALU against EON's closed form. Returns the multiplier on the
// Lambertian term, so roughness 0 returns exactly 1 and the existing path is
// bit-unchanged.
float diffuseOrenNayar(float ndotl, float ndotv, float ldotv, float sigma) {
    if (sigma <= 0.0) return 1.0;
    float s = ldotv - ndotl * ndotv;
    // The standard rewrite of max(0,cos(phi))·sin(alpha)·tan(beta): when the
    // half-plane term is negative there is no interreflection to add, and t
    // becomes 1 so the B term vanishes rather than going the wrong way.
    float t = (s > 0.0) ? max(max(ndotl, ndotv), 1e-4) : 1.0;
    float A = 1.0 / (1.0 + (0.5 - 2.0 / (3.0 * PI)) * sigma);
    float B = sigma * A;
    return A + B * s / t;
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

// This fragment's material row, indexed in place rather than copied.
//
// `MatExt me = matExt.materials[i]` loads the WHOLE row into a local — all
// MV_MAT_VEC4S + 2*MV_TEX_SLOTS vec4s of it, 608 bytes today — and then
// xfUV(MatExt m, ...) copied that local again, by value, at every one of its
// call sites. Indexing the buffer in place lets the compiler load only the
// lanes a fragment actually reads, and keeps a 600-byte-and-growing struct off
// the stack on MoltenVK.
int gMat;
#define MAT matExt.materials[gMat]

// KHR_texture_transform, per the spec's matrix = translation * rotation * scale.
// Scale first, then rotate, then offset.
vec2 xfUV(int slot, vec2 uv) {
    vec4 os = MAT.uvXf[slot];
    float r = MAT.uvRot[slot].x;
    vec2 p = uv * os.zw;
    if (r != 0.0) {
        float c = cos(r), s = sin(r);
        p = vec2(c * p.x - s * p.y, s * p.x + c * p.y);
    }
    return p + os.xy;
}

void main() {
    // Foreground-only clip (transparent mode): drop geometry behind the
    // display plane. 0 = disabled (opaque path unaffected).
    if (ubo.lightDir.w > 0.0 && inViewZ > ubo.lightDir.w) discard;

    // Set FIRST because KHR_texture_transform lives in the material row and the
    // five core maps below are sampled before any extension factor is touched.
    gMat = int(pc.emissive.w + 0.5);

    vec4 baseSample = texture(baseColorTex, xfUV(XF_BASE_COLOR, inUV));
    vec3 albedo = srgbToLinear(baseSample.rgb) * pc.baseColorFactor.rgb;

    vec3 mr = texture(mrTex, xfUV(XF_MR, inUV)).rgb;        // g=roughness, b=metallic (linear)
    float metallic  = clamp(mr.b * pc.mrParams.x, 0.0, 1.0);
    float roughness = clamp(mr.g * pc.mrParams.y, 0.04, 1.0);
    float a = roughness * roughness;
    float ao = texture(occlusionTex, xfUV(XF_OCCLUSION, inUV)).r;
    vec3 emissive = srgbToLinear(texture(emissiveTex, xfUV(XF_EMISSIVE, inUV)).rgb) * pc.emissive.rgb;

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
    vec3 N = perturbNormal(Ng, xfUV(XF_NORMAL, inUV), frameT, frameB, frameValid);

    vec3 L = normalize(ubo.lightDir.xyz);
    vec3 H = normalize(V + L);
    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, H), 0.0);


    float ior                = MAT.p0.x;
    float specularFactor     = MAT.p0.y;
    float clearcoatFactor    = MAT.p0.z;
    float clearcoatRoughness = MAT.p0.w;
    vec3  specularColor      = MAT.p1.rgb;
    float sheenRoughness     = MAT.p1.w;
    vec3  sheenColor         = MAT.p2.rgb;
    float emissiveStrength   = MAT.p2.w;
    float anisoStrength      = clamp(MAT.p3.x, 0.0, 1.0);
    float anisoRotation      = MAT.p3.y;
    float iridescenceFactor  = clamp(MAT.p3.z, 0.0, 1.0);
    float iridescenceIor     = MAT.p3.w;
    float iridThickness      = MAT.p4.y;   // no thickness texture → the maximum
    float transmissionFactor = clamp(MAT.p4.z, 0.0, 1.0);
    float volumeThickness    = MAT.p4.w;
    vec3  attenuationColor   = MAT.p5.rgb;
    float attenuationDist    = MAT.p5.w;

    // Fold the texture variants into the factors. sheenColor/specularColor are
    // sRGB-encoded per spec; the rest are linear single channels.
    clearcoatFactor    *= texture(clearcoatTex, xfUV(XF_CLEARCOAT, inUV)).r;
    clearcoatRoughness  = clamp(clearcoatRoughness * texture(clearcoatRoughTex, xfUV(XF_CLEARCOAT_ROUGH, inUV)).g, 0.03, 1.0);
    sheenColor         *= srgbToLinear(texture(sheenColorTex, xfUV(XF_SHEEN_COLOR, inUV)).rgb);
    sheenRoughness      = clamp(sheenRoughness * texture(sheenRoughTex, xfUV(XF_SHEEN_ROUGH, inUV)).a, 0.05, 1.0);
    specularFactor     *= texture(specularTex, xfUV(XF_SPECULAR, inUV)).a;
    specularColor      *= srgbToLinear(texture(specularColorTex, xfUV(XF_SPECULAR_COLOR, inUV)).rgb);
    transmissionFactor  = clamp(transmissionFactor * texture(transmissionTex, xfUV(XF_TRANSMISSION, inUV)).r, 0.0, 1.0);
    volumeThickness    *= texture(thicknessTex, xfUV(XF_THICKNESS, inUV)).g;
    // KHR_materials_scatter (draft; issue #79). Strength rides the texture's
    // ALPHA channel, the multi-scatter colour its RGB (sRGB-encoded), per spec.
    float scatterStrength = clamp(MAT.p6.x * texture(scatterStrengthTex, xfUV(XF_SCATTER_STRENGTH, inUV)).a, 0.0, 1.0);
    float scatterG        = clamp(MAT.p6.y, -0.99, 0.99);
    vec3  multiscatterColor = MAT.p7.rgb * srgbToLinear(texture(multiscatterColorTex, xfUV(XF_MULTISCATTER_COLOR, inUV)).rgb);
    // KHR_materials_diffuse_roughness (draft; issue #84). R channel, per spec.
    float diffuseRoughness = clamp(MAT.p6.z * texture(diffuseRoughTex, xfUV(XF_DIFFUSE_ROUGHNESS, inUV)).r, 0.0, 1.0);
    // KHR_materials_fuzz (draft; issue #84). Weight on R; the colour and
    // roughness ride the SHEEN lanes read above, because they are the same
    // quantity from the same channels. hasFuzz picks which of the two
    // extensions those lanes belong to, and where in the stack the lobe goes.
    bool  hasFuzz    = MAT.p7.w > 0.5;
    float fuzzFactor = clamp(MAT.p6.w * texture(fuzzTex, xfUV(XF_FUZZ, inUV)).r, 0.0, 1.0);


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
    // KHR_materials_diffuse_roughness: the Lambertian diffuse becomes a
    // microfacet one. The multiplier is exactly 1 at roughness 0, so a material
    // without the extension takes the identical path it always did.
    float dr = diffuseOrenNayar(ndotl, ndotv, dot(L, V), diffuseRoughness);
    vec3 direct = (kd * albedo / PI * dr + spec) * vec3(3.0 * ubo.tone.z) * ndotl;

    // Ambient = image-based lighting (split-sum): irradiance cube for diffuse,
    // prefiltered cube + BRDF LUT for specular.
    vec3 Fr = Fr_base;
    // Diffuse IBL under diffuse roughness. A view-dependent diffuse lobe does not
    // fit prefiltered irradiance, which is indexed by normal alone; the spec
    // offers three ways out and calls this one — bend the shading normal toward
    // the view by the roughness — "the least correct of these solutions but also
    // the most performant". It flattens the diffuse falloff and lifts the
    // terminator, which is the visible half of the effect. The other two options
    // (per-frame CDF sampling, or baking an average light direction into the
    // prefilter) both mean rebuilding the IBL pipeline for one draft extension.
    //
    // Half the roughness as the bend fraction: at full roughness the diffuse
    // normal is halfway to the view, which is as far as this can be pushed
    // before the sphere loses its shading entirely.
    vec3 Nd = (diffuseRoughness > 0.0)
            ? normalize(mix(N, V, 0.5 * diffuseRoughness)) : N;
    vec3 diffuseIBL = texture(irradianceMap, Nd).rgb * albedo * (1.0 - metallic);
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
    // hasFuzz means these lanes are KHR_materials_fuzz's, not sheen's, and the
    // lobe belongs above the coat rather than here. The fuzz block near the end
    // of main() picks them up. Per spec, fuzz takes precedence over sheen where
    // a material carries both.
    float sheenMax = max(sheenColor.r, max(sheenColor.g, sheenColor.b));
    if (!hasFuzz && sheenMax > 0.0) {
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

    // ── KHR_materials_coat (draft) + KHR_materials_clearcoat ─────────────────
    // A second, dielectric GGX lobe layered over everything above. The base is
    // attenuated by the coat's Fresnel so the layering conserves energy: what
    // the coat reflects, the base doesn't get.
    //
    // ONE lobe serves both extensions. KHR_materials_coat is a superset that the
    // spec maps clearcoat onto 1:1 (coatFactor <- clearcoatFactor and so on), so
    // the loader folds clearcoat into the coat fields and `hasCoat` gates only
    // the four things coat ADDS: a tunable IOR, a coloured tint, darkening, and
    // anisotropy. A clearcoat-only asset arrives here with ior 1.5, white tint,
    // darkening 0 and anisotropy 0, which reduces this block exactly to the
    // clearcoat lobe that shipped before — verified byte-for-byte, not asserted.
    //
    // The coat shades with the BASE normal. Both extensions allow a separate
    // coat/clearcoat normal texture; neither is read. README, known gaps.
    if (clearcoatFactor > 0.0) {
        // f0 from the coat's IOR rather than the literal 0.04 this used to
        // hardcode. For a clearcoat-only asset coatIor is 1.5 and
        // ((1.5-1)/(1.5+1))^2 is exactly 0.04, so this is an identity there.
        //
        // The spec allows coatIor 0 for backwards compatibility. Clamping to 1
        // maps that to f0 = 0 — a coat that reflects nothing head-on and still
        // Fresnels toward 1 at grazing — rather than to ((0-1)/(0+1))^2 = 1, a
        // full mirror, which is certainly not what "no IOR" is asking for.
        float coatIor = max(MAT.p8.x, 1.0);
        float ccF0 = (coatIor - 1.0) / (coatIor + 1.0);
        ccF0 *= ccF0;

        bool  hasCoat      = MAT.p9.w > 0.5;
        float coatDarkAmt  = MAT.p8.y;              // 0 for clearcoat-only assets
        float coatAnisoStr = clamp(MAT.p8.z, 0.0, 1.0);
        float coatAnisoRot = MAT.p8.w;
        vec3  coatColor    = MAT.p9.rgb;
        if (hasCoat) {
            coatColor *= srgbToLinear(texture(coatColorTex, xfUV(XF_COAT_COLOR, inUV)).rgb);
            // Per spec: strength rides B; RG is a direction VECTOR biased into
            // [0,1], whose angle ADDS to the authored rotation. The absent-map
            // default for this slot is (1, 0.5, 1), not white — see
            // createSamplerAndDefaults() for why white would be a 45° rotation.
            vec3 ta = texture(coatAnisotropyTex, xfUV(XF_COAT_ANISOTROPY, inUV)).rgb;
            coatAnisoStr *= ta.b;
            coatAnisoRot += atan(ta.g * 2.0 - 1.0, ta.r * 2.0 - 1.0);
        }

        float ca = clearcoatRoughness * clearcoatRoughness;
        float ccF = (ccF0 + (1.0 - ccF0) * pow(clamp(1.0 - ndotv, 0.0, 1.0), 5.0)) * clearcoatFactor;
        float ccFd = (ccF0 + (1.0 - ccF0) * pow(clamp(1.0 - vdoth, 0.0, 1.0), 5.0));

        // Coat anisotropy, direct light only — the same treatment, and the same
        // D/V pair, the base material's KHR_materials_anisotropy gets above, so
        // the two layers stretch consistently. Needs a tangent frame; without
        // one (no UVs) it falls back to the isotropic lobe rather than guessing
        // a direction. Anisotropic IBL is not attempted here for the reason
        // documented on the base lobe: bending the reflection across the
        // analytic sky's hard horizon reads as a seam, not as a stretch.
        float ccSpec;
        if (coatAnisoStr > 0.0 && frameValid) {
            vec2 cdir = vec2(cos(coatAnisoRot), sin(coatAnisoRot));
            vec3 cT = normalize(frameT * cdir.x + frameB * cdir.y);
            vec3 cB = normalize(cross(N, cT));
            float cat = mix(ca, 1.0, coatAnisoStr * coatAnisoStr);
            float cab = ca;
            float ccD = D_GGX_anisotropic(ndoth, dot(cT, H), dot(cB, H), cat, cab);
            float ccV = V_GGX_anisotropic(ndotl, ndotv, dot(cB, V), dot(cT, V),
                                          dot(cT, L), dot(cB, L), cat, cab);
            ccSpec = ccD * ccV * ccFd;
        } else {
            float ccD = D_GGX(ndoth, ca);
            float ccG = G_SchlickSmith(ndotv, ndotl, ca);
            ccSpec = (ccD * ccG) * ccFd / max(4.0 * ndotv * ndotl, 1e-4);
        }

        vec2 ccAb = texture(brdfLUT, vec2(ndotv, clearcoatRoughness)).rg;
        vec3 ccIbl = textureLod(prefilteredMap, reflect(-V, N), clearcoatRoughness * maxLod).rgb
                   * (ccF0 * ccAb.x + ccAb.y);

        // What the base sees through the coat: tint, then darkening. Both are
        // KHR_materials_coat only; for clearcoat this whole term is vec3(1).
        vec3 baseThroughCoat = vec3(1.0);
        if (hasCoat) {
            // Coloured absorption. The authored colour is the tint at NORMAL
            // incidence and represents light crossing the coat TWICE, so the
            // view-dependent path length is 1/cos(theta_t) after refraction.
            // Total internal reflection (sin2T >= 1) cannot happen entering a
            // denser medium, but the guard costs nothing and keeps the pow()
            // away from a NaN if coatIor is ever driven below 1.
            float cosI  = clamp(ndotv, 1e-4, 1.0);
            float sin2T = (1.0 - cosI * cosI) / max(coatIor * coatIor, 1e-4);
            vec3 tint = vec3(0.0);
            if (sin2T < 1.0) {
                float cosT = sqrt(1.0 - sin2T);
                tint = pow(max(coatColor, vec3(1e-4)), vec3(1.0 / max(cosT, 1e-4)));
            }
            // Darkening: light entering the base scatters back up and is partly
            // reflected DOWN again by the underside of the coat, and each such
            // round trip loses energy. Per spec T = (1-R)^2 is the two-way
            // transmittance, and a rough coat scatters that internal reflection
            // incoherently — hence the (1 - roughness*0.5) factor on T.
            //
            // DEVIATION, deliberate. The spec gives R two forms: for direct
            // light 0.5*(fresnel(N,V) + fresnel(N,L)), and for IBL
            // 0.5*(fresnel(N,V) + F_0 + 0.5*F_90). By this point `color` is the
            // direct and ambient lobes already summed, so there is nothing left
            // to apply two different R to, and we use the direct-light form for
            // both. It over-darkens the ambient contribution where N·L is small
            // — the unlit side of a sphere darkens as if it were lit — because
            // fresnel(N,L) goes to 1 at the terminator. Splitting the lobes
            // apart to fix this means carrying `direct` and `ambient` separately
            // through the sheen and transmission blocks; worth doing if coat
            // leaves draft, not worth the churn while it can still change.
            // Flagged upstream with the two default mismatches (see the PR).
            float fV = ccF0 + (1.0 - ccF0) * pow(clamp(1.0 - ndotv, 0.0, 1.0), 5.0);
            float fL = ccF0 + (1.0 - ccF0) * pow(clamp(1.0 - ndotl, 0.0, 1.0), 5.0);
            float R  = 0.5 * (fV + fL);
            float T  = (1.0 - R) * (1.0 - R) * (1.0 - clearcoatRoughness * 0.5);
            baseThroughCoat = tint * mix(1.0, T, clamp(coatDarkAmt, 0.0, 1.0) * clearcoatFactor);
        }

        color = color * (1.0 - ccF) * baseThroughCoat
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
        float worldThickness = volumeThickness
                             * max(max(modelScale.x, modelScale.y), modelScale.z);
        vec3 refracted = refract(-V, N, 1.0 / max(ior, 1.0001));
        vec3 exitPos = probe ? inWorldPos
                             : inWorldPos + normalize(refracted) * worldThickness;

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

            // ── KHR_materials_scatter (draft; issue #79) ─────────────────────
            // The spec's thin-walled model mixes the specular BTDF toward a
            // diffuse scatter lobe:
            //
            //   mix(specular_btdf(a) * baseColor,
            //       scatter_bsdf(multiscatterColor, g),
            //       scatterStrength)
            //
            // and splits that lobe's energy by anisotropy: (1+g)/2 forward
            // through the surface, (1-g)/2 backward as reflection. g=+1 is a
            // pure Lambertian BTDF, g=-1 a pure Lambertian BRDF (opaque).
            //
            // VOLUMETRIC MODE USES THIS SAME PATH. The spec explicitly allows
            // it — "it is acceptable to approximate volumetric mode using
            // thin-walled mode behavior" for dense subsurface materials — and
            // full transport (Kulla-Conty multi->single albedo remap driving a
            // Henyey-Greenstein walk) has no place in a forward raster pass.
            // Dense is exactly the regime these conformance assets sit in:
            // attenuationDistance 0.01 against thicknessFactor 8.9.
            //
            // Substituting the lobe also side-steps the absorption above, which
            // is the whole point of the extension: in a dense volume Beer-
            // Lambert removes essentially all the transmitted light, and
            // scattering is what puts it back. Rendered without scatter these
            // materials go nearly black, which is exactly what we measured.
            // How MUCH of the light actually undergoes multiple scattering. This
            // is the term that makes attenuationDistance matter: optical depth
            // tau = thickness / attenuationDistance, so a sparse volume (long
            // attenuation distance) barely scatters and stays a mostly-clear
            // specular BTDF showing the backdrop, while a dense one is fully
            // diffused into the scatter lobe.
            //
            // Getting this wrong is not subtle. Mixing on scatterStrength alone
            // discards the Beer-Lambert result above, and since every asset in
            // the conformance set sets scatterStrengthFactor = 1 that collapses
            // to "always fully scattered" — the 4x4 density grid then renders
            // four IDENTICAL rows, which is how the bug was caught.
            //
            // Thin-walled mode has no volume to be optically deep, so it scatters
            // on strength alone, which is what the spec's thin-walled BSDF says.
            float opticalDepth = (attenuationDist > 0.0 && !isinf(attenuationDist))
                               ? max(worldThickness, 0.0) / attenuationDist
                               : 0.0;
            float density = (volumeThickness > 0.0) ? (1.0 - exp(-opticalDepth)) : 1.0;
            float scatterMix = scatterStrength * density;
            // The spec's Kulla-Conty remap, multi-scatter albedo -> single-scatter
            // albedo. Gated behind a switch because whether it BELONGS here is a
            // real question, not a formality: the spec applies it to derive the
            // transport coefficients (sigma_s = sigma_t * rho_ss), and we do no
            // transport. A single Lambertian lobe standing in for the CONVERGED
            // multi-bounce result should carry the multi-scatter albedo the artist
            // authored, not the per-bounce one. Measured both ways below.
            vec3 lobeAlbedo = multiscatterColor;
            if (ubo.viewport.z > 0.5) {   // DXR_MODELVIEWER_KULLA_CONTY=1
                vec3 rms = clamp(multiscatterColor * scatterStrength, vec3(0.0), vec3(1.0));
                vec3 ks = vec3(4.09712) + 4.20863 * rms
                        - sqrt(vec3(9.59217) + 41.6808 * rms + 17.7126 * rms * rms);
                lobeAlbedo = (vec3(1.0) - ks * ks) / (vec3(1.0) - scatterG * ks * ks);
            }
            if (scatterMix > 0.0) {
                // Backward half: a Lambertian reflection lobe of multi-scatter
                // albedo. Same shape as the diffuse lobe, different albedo.
                vec3 scatterBack = (lobeAlbedo / PI) * vec3(3.0 * ubo.tone.z) * ndotl
                                 + texture(irradianceMap, N).rgb * lobeAlbedo * ao;
                // Forward half: what lies behind, fully diffused. The top mip of
                // the scene chain is the most-blurred copy available, which is
                // the closest stand-in for a diffuse transmission lobe.
                float diffuseLod = float(textureQueryLevels(sceneColor) - 1);
                vec3 scatterFwd = textureLod(sceneColor, uv, diffuseLod).rgb * lobeAlbedo;

                float fwd = 0.5 * (1.0 + scatterG);
                float bwd = 0.5 * (1.0 - scatterG);
                vec3 scatterLobe = fwd * scatterFwd + bwd * scatterBack;

                transmitted = mix(transmitted, scatterLobe, scatterMix);
            }

            // Transmission replaces the DIFFUSE lobe — the specular reflection
            // off the surface stays. That is what makes glass read as glass
            // rather than as a hole in the image.
            // Same `dr` the direct diffuse was built with, so transmission
            // subtracts exactly the lobe that was added rather than a
            // Lambertian approximation of it.
            vec3 diffuseTerm = (kd * albedo / PI * dr) * vec3(3.0 * ubo.tone.z) * ndotl
                             + diffuseIBL * ao;
            vec3 transmissionTerm = transmitted * (1.0 - metallic);
            color += (transmissionTerm - diffuseTerm) * transmissionFactor;
        }
    }

    // ── KHR_materials_fuzz (draft) ───────────────────────────────────────────
    // Fine surface fibres — peach skin, velvet, dust, soot. Intended to replace
    // KHR_materials_sheen, and its two differences from sheen are both here.
    //
    // POSITION. Fuzz is the TOPMOST layer, above the coat; sheen sits below it.
    // That is why this block is at the end of main() and the sheen block is
    // before the coat's. `hasFuzz` routes the shared colour/roughness lanes to
    // one place or the other.
    //
    // WEIGHT. Sheen used its colour as an intensity, so a black sheen colour
    // disabled the layer and black fuzz was inexpressible. Fuzz has a separate
    // weight, and the spec's layering
    //   fuzz = fuzzColor·refl·weight + base·mix(1, 1-refl, weight)
    // multiplies the COLOUR rather than gating the layer — so a black fuzzColor
    // darkens what is underneath instead of vanishing. Soot is the motivating
    // case, and it is the reason the extension exists.
    //
    // The reflectance is the directional albedo of the Charlie/Ashikhmin lobe,
    // read from the table sheen_lut.frag already bakes. The spec recommends the
    // Zeltner/Burley/Chiang LTC sheen model, which is the same family; reusing
    // one table keeps the layering weight consistent with the lobe actually
    // evaluated, exactly as sheen's energy compensation does.
    //
    // KNOWN LIMIT, measured: Efz saturates at its 1.0 clamp for fuzz roughness
    // below about 0.5 (dumped straight out of the table: 1.00 at 0.3, 0.54 at
    // 1.0). D_Charlie x V_Ashikhmin is not energy-conserving down there and its
    // directional albedo genuinely integrates above 1. Where it saturates,
    // `1 - Efz` is zero and the layer transmits NOTHING — a black fuzz nulls the
    // surface to exactly 0 rather than darkening it. Sheen has always had this
    // (its energy compensation removes the whole base at those roughnesses) but
    // hides it, because sheen's colour is its intensity so it always has a lobe
    // to put back. Fuzz's separate weight is what makes it visible. Fixing it
    // means renormalising the lobe, which would change every sheen material —
    // out of scope here, where sheen is required to stay byte-identical. See the
    // follow-up issue.
    //
    // Skipped under the #75 transmission probe, which asserts on `color` being
    // the raw scene sample; a layer over the top would break that.
    if (hasFuzz && fuzzFactor > 0.0 && !probe) {
        float Efz = texture(sheenLUT, vec2(ndotv, sheenRoughness)).r;
        vec3 fuzzDirect = sheenColor * D_Charlie(ndoth, sheenRoughness)
                        * V_Ashikhmin(ndotl, ndotv) * 3.0 * ubo.tone.z * ndotl;
        // Ambient fuzz: the irradiance cube stands in for the full integral,
        // weighted by the same directional albedo. Same approximation the sheen
        // lobe makes, and the same caveat.
        vec3 fuzzAmb = sheenColor * texture(irradianceMap, N).rgb * Efz * ao;
        color = (fuzzDirect + fuzzAmb) * fuzzFactor
              + color * mix(1.0, 1.0 - Efz, fuzzFactor);
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
