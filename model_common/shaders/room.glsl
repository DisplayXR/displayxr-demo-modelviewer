// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Analytic emulation of three.js's RoomEnvironment, used as the IBL
// environment source (irradiance + prefilter generation) for lighting mode
// `room`. Returns LINEAR radiance for a direction, exactly as the PMREM the
// storefront page bakes would.
//
// WHY an emulation rather than a captured HDRI: the page does
//   scene.environment = new PMREMGenerator(renderer)
//                         .fromScene(new RoomEnvironment(), 0.04).texture
// and nothing else — no punctual lights at all. RoomEnvironment is not an
// image; it is a tiny procedural SCENE (three r0.180.0,
// examples/jsm/environments/RoomEnvironment.js): a back-side box "room", one
// point light, six unlit furniture boxes, and six emissive panels standing in
// for area lights. `fromScene` renders it from the SCENE ORIGIN, so the PMREM
// is exactly "what a camera at (0,0,0) sees looking in direction d" — which is
// a closed-form ray cast against 13 boxes. Shipping the emulation instead of a
// baked .hdr keeps the viewer asset-free (an undock must not wait on a
// download) and keeps the two renderers reading the SAME numbers rather than a
// resampled, tone-mapped picture of them.
//
// What is reproduced exactly: every position/scale/intensity in the three
// source, the point light's `getDistanceAttenuation` (decay 2 with three's
// pow2(saturate(1 - pow4(d/cutoff))) window), Lambert `albedo/PI`, and three's
// ColorManagement decode of the hex colours (0xffffff → 1.0 linear).
//
// Deviations, all deliberate and all small:
//   * the room + furniture are shaded DIFFUSE ONLY. Their MeshStandardMaterial
//     is roughness 1 / metalness 0, whose specular lobe is a broad, dim wash
//     under a single point light — invisible next to panels that are 17-100x
//     brighter, and it would cost a GGX evaluation on every one of the ~100M
//     samples the prefilter takes.
//   * no shadowing. Neither has the page: RoomEnvironment enables no shadow
//     map, so its furniture does not occlude the point light either.
//   * the emissive panels light nothing (they only occlude and emit). Same in
//     three — emissive is not a light source in a raster pass.

// ── The scene, verbatim from RoomEnvironment.js (three r0.180.0) ────────────
// BoxGeometry is a UNIT cube, so `scale` IS the full extent and half = scale/2.

const vec3  ROOM_LIGHT_POS = vec3(0.418, 16.199, 0.300);
const float ROOM_LIGHT_I   = 900.0;   // PointLight(0xffffff, 900, 28, 2)
const float ROOM_LIGHT_CUT = 28.0;

// room: Mesh(box, MeshStandardMaterial({side: BackSide})) — colour defaults to
// 0xffffff, so the albedo is 1.0, not a grey.
const vec3 ROOM_CENTER = vec3(-0.757, 13.219, 0.717);
const vec3 ROOM_HALF   = vec3(31.713, 28.305, 28.591) * 0.5;
const float ROOM_ALBEDO = 1.0;

// Six emissive panels (createAreaLightMaterial): colour black, emissive white,
// emissiveIntensity = the radiance below. Axis-aligned (no rotation).
const vec3 ROOM_PANEL_C[6] = vec3[6](
    vec3(-16.116, 14.370,   8.208),   // -x right
    vec3(-16.109, 18.021,  -8.207),   // -x left
    vec3( 14.904, 12.198,  -1.832),   // +x
    vec3( -0.462,  8.890,  14.520),   // +z
    vec3(  3.235, 11.486, -12.541),   // -z
    vec3(  0.000, 20.000,   0.000));  // +y
const vec3 ROOM_PANEL_H[6] = vec3[6](
    vec3(0.050, 1.2140, 1.3695),
    vec3(0.050, 1.2125, 1.3755),
    vec3(0.075, 2.1325, 3.1655),
    vec3(2.190, 2.7205, 0.0440),
    vec3(1.250, 1.0000, 0.0500),
    vec3(0.500, 0.0500, 0.5000));
const float ROOM_PANEL_I[6] = float[6](50.0, 50.0, 17.0, 43.0, 20.0, 100.0);

// Six furniture boxes (the InstancedMesh), default white standard material.
// Each carries a rotation about Y only, so the ray test is an AABB test in the
// box's own frame — cos/sin precomputed from the source's radians.
const vec3 ROOM_BOX_C[6] = vec3[6](
    vec3(-10.906,  2.009,  1.846),
    vec3( -5.607, -0.754, -0.758),
    vec3(  6.167,  0.857,  7.803),
    vec3( -2.017,  0.018,  6.124),
    vec3(  2.291, -0.756, -2.621),
    vec3( -2.193, -0.369, -5.547));
const vec3 ROOM_BOX_H[6] = vec3[6](
    vec3(1.1640, 3.9525, 2.3255),
    vec3(0.9850, 0.7670, 1.9775),
    vec3(1.9635, 3.1425, 1.8435),
    vec3(1.0010, 2.2830, 1.0320),
    vec3(0.7730, 0.7760, 0.7480),
    vec3(1.9375, 1.7435, 1.4930));
// (cos, sin) of rotation.y = -0.195, 0.994, 0.561, 0.333, -0.286, 0.516
const vec2 ROOM_BOX_ROT[6] = vec2[6](
    vec2( 0.9810603, -0.1937678),
    vec2( 0.5460827,  0.8377235),
    vec2( 0.8467107,  0.5320570),
    vec2( 0.9451838,  0.3268818),
    vec2( 0.9593145, -0.2821062),
    vec2( 0.8698444,  0.4933561));

// ── Ray helpers ─────────────────────────────────────────────────────────────

// 1/d with the axis-parallel case (d component 0) pushed to a large finite
// value of the RIGHT sign. A bare 1.0/0.0 is +inf either way, and (lo-o)*inf
// with lo == o is a NaN that poisons the min/max chain.
vec3 roomInvDir(vec3 d) {
    vec3 s = vec3(d.x < 0.0 ? -1.0 : 1.0, d.y < 0.0 ? -1.0 : 1.0, d.z < 0.0 ? -1.0 : 1.0);
    return s / max(abs(d), vec3(1e-8));
}

// Slab test. `slabLo`/`slabHi` are the per-axis entry/exit parameters kept so
// the caller can name the winning face; the ray hits iff far >= max(near, 0).
void roomSlabs(vec3 o, vec3 invd, vec3 c, vec3 h, out float tNear, out float tFar,
               out vec3 slabLo, out vec3 slabHi) {
    vec3 t0 = (c - h - o) * invd;
    vec3 t1 = (c + h - o) * invd;
    slabLo = min(t0, t1);
    slabHi = max(t0, t1);
    tNear = max(max(slabLo.x, slabLo.y), slabLo.z);
    tFar  = min(min(slabHi.x, slabHi.y), slabHi.z);
}

// The face normal of the slab whose parameter equals `t`, pointing back along
// the ray (i.e. toward the camera): -sign(d) on the winning axis.
vec3 roomFaceNormal(vec3 d, vec3 slab, float t) {
    if (t == slab.x) return vec3(d.x < 0.0 ? 1.0 : -1.0, 0.0, 0.0);
    if (t == slab.y) return vec3(0.0, d.y < 0.0 ? 1.0 : -1.0, 0.0);
    return vec3(0.0, 0.0, d.z < 0.0 ? 1.0 : -1.0);
}

// Outgoing radiance of a white Lambertian surface lit by the room's single
// point light. Mirrors three's getPointLightInfo + BRDF_Lambert:
//   irradiance = I * getDistanceAttenuation(d, 28, 2) * NdotL
//   L_out      = irradiance * albedo / PI
vec3 roomSurface(vec3 p, vec3 n) {
    vec3  toL = ROOM_LIGHT_POS - p;
    float d2  = dot(toL, toL);
    float d   = sqrt(max(d2, 1e-12));
    float ndl = max(dot(n, toL / d), 0.0);
    float w   = clamp(1.0 - pow(d / ROOM_LIGHT_CUT, 4.0), 0.0, 1.0);
    float att = (w * w) / max(d2, 0.01);
    return vec3(ROOM_ALBEDO * 0.31830988618 * ROOM_LIGHT_I * att * ndl);
}

// ── The environment ─────────────────────────────────────────────────────────
// Cast from the scene origin — where PMREMGenerator.fromScene puts its camera.
vec3 roomRadiance(vec3 dir) {
    vec3 d = normalize(dir);
    vec3 o = vec3(0.0);
    vec3 invd = roomInvDir(d);

    // The room is the backdrop: we are inside it, so the hit is where the ray
    // EXITS, and the surface we see is its inward face.
    float rn, rf; vec3 rlo, rhi;
    roomSlabs(o, invd, ROOM_CENTER, ROOM_HALF, rn, rf, rlo, rhi);
    float bestT = rf;
    vec3  bestN = roomFaceNormal(d, rhi, rf);
    int   bestPanel = -1;

    // Emissive panels (axis-aligned).
    for (int i = 0; i < 6; ++i) {
        float tn, tf; vec3 slo, shi;
        roomSlabs(o, invd, ROOM_PANEL_C[i], ROOM_PANEL_H[i], tn, tf, slo, shi);
        if (tf >= max(tn, 0.0) && tn > 0.0 && tn < bestT) {
            bestT = tn; bestPanel = i;
        }
    }

    // Furniture (rotated about Y → test in the box's own frame).
    for (int i = 0; i < 6; ++i) {
        vec2 cs = ROOM_BOX_ROT[i];
        vec3 rel = o - ROOM_BOX_C[i];
        // Ry(-theta): x' = c*x - s*z, z' = s*x + c*z
        vec3 lo3 = vec3(cs.x * rel.x - cs.y * rel.z, rel.y, cs.y * rel.x + cs.x * rel.z);
        vec3 ld  = vec3(cs.x * d.x   - cs.y * d.z,   d.y,   cs.y * d.x   + cs.x * d.z);
        vec3 linv = roomInvDir(ld);
        float tn, tf; vec3 slab, shi;
        roomSlabs(lo3, linv, vec3(0.0), ROOM_BOX_H[i], tn, tf, slab, shi);
        if (tf >= max(tn, 0.0) && tn > 0.0 && tn < bestT) {
            bestT = tn;
            bestPanel = -1;
            vec3 ln = roomFaceNormal(ld, slab, tn);
            // Ry(+theta) back to world.
            bestN = vec3(cs.x * ln.x + cs.y * ln.z, ln.y, -cs.y * ln.x + cs.x * ln.z);
        }
    }

    if (bestPanel >= 0) return vec3(ROOM_PANEL_I[bestPanel]);
    return roomSurface(o + d * bestT, bestN);
}
