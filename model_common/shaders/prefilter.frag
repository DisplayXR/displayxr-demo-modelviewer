// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Prefiltered specular environment: GGX importance-sample the environment
// (loaded HDRI, else the analytic sky — see env.glsl) for the mip's roughness.
// One mip per roughness level (set via push constant).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ibl_common.glsl"
#include "env.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform P { int face; float roughness; float envIsHdri; float envIsRoom; } pc;

// The room environment is a PMREM on the page: the storefront bakes it with
// PMREMGenerator.fromScene(new RoomEnvironment(), 0.04), and that sigma is a
// blur applied to the sharpest mip. Our analytic room is perfectly sharp, so
// mip 0 (roughness 0) would give a mirror a harder panel edge than the page
// shows. A small roughness floor stands in for the sigma; it costs nothing
// anywhere else, because every mip above the floor is already rougher.
const float ROOM_SIGMA_ROUGHNESS = 0.04;

void main() {
    vec3 N = dirForFace(pc.face, inUV);
    vec3 V = N;   // common approximation: view = reflection = normal
    float roughness = (pc.envIsRoom > 0.5)
                    ? max(pc.roughness, ROOM_SIGMA_ROUGHNESS) : pc.roughness;

    const uint NS = 1024u;
    vec3 color = vec3(0.0);
    float totalW = 0.0;
    for (uint i = 0u; i < NS; ++i) {
        vec2 Xi = hammersley(i, NS);
        vec3 H = importanceSampleGGX(Xi, roughness, N);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            color += envRadiance(L, pc.envIsHdri, pc.envIsRoom) * NdotL;
            totalW += NdotL;
        }
    }
    outColor = vec4(color / max(totalW, 1e-3), 1.0);
}
