#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Acceptance test for issue #75: does transmission sample the scene in the
colour space it composites into?

Run the viewer on assets/transmission_test.glb with

    DXR_MODELVIEWER_TRANSMISSION_PROBE=1
    DXR_MODELVIEWER_DETERMINISTIC=1

capture an atlas, and point this at it. The probe forces thickness 0 (so the
refraction ray degenerates to the fragment's own screen position) and mip 0, and
makes every transmissive surface output its raw scene sample through the
shader's own display transform instead of shading it. If the sample is in the
right space, each transmissive sphere then reproduces the backdrop pixels behind
it and disappears into the plate. If it is not, it does not — and the size of
the residual is the size of the colour-space error.

Sphere 0 (transmissionFactor 0) transmits nothing, is unaffected by the probe,
and stays fully visible. It is the control: a run where sphere 0 ALSO vanishes
is not a pass, it is a capture of an empty frame or a mislocated geometry fit.

WHY THIS SCRIPT DERIVES GEOMETRY FROM THE BACKDROP, NOT FROM THE SPHERES
------------------------------------------------------------------------
Every wrong answer in the #75 thread came from a contaminated reference, not
from arithmetic. Segmenting the sphere row to find the spheres cannot work here
by construction: a passing run has six of seven spheres invisible, so there is
nothing to segment. Taking a per-row median across the whole plate does not work
either — on the sphere row the spheres span more than half the plate's width, so
a failing run drags the "reference" toward the very thing being measured and
understates the defect by about an order of magnitude.

So the backdrop plate is located instead (it is a saturated red->blue ramp
against a pale sky — high contrast, and present whether or not the spheres
render), and the sphere centres are derived from it using the generator's own
constants: the plate is BACKDROP_W units wide and centred on x = 0, the spheres
sit at (i - 3) * SPACING with radius RADIUS. The baseline for a given row comes
from the outer margins of the plate, which are sphere-free by construction
(spheres reach +-3.9 units, the plate +-6.0) and carry the identical gradient
value, since the ramp is a function of row only.

Usage:
    python3 scripts/check_transmission_probe.py <atlas.png> [--views N]
                                                [--tol 1.0] [--dump-rows]

Exit status is 0 only if every transmissive sphere is within --tol (in 0-255
units) of the backdrop AND the opaque control is not.
"""

import argparse
import os
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    raise SystemExit("needs numpy and pillow:  python3 -m pip install numpy pillow")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transmission_test_scene import STEPS, locate   # noqa: E402


def check_tile(tile, tol, dump_rows, what):
    """Measure every sphere against the backdrop. Returns (ok, list of rows)."""
    g = locate(tile, what)
    h, w = tile.shape[0], tile.shape[1]
    ref = g.row_baseline(tile)

    # Sanity: the two margins must agree on the sphere row, or the plate is not a
    # pure vertical ramp in this capture (a shadow, an overlay, a mislocated
    # plate) and the baseline is not trustworthy.
    band = slice(int(g.cy - 2 * g.r_px), int(g.cy + 2 * g.r_px) + 1)
    mw = max(3, int(g.plate_w * 0.10) | 1)
    skew = np.abs(np.median(tile[band, g.x0 + 2:g.x0 + 2 + mw], axis=1)
                  - np.median(tile[band, g.x1 - 1 - mw:g.x1 - 1], axis=1)).max()
    if skew > 2.0:
        print("  WARNING: left/right plate margins differ by %.1f/255 on the sphere "
              "row — the baseline may not be trustworthy" % skew)

    rows = []
    yy, xx = np.mgrid[0:h, 0:w]
    for i, cx in enumerate(g.centres):
        # 70% of the radius: away from the silhouette, where a one-pixel error in
        # the derived centre would mix in sky or plate and manufacture a residual.
        disc = ((xx - cx) ** 2 + (yy - g.cy) ** 2) <= (0.70 * g.r_px) ** 2
        px = tile[disc]
        base = ref[yy[disc]]
        d = np.abs(px - base).max(axis=1)
        signed = (px - base).mean(axis=0)          # per-channel, signed
        rows.append((i, len(px), float(d.mean()), float(d.max()), signed))

    if dump_rows:
        print("  plate x %d..%d y %d..%d  |  %.2f px/unit  |  r %.1f px  |  centre y %.1f"
              % (g.x0, g.x1, g.y0, g.y1, g.ppu, g.r_px, g.cy))

    ok = rows[0][3] > tol and all(r[3] <= tol for r in rows[1:])
    return ok, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--views", type=int, default=2,
                    help="view tiles side by side in the atlas (default 2)")
    ap.add_argument("--tol", type=float, default=1.0,
                    help="max allowed |sphere - backdrop| in 0-255 units (default 1)")
    ap.add_argument("--dump-rows", action="store_true",
                    help="also print the derived plate geometry")
    ap.add_argument("--report", action="store_true",
                    help="measure a NORMALLY SHADED capture (probe off) instead of "
                         "asserting: prints signed per-channel sphere-minus-backdrop, "
                         "the quantity issue #75 tabulated. Always exits 0 — shaded "
                         "glass is supposed to differ from the backdrop.")
    a = ap.parse_args()

    im = Image.open(a.png).convert("RGB")
    full = np.asarray(im, dtype=float)
    tw = full.shape[1] // a.views
    print("%s  %dx%d, %d view tile(s) of %d px"
          % (a.png, full.shape[1], full.shape[0], a.views, tw))

    all_ok = True
    for v in range(a.views):
        tile = full[:, v * tw:(v + 1) * tw]
        print("\nview %d" % v)
        ok, rows = check_tile(tile, a.tol, a.dump_rows, "view %d" % v)
        if a.report:
            print("  sphere  transmission   sphere - backdrop (R, G, B)")
            for i, n, mean, mx, sg in rows:
                print("  %6d  %12.2f   [%7.1f, %7.1f, %7.1f]%s"
                      % (i, i / (STEPS - 1), sg[0], sg[1], sg[2],
                         "  <- opaque control" if i == 0 else ""))
            continue
        print("  sphere  transmission   px    mean|d|   max|d|")
        for i, n, mean, mx, sg in rows:
            tag = "  <- opaque control" if i == 0 else ("" if mx <= a.tol else "  <- FAIL")
            print("  %6d  %12.2f  %5d  %8.3f  %7.1f%s"
                  % (i, i / (STEPS - 1), n, mean, mx, tag))
        print("  %s" % ("PASS — every transmissive sphere reproduces the backdrop "
                        "within %.1f/255, control does not" % a.tol if ok else
                        "FAIL"))
        all_ok = all_ok and ok

    if a.report:
        return 0
    print("\n%s" % ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
