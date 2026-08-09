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

import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    raise SystemExit("needs numpy and pillow:  python3 -m pip install numpy pillow")


def load_tile(path):
    """Left view tile of the atlas, as float RGB. Views differ only by parallax."""
    im = Image.open(path).convert("RGB")
    w, _ = im.size
    return np.asarray(im, dtype=float)[:, : w // 2]


def find_spheres(tile):
    """Locate the sphere row. Returns (cy, radius, [centre_x, ...])."""
    h, w = tile.shape[0], tile.shape[1]
    lum = 0.2126 * tile[:, :, 0] + 0.7152 * tile[:, :, 1] + 0.0722 * tile[:, :, 2]
    mx, mn = tile.max(axis=2), tile.min(axis=2)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1), 0)
    ys, xs = np.where(sat > 0.30)                    # the plane; the sky around it is pale
    if len(ys) == 0:
        raise SystemExit("no backdrop plane found — is this a transmission_test capture?")
    py0, py1, px0, px1 = ys.min(), ys.max(), xs.min(), xs.max()

    bglum = np.array([np.median(lum[y, px0:px1 + 1]) for y in range(h)])
    dev = np.abs(lum - bglum[:, None])
    dev[:, :px0] = 0
    dev[:, px1 + 1:] = 0
    rows = (dev > 10).sum(axis=1)
    band = np.where(rows > rows.max() * 0.30)[0]
    cy = (band.min() + band.max()) / 2.0

    # Segment on the OPAQUE spheres and extrapolate: the transmissive ones barely
    # depart from the backdrop (the point of the test), so any threshold either
    # drops or merges them. The row is regular, so two clean centres give the pitch.
    prof = lum[int(cy)] - np.median(lum[int(cy), px0 + 20:px1 - 20])
    xs = np.where(prof > 6)[0]
    xs = xs[(xs > px0 + 20) & (xs < px1 - 20)]
    groups, s, p = [], xs[0], xs[0]
    for x in xs[1:]:
        if x - p > 6:
            groups.append((s, p))
            s = x
        p = x
    groups.append((s, p))
    solid = [g for g in groups if g[1] - g[0] > 60]
    if len(solid) < 2:
        raise SystemExit(f"found {len(solid)} segmentable spheres, need 2 to set the pitch")
    rad = (solid[0][1] - solid[0][0]) / 2.0
    c0 = (solid[0][0] + solid[0][1]) / 2.0
    pitch = (solid[1][0] + solid[1][1]) / 2.0 - c0
    return cy, rad, [c0 + i * pitch for i in range(7)], (px0, px1)


def fit(interior, bg, ds, cy, height):
    """Best (error, k) for the upright and inverted mappings. Shape-only."""
    out = {}
    for sign, name in ((+1, "upright"), (-1, "inverted")):
        errs = []
        for k in np.arange(0.05, 3.01, 0.05):
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
    cy, rad, centres, (px0, px1) = find_spheres(tile)
    bg = np.array([np.median(chroma[y, px0:px1 + 1]) for y in range(h)])
    ds = np.arange(-0.55 * rad, 0.55 * rad, 1.0)
    print(f"tile {tile.shape[1]}x{h}   sphere row cy {cy:.0f}   radius {rad:.0f}")

    def interior(cx):
        x0, x1 = int(cx - rad * 0.18), int(cx + rad * 0.18) + 1
        return np.array([chroma[int(cy + d), x0:x1].mean() for d in ds])

    base = interior(centres[0])                     # transmissionFactor = 0
    print("\ntransmitted component only (sphere i minus the opaque control)")
    print(f"{'sph':<5}{'trans':>7}{'amplitude':>11}{'upright':>10}{'inverted':>10}{'k':>7}   verdict")
    for i in range(1, 7):
        d_i = interior(centres[i]) - base
        r = fit(d_i - d_i.mean(), bg, ds, cy, h)
        ue, uk = r["upright"]
        ie, ik = r["inverted"]
        v = ("INVERTED" if ie < ue * 0.85 else "UPRIGHT" if ue < ie * 0.85 else "ambiguous")
        print(f"{i:<5}{i / 6.0:7.2f}{d_i.max() - d_i.min():11.1f}"
              f"{ue:10.2f}{ie:10.2f}{(ik if v == 'INVERTED' else uk):7.2f}   {v}")

    if raw_too:
        print("\n--raw: un-differenced fit. Sphere 0 transmits NOTHING, so its verdict")
        print("is the confound, not a measurement - see the module docstring.")
        print(f"{'sph':<5}{'upright':>10}{'inverted':>10}   verdict")
        for i in range(7):
            ivals = interior(centres[i])
            r = fit(ivals - ivals.mean(), bg, ds, cy, h)
            ue, _ = r["upright"]
            ie, _ = r["inverted"]
            v = ("INVERTED" if ie < ue * 0.85 else "UPRIGHT" if ue < ie * 0.85 else "ambiguous")
            note = "   <- opaque control" if i == 0 else ""
            print(f"{i:<5}{ue:10.2f}{ie:10.2f}   {v}{note}")


if __name__ == "__main__":
    main()
