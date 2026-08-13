#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Measure a coat_test.glb capture: does KHR_materials_coat actually do anything?

Issue #81. Six rows, seven spheres each, all over the same rough red base:

    0  clearcoat_ref    CONTROL — KHR_materials_clearcoat, factor 0 -> 1
    1  coat_factor      the same material in coat's spelling
    2  coat_color       coatColorFactor white -> amber
    3  coat_darkening   coatDarkeningFactor 0 -> 1
    4  coat_ior         coatIor 1.0 -> 2.0
    5  coat_aniso       coatAnisotropyStrength 0 -> 1

WHAT THIS DOES AND DOES NOT ESTABLISH.

Rows 0 and 1 are the same material in clearcoat's spelling and coat's, and they
are compared here — but that comparison is CONFOUNDED and is reported as
information, not as a pass. The two rows sit at different heights in the scene,
the environment is a vertical gradient, and these spheres are mirror-ish, so
identical materials at different heights genuinely do not render identically.
Measured, that floor is under a level; the tolerance is set above it and this
check can only catch a gross divergence.

THE REAL clearcoat/coat EQUALITY TEST IS ELSEWHERE, and it is exact: capture
assets/material_grid.glb — which carries KHR_materials_clearcoat and no coat —
before and after the coat change and diff them with compare_captures.py. Same
geometry, same heights, bit-exact captures, and the answer is 0.000 rather than
"under a level". Do not weaken that into this.

THE REMAINING ROWS ARE DIRECTIONS, not values. We light with the procedural
analytic sky rather than the outdoor HDRI the Khronos reference screenshots use,
so absolute RGB is not comparable to anything published; what is claimed is the
sign of each response, and its magnitude only to an order.

Two of them are deliberately weak criteria, for reasons that are physics rather
than laziness. DARKENING is a one-pass T = (1-R)^2 with R about 0.04, so the
spec's own model yields only a few per cent. ANISOTROPY here is direct-light
only, exactly like the base material's KHR_materials_anisotropy, and one
directional light against a full sky contributes little to a whole-sphere mean —
the grid probe classes the base anisotropy row as "soft" for the same reason.
Neither is evidence the code is wrong; asserting a large swing for either would
be asserting something untrue.

WHOLE SPHERES, NOT A CENTRE DISC. Each cell is averaged over every foreground
pixel in it. A small centred patch catches the specular highlight at different
positions as the sweep changes the coat, which swings the reading by an order of
magnitude and has already produced one entirely fictitious result in this work.

Usage:
    python3 scripts/probe_coat_test.py <atlas.png> [--tol 1.0]

Capture with:
    SIM_DISPLAY_OUTPUT=2d ./scripts/capture_reference.sh assets/coat_test.glb /tmp/cap-coat-test --wait 16

Exit 0 if the clearcoat/coat equality holds within --tol (mean levels, 0..255)
and every row that must move does; 1 otherwise.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_material_grid as pmg   # noqa: E402

try:
    import numpy as np
    from PIL import Image
except ImportError:
    raise SystemExit("numpy + Pillow required: python3 -m pip install numpy pillow")

ROWS = ["clearcoat_ref", "coat_factor", "coat_color",
        "coat_darkening", "coat_ior", "coat_aniso"]


def main():
    args = sys.argv[1:]
    if not args or "-h" in args or "--help" in args:
        raise SystemExit(__doc__)
    tol = 2.0
    if "--tol" in args:
        i = args.index("--tol")
        tol = float(args[i + 1])
        del args[i:i + 2]
    path = args[0]

    rgb = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    w, h, ch, prows = pmg.load_png(path)
    is_fg, sx, sy = pmg.foreground(w, h, ch, prows)
    tiles, sphere_cols, xb, yb = pmg.detect_tiles(w, h, is_fg, sy)
    if not tiles:
        raise SystemExit("no geometry found — was coat_test.glb actually loaded?")
    tile_data = pmg.detect_rows(tiles, sphere_cols, is_fg)

    tx0, tx1, ty0, ty1, cols, rws, raw_n = tile_data[0]
    print(f"atlas {w}x{h}   tile x[{tx0},{tx1}] y[{ty0},{ty1}]   "
          f"columns {len(cols)}/7   rows {raw_n}/{len(ROWS)}")
    if len(cols) != 7 or raw_n != len(ROWS):
        raise SystemExit("detection is off — attach the PNG rather than trusting numbers")

    # Foreground mask over the whole tile, built once. Same per-scanline-median
    # background estimate the grid probe uses, so "sphere" means the same thing
    # in both tools.
    mask = np.zeros((h, w), dtype=bool)
    for y in range(ty0, ty1):
        for x in range(tx0, tx1):
            mask[y, x] = is_fg(x, y)

    means = []      # [row][col] -> (r, g, b)
    for r in range(len(ROWS)):
        ry0, ry1 = rws[r]
        rowvals = []
        for c in range(7):
            cx0, cx1 = cols[c]
            m = mask[ry0:ry1, cx0:cx1]
            px = rgb[ry0:ry1, cx0:cx1][m]
            rowvals.append(px.mean(axis=0) if px.size else np.zeros(3))
        means.append(rowvals)

    def luma(v):
        return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]

    print(f"\nwhole-sphere mean luma per column ({int(mask.sum())} foreground px in tile)")
    print(f"  {'row':<15} " + " ".join(f"{i:>6}" for i in range(7)))
    for r, name in enumerate(ROWS):
        print(f"  {name:<15} " + " ".join(f"{luma(v):6.1f}" for v in means[r]))

    ok = True

    # 1. clearcoat vs coat. Confounded by row height — see the module docstring.
    #    A gross divergence still shows here; an exact equality does not, and is
    #    not what this number is for.
    print("\nclearcoat vs coat, same material both spellings")
    print("  (confounded: different scene heights see different sky — the exact")
    print("   test is material_grid.glb before/after with compare_captures.py)")
    worst = 0.0
    for c in range(7):
        d = float(np.abs(means[0][c] - means[1][c]).max())
        worst = max(worst, d)
        print(f"  column {c}  max |dRGB| {d:6.3f}")
    if worst > tol:
        print(f"  FAIL max {worst:.3f} > tol {tol} — too far apart to be the same material")
        ok = False
    else:
        print(f"  OK  max {worst:.3f} <= tol {tol} (at the confounding floor)")

    # 2. Directions. Each is a claim about the sign of a response, checked over
    #    the full sweep rather than between adjacent columns — neighbouring steps
    #    can be within noise of each other while the row as a whole moves.
    def monotone(vals, sign, slack=0.05):
        """Monotone to within `slack` per step — a sweep may be flat between two
        adjacent steps without being non-monotone."""
        return all(sign * (vals[i + 1] - vals[i]) > -slack for i in range(len(vals) - 1))

    checks = [
        ("coat_color", 2, "amber tint reddens: B falls further than R",
         lambda m: (m[6][2] - m[0][2]) < (m[6][0] - m[0][0]) - 1.0),
        ("coat_darkening", 3, "darkening darkens, monotonically (small: one-pass T)",
         lambda m: luma(m[6]) < luma(m[0]) - 0.3
                   and monotone([luma(v) for v in m], -1)),
        # A higher coat IOR raises f0, so the coat reflects more and the base
        # receives less. Whether the sphere brightens or darkens depends on
        # whether the coat's mirror is brighter than what it covers — over a
        # bright neutral base under a sky-and-ground environment it DARKENS.
        # So the claim is monotonicity, not a direction. (The first version of
        # this check asserted "brighter", which only held while issue #87 had
        # Fresnel pinned at grazing.)
        ("coat_ior", 4, "IOR changes the coat/base balance, monotonically",
         lambda m: abs(luma(m[6]) - luma(m[0])) > 0.3
                   and (monotone([luma(v) for v in m], +1)
                        or monotone([luma(v) for v in m], -1))),
        ("coat_aniso", 5, "anisotropy changes the lobe (soft: direct light only)",
         lambda m: abs(luma(m[6]) - luma(m[0])) > 0.2),
    ]
    print("\nresponse directions:")
    for name, r, what, pred in checks:
        good = pred(means[r])
        d = luma(means[r][6]) - luma(means[r][0])
        print(f"  {name:<15} {'OK  ' if good else 'FAIL'}  {what}   (dLuma {d:+.2f})")
        ok = ok and good

    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
