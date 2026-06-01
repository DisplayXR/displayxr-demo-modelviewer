// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Metallic-roughness PBR fragment shader (Cook-Torrance GGX) with a single
// directional light + a constant ambient term. v1 uses material FACTORS only
// (no textures, no IBL) — base-color/normal/MR/emissive maps + image-based
// lighting are the follow-up. See ../../PORTING.md.
#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 mrParams;     // x=metallic, y=roughness
    vec4 emissive;     // rgb
} pc;

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

void main() {
    vec3 albedo = pc.baseColorFactor.rgb;
    float metallic = clamp(pc.mrParams.x, 0.0, 1.0);
    float roughness = clamp(pc.mrParams.y, 0.04, 1.0);
    float a = roughness * roughness;

    vec3 N = normalize(inNormal);
    vec3 V = normalize(ubo.cameraPos.xyz - inWorldPos);
    // Two-sided: face the normal toward the viewer.
    if (dot(N, V) < 0.0) N = -N;
    vec3 L = normalize(ubo.lightDir.xyz);
    vec3 H = normalize(V + L);

    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, H), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float D = D_GGX(ndoth, a);
    float G = G_SchlickSmith(ndotv, ndotl, a);
    vec3  F = F_Schlick(max(dot(H, V), 0.0), f0);

    vec3 spec = (D * G) * F / max(4.0 * ndotv * ndotl, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / PI;

    vec3 lightColor = vec3(3.0);  // directional intensity
    vec3 color = (diffuse + spec) * lightColor * ndotl;

    // Constant ambient so unlit faces aren't pure black (cheap IBL stand-in).
    color += albedo * 0.18;
    color += pc.emissive.rgb;

    outColor = vec4(color, pc.baseColorFactor.a);
}
