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
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    raise SystemExit("needs numpy and pillow:  python3 -m pip install numpy pillow")

# Scene constants — must match scripts/make_material_grid.py:build_transmission_test.
BACKDROP_W = 12.0     # plate width in world units, centred on x = 0
SPACING = 1.15        # sphere pitch
RADIUS = 0.45
STEPS = 7             # sphere count; index 0 is the opaque control
MARGIN_FRAC = 0.10    # outer fraction of the plate used as the row baseline


def plate_mask(tile):
    """Boolean mask of backdrop-plate pixels.

    Discriminates on GREEN SUPPRESSION, not on saturation. The plate texture is
    generated as (R, 24, B) with R and B ramping 0..255, so green sits far below
    the brighter of the two everywhere on it; the analytic sky is a pale blue
    whose green lands BETWEEN its red and blue. Measured on a macOS capture:
    plate 0.02-0.10, sky 0.74 — three-quarters of the range apart, and the
    ordering survives exposure and either tone curve because it is a property of
    the asset rather than of the grade.

    Saturation was the obvious first choice and is the wrong one: the sky is a
    strong blue and scored 0.37 against a 0.30 threshold, so the "plate" swallowed
    the entire frame and the geometry derived from it was silently garbage.
    """
    mx = np.maximum(tile[:, :, 0], tile[:, :, 2])
    green_ratio = tile[:, :, 1] / np.maximum(mx, 1.0)
    return (green_ratio < 0.35) & (mx > 20)


def find_backdrop(tile):
    """Bounding box of the backdrop plate: (y0, y1, x0, x1) inclusive."""
    ys, xs = np.where(plate_mask(tile))
    if len(ys) < 1000:
        raise SystemExit(
            "could not locate the backdrop plate (%d matching pixels) — is this a "
            "transmission_test capture, and did the model actually load?" % len(ys))
    return int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max())


def check_tile(tile, tol, dump_rows):
    """Measure every sphere against the backdrop. Returns (ok, list of rows)."""
    y0, y1, x0, x1 = find_backdrop(tile)
    h, w = tile.shape[0], tile.shape[1]
    plate_w, plate_h = x1 - x0 + 1, y1 - y0 + 1

    # A plate clipped by the frame edge would make the units-per-pixel scale
    # below wrong, and every sphere centre with it. Say so rather than measure
    # confidently in the wrong place — that is the failure mode this whole file
    # exists to avoid.
    if x0 <= 1 or y0 <= 1 or x1 >= w - 2 or y1 >= h - 2:
        raise SystemExit(
            "backdrop plate touches the frame edge (x %d..%d of %d, y %d..%d of %d) — "
            "it is cropped, so sphere positions cannot be derived from it. Reframe "
            "the window so the whole plate is visible." % (x0, x1, w, y0, y1, h))

    ppu = plate_w / BACKDROP_W               # pixels per world unit
    cx0 = x0 + plate_w / 2.0                 # world x = 0
    cy = y0 + plate_h / 2.0                  # spheres sit at world y = 0
    r_px = RADIUS * ppu

    # Row baseline from the plate's outer margins — sphere-free by construction.
    # Odd width so the median is a real sample rather than the mean of two, which
    # otherwise puts a spurious 0.5/255 floor under every residual.
    mw = max(3, int(plate_w * MARGIN_FRAC) | 1)
    left = tile[:, x0 + 2:x0 + 2 + mw]
    right = tile[:, x1 - 1 - mw:x1 - 1]
    ref = np.median(np.concatenate([left, right], axis=1), axis=1)   # (h, 3) per row

    # Sanity: the two margins must agree, or the plate is not a pure vertical
    # ramp in this capture (a shadow, an overlay, or a mislocated plate).
    band = slice(int(cy - 2 * r_px), int(cy + 2 * r_px) + 1)
    skew = np.abs(np.median(left[band], axis=1) - np.median(right[band], axis=1)).max()
    if skew > 2.0:
        print("  WARNING: left/right plate margins differ by %.1f/255 on the sphere "
              "row — the baseline may not be trustworthy" % skew)

    rows = []
    yy, xx = np.mgrid[0:h, 0:w]
    for i in range(STEPS):
        cx = cx0 + (i - (STEPS - 1) / 2.0) * SPACING * ppu
        # 70% of the radius: away from the silhouette, where a one-pixel error in
        # the derived centre would mix in sky or plate and manufacture a residual.
        disc = ((xx - cx) ** 2 + (yy - cy) ** 2) <= (0.70 * r_px) ** 2
        px = tile[disc]
        base = ref[yy[disc]]
        d = np.abs(px - base).max(axis=1)
        signed = (px - base).mean(axis=0)          # per-channel, signed
        rows.append((i, len(px), float(d.mean()), float(d.max()), signed))

    if dump_rows:
        print("  plate x %d..%d y %d..%d  |  %.2f px/unit  |  r %.1f px  |  centre y %.1f"
              % (x0, x1, y0, y1, ppu, r_px, cy))

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
        ok, rows = check_tile(tile, a.tol, a.dump_rows)
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
