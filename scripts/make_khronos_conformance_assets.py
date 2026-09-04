#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Emit Khronos-shaped conformance assets for the OpenPBR-alignment extensions.

Target: https://github.com/KhronosGroup/glTF-Sample-Assets/ - @MiiBond confirmed
on the KHR_materials_coat PR thread that no conformance assets exist for coat,
fuzz or diffuse_roughness, and that CONFORMANCE assets (verify an implementation
matches expected behaviour) are wanted ahead of DEMO assets (eye candy).

WHY THIS IS A SEPARATE SCRIPT FROM make_material_grid.py.

Our in-tree assets and an upstream conformance asset have different jobs:

  * assets/material_grid.glb is a FROZEN REGRESSION BASELINE. Every "did this
    change the render" measurement in this repo is a diff against a stored
    capture of it, so its bytes must not move.
  * assets/coat_test.glb documents a KNOWN CONFOUND: its clearcoat control row
    and its coat row sit at different heights, under a vertical sky gradient, so
    two identical materials genuinely do not render identically. The docstring in
    scripts/probe_coat_test.py says so and points the exact equality test
    elsewhere. That is honest for an internal probe and WRONG for a conformance
    asset, where the whole claim is "these two must match".

So the upstream coat asset pairs the control ADJACENT IN THE SAME ROW - same
height, same environment, same everything but the spelling of the extension.
That is the one substantive change from the in-tree version; the sweeps are
otherwise the same code, re-emitted at eight columns instead of seven.

Reproducibility is the point: these are generated, not modelled, so regenerating
them against a spec change costs one command.

Usage:
    python scripts/make_khronos_conformance_assets.py [outdir]

Writes <outdir>/Models/<Name>/ with both required variants (glTF/ and
glTF-Binary/), metadata.json and README.body.md.
Default outdir is ./khronos_submission.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from khronos_gltf_pack import (            # noqa: E402
    parse_glb, prune, write_glb, write_gltf_variant,
)
from make_material_grid import (           # noqa: E402
    build_glb, ripple_normal_png,
    m_coat_clearcoat_ref, m_coat_factor, m_coat_color, m_coat_darkening,
    m_coat_ior, m_coat_anisotropy, m_coat_normal,
    m_sheen_ref, m_fuzz_factor, m_fuzz_black, m_fuzz_rough,
    pbr,
)

# Eight, not seven, so the paired control row divides evenly into four pairs.
COLS = 8
PAIR_VALUES = [0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0]

# The upstream tooling renders credits from these fields directly -- omit
# licenseUrl/text and the generated LICENSE.md reads "[undefined]()". spdx and
# icon are what the REUSE compliance job and the listing tables consume.
LEGAL = [{
    "license": "CC0-1.0",
    "licenseUrl": "https://creativecommons.org/publicdomain/zero/1.0/legalcode",
    "text": "Creative Commons Zero v1.0 Universal",
    "spdx": "CC0-1.0",
    "icon": "https://licensebuttons.net/p/zero/1.0/88x31.png",
    "year": "2026",
    "artist": "DisplayXR",
    "what": "Entire Model",
    "owner": "DisplayXR",
}]


def paired_clearcoat_coat(t):
    """CONTROL: one material in clearcoat's spelling and in coat's, side by side.

    Even columns carry KHR_materials_clearcoat, odd columns the KHR_materials_coat
    material the conversion table in the coat spec says it maps to. A pair shares a
    row, so it shares a height and therefore an environment - which is what makes
    "these must render identically" a testable claim rather than an approximate one.

    The column index is recovered from the sweep parameter because the builder
    hands rows a normalised t; with COLS columns, t = c / (COLS - 1) exactly.
    """
    c = int(round(t * (COLS - 1)))
    v = PAIR_VALUES[c // 2]
    return m_coat_clearcoat_ref(v) if c % 2 == 0 else m_coat_factor(v)


COAT_ROWS_UPSTREAM = [
    ("mapping_control", "CONTROL: clearcoat|coat pairs, equal height, must match", paired_clearcoat_coat),
    ("coat_factor",     "coatFactor 0 -> 1",                                       m_coat_factor),
    ("coat_color",      "coatColorFactor white -> amber, coat 0.5",                m_coat_color),
    ("coat_darkening",  "coatDarkeningFactor 0 -> 1, coat 0.5",                    m_coat_darkening),
    ("coat_ior",        "coatIor 1.0 -> 2.0 (f0 0 -> 0.111)",                      m_coat_ior),
    ("coat_aniso",      "coatAnisotropyStrength 0 -> 1, coatRough 0.15",           m_coat_anisotropy),
    ("coat_normal",     "coatNormalTexture ripple, coatFactor 0 -> 1",             m_coat_normal),
]

FUZZ_ROWS_UPSTREAM = [
    ("sheen_ref",   "REFERENCE: KHR_materials_sheen, sheenColorFactor white -> black", m_sheen_ref),
    ("fuzz_factor", "fuzzFactor 0 -> 1, white fuzz",                                   m_fuzz_factor),
    ("fuzz_black",  "fuzzColorFactor white -> BLACK at weight 1",                      m_fuzz_black),
    ("fuzz_rough",  "fuzzRoughnessFactor 0.05 -> 1",                                   m_fuzz_rough),
]


# ---- KHR_materials_diffuse_roughness ---------------------------------------
# The in-tree rows sweep a light neutral (DIFF_BASE) and read almost flat. Two
# reasons, both fixed here rather than upstream, because the in-tree asset is a
# frozen baseline.
#
# 1. A near-white base under a bright sky sits at the top of the range, so a
#    real radiometric change has nowhere to go perceptually. This uses a
#    mid-tone clay instead. NOT a dark one: the Oren-Nayar interreflection term
#    scales with albedo, so going dark would cut the very signal being shown.
# 2. A gradient across eight spheres is the wrong way to present an effect this
#    small. Row 0 pairs "extension absent" against "roughness 1.0" as touching
#    neighbours, so the whole delta lands in one A/B comparison instead of being
#    spread thin. Same device as the coat asset's mapping control.
DIFF_UPSTREAM_BASE = (0.45, 0.28, 0.21)

# Four albedos rather than four copies of one: the effect scales with albedo, so
# the row shows that dependence instead of repeating itself four times.
DIFF_PAIR_ALBEDOS = [
    (0.12, 0.10, 0.09),
    (0.28, 0.20, 0.16),
    (0.45, 0.28, 0.21),
    (0.72, 0.60, 0.52),
]


def diffuse_albedo_control(t):
    """CONTROL: extension ABSENT vs roughness 1.0, adjacent, at four albedos.

    Even columns carry no extension at all, so they are plain glTF 2.0 Lambertian
    diffuse; odd columns are the same material with diffuseRoughnessFactor at 1.0.
    A pair shares a row, so it shares a height and an environment, and the only
    difference between the two spheres is the extension itself.
    """
    c = int(round(t * (COLS - 1)))
    albedo = DIFF_PAIR_ALBEDOS[c // 2]
    mat = {"pbrMetallicRoughness": pbr(albedo, 0.0, 0.95)}
    if c % 2 == 1:
        mat["extensions"] = {"KHR_materials_diffuse_roughness": {
            "diffuseRoughnessFactor": 1.0}}
    return mat


def diffuse_matte(t):
    """diffuseRoughnessFactor 0 -> 1 on a matte mid-tone base."""
    return {"pbrMetallicRoughness": pbr(DIFF_UPSTREAM_BASE, 0.0, 0.95),
            "extensions": {"KHR_materials_diffuse_roughness": {
                "diffuseRoughnessFactor": t}}}


def diffuse_gloss(t):
    """The same sweep on a SEMI-GLOSS base. diffuse_roughness must not touch the
    specular lobe -- if this row tracked row 1 exactly, the two roughnesses would
    be coupled, which is the one thing the extension exists to separate."""
    return {"pbrMetallicRoughness": pbr(DIFF_UPSTREAM_BASE, 0.0, 0.35),
            "extensions": {"KHR_materials_diffuse_roughness": {
                "diffuseRoughnessFactor": t}}}


DIFFUSE_ROWS_UPSTREAM = [
    ("albedo_control", "CONTROL: absent|roughness 1.0 pairs at four albedos", diffuse_albedo_control),
    ("diffuse_matte",  "diffuseRoughnessFactor 0 -> 1, matte base",           diffuse_matte),
    ("diffuse_gloss",  "diffuseRoughnessFactor 0 -> 1, semi-gloss base",      diffuse_gloss),
]


COAT_DESC = """\
A parameter sweep for `KHR_materials_coat`. Seven rows of eight spheres over a
common base -- a neutral light-grey dielectric, `baseColorFactor` (0.78, 0.78,
0.80) at roughness 0.40 -- where each row varies exactly one coat property from
left to right, so a difference between two implementations localises to a single
property rather than to "the coat looks wrong".

## Row 0 is a conformance control, and it is the point of the asset

The coat specification states that the `KHR_materials_clearcoat` parameters are
"fully transferable to this extension, with no changes". Row 0 tests that claim
directly: **even columns carry `KHR_materials_clearcoat`, odd columns carry the
`KHR_materials_coat` material it is said to map to**, at four sweep values.

Each pair shares a row, and therefore a height and an environment. This matters
more than it looks. An earlier version of this asset placed the clearcoat control
and the coat sweep in *separate rows*; under any vertically varying environment
(which is to say, any realistic IBL) two identical mirror-ish materials at
different heights do not render identically, and the comparison silently stops
being a test. Pairs at equal height make "these must match" exact.

A conforming implementation should render each pair as two indistinguishable
spheres. Any visible difference within a pair is a defect in the clearcoat-to-coat
mapping.

One caveat, and it is the reason the asset pins the value rather than relying on
a default. The coat columns set `coatDarkeningFactor` to `0.0` explicitly, because
`KHR_materials_clearcoat` never modelled internal-reflection darkening and the
pair could not otherwise match. Were the extension's default of `1.0` to apply
here, the coat halves would render darker than their clearcoat partners and the
control would fail for a reason that has nothing to do with the mapping. Row 3
sweeps darkening on purpose, in isolation.

## The remaining rows

| Row | Property swept |
|---|---|
| 1 | `coatFactor`, 0 to 1 |
| 2 | `coatColorFactor`, white to amber, at `coatFactor` 0.5 |
| 3 | `coatDarkeningFactor`, 0 to 1, at `coatFactor` 0.5 |
| 4 | `coatIor`, 1.0 to 2.0 |
| 5 | `coatAnisotropyStrength`, 0 to 1, coat roughness 0.15 |
| 6 | `coatNormalTexture`, a ripple normal map, `coatFactor` 0 to 1 |

Two of these are deliberately subtle, for physical reasons rather than authoring
ones. **Darkening** is a one-bounce round trip with a reflectance around 0.04, so
the spec's own model yields only a few per cent across the row. **Anisotropy** on
the coat lobe is a direct-light effect, so its whole-sphere contribution under a
dominant environment light is small. Neither row should be expected to swing
hard, and an implementation is not wrong for rendering them gently.

The base material is deliberately smooth so that any ripple visible in row 6
belongs to the coat's own normal rather than to the base.

## A note on glTF-Validator output

This asset validates with no errors and no warnings, but it does report
`UNUSED_MESH_TANGENT` and `UNUSED_OBJECT` (for `TEXCOORD_0`) against every
primitive. Those are false positives and the attributes should not be stripped:
the validator does not yet support `KHR_materials_coat`, so it cannot see that
row 6 samples `coatNormalTexture`, and the extension additionally requires a
tangent space for row 5 -- "A mesh primitive using coat anisotropy **MUST** have
a defined tangent space". The attributes are carried uniformly across all rows so
that every sphere in the asset is the same geometry.
"""

FUZZ_DESC = """\
A parameter sweep for `KHR_materials_fuzz`. Four rows of eight spheres over a
dark navy fabric base, chosen because fuzz is a fabric model and because a dark
base is where a fuzz lobe is most legible.

## Row 0 is a reference, not an equality control

Row 0 carries `KHR_materials_sheen`, the extension fuzz is intended to supersede,
sweeping `sheenColorFactor` from white to black. Row 2 sweeps `fuzzColorFactor`
across the same range.

These two rows are **expected to differ**, and that difference is the reason the
extension exists: a black sheen colour disables the sheen layer entirely, so row 0
fades out, while fuzz keeps `fuzzFactor` as an independent weight, so row 2 goes
sooty instead of vanishing. An implementation that renders rows 0 and 2 alike has
almost certainly routed fuzz through its sheen path.

Unlike the paired control in the coat asset, this is a qualitative claim rather
than a pixel-equality one, so the two rows are not height-matched.

## Rows

| Row | Property swept |
|---|---|
| 0 | `KHR_materials_sheen` reference: `sheenColorFactor` white to black |
| 1 | `fuzzFactor`, 0 to 1, white fuzz |
| 2 | `fuzzColorFactor`, white to black, at full weight |
| 3 | `fuzzRoughnessFactor`, 0.05 to 1 |

Row 3 is worth checking against the schema rather than the prose: at the time of
writing the README parameter table gives `fuzzRoughnessFactor` a default of `0.5`
and the schema gives `0.0`. This asset sets the value explicitly in every column,
so it renders identically either way and does not depend on the resolution.
"""

DIFFUSE_DESC = """\
A parameter sweep for `KHR_materials_diffuse_roughness`. Three rows of eight
spheres over a mid-tone clay base.

The extension replaces glTF's purely Lambertian diffuse with a view-dependent
one, flattening the falloff and lifting the terminator. It is a genuinely small
effect, and this asset is built around that fact rather than in spite of it.

## Row 0 is the control, and it is how to read this asset

A gradient across eight spheres is the wrong way to present a change of a few
levels -- nobody can see it. So row 0 places the two endpoints **side by side**:
**even columns carry no extension at all** (plain glTF 2.0 Lambertian diffuse),
and **odd columns are the same material with `diffuseRoughnessFactor` at 1.0**.
Each pair shares a row, so it shares a height and an environment, and the only
difference within a pair is the extension itself.

The four pairs use four different base albedos rather than four copies of one,
because the effect is albedo-dependent and a single albedo would say nothing
about that. What a conforming implementation must show is a visible difference
*inside* every pair, in the same direction -- the rough sphere is lighter -- with
the absolute difference growing from the darkest pair to the lightest.

This also gives the cheapest conformance check in the asset, and the first one to
run: at `diffuseRoughnessFactor` 0 the material must be **indistinguishable**
from an even column. If it is not, the parameter is being applied where it
should not be.

## Rows

| Row | Content |
|---|---|
| 0 | control pairs: extension absent, then `diffuseRoughnessFactor` 1.0, at four albedos |
| 1 | matte base, `diffuseRoughnessFactor` 0 to 1 |
| 2 | semi-gloss base, `diffuseRoughnessFactor` 0 to 1 |

Row 2 exists because the realistic authoring case has a specular lobe present.
`diffuse_roughness` must not touch the specular lobe -- if row 2 tracked row 1
exactly, the two roughnesses would be coupled, which is the one thing this
extension exists to separate. An implementation that applies the parameter to
the wrong term tends to look plausible on row 1 and wrong on row 2.

## Expect a small effect, and check it by measurement

Measured on our own implementation, as mean luma (Rec. 709, 0-255) of a patch at
each sphere's centre:

| Row 0 pair (base albedo) | extension absent | roughness 1.0 | delta | relative |
|---|---|---|---|---|
| 0.12 | 82.0 | 85.5 | **+3.6** | +4.4% |
| 0.28 | 112.6 | 117.9 | **+5.3** | +4.7% |
| 0.45 | 132.3 | 139.2 | **+6.9** | +5.2% |
| 0.72 | 181.1 | 190.5 | **+9.4** | +5.2% |

| Sweep row | leftmost | rightmost | change |
|---|---|---|---|
| 1, matte | 131.3 | 136.7 | **+5.4** |
| 2, semi-gloss | 215.9 | 212.5 | **-3.4** |

These are specific to our environment, exposure and tone curve, so do not match
them directly. Three things should reproduce. Every pair in row 0 differs in the
same direction, the rough sphere being the lighter one. The absolute difference
grows with base albedo, while the *relative* difference stays near 5% across the
whole range. And row 2 moves in the opposite direction to row 1, because the
diffuse lobe is a smaller share of a glossier material's response.

Note that we implement Fujii's energy-preserving qualitative Oren-Nayar rather
than EON, which the specification permits and which we state plainly; the two
models weight the interreflection term differently, so an EON implementation may
reasonably show a steeper albedo trend than the one tabulated here.

## What this asset can and cannot test

The **normative** part of this extension is the direct-light diffuse BRDF. How
the parameter affects image-based lighting is explicitly left to the
implementation -- the specification offers several options and notes that the
cheapest is also the least correct. A viewer whose lighting is dominated by an
environment map is therefore exercising implementation-defined behaviour more
than specified behaviour, and two conforming renderers may legitimately differ
on these spheres by more than they differ on a direct-lit scene. This asset
cannot resolve that on its own; it is worth knowing before treating any single
number here as a pass or fail.
"""

MODELS = [
    ("CoatParameterSweep", "Coat Parameter Sweep",
     "Parameter sweep for KHR_materials_coat, with a height-matched "
     "clearcoat-to-coat mapping control.",
     COAT_ROWS_UPSTREAM, ["KHR_materials_clearcoat", "KHR_materials_coat"],
     COAT_DESC, True, ()),
    ("FuzzParameterSweep", "Fuzz Parameter Sweep",
     "Parameter sweep for KHR_materials_fuzz, with a KHR_materials_sheen "
     "reference row.",
     FUZZ_ROWS_UPSTREAM, ["KHR_materials_sheen", "KHR_materials_fuzz"],
     FUZZ_DESC, False, ("TEXCOORD_0", "TANGENT")),
    ("DiffuseRoughnessParameterSweep", "Diffuse Roughness Parameter Sweep",
     "Parameter sweep for KHR_materials_diffuse_roughness over a matte and a "
     "glossy base.",
     DIFFUSE_ROWS_UPSTREAM, ["KHR_materials_diffuse_roughness"],
     DIFFUSE_DESC, False, ("TEXCOORD_0", "TANGENT")),
]


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(__file__).resolve().parent.parent / "khronos_submission"

    for dirname, name, summary, rows, exts, desc, needs_ripple, drop_attrs in MODELS:
        mdir = out / "Models" / dirname
        (mdir / "glTF-Binary").mkdir(parents=True, exist_ok=True)
        (mdir / "screenshot").mkdir(parents=True, exist_ok=True)

        glb, nmat, nrows, ncols, layout = build_glb(
            row_specs=rows, textured=[], extensions=exts,
            scene_name=dirname, cols=COLS,
            generator=("DisplayXR model viewer, "
                       "scripts/make_khronos_conformance_assets.py"),
            extra_png=ripple_normal_png() if needs_ripple else None)

        # The shared builder emits one mesh layout and a gradient image for
        # every asset, because the in-tree grid it also feeds needs them. Most
        # of these need neither, so sweep unreferenced objects before shipping.
        doc, binary = parse_glb(glb)
        doc, binary = prune(doc, binary, drop_attributes=drop_attrs)
        packed = write_glb(doc, binary)
        (mdir / "glTF-Binary" / (dirname + ".glb")).write_bytes(packed)
        # `glTF` (separate .gltf + .bin + images) is REQUIRED for every model.
        nbin, nimg = write_gltf_variant(doc, binary, mdir / "glTF", dirname)

        (mdir / "metadata.json").write_text(json.dumps({
            "version": 2,
            "name": name,
            "path": "./Models/" + dirname,
            "summary": summary,
            "screenshot": "screenshot/screenshot.png",
            "tags": ["testing", "pbrtest", "extension"],
            "createReadme": True,
            "legal": LEGAL,
        }, indent=2) + "\n", encoding="utf-8")

        # README.body.md, NOT README.md: a model's README.md is generated from
        # metadata.json plus this file, so a hand-written one is overwritten.
        # Sections start at "##" per CONTRIBUTING; the title, extension list and
        # summary are omitted because they are generated from metadata.json.
        (mdir / "README.body.md").write_text(
            "## Screenshot\n\n"
            "![screenshot](screenshot/screenshot_large.png)\n"
            "<br/>_Rendered by the [DisplayXR Model Viewer]"
            "(https://github.com/DisplayXR/displayxr-demo-modelviewer) "
            "under its procedural sky environment._\n\n"
            "## Description\n\n" + desc
            + "\n## Provenance\n\n"
              "Generated procedurally rather than modelled, so it can be "
              "regenerated against a specification change in one command. The "
              "generator is [`make_khronos_conformance_assets.py`]"
              "(https://github.com/DisplayXR/displayxr-demo-modelviewer/blob/main/"
              "scripts/make_khronos_conformance_assets.py) in the "
              "[DisplayXR model viewer](https://github.com/DisplayXR/"
              "displayxr-demo-modelviewer).\n",
            encoding="utf-8")

        print(dirname + ": glb=" + format(len(packed), ",") + "B  bin="
              + format(nbin, ",") + "B  images=" + str(nimg) + ", "
              + str(nrows) + " rows x " + str(ncols) + " cols = "
              + str(nmat) + " materials")
        for line in layout:
            print("   " + line)

    print("\nwrote " + str(out))
    print("Next: scripts/make_khronos_screenshots.py (needs viewer captures),")
    print("      then validate every .glb and .gltf with the glTF-Validator.")


if __name__ == "__main__":
    main()
