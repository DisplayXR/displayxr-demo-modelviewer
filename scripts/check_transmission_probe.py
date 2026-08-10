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

LOCATING THE SPHERES IS THE HARD PART, AND IT FAILS SILENTLY
------------------------------------------------------------
Every wrong answer in the #75 thread came from a contaminated or mislocated
reference, not from arithmetic — and both failure modes look like a pass.

The row baseline must come from the plate's sphere-free outer margins. A per-row
median across the whole plate is contaminated: on the sphere row the spheres
span more than half the plate's width, so a failing run drags the "reference"
toward the very thing being measured and understates the defect by about an
order of magnitude. See transmission_test_scene.row_baseline().

The sphere CENTRES cannot simply be projected from the plate. The spheres sit at
z = 0 and the plate at z = -2.5, so they are magnified against it (measured
x1.063 on macOS/sim_display, x1.068 on Windows with a hardware DP) and, under a
tracked off-axis viewer, the near row does not project to the far plate's centre
(measured 25 px low on macOS, 50 px low on Windows — comparable to a sphere
radius). Sampling on plate-projected centres therefore measures plate against
plate, which matches the baseline trivially and reads as a clean PASS on a
completely unfixed build. See transmission_test_scene for how the row is
actually located.

Two guards exist because that failure is silent:
  - the opaque control must clear CONTROL_MIN in absolute terms (a grossly
    mislocated fit collapses it — 3.6/255 measured on a real DP);
  - the located disc must be geometrically self-consistent, since radius and
    pitch both live in the z = 0 plane and are therefore locked to each other.
The first alone is not enough: a fit can be off by most of a radius, keep enough
overlap to hold the control in the hundreds, and still have the transmissive
discs straddling plate. That is precisely what happened on macOS and it is why
the second guard is not optional.

Usage:
    python3 scripts/check_transmission_probe.py <atlas.png> [--views N]
                                                [--tol 1.0] [--dump-rows]

Exit status is 0 only if every transmissive sphere is within --tol (in 0-255
units) of the backdrop. A control that fails its guard RAISES rather than
returning a verdict — a table built on a mislocated fit is not a failing
measurement, it is not a measurement.
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

# Floor for the opaque control's residual, in 0-255 units. Well under the
# 189-243 a correctly located control measures, well over the single digits a
# mislocated one produces. See the guard in check_tile().
CONTROL_MIN = 30.0


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

    # The control guard is ABSOLUTE, not "> tol". Sphere 0 is fully opaque
    # against a saturated red->blue ramp, so a correctly located sample reads in
    # the hundreds — 189-243 measured on Windows/hardware DP and on
    # macOS/sim_display. A control that comes back small does not mean the
    # render changed; it means
    # the sample disc is not on the sphere, and every OTHER row in the table is
    # then measuring plate-against-plate, which matches the baseline trivially
    # and reads as a clean pass on a completely unfixed build.
    #
    # `> tol` was the original rule and it is useless for this: a mislocated fit
    # measured at 3.6 on a real DP clears a threshold of 1.0 comfortably. It was
    # caught by a human noticing that 3.6 is impossible for an opaque sphere,
    # which is exactly the job this line is supposed to be doing.
    if rows[0][3] < CONTROL_MIN:
        raise SystemExit(
            "%s: opaque control (sphere 0) reads only %.1f/255 against the backdrop, "
            "but it is fully opaque and must read >= %.0f. The sample discs are not "
            "on the spheres — this is a MISLOCATED FIT, not a passing render, and the "
            "rest of the table is meaningless. Do not read it as a pass."
            % (what, rows[0][3], CONTROL_MIN))

    ok = all(r[3] <= tol for r in rows[1:])
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
