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

Writes <outdir>/Models/<Name>/{glTF-Binary/<Name>.glb, metadata.json, README.md}.
Default outdir is ./khronos_submission.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_material_grid import (           # noqa: E402
    build_glb, ripple_normal_png,
    m_coat_clearcoat_ref, m_coat_factor, m_coat_color, m_coat_darkening,
    m_coat_ior, m_coat_anisotropy, m_coat_normal,
    m_sheen_ref, m_fuzz_factor, m_fuzz_black, m_fuzz_rough,
    m_diffuse_rough, m_diffuse_rough_spec,
)

# Eight, not seven, so the paired control row divides evenly into four pairs.
COLS = 8
PAIR_VALUES = [0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0]

LEGAL = [{
    "license": "CC0-1.0",
    "year": "2026",
    "artist": "DisplayXR",
    "what": "Entire Model",
    "owner": "Leia Inc.",
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

DIFFUSE_ROWS_UPSTREAM = [
    ("diffuse_rough_matte", "diffuseRoughnessFactor 0 -> 1, matte base", m_diffuse_rough),
    ("diffuse_rough_gloss", "diffuseRoughnessFactor 0 -> 1, gloss base", m_diffuse_rough_spec),
]


COAT_DESC = """\
A parameter sweep for `KHR_materials_coat`. Seven rows of eight spheres over a
common rough red base; each row varies exactly one coat property from left to
right, so a difference between two implementations localises to a single
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
A parameter sweep for `KHR_materials_diffuse_roughness`. Two rows of eight
spheres sweeping `diffuseRoughnessFactor` from 0 to 1.

The extension changes how the diffuse lobe falls off with view angle, replacing
glTF's purely Lambertian diffuse response. That is a subtle effect and it is easy
to bury, so the two rows differ only in what surrounds it:

| Row | Base |
|---|---|
| 0 | Matte neutral - the diffuse response in isolation |
| 1 | Glossier base - the same sweep with a specular highlight competing |

Row 0 is the one to measure. Row 1 exists because the realistic authoring case has
a specular lobe present, and an implementation that applies diffuse roughness to
the wrong term tends to look plausible on row 0 and wrong on row 1.

At `diffuseRoughnessFactor` 0 the material must be indistinguishable from a base
glTF 2.0 material with the extension absent. That is the cheapest conformance
check in the asset and the first one to run.

Note on defaults: the extension's README parameter table and its JSON schema
currently disagree on the default for `diffuseRoughnessFactor` (`0.0` versus
`1.0`). Every column here sets the value explicitly, so this asset is unaffected
by the resolution - but a viewer that renders row 0's leftmost sphere differently
from a no-extension material has likely applied the schema default somewhere.
"""

MODELS = [
    ("CoatParameterSweep", "Coat Parameter Sweep",
     "Parameter sweep for KHR_materials_coat, with a height-matched "
     "clearcoat-to-coat mapping control.",
     COAT_ROWS_UPSTREAM, ["KHR_materials_clearcoat", "KHR_materials_coat"],
     COAT_DESC, True),
    ("FuzzParameterSweep", "Fuzz Parameter Sweep",
     "Parameter sweep for KHR_materials_fuzz, with a KHR_materials_sheen "
     "reference row.",
     FUZZ_ROWS_UPSTREAM, ["KHR_materials_sheen", "KHR_materials_fuzz"],
     FUZZ_DESC, False),
    ("DiffuseRoughnessParameterSweep", "Diffuse Roughness Parameter Sweep",
     "Parameter sweep for KHR_materials_diffuse_roughness over a matte and a "
     "glossy base.",
     DIFFUSE_ROWS_UPSTREAM, ["KHR_materials_diffuse_roughness"],
     DIFFUSE_DESC, False),
]


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(__file__).resolve().parent.parent / "khronos_submission"

    for dirname, name, summary, rows, exts, desc, needs_ripple in MODELS:
        mdir = out / "Models" / dirname
        (mdir / "glTF-Binary").mkdir(parents=True, exist_ok=True)
        (mdir / "screenshot").mkdir(parents=True, exist_ok=True)

        glb, nmat, nrows, ncols, layout = build_glb(
            row_specs=rows, textured=[], extensions=exts,
            scene_name=dirname, cols=COLS,
            generator=("DisplayXR model viewer, "
                       "scripts/make_khronos_conformance_assets.py"),
            extra_png=ripple_normal_png() if needs_ripple else None)

        (mdir / "glTF-Binary" / (dirname + ".glb")).write_bytes(glb)

        (mdir / "metadata.json").write_text(json.dumps({
            "version": 2,
            "name": name,
            "path": "./Models/" + dirname,
            "summary": summary,
            "screenshot": "screenshot/screenshot.jpg",
            "tags": ["testing", "pbrtest", "extension"],
            "legal": LEGAL,
        }, indent=2) + "\n", encoding="utf-8")

        (mdir / "README.md").write_text(
            "# " + name + "\n\n## Extensions Used\n\n"
            + "".join("* " + e + "\n" for e in exts)
            + "\n## Summary\n\n" + summary + "\n\n## Description\n\n" + desc
            + "\n## Provenance\n\n"
              "Generated procedurally rather than modelled, so it can be "
              "regenerated against a specification change in one command. The "
              "generator is [`make_khronos_conformance_assets.py`]"
              "(https://github.com/DisplayXR/displayxr-demo-modelviewer/blob/main/"
              "scripts/make_khronos_conformance_assets.py) in the "
              "[DisplayXR model viewer](https://github.com/DisplayXR/"
              "displayxr-demo-modelviewer).\n",
            encoding="utf-8")

        print(dirname + ": " + format(len(glb), ",") + " bytes, "
              + str(nrows) + " rows x " + str(ncols) + " cols = "
              + str(nmat) + " materials")
        for line in layout:
            print("   " + line)

    print("\nwrote " + str(out))
    print("STILL REQUIRED before opening the PR:")
    print("  - screenshot/screenshot.jpg per model (Khronos requires one)")
    print("  - glTF-Validator pass on each .glb")


if __name__ == "__main__":
    main()
