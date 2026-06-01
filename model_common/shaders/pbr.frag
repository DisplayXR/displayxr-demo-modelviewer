// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// SKELETON PBR fragment shader: a single directional light with a Lambert +
// trivial specular term, no textures, no IBL. Replace with the full
// pbr.frag from SaschaWillems/Vulkan-glTF-PBR (metallic-roughness BRDF,
// base-color/normal/MR/AO/emissive maps, irradiance + prefiltered-env IBL,
// BRDF LUT) during the port. See ../../PORTING.md.
#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(inNormal);
    vec3 L = normalize(vec3(0.4, 0.8, 0.5));
    float ndotl = max(dot(N, L), 0.0);
    vec3 base = vec3(0.8);
    vec3 color = base * (0.15 + 0.85 * ndotl);
    outColor = vec4(color, 1.0);
}
