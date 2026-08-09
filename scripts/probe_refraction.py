#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""How does transmissive glass map the background behind it? Measured, not eyeballed.

Companion to probe_material_grid.py, for transmission_test.glb. That probe answers
"does the transmission row vary at all", which catches a row that has stopped
rendering. It cannot tell you HOW the glass maps what is behind it — so a change
that leaves transmission varying but samples the wrong region, at the wrong scale,
or the wrong way up, passes it silently. This measures the mapping itself.

transmission_test.glb is 7 spheres (transmissionFactor 0 .. 1, identical geometry
and lighting) on a plane whose gradient runs red at top to blue at bottom. For a
sphere at (cx, cy) we sample the interior down its centre line and fit two models:

    UPRIGHT   interior(cy + d) ~ bg(cy + k*d)
    INVERTED  interior(cy + d) ~ bg(cy - k*d)

sweeping the magnification k and comparing normalised shapes, so only the SHAPE
and the SIGN matter — not brightness, tint or contrast.

THE TRAP, and why this script differences against sphere 0
----------------------------------------------------------
Fitting a sphere's raw interior does not work, and fails in a way that looks
convincing. Every sphere is lit from above, so it carries its own top-bright
shading gradient, and the backdrop is also brightest (reddest) at the top. The
two correlate. Fit the raw interior and EVERY sphere reports "upright" with a
low error — including sphere 0, which has transmissionFactor = 0 and therefore
transmits nothing at all and cannot be showing the backdrop under any model.

That tell — the opaque control scoring like a transmissive one — is how the
confound was caught. (Run --raw to see it: sphere 0 lands at an error close to
the genuinely transmissive spheres, when it should have no defensible fit at
all. How it is labelled depends on the error metric; the point is that it scores
at all.) The fix is to difference against it:

    D_i(d) = interior_i(d) - interior_0(d)

All seven spheres are the same mesh under the same lighting and differ only in
transmissionFactor, so the subtraction cancels shading, Fresnel and IBL and
leaves the transmitted contribution. D_i is what gets fitted.

Reading the output
------------------
  amplitude  chroma range of the transmitted-only component. Must grow with
             transmissionFactor. If it does not, transmission is not being
             applied — that is a probe_material_grid.py-class failure.
  verdict    which mapping the transmitted component follows.
  k          fitted magnification. k railing at the top of the sweep means the
             interior spans more of the backdrop than fits in the window.

As of v0.19.0 the renderer produces a MAGNIFIED UPRIGHT image on macOS, Linux
and Windows alike. That is a property of the single-sample screen-space
refraction this renderer uses — one refraction at the entry surface, marched by
thickness, projected and sampled, with no exit interface — and not a defect.
This script exists to notice when that mapping CHANGES, whichever way it goes.

Usage:
    python3 scripts/probe_refraction.py <atlas.png> [--raw]

    --raw  also run the un-differenced fit. Kept only to demonstrate the
           confound: watch sphere 0, which transmits nothing, score anyway.
"""

import os
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    raise SystemExit("needs numpy and pillow:  python3 -m pip install numpy pillow")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transmission_test_scene import STEPS, locate   # noqa: E402


def load_tile(path):
    """Left view tile of the atlas, as float RGB. Views differ only by parallax."""
    im = Image.open(path).convert("RGB")
    w, _ = im.size
    return np.asarray(im, dtype=float)[:, : w // 2]


def find_spheres(tile):
    """Locate the sphere row. Returns (cy, radius, [centre_x, ...], Geometry).

    Derived from the BACKDROP plate, not segmented out of the sphere row — see
    transmission_test_scene. The old segmentation thresholded the sphere row's
    luminance, kept the blobs wider than 60 px, and extrapolated a pitch from the
    first two. Every constant in that sentence was tuned on a 1000x1440 window;
    on a 2560x720 macOS atlas the spheres are ~64 px wide, so `> 60` kept almost
    nothing and five of six spheres came back `nan` / `ambiguous` — a wrong
    answer wearing a measurement's clothes. It also got structurally harder once
    issue #75 landed, because correctly-composited glass departs from the plate
    LESS than the double-tone-mapped version it replaced.
    """
    g = locate(tile, "probe_refraction")
    return g.cy, g.r_px, list(g.centres), g


K_SWEEP = np.arange(0.05, 3.01, 0.05)
K_MAX = float(K_SWEEP[-1])


def fit(interior, bg, ds, cy, height):
    """Best (error, k) for the upright and inverted mappings. Shape-only."""
    out = {}
    for sign, name in ((+1, "upright"), (-1, "inverted")):
        errs = []
        for k in K_SWEEP:
            ref = np.array([bg[int(np.clip(cy + sign * k * d, 0, height - 1))] for d in ds])
            if ref.std() < 1e-6 or interior.std() < 1e-6:
                continue
            errs.append((np.abs(interior / interior.std() - ref / ref.std()).mean(), k))
        out[name] = min(errs) if errs else (99.0, 0.0)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        raise SystemExit(__doc__)
    raw_too = "--raw" in sys.argv

    tile = load_tile(args[0])
    h = tile.shape[0]
    chroma = tile[:, :, 0] - tile[:, :, 2]          # monotonic along the backdrop gradient
    cy, rad, centres, geom = find_spheres(tile)
    # Background reference per row from the plate's OUTER MARGINS. A median
    # across the full plate row is contaminated: on the sphere row the spheres
    # span more than half the plate's width, so the "reference" is dragged toward
    # the thing being measured. That is the trap issue #75 kept falling into.
    bg = geom.row_baseline(tile)
    bg = bg[:, 0] - bg[:, 2]                        # same chroma axis as `chroma`
    ds = np.arange(-0.55 * rad, 0.55 * rad, 1.0)
    print(f"tile {tile.shape[1]}x{h}   sphere row cy {cy:.0f}   radius {rad:.0f}")

    def interior(cx):
        x0, x1 = int(cx - rad * 0.18), int(cx + rad * 0.18) + 1
        return np.array([chroma[int(cy + d), x0:x1].mean() for d in ds])

    base = interior(centres[0])                     # transmissionFactor = 0
    print("\ntransmitted component only (sphere i minus the opaque control)")
    print(f"{'sph':<5}{'trans':>7}{'amplitude':>11}{'upright':>10}{'inverted':>10}{'k':>7}   verdict")
    for i in range(1, STEPS):
        d_i = interior(centres[i]) - base
        r = fit(d_i - d_i.mean(), bg, ds, cy, h)
        ue, uk = r["upright"]
        ie, ik = r["inverted"]
        v = ("INVERTED" if ie < ue * 0.85 else "UPRIGHT" if ue < ie * 0.85 else "ambiguous")
        k = ik if v == "INVERTED" else uk
        # A k that lands on the top of the sweep did not find a minimum — the
        # interior spans more backdrop than the sweep can reach, so the reported
        # error is whatever the boundary happened to give and the verdict beside
        # it is not evidence. Say that instead of printing it like a result.
        note = "  (k railed at %.2f — fit unreliable)" % K_MAX if k >= K_MAX - 1e-9 else ""
        print(f"{i:<5}{i / (STEPS - 1):7.2f}{d_i.max() - d_i.min():11.1f}"
              f"{ue:10.2f}{ie:10.2f}{k:7.2f}   {v}{note}")

    if raw_too:
        print("\n--raw: un-differenced fit. Sphere 0 transmits NOTHING, so its verdict")
        print("is the confound, not a measurement - see the module docstring.")
        print(f"{'sph':<5}{'upright':>10}{'inverted':>10}   verdict")
        for i in range(STEPS):
            ivals = interior(centres[i])
            r = fit(ivals - ivals.mean(), bg, ds, cy, h)
            ue, _ = r["upright"]
            ie, _ = r["inverted"]
            v = ("INVERTED" if ie < ue * 0.85 else "UPRIGHT" if ue < ie * 0.85 else "ambiguous")
            note = "   <- opaque control" if i == 0 else ""
            print(f"{i:<5}{ue:10.2f}{ie:10.2f}   {v}{note}")


if __name__ == "__main__":
    main()
