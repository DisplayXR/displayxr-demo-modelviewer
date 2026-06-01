// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Metallic-roughness PBR vertex shader (v1: static geometry, no skinning).
// viewProj is proj * Y-flipped-view (see ModelRenderer::updateUniforms), so
// the model orientation matches the GS demo's pose convention exactly.
// Texture (uv) + skinning + morph targets are follow-ups. See ../../PORTING.md.
#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;     // proj * Y-flipped view
    mat4 view;         // Z-forward-adjusted view (for the foreground clip)
    vec4 cameraPos;    // world-space, .w unused
    vec4 lightDir;     // .xyz = world dir TO light, .w = clipFar (view-space; 0=off)
    mat4 invViewProj;  // (skybox only)
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 mrParams;     // x=metallic, y=roughness
    vec4 emissive;     // rgb
} pc;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out float outViewZ;   // view-space forward distance

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    outWorldPos = world.xyz;
    outNormal = normalize(mat3(pc.model) * inNormal);
    outUV = inUV;
    outViewZ = (ubo.view * world).z;
    gl_Position = ubo.viewProj * world;
}
