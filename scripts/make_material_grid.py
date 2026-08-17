#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""
Generate the material-grid reference scene: assets/material_grid.glb

Phase 1 of issue #70. A sphere per material axis, laid out as a grid — one row
per material family, one column per step of that family's parameter sweep.

Why a generator and not a checked-in binary
-------------------------------------------
The grid is a *measurement instrument*, not art. Every value in it needs to be
inspectable and re-derivable: when a phase 2 shader change moves a pixel, the
question is always "what exactly is that sphere's roughness / clearcoat /
anisotropy?", and a 300 KB opaque .glb can't answer it. The script can, and it
regenerates byte-identical output.

It deliberately writes the real `KHR_materials_*` extensions even though the
viewer implements none of them today. That makes it three things at once:

  1. the development target for phase 2 (implement a row, watch it come alive);
  2. the asset that proves unsupported features degrade *explicitly* — every row
     from CLEARCOAT down currently renders as its base metallic-roughness layer,
     which is the correct fallback, and the viewer should say so rather than
     silently pretend;
  3. a stable reference the OpenPBR-authored hero scene can be checked against.

The extensions go in `extensionsUsed`, never `extensionsRequired`, so a
conformant loader that doesn't implement them still loads the file instead of
refusing it.

Usage:
    python3 scripts/make_material_grid.py [out.glb]
"""

import json
import math
import struct
import sys
import zlib
from pathlib import Path

# ── Geometry ────────────────────────────────────────────────────────────────
LON, LAT = 48, 32          # sphere tessellation (smooth enough for a specular ref)
RADIUS = 0.45
SPACING = 1.0              # centre-to-centre, so 0.1 of gap between spheres
STEPS = 7                  # columns = sweep steps per family


def uv_sphere(radius, lon, lat):
    """Positions / normals / uvs / TANGENTs / indices for a lat-long sphere.

    The tangent is analytic — d(position)/d(longitude), i.e. the direction of
    increasing u — rather than derived from the triangles. That matters for the
    anisotropy row: anisotropy's direction is DEFINED in the tangent frame, and
    a frame reconstructed from screen-space UV derivatives flips across the UV
    seam and degenerates at the poles. Authoring TANGENT is what lets the viewer
    apply anisotropy to image-based lighting at all (issue #70).
    """
    pos, nrm, uv, tan, idx = [], [], [], [], []
    for j in range(lat + 1):
        v = j / lat
        theta = v * math.pi
        st, ct = math.sin(theta), math.cos(theta)
        for i in range(lon + 1):
            u = i / lon
            phi = u * 2.0 * math.pi
            sp, cp = math.sin(phi), math.cos(phi)
            n = (st * cp, ct, st * sp)
            nrm.append(n)
            pos.append((n[0] * radius, n[1] * radius, n[2] * radius))
            uv.append((u, v))
            # d/dphi of the position, normalised: points along +u (east).
            # Handedness +1 gives bitangent = cross(normal, tangent).
            tan.append((-sp, 0.0, cp, 1.0))
    for j in range(lat):
        for i in range(lon):
            a = j * (lon + 1) + i
            b = a + lon + 1
            # CCW when viewed from outside, matching glTF's front-face winding.
            idx += [a, b, a + 1, a + 1, b, b + 1]
    return pos, nrm, uv, tan, idx


# ── Material families ───────────────────────────────────────────────────────
# Each entry: (row label, docstring, builder(t) -> glTF material dict) where t
# sweeps 0..1 across the row. Base colours are chosen so a family stays
# recognisable at a glance once its extension is implemented.

def lerp(a, b, t):
    return a + (b - a) * t


# ── The one texture the textured row shares ─────────────────────────────────
# A vertical ramp with ALL FOUR channels carrying the same 0→1 gradient. The
# extensions read different channels (clearcoat R, clearcoatRoughness G,
# sheenRoughness A, specular A, transmission R, thickness G…), so one image
# drives every one of them and each sphere shows a pole-to-pole sweep of its own
# property. If texture support regresses, the row goes flat — which is the same
# tell the rest of the grid uses.
TEX_W, TEX_H = 8, 64


def gradient_png():
    px = bytearray()
    for y in range(TEX_H):
        v = int(round(255.0 * y / (TEX_H - 1)))
        px += bytes((v, v, v, v)) * TEX_W
    raw = b"".join(b"\x00" + bytes(px[y * TEX_W * 4:(y + 1) * TEX_W * 4])
                   for y in range(TEX_H))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", TEX_W, TEX_H, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def ripple_normal_png(w=64, h=64, periods=4, amp=0.60):
    """A tangent-space normal map: sinusoidal ripples running along +u.

    Needed because coat_test's coat-normal row has to perturb the COAT and
    nothing else, and the grid's shared gradient ramp cannot serve — decoded as a
    normal it gives (v,v,v)*2-1, which is degenerate and normalizes 0/0 at the
    midpoint. This encodes a genuine unit normal per texel, so a wiring error in
    the coat-normal path shows up as a flat or wrongly-oriented sphere rather
    than as noise."""
    import math
    px = bytearray()
    for y in range(h):
        for x in range(w):
            # slope of sin along u -> normal tilts in the u direction only
            dzdx = amp * math.cos(2.0 * math.pi * periods * x / w)
            n = (-dzdx, 0.0, 1.0)
            L = math.sqrt(n[0] * n[0] + n[2] * n[2])
            nx, ny, nz = n[0] / L, 0.0, n[2] / L
            px += bytes((int(round((nx * 0.5 + 0.5) * 255)),
                         int(round((ny * 0.5 + 0.5) * 255)),
                         int(round((nz * 0.5 + 0.5) * 255)), 255))
    raw = b"".join(b"\x00" + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\x0a"[:8]
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def pbr(base, metallic, roughness):
    return {"baseColorFactor": list(base) + [1.0],
            "metallicFactor": metallic,
            "roughnessFactor": roughness}


def m_dielectric(t):
    return {"pbrMetallicRoughness": pbr((0.80, 0.80, 0.82), 0.0, max(0.03, t))}


def m_metal(t):
    return {"pbrMetallicRoughness": pbr((0.94, 0.78, 0.38), 1.0, max(0.03, t))}


def m_clearcoat(t):
    return {"pbrMetallicRoughness": pbr((0.55, 0.06, 0.06), 0.0, 0.55),
            "extensions": {"KHR_materials_clearcoat": {
                "clearcoatFactor": t, "clearcoatRoughnessFactor": 0.06}}}


def m_sheen(t):
    return {"pbrMetallicRoughness": pbr((0.14, 0.20, 0.42), 0.0, 0.85),
            "extensions": {"KHR_materials_sheen": {
                "sheenColorFactor": [0.85, 0.80, 0.70],
                "sheenRoughnessFactor": max(0.05, t)}}}


def m_anisotropy(t):
    return {"pbrMetallicRoughness": pbr((0.85, 0.85, 0.88), 1.0, 0.35),
            "extensions": {"KHR_materials_anisotropy": {
                "anisotropyStrength": t, "anisotropyRotation": 0.0}}}


def m_iridescence(t):
    return {"pbrMetallicRoughness": pbr((0.05, 0.05, 0.06), 0.0, 0.12),
            "extensions": {"KHR_materials_iridescence": {
                "iridescenceFactor": 1.0,
                "iridescenceIor": 1.3,
                "iridescenceThicknessMinimum": 100.0,
                "iridescenceThicknessMaximum": lerp(200.0, 800.0, t)}}}


def m_specular_ior(t):
    return {"pbrMetallicRoughness": pbr((0.72, 0.72, 0.75), 0.0, 0.20),
            "extensions": {
                "KHR_materials_specular": {"specularFactor": t},
                "KHR_materials_ior": {"ior": lerp(1.0, 2.0, t)}}}


def m_transmission(t):
    return {"pbrMetallicRoughness": pbr((1.0, 1.0, 1.0), 0.0, 0.05),
            "extensions": {
                "KHR_materials_transmission": {"transmissionFactor": t},
                "KHR_materials_ior": {"ior": 1.5},
                "KHR_materials_volume": {
                    "thicknessFactor": 0.9,
                    "attenuationDistance": 1.5,
                    "attenuationColor": [0.85, 0.95, 0.92]}}}


def m_emissive(t):
    return {"pbrMetallicRoughness": pbr((0.02, 0.02, 0.02), 0.0, 0.6),
            "emissiveFactor": [0.10, 0.55, 0.85],
            "extensions": {"KHR_materials_emissive_strength": {
                "emissiveStrength": lerp(0.0, 6.0, t)}}}


# Row 9 — one sphere per texture-driven property, all reading the same ramp.
TEXTURED = [
    ("clearcoatTex", {"pbrMetallicRoughness": pbr((0.55, 0.06, 0.06), 0.0, 0.55),
                      "extensions": {"KHR_materials_clearcoat": {
                          "clearcoatFactor": 1.0, "clearcoatRoughnessFactor": 0.06,
                          "clearcoatTexture": {"index": 0}}}}),
    ("clearcoatRoughTex", {"pbrMetallicRoughness": pbr((0.55, 0.06, 0.06), 0.0, 0.55),
                           "extensions": {"KHR_materials_clearcoat": {
                               "clearcoatFactor": 1.0, "clearcoatRoughnessFactor": 1.0,
                               "clearcoatRoughnessTexture": {"index": 0}}}}),
    ("sheenColorTex", {"pbrMetallicRoughness": pbr((0.14, 0.20, 0.42), 0.0, 0.85),
                       "extensions": {"KHR_materials_sheen": {
                           "sheenColorFactor": [1.0, 0.95, 0.85],
                           "sheenRoughnessFactor": 0.3,
                           "sheenColorTexture": {"index": 0}}}}),
    ("sheenRoughTex", {"pbrMetallicRoughness": pbr((0.14, 0.20, 0.42), 0.0, 0.85),
                       "extensions": {"KHR_materials_sheen": {
                           "sheenColorFactor": [1.0, 0.95, 0.85],
                           "sheenRoughnessFactor": 1.0,
                           "sheenRoughnessTexture": {"index": 0}}}}),
    ("specularTex", {"pbrMetallicRoughness": pbr((0.72, 0.72, 0.75), 0.0, 0.20),
                     "extensions": {"KHR_materials_specular": {
                         "specularFactor": 1.0,
                         "specularTexture": {"index": 0}}}}),
    ("transmissionTex", {"pbrMetallicRoughness": pbr((1.0, 1.0, 1.0), 0.0, 0.05),
                         "extensions": {
                             "KHR_materials_transmission": {
                                 "transmissionFactor": 1.0,
                                 "transmissionTexture": {"index": 0}},
                             "KHR_materials_ior": {"ior": 1.5},
                             "KHR_materials_volume": {"thicknessFactor": 0.9}}}),
    ("thicknessTex", {"pbrMetallicRoughness": pbr((1.0, 1.0, 1.0), 0.0, 0.05),
                      "extensions": {
                          "KHR_materials_transmission": {"transmissionFactor": 1.0},
                          "KHR_materials_ior": {"ior": 1.5},
                          "KHR_materials_volume": {
                              "thicknessFactor": 1.6,
                              "attenuationDistance": 0.6,
                              "attenuationColor": [0.35, 0.75, 0.55],
                              "thicknessTexture": {"index": 0}}}}),
]


ROWS = [
    ("dielectric",   "metallic 0, roughness 0.03 → 1.0",            m_dielectric),
    ("metal",        "metallic 1, roughness 0.03 → 1.0",            m_metal),
    ("clearcoat",    "clearcoatFactor 0 → 1 over a rough red base", m_clearcoat),
    ("sheen",        "sheenRoughnessFactor 0.05 → 1.0",             m_sheen),
    ("anisotropy",   "anisotropyStrength 0 → 1 on a brushed metal", m_anisotropy),
    ("iridescence",  "film thickness 200 → 800 nm",                 m_iridescence),
    ("specular_ior", "specularFactor 0 → 1, ior 1.0 → 2.0",         m_specular_ior),
    ("transmission", "transmissionFactor 0 → 1, ior 1.5, volume",   m_transmission),
    ("emissive",     "emissiveStrength 0 → 6",                      m_emissive),
]

# Row 9 is built from TEXTURED rather than a sweep function.
TEXTURED_ROW_LABEL = ("textured", "one texture-driven property per column")


# ── KHR_materials_coat sweep (draft; issue #81) ──────────────────────────────
# A SEPARATE asset rather than five more rows on material_grid.glb, for the same
# reason #79 validated scatter against Khronos' conformance assets instead of
# growing the grid: material_grid.glb is the regression baseline that "this
# change renders byte-identically" is measured against, and an asset that moves
# every time an extension lands cannot serve that purpose. Coat needs its own
# sweep because, unlike scatter, Khronos has published no conformance asset for
# it — glTF-Sample-Assets#269 carries the scatter models only.
#
# Every row shares one base material: a rough red dielectric, the same one the
# grid's clearcoat row uses. That is deliberate. Row 0 is plain
# KHR_materials_clearcoat and row 1 is the KHR_materials_coat spelling of the
# identical material, so the spec's claim that coat is a 1:1 superset of
# clearcoat becomes a measurement WITHIN a single capture — the two rows must
# read the same — rather than something taken on trust.
# A light NEUTRAL base, not the grid's red one. An amber tint on a red base is
# very nearly a no-op — the base has almost no green or blue left to remove —
# and the first cut of this asset used red and measured a flat row because of it.
# Coat's tint and darkening both act on what the BASE shows through, so the base
# has to be bright and uncoloured for either to be legible.
COAT_BASE = (0.78, 0.78, 0.80)
COAT_ROUGH = 0.40


def m_coat_clearcoat_ref(t):
    """Control: the old extension, factor 0 -> 1."""
    return {"pbrMetallicRoughness": pbr(COAT_BASE, 0.0, COAT_ROUGH),
            "extensions": {"KHR_materials_clearcoat": {
                "clearcoatFactor": t, "clearcoatRoughnessFactor": 0.06}}}


def m_coat_factor(t):
    """The same sweep in coat's spelling. coatDarkeningFactor 0 to match the
    control — the spec defaults it to 1, and that difference is row 3's job."""
    return {"pbrMetallicRoughness": pbr(COAT_BASE, 0.0, COAT_ROUGH),
            "extensions": {"KHR_materials_coat": {
                "coatFactor": t, "coatRoughnessFactor": 0.06,
                "coatDarkeningFactor": 0.0}}}


def m_coat_color(t):
    """White -> amber tint. coatFactor 0.5, not 1: the tint acts on the base,
    while the coat's OWN reflection is untinted (correctly — it never entered
    the coat), so at full coat the sphere is half sky-reflection and the tint
    reads at half strength. Half a coat keeps the tinted base dominant.
    The tint should also deepen toward the rim, where the refracted path
    through the coat is longer."""
    return {"pbrMetallicRoughness": pbr(COAT_BASE, 0.0, COAT_ROUGH),
            "extensions": {"KHR_materials_coat": {
                "coatFactor": 0.5, "coatRoughnessFactor": 0.06,
                "coatDarkeningFactor": 0.0,
                "coatColorFactor": [1.0, 1.0 - 0.65 * t, 1.0 - 0.9 * t]}}}


def m_coat_darkening(t):
    """coatDarkeningFactor 0 -> 1. The property whose absence users notice: it
    is what makes a coated surface read as wet.

    coatFactor 0.5 for the same reason as the tint row. Darkening acts on the
    base, and at full coat the base is almost entirely displaced by the coat's
    own mirror reflection of the sky — rows 3-5 of the first cut of this asset
    all rendered as the identical dark sphere, with nothing left to darken.

    Expect a SMALL effect. The spec's transmittance is T = (1-R)^2, a single
    two-way pass, and R is the coat's Fresnel — about 0.04 head-on. Real
    wet-look darkening is stronger because light bounces inside the coat many
    times; a one-pass model gives roughly 10 %, and half of that at coat 0.5."""
    return {"pbrMetallicRoughness": pbr(COAT_BASE, 0.0, COAT_ROUGH),
            "extensions": {"KHR_materials_coat": {
                "coatFactor": 0.5, "coatRoughnessFactor": 0.06,
                "coatDarkeningFactor": t}}}


def m_coat_ior(t):
    """coatIor 1.0 -> 2.0. f0 runs 0 -> 0.111, so the coat's reflection
    strengthens across the row. 1.5 (column 3.5) is the clearcoat constant."""
    return {"pbrMetallicRoughness": pbr(COAT_BASE, 0.0, COAT_ROUGH),
            "extensions": {"KHR_materials_coat": {
                "coatFactor": 1.0, "coatRoughnessFactor": 0.06,
                "coatDarkeningFactor": 0.0,
                "coatIor": 1.0 + t}}}


def m_coat_anisotropy(t):
    """coatAnisotropyStrength 0 -> 1 on a smoother coat, where the highlight is
    tight enough for the stretch to be visible. Direct light only, like the base
    material's anisotropy."""
    return {"pbrMetallicRoughness": pbr(COAT_BASE, 0.0, COAT_ROUGH),
            "extensions": {"KHR_materials_coat": {
                "coatFactor": 1.0, "coatRoughnessFactor": 0.15,
                "coatDarkeningFactor": 0.0,
                "coatAnisotropyStrength": t, "coatAnisotropyRotation": 0.0}}}


def m_coat_normal(t):
    """coatFactor 0 -> 1 with a rippled coatNormalTexture on the COAT only.

    The base is smooth, so any ripple visible in the highlight belongs to the
    coat's own normal — which is the whole point of the property. At factor 0 the
    lobe is off and the sphere is smooth; the ripple should appear and strengthen
    across the row."""
    return {"pbrMetallicRoughness": pbr(COAT_BASE, 0.0, COAT_ROUGH),
            "extensions": {"KHR_materials_coat": {
                "coatFactor": t, "coatRoughnessFactor": 0.10,
                "coatDarkeningFactor": 0.0,
                "coatNormalTexture": {"index": 1}}}}


COAT_ROWS = [
    ("clearcoat_ref", "CONTROL: KHR_materials_clearcoat, factor 0 → 1", m_coat_clearcoat_ref),
    ("coat_factor",   "coatFactor 0 → 1 (must match row 0)",            m_coat_factor),
    ("coat_color",    "coatColorFactor white → amber, coat 0.5",        m_coat_color),
    ("coat_darkening", "coatDarkeningFactor 0 → 1, coat 0.5",           m_coat_darkening),
    ("coat_ior",      "coatIor 1.0 → 2.0 (f0 0 → 0.111)",               m_coat_ior),
    ("coat_aniso",    "coatAnisotropyStrength 0 → 1, coatRough 0.15",   m_coat_anisotropy),
    ("coat_normal",   "coatNormalTexture ripple, coatFactor 0 → 1",     m_coat_normal),
]

COAT_EXTENSIONS_USED = ["KHR_materials_clearcoat", "KHR_materials_coat"]


# ── KHR_materials_fuzz + KHR_materials_diffuse_roughness sweep (issue #84) ───
# A third asset, for the same reason coat_test.glb is a second one: Khronos
# publishes no conformance asset for either extension, and material_grid.glb is
# the frozen regression baseline.
#
# The fuzz rows use a dark navy fabric base — the same one the grid's sheen row
# uses — because fuzz is a fabric model and because row 0 is a sheen control
# that has to be comparable. The diffuse-roughness rows use a matte neutral: the
# effect is a change in how the diffuse lobe falls off, so anything glossy would
# bury it under a specular highlight.
FUZZ_BASE = (0.14, 0.20, 0.42)
DIFF_BASE = (0.62, 0.60, 0.58)


def m_sheen_ref(t):
    """CONTROL: the extension fuzz replaces, colour swept toward black.

    This row is the argument for the extension. sheenColorFactor IS sheen's
    intensity, so sweeping it to black does not make the fabric sooty — it
    fades the layer out and leaves the bare base. Row 2 is the same sweep in
    fuzz, which does darken."""
    g = 1.0 - t
    return {"pbrMetallicRoughness": pbr(FUZZ_BASE, 0.0, 0.85),
            "extensions": {"KHR_materials_sheen": {
                "sheenColorFactor": [g, g, g], "sheenRoughnessFactor": 0.7}}}


def m_fuzz_factor(t):
    """fuzzFactor 0 -> 1 with white fuzz: the layer fading in."""
    return {"pbrMetallicRoughness": pbr(FUZZ_BASE, 0.0, 0.85),
            "extensions": {"KHR_materials_fuzz": {
                "fuzzFactor": t, "fuzzColorFactor": [1.0, 1.0, 1.0],
                "fuzzRoughnessFactor": 0.7}}}


def m_fuzz_black(t):
    """fuzzColorFactor white -> black at full weight. THE headline case: black
    soot. Under sheen this sweep fades the layer away (row 0); under fuzz the
    weight is separate from the colour, so it darkens instead."""
    g = 1.0 - t
    return {"pbrMetallicRoughness": pbr(FUZZ_BASE, 0.0, 0.85),
            "extensions": {"KHR_materials_fuzz": {
                "fuzzFactor": 1.0, "fuzzColorFactor": [g, g, g],
                "fuzzRoughnessFactor": 0.7}}}


def m_fuzz_rough(t):
    """fuzzRoughnessFactor 0.05 -> 1: tight fibre-like reflection at the low end,
    broad dust-like scattering at the high end."""
    return {"pbrMetallicRoughness": pbr(FUZZ_BASE, 0.0, 0.85),
            "extensions": {"KHR_materials_fuzz": {
                "fuzzFactor": 1.0, "fuzzColorFactor": [1.0, 1.0, 1.0],
                "fuzzRoughnessFactor": max(0.05, t)}}}


def m_diffuse_rough(t):
    """diffuseRoughnessFactor 0 -> 1 on a matte neutral. Oren-Nayar
    back-scattering flattens the falloff and lifts the terminator."""
    return {"pbrMetallicRoughness": pbr(DIFF_BASE, 0.0, 0.95),
            "extensions": {"KHR_materials_diffuse_roughness": {
                "diffuseRoughnessFactor": t}}}


def m_diffuse_rough_spec(t):
    """The same sweep on a SEMI-GLOSS base. diffuse_roughness must not touch the
    specular lobe — if this row tracked row 4 exactly the two roughnesses would
    be coupled, which is the one thing the extension exists to separate."""
    return {"pbrMetallicRoughness": pbr(DIFF_BASE, 0.0, 0.35),
            "extensions": {"KHR_materials_diffuse_roughness": {
                "diffuseRoughnessFactor": t}}}


FUZZ_ROWS = [
    ("sheen_ref",     "CONTROL: sheenColorFactor white → black",     m_sheen_ref),
    ("fuzz_factor",   "fuzzFactor 0 → 1, white fuzz",                m_fuzz_factor),
    ("fuzz_black",    "fuzzColorFactor white → BLACK at weight 1",   m_fuzz_black),
    ("fuzz_rough",    "fuzzRoughnessFactor 0.05 → 1",                m_fuzz_rough),
    ("diffuse_rough", "diffuseRoughnessFactor 0 → 1, matte base",    m_diffuse_rough),
    ("diffuse_gloss", "diffuseRoughnessFactor 0 → 1, gloss base",    m_diffuse_rough_spec),
]

FUZZ_EXTENSIONS_USED = ["KHR_materials_sheen", "KHR_materials_fuzz",
                        "KHR_materials_diffuse_roughness"]

# Extensions this scene references. extensionsUsed ONLY — putting any of these
# in extensionsRequired would make a conformant loader that lacks them refuse
# the file, which defeats the point of it being a degradation test.
EXTENSIONS_USED = [
    "KHR_materials_clearcoat",
    "KHR_materials_sheen",
    "KHR_materials_anisotropy",
    "KHR_materials_iridescence",
    "KHR_materials_specular",
    "KHR_materials_ior",
    "KHR_materials_transmission",
    "KHR_materials_volume",
    "KHR_materials_emissive_strength",
]


def build_glb(row_specs=None, textured=None, extensions=None, scene_name="material_grid",
              extra_png=None, cols=None, generator=None):
    """Build a sweep grid: one row per (name, doc, make) spec, STEPS columns wide.

    Parameterised so a second sweep can be emitted without cloning the builder —
    coat_test.glb is exactly this with different rows and no textured row. The
    defaults reproduce material_grid.glb byte-for-byte, which matters: that asset
    is the regression baseline every "did this change the render" measurement is
    taken against, so it must not move when a new extension is added.
    """
    row_specs = ROWS if row_specs is None else row_specs
    textured = TEXTURED if textured is None else textured
    extensions = EXTENSIONS_USED if extensions is None else extensions
    # Column count is per-asset. It defaults to STEPS so material_grid.glb —
    # the frozen regression baseline — is still emitted byte-for-byte.
    cols = STEPS if cols is None else cols
    # asset.generator names whichever script actually emitted the file; the
    # Khronos conformance assets are built by a different one.
    generator = ("DisplayXR model-viewer material grid "
                 "(scripts/make_material_grid.py, issue #70)"
                 if generator is None else generator)
    pos, nrm, uv, tan, idx = uv_sphere(RADIUS, LON, LAT)

    pos_b = b"".join(struct.pack("<3f", *p) for p in pos)
    nrm_b = b"".join(struct.pack("<3f", *n) for n in nrm)
    uv_b = b"".join(struct.pack("<2f", *t) for t in uv)
    tan_b = b"".join(struct.pack("<4f", *t) for t in tan)
    idx_b = b"".join(struct.pack("<H", i) for i in idx)
    assert len(idx_b) % 4 == 0, "index block must stay 4-byte aligned"
    png = gradient_png()
    png_pad = b"\x00" * ((4 - len(png) % 4) % 4)
    blob = pos_b + nrm_b + uv_b + tan_b + idx_b + png + png_pad
    # A SECOND image, only when a caller asks for one (coat_test's coat-normal
    # map). Appended after the existing padding so every byte before it — and
    # therefore material_grid.glb in its entirety — is untouched.
    extra_pad = b""
    if extra_png is not None:
        extra_pad = b"\x00" * ((4 - len(extra_png) % 4) % 4)
        blob = blob + extra_png + extra_pad

    pmin = [min(p[k] for p in pos) for k in range(3)]
    pmax = [max(p[k] for p in pos) for k in range(3)]

    off = 0
    views = []
    for data, target in ((pos_b, 34962), (nrm_b, 34962), (uv_b, 34962),
                         (tan_b, 34962), (idx_b, 34963)):
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data), "target": target})
        off += len(data)

    png_view = len(views)
    views.append({"buffer": 0, "byteOffset": off, "byteLength": len(png)})
    extra_view = None
    if extra_png is not None:
        extra_off = off + len(png) + len(png_pad)
        extra_view = len(views)
        views.append({"buffer": 0, "byteOffset": extra_off, "byteLength": len(extra_png)})

    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
         "min": pmin, "max": pmax},
        {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uv), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5126, "count": len(tan), "type": "VEC4"},
        {"bufferView": 4, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
    ]

    materials, meshes, nodes, layout = [], [], [], []
    rows = len(row_specs) + (1 if textured else 0)
    for r, (name, doc, make) in enumerate(row_specs):
        for c in range(cols):
            t = c / (cols - 1)
            mat = make(t)
            mat["name"] = f"{r:02d}_{name}_{t:.2f}"
            mi = len(materials)
            materials.append(mat)
            meshes.append({"name": mat["name"], "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 3},
                "indices": 4, "material": mi}]})
            # Grid centred on the origin; row 0 on top, sweep running left→right.
            x = (c - (cols - 1) / 2.0) * SPACING
            y = ((rows - 1) / 2.0 - r) * SPACING
            nodes.append({"name": mat["name"], "mesh": mi, "translation": [x, y, 0.0]})
        layout.append(f"  row {r}  {name:<13} {doc}")

    # Last row (when present): texture-driven variants, one property per column.
    r = len(row_specs)
    for c, (tname, mat) in enumerate(textured or []):
        mat = dict(mat)
        mat["name"] = f"{r:02d}_textured_{tname}"
        mi = len(materials)
        materials.append(mat)
        meshes.append({"name": mat["name"], "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 3},
            "indices": 4, "material": mi}]})
        x = (c - (cols - 1) / 2.0) * SPACING
        y = ((rows - 1) / 2.0 - r) * SPACING
        nodes.append({"name": mat["name"], "mesh": mi, "translation": [x, y, 0.0]})
    if textured:
        layout.append(f"  row {r}  {'textured':<13} one texture-driven property per column")

    gltf = {
        "asset": {"version": "2.0",
                  "generator": generator},
        "extensionsUsed": extensions,
        "scene": 0,
        "scenes": [{"name": scene_name, "nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
        "images": ([{"bufferView": png_view, "mimeType": "image/png"}]
                   + ([{"bufferView": extra_view, "mimeType": "image/png"}]
                      if extra_view is not None else [])),
        "samplers": [{"magFilter": 9729, "minFilter": 9729,
                      "wrapS": 33071, "wrapT": 33071}],
        "textures": ([{"source": 0, "sampler": 0}]
                     + ([{"source": 1, "sampler": 0}] if extra_view is not None else [])),
    }

    json_b = json.dumps(gltf, separators=(",", ":")).encode()
    json_b += b" " * ((4 - len(json_b) % 4) % 4)          # pad with spaces
    blob += b"\x00" * ((4 - len(blob) % 4) % 4)           # pad with zeros

    total = 12 + 8 + len(json_b) + 8 + len(blob)
    out = struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(json_b), 0x4E4F534A) + json_b
    out += struct.pack("<II", len(blob), 0x004E4942) + blob
    return out, len(materials), rows, cols, layout


# ── Transmission test scene ─────────────────────────────────────────────────
# A uniform sky is the worst possible backdrop for verifying refraction: a glass
# ball that samples the RIGHT region and one that samples the WRONG region both
# come out sky-coloured, and one that is not drawn at all looks the same again.
# (Windows found exactly this ambiguity.)
#
# So this scene puts a strong VERTICAL COLOUR GRADIENT behind the spheres — red
# at the top, blue at the bottom — so that "refracts correctly", "samples the
# wrong region" and "was never drawn" produce three visibly different pictures
# instead of three identical sky-coloured ones.
#
# EXPECTED RESULT: the sphere interiors are UPRIGHT and MAGNIFIED. Not inverted.
#
# This was originally built around "a thick glass ball is a lens, and a lens
# inverts", and that premise was wrong for this renderer. Measured on macOS,
# Linux and Windows independently: upright wins decisively on all three, and the
# magnification factor grows monotonically with transmissionFactor (~0.45 at
# transmission 0 to ~1.45 at transmission 1).
#
# The reason is structural, not a bug. We sample the scene at the refracted ray's
# EXIT POINT projected to screen: refract once at the entry surface, walk
# thicknessFactor along that ray, project. The refracted ray is dominated by its
# into-the-screen component, so projecting it pulls the sample toward the
# vanishing point — magnification — while the small transverse bend merely shifts
# it. A real ball inverts because rays CROSS inside it: two refractions plus
# propagation. A single-sample screen-space displacement has one refraction, no
# exit interface and no crossing, so it can distort and magnify but never flip.
#
# Physical optics says a ball of this radius and IOR, with the backdrop well
# outside its ~0.68-unit focal length, must invert. That is true of glass and
# not of this shader. Do not "fix" the upright result — it is the approximation
# behaving as specified.
#
# Kept as its own asset rather than added to the grid: a full-frame backdrop
# would make every pixel foreground and break the grid probe's contrast-based
# tile/row detection.

def gradient_backdrop_png():
    """Vertical red -> blue ramp. Deliberately saturated: the discriminator is
    the SIGN of (R-B), so the further from neutral, the harder it is to fake."""
    w, h = 8, 64
    px = bytearray()
    for y in range(h):
        t = y / (h - 1)
        r = int(round(255 * (1.0 - t)))
        b = int(round(255 * t))
        px += bytes((r, 24, b, 255)) * w
    raw = b"".join(b"\x00" + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def build_transmission_test():
    pos, nrm, uv, tan, idx = uv_sphere(RADIUS, LON, LAT)
    nsv = len(pos)

    # Backdrop quad, behind the spheres and wide enough to fill the frame.
    BW, BH, BZ = 12.0, 7.0, -2.5
    qpos = [(-BW / 2, BH / 2, BZ), (BW / 2, BH / 2, BZ),
            (-BW / 2, -BH / 2, BZ), (BW / 2, -BH / 2, BZ)]
    qnrm = [(0.0, 0.0, 1.0)] * 4
    quv = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
    qtan = [(1.0, 0.0, 0.0, 1.0)] * 4
    qidx = [0, 2, 1, 1, 2, 3]

    # Sphere and backdrop get SEPARATE position/normal/uv/tangent accessors.
    # Sharing one position accessor is legal — an accessor's min/max must cover
    # all of its own data — but it makes every primitive's bounds resolve to the
    # merged box (a 0.9-unit sphere reporting 12 units wide), which misleads
    # anything deriving per-primitive bounds: culling, and this viewer's auto-fit.
    pos_b = b"".join(struct.pack("<3f", *p) for p in pos)
    nrm_b = b"".join(struct.pack("<3f", *n) for n in nrm)
    uv_b = b"".join(struct.pack("<2f", *t) for t in uv)
    tan_b = b"".join(struct.pack("<4f", *t) for t in tan)
    qpos_b = b"".join(struct.pack("<3f", *p) for p in qpos)
    qnrm_b = b"".join(struct.pack("<3f", *n) for n in qnrm)
    quv_b = b"".join(struct.pack("<2f", *t) for t in quv)
    qtan_b = b"".join(struct.pack("<4f", *t) for t in qtan)
    idx_b = b"".join(struct.pack("<H", i) for i in idx)
    qidx_b = b"".join(struct.pack("<H", i) for i in qidx)   # own accessor -> no +nsv
    png = gradient_backdrop_png()
    pad = lambda d: d + b"\x00" * ((4 - len(d) % 4) % 4)
    blob = (pos_b + nrm_b + uv_b + tan_b + qpos_b + qnrm_b + quv_b + qtan_b
            + pad(idx_b) + pad(qidx_b) + png)

    off, views = 0, []
    for data, target in ((pos_b, 34962), (nrm_b, 34962), (uv_b, 34962), (tan_b, 34962),
                         (qpos_b, 34962), (qnrm_b, 34962), (quv_b, 34962), (qtan_b, 34962),
                         (pad(idx_b), 34963), (pad(qidx_b), 34963), (png, None)):
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        views.append(v)
        off += len(data)

    accessors = [
        # 0-3 sphere: bounds are the SPHERE's, not the merged scene's.
        {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
         "min": [min(p[k] for p in pos) for k in range(3)],
         "max": [max(p[k] for p in pos) for k in range(3)]},
        {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uv), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5126, "count": len(tan), "type": "VEC4"},
        # 4-7 backdrop quad.
        {"bufferView": 4, "componentType": 5126, "count": len(qpos), "type": "VEC3",
         "min": [min(p[k] for p in qpos) for k in range(3)],
         "max": [max(p[k] for p in qpos) for k in range(3)]},
        {"bufferView": 5, "componentType": 5126, "count": len(qnrm), "type": "VEC3"},
        {"bufferView": 6, "componentType": 5126, "count": len(quv), "type": "VEC2"},
        {"bufferView": 7, "componentType": 5126, "count": len(qtan), "type": "VEC4"},
        {"bufferView": 8, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        {"bufferView": 9, "componentType": 5123, "count": len(qidx), "type": "SCALAR"},
    ]

    materials, meshes, nodes = [], [], []
    # Backdrop: rough, non-metal, so it reads as its texture rather than a mirror.
    materials.append({"name": "backdrop",
                      "pbrMetallicRoughness": {
                          "baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0,
                          "roughnessFactor": 0.9,
                          "baseColorTexture": {"index": 0}}})
    meshes.append({"name": "backdrop", "primitives": [{
        "attributes": {"POSITION": 4, "NORMAL": 5, "TEXCOORD_0": 6, "TANGENT": 7},
        "indices": 9, "material": 0}]})
    nodes.append({"name": "backdrop", "mesh": 0, "translation": [0.0, 0.0, 0.0]})

    for c in range(STEPS):
        t = c / (STEPS - 1)
        mat = m_transmission(t)
        mat["name"] = f"glass_{t:.2f}"
        # Thicker than the grid's so the lens inversion is unambiguous.
        mat["extensions"]["KHR_materials_volume"]["thicknessFactor"] = 1.2
        mi = len(materials)
        materials.append(mat)
        meshes.append({"name": mat["name"], "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 3},
            "indices": 8, "material": mi}]})
        nodes.append({"name": mat["name"], "mesh": mi,
                      "translation": [(c - (STEPS - 1) / 2.0) * 1.15, 0.0, 0.0]})

    gltf = {
        "asset": {"version": "2.0",
                  "generator": "DisplayXR transmission test (issue #70)"},
        "extensionsUsed": ["KHR_materials_transmission", "KHR_materials_ior",
                           "KHR_materials_volume"],
        "scene": 0,
        "scenes": [{"name": "transmission_test", "nodes": list(range(len(nodes)))}],
        "nodes": nodes, "meshes": meshes, "materials": materials,
        "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
        "images": [{"bufferView": 10, "mimeType": "image/png"}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}],
        "textures": [{"source": 0, "sampler": 0}],
    }
    jb = json.dumps(gltf, separators=(",", ":")).encode()
    jb += b" " * ((4 - len(jb) % 4) % 4)
    blob += b"\x00" * ((4 - len(blob) % 4) % 4)
    total = 12 + 8 + len(jb) + 8 + len(blob)
    out = struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(jb), 0x4E4F534A) + jb
    out += struct.pack("<II", len(blob), 0x004E4942) + blob
    return out


def main():
    dest = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(__file__).resolve().parent.parent / "assets" / "material_grid.glb"
    glb, nmat, rows, cols, layout = build_glb()
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(glb)
    print(f"wrote {dest} ({len(glb):,} bytes)")
    print(f"{rows} families x {cols} steps = {nmat} materials, sweep runs left to right:")
    print("\n".join(layout))

    cdest = dest.parent / "coat_test.glb"
    cglb, cnmat, crows, ccols, clayout = build_glb(
        row_specs=COAT_ROWS, textured=[], extensions=COAT_EXTENSIONS_USED,
        scene_name="coat_test", extra_png=ripple_normal_png())
    cdest.write_bytes(cglb)
    print(f"wrote {cdest} ({len(cglb):,} bytes)")
    print(f"{crows} families x {ccols} steps = {cnmat} materials (KHR_materials_coat, #81):")
    print("\n".join(clayout))
    print("  rows 0 and 1 are the same material in both spellings. Measure with")
    print("  scripts/probe_coat_test.py; note the two rows sit at different heights")
    print("  and so see different sky, which bounds how equal they can read.")

    fdest = dest.parent / "diffuse_fuzz_test.glb"
    fglb, fnmat, frows, fcols, flayout = build_glb(
        row_specs=FUZZ_ROWS, textured=[], extensions=FUZZ_EXTENSIONS_USED,
        scene_name="diffuse_fuzz_test")
    fdest.write_bytes(fglb)
    print(f"wrote {fdest} ({len(fglb):,} bytes)")
    print(f"{frows} families x {fcols} steps = {fnmat} materials (fuzz + diffuse roughness, #84):")
    print("\n".join(flayout))
    print("  rows 0 and 2 are the same colour sweep under sheen and under fuzz —")
    print("  sheen fades out, fuzz goes sooty. Check with scripts/probe_fuzz_test.py.")

    tdest = dest.parent / "transmission_test.glb"
    tdest.write_bytes(build_transmission_test())
    print(f"wrote {tdest} ({tdest.stat().st_size:,} bytes)")
    print("  7 glass spheres, transmissionFactor 0 -> 1, over a red(top)->blue(bottom)")
    print("  backdrop. Expect the interior to be UPRIGHT and MAGNIFIED, not inverted —")
    print("  see the docstring: screen-space refraction cannot flip an image.")


if __name__ == "__main__":
    main()
