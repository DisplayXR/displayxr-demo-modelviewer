// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Sheen directional albedo E(N·V, sheenRoughness), baked once at startup.
//
// KHR_materials_sheen specifies that the base layer is scaled by
// 1 - max3(sheenColor) * E, so that sheen REDISTRIBUTES energy instead of adding
// it. Without E, a sheened fabric is brighter than the light falling on it —
// which is the approximation this viewer previously shipped and documented.
//
// E is the hemispherical integral of the sheen BRDF against the cosine:
//
//     E(V) = ∫ D_Charlie(N·H, α) · V_Ashikhmin(N·L, N·V) · (N·L) dω_L
//
// Sheen colour is deliberately not part of it — it factors out, so one scalar
// table serves every tint. Integrated with uniform hemisphere sampling: the
// Charlie lobe is broad and grazing-weighted, so importance sampling GGX-style
// would fit it badly, and 2048 uniform samples of a smooth integrand converge
// fine for a 64×64 table baked once.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ibl_common.glsl"    // hammersley, PI
#define SHEEN_PI PI
#include "sheen.glsl"

layout(location = 0) in vec2 inUV;   // NDC [-1,1] from the fullscreen triangle
layout(location = 0) out vec4 outColor;

void main() {
    // Map the fullscreen triangle to the table's axes.
    float ndotv = clamp(inUV.x * 0.5 + 0.5, 1e-3, 1.0);
    float rough = clamp(inUV.y * 0.5 + 0.5, 1e-3, 1.0);

    // View in the tangent frame, N = +Z.
    vec3 V = vec3(sqrt(max(1.0 - ndotv * ndotv, 0.0)), 0.0, ndotv);

    const uint NS = 2048u;
    float sum = 0.0;
    for (uint i = 0u; i < NS; ++i) {
        vec2 Xi = hammersley(i, NS);
        // Uniform over the hemisphere: pdf = 1/(2π).
        float cosT = Xi.y;
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        float phi = 2.0 * PI * Xi.x;
        vec3 L = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
        vec3 H = normalize(L + V);

        float ndotl = max(L.z, 0.0);
        float ndoth = max(H.z, 0.0);
        if (ndotl <= 0.0) continue;
        sum += D_Charlie(ndoth, rough) * V_Ashikhmin(ndotl, ndotv) * ndotl;
    }
    // × 2π/N for the uniform-hemisphere pdf.
    float E = sum * (2.0 * PI) / float(NS);
    outColor = vec4(clamp(E, 0.0, 1.0), 0.0, 0.0, 1.0);
}
