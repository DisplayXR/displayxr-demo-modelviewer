// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// SKELETON metallic-roughness PBR vertex shader. Replace with the full
// pbr.vert from SaschaWillems/Vulkan-glTF-PBR (skinning + morph targets +
// node matrices) during the port. See ../../PORTING.md.
#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0) uniform UBO {
    mat4 proj;   // asymmetric per-eye Kooima projection (supplied by the demo)
    mat4 view;
    mat4 model;
} ubo;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;

void main() {
    vec4 world = ubo.model * vec4(inPos, 1.0);
    outWorldPos = world.xyz;
    outNormal = mat3(ubo.model) * inNormal;
    outUV = inUV;
    gl_Position = ubo.proj * ubo.view * world;
}
