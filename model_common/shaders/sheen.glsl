// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// KHR_materials_sheen BRDF — the Charlie distribution plus Ashikhmin
// visibility, as specified by the extension. Models retroreflective fabric
// fuzz, which GGX cannot: the lobe peaks at grazing angles rather than around
// the mirror direction.
//
// Shared deliberately between pbr.frag (which evaluates it) and sheen_lut.frag
// (which integrates it into a directional-albedo table). Energy compensation is
// only correct if the table is the integral of the *same* function that gets
// evaluated — keeping one copy makes that true by construction rather than by
// vigilance.

#ifndef SHEEN_PI
#define SHEEN_PI 3.14159265359
#endif

float D_Charlie(float ndoth, float sheenRough) {
    float alpha = max(sheenRough * sheenRough, 1e-4);
    float invAlpha = 1.0 / alpha;
    float cos2h = ndoth * ndoth;
    float sin2h = max(1.0 - cos2h, 1e-7);
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * SHEEN_PI);
}

float V_Ashikhmin(float ndotl, float ndotv) {
    return clamp(1.0 / (4.0 * (ndotl + ndotv - ndotl * ndotv)), 0.0, 1.0);
}
