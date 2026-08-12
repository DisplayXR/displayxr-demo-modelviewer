#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Measure a diffuse_fuzz_test.glb capture: do fuzz and diffuse roughness work?

Issue #84. Six rows, seven spheres each:

    0  sheen_ref       CONTROL — KHR_materials_sheen, sheenColorFactor -> black
    1  fuzz_factor     fuzzFactor 0 -> 1, white fuzz
    2  fuzz_black      fuzzColorFactor white -> BLACK at weight 1
    3  fuzz_rough      fuzzRoughnessFactor 0.05 -> 1
    4  diffuse_rough   diffuseRoughnessFactor 0 -> 1, matte base
    5  diffuse_gloss   diffuseRoughnessFactor 0 -> 1, semi-gloss base

ROWS 0 AND 2 ARE THE POINT. They are the same colour sweep, white to black,
written first as sheen and then as fuzz. Under sheen the colour IS the layer's
intensity, so sweeping it to black fades the layer out and converges on the bare
base. Under fuzz the weight is a separate parameter, so the same sweep DARKENS —
black soot, which sheen could not express. The two rows must therefore move in
OPPOSITE directions, and that divergence is what this asserts.

Row 5 exists to catch coupling: diffuse roughness must not touch the specular
lobe. If rows 4 and 5 responded identically despite their different base
roughness, the two roughnesses would be tied together, which is the one thing
the extension exists to separate.

WHOLE SPHERES, NOT A CENTRE DISC — see probe_coat_test.py for what a centre
patch does to these numbers.

Absolute RGB is not comparable to the Khronos reference screenshots: they use an
outdoor HDRI and we use the procedural analytic sky. Directions and magnitudes.

Usage:
    python3 scripts/probe_fuzz_test.py <atlas.png>

Capture with:
    SIM_DISPLAY_OUTPUT=2d ./scripts/capture_reference.sh assets/diffuse_fuzz_test.glb /tmp/cap-fuzz --wait 16
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

ROWS = ["sheen_ref", "fuzz_factor", "fuzz_black",
        "fuzz_rough", "diffuse_rough", "diffuse_gloss"]


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
        raise SystemExit("no geometry found — was diffuse_fuzz_test.glb actually loaded?")
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

    def luma_row(r):
        return [luma(v) for v in means[r]]

    def monotone(vals, sign, slack=0.05):
        return all(sign * (vals[i + 1] - vals[i]) > -slack for i in range(len(vals) - 1))

    # 1. The headline: the same white->black colour sweep under sheen and under
    #    fuzz has to go opposite ways. Both rows are in this capture, but they
    #    sit at different heights, so only the SIGN of each is claimed — not any
    #    comparison of their magnitudes.
    sheen_d = luma_row(0)[6] - luma_row(0)[0]
    fuzz_d = luma_row(2)[6] - luma_row(2)[0]
    print("\nsame colour sweep, two extensions (the reason fuzz exists):")
    print(f"  sheen_ref   dLuma {sheen_d:+7.2f}   (black sheen = layer off, converges on the base)")
    print(f"  fuzz_black  dLuma {fuzz_d:+7.2f}   (black fuzz = soot, darkens the surface)")
    if fuzz_d < -1.0 and fuzz_d < sheen_d - 1.0:
        print("  OK   fuzz darkens where sheen fades out")
    else:
        print("  FAIL fuzz is not behaving differently from sheen")
        ok = False

    # 2. Directions.
    checks = [
        ("fuzz_factor", 1, "white fuzz adds light, monotonically",
         lambda: luma_row(1)[6] > luma_row(1)[0] + 0.3 and monotone(luma_row(1), +1)),
        ("fuzz_black", 2, "sooty fuzz darkens, monotonically",
         lambda: monotone(luma_row(2), -1)),
        ("fuzz_rough", 3, "fuzz roughness changes the lobe",
         lambda: abs(luma_row(3)[6] - luma_row(3)[0]) > 0.3),
        ("diffuse_rough", 4, "diffuse roughness changes the falloff",
         lambda: abs(luma_row(4)[6] - luma_row(4)[0]) > 0.3),
        ("diffuse_gloss", 5, "and does so on a gloss base too (not specular-coupled)",
         lambda: abs(luma_row(5)[6] - luma_row(5)[0]) > 0.3),
    ]
    print("\nresponse directions:")
    for name, r, what, pred in checks:
        good = pred()
        d = luma_row(r)[6] - luma_row(r)[0]
        print(f"  {name:<15} {'OK  ' if good else 'FAIL'}  {what}   (dLuma {d:+.2f})")
        ok = ok and good

    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
