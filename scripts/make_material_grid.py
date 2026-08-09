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


def build_glb():
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

    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
         "min": pmin, "max": pmax},
        {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uv), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5126, "count": len(tan), "type": "VEC4"},
        {"bufferView": 4, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
    ]

    materials, meshes, nodes, layout = [], [], [], []
    rows, cols = len(ROWS) + 1, STEPS   # +1 for the textured row
    for r, (name, doc, make) in enumerate(ROWS):
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

    # Row 9: texture-driven variants, one property per column.
    r = len(ROWS)
    for c, (tname, mat) in enumerate(TEXTURED):
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
    layout.append(f"  row {r}  {'textured':<13} one texture-driven property per column")

    gltf = {
        "asset": {"version": "2.0",
                  "generator": "DisplayXR model-viewer material grid "
                               "(scripts/make_material_grid.py, issue #70)"},
        "extensionsUsed": EXTENSIONS_USED,
        "scene": 0,
        "scenes": [{"name": "material_grid", "nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
        "images": [{"bufferView": png_view, "mimeType": "image/png"}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729,
                      "wrapS": 33071, "wrapT": 33071}],
        "textures": [{"source": 0, "sampler": 0}],
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

    tdest = dest.parent / "transmission_test.glb"
    tdest.write_bytes(build_transmission_test())
    print(f"wrote {tdest} ({tdest.stat().st_size:,} bytes)")
    print("  7 glass spheres, transmissionFactor 0 -> 1, over a red(top)->blue(bottom)")
    print("  backdrop. Expect the interior to be UPRIGHT and MAGNIFIED, not inverted —")
    print("  see the docstring: screen-space refraction cannot flip an image.")


if __name__ == "__main__":
    main()
