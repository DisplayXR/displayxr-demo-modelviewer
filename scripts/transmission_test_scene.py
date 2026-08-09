#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Locate transmission_test.glb's geometry in a capture, from the BACKDROP.

Shared by check_transmission_probe.py and probe_refraction.py.

Both probes need the same three things — where the sphere row is, how big the
spheres are, and what the backdrop is doing behind them — and both got them
wrong in the same way first: by segmenting the spheres out of the image. That
cannot be made reliable here, because the spheres this asset exists to measure
are the ones designed to be nearly invisible. Segmentation either merges them
into the plate, drops them, or (worst) finds some of them and extrapolates a
pitch from a framing that does not hold on another machine. That is where
probe_refraction.py's `nan` / `ambiguous` results on a 2560x720 macOS atlas came
from: a sphere-locating heuristic written against a 1000x1440 window.

So locate the ONE thing in the scene that is always fully visible, never
ambiguous, and independent of every material under test — the backdrop plate —
and derive everything else from the generator's constants. The plate is
BACKDROP_W x BACKDROP_H units centred on the origin; the spheres sit in a row on
it at (i - 3) * SPACING with radius RADIUS. See make_material_grid.py's
build_transmission_test().

Every failure here raises SystemExit with what was actually measured. A probe
that cannot find its subject must say so, not return a number.
"""

import numpy as np

# Scene constants — must match scripts/make_material_grid.py.
BACKDROP_W = 12.0
BACKDROP_H = 7.0
SPACING = 1.15
RADIUS = 0.45
STEPS = 7             # index 0 is the opaque control (transmissionFactor 0)
MARGIN_FRAC = 0.10    # outer fraction of the plate used as the row baseline


def plate_mask(tile):
    """Boolean mask of backdrop-plate pixels.

    Discriminates on GREEN SUPPRESSION, not saturation. The plate texture is
    generated as (R, 24, B) with R and B ramping 0..255, so green sits far below
    the brighter of the two everywhere on it, while the analytic sky is a pale
    blue whose green lands BETWEEN its red and blue. Measured on a macOS
    capture: plate 0.02-0.10, sky 0.74. The ordering is a property of the asset,
    not of the grade, so it survives exposure changes and either tone curve.

    Saturation is the obvious first choice and is wrong: the sky is a strong blue
    and scores ~0.37 against the 0.30 threshold that reads naturally, so the
    "plate" swallows the whole frame and everything derived from it is silently
    garbage rather than an error.
    """
    mx = np.maximum(tile[:, :, 0], tile[:, :, 2])
    green_ratio = tile[:, :, 1] / np.maximum(mx, 1.0)
    return (green_ratio < 0.35) & (mx > 20)


class Geometry(object):
    """Where everything is, in this tile's pixels."""

    def __init__(self, y0, y1, x0, x1):
        self.y0, self.y1, self.x0, self.x1 = y0, y1, x0, x1
        self.plate_w = x1 - x0 + 1
        self.plate_h = y1 - y0 + 1
        self.ppu = self.plate_w / BACKDROP_W          # pixels per world unit
        self.cy = y0 + self.plate_h / 2.0             # spheres sit at world y = 0
        self.r_px = RADIUS * self.ppu
        cx0 = x0 + self.plate_w / 2.0                 # world x = 0
        self.centres = [cx0 + (i - (STEPS - 1) / 2.0) * SPACING * self.ppu
                        for i in range(STEPS)]

    def aspect_error(self):
        """How far the plate's measured aspect is from the asset's, as a ratio.

        A cheap independent check that the located rectangle really is the plate:
        it is 12 x 7 units, so a square-pixel capture must show that aspect.
        """
        return abs((self.plate_w / self.plate_h) / (BACKDROP_W / BACKDROP_H) - 1.0)

    def row_baseline(self, tile):
        """Per-row backdrop value from the plate's OUTER MARGINS, shape (h, 3).

        NOT a median across the full plate row. On the sphere row the spheres
        span more than half the plate's width, so a full-row median is dragged
        toward the very thing being measured — the contamination that produced
        every wrong number in issue #75, understating the defect by about an
        order of magnitude. The margins are sphere-free by construction (spheres
        reach +-3.9 units, the plate +-6.0) and, because the ramp is a function
        of row alone, they carry the identical value.

        The two margins are sampled at widths mw and mw+1 so the COMBINED count
        is odd and the median is a real sample rather than the mean of the middle
        two — an even count otherwise puts a spurious 0.5/255 floor under every
        residual, which matters when the pass threshold is 1/255.
        """
        mw = max(3, int(self.plate_w * MARGIN_FRAC))
        left = tile[:, self.x0 + 2:self.x0 + 2 + mw]
        right = tile[:, self.x1 - 2 - mw:self.x1 - 1]
        return np.median(np.concatenate([left, right], axis=1), axis=1)


def locate(tile, what="capture"):
    """Find the plate in a float RGB tile and return a Geometry. Raises on doubt."""
    h, w = tile.shape[0], tile.shape[1]
    ys, xs = np.where(plate_mask(tile))
    if len(ys) < 1000:
        raise SystemExit(
            "%s: could not locate the backdrop plate (%d matching pixels) — is this "
            "a transmission_test capture, and did the model actually load?"
            % (what, len(ys)))

    g = Geometry(int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max()))

    # A plate clipped by the frame edge makes pixels-per-unit wrong and every
    # sphere centre with it. Say so rather than measure confidently in the wrong
    # place — that is the failure mode this module exists to remove.
    if g.x0 <= 1 or g.y0 <= 1 or g.x1 >= w - 2 or g.y1 >= h - 2:
        raise SystemExit(
            "%s: backdrop plate touches the frame edge (x %d..%d of %d, y %d..%d of "
            "%d) — it is cropped, so sphere positions cannot be derived from it. "
            "Reframe so the whole plate is visible." % (what, g.x0, g.x1, w, g.y0, g.y1, h))

    if g.aspect_error() > 0.08:
        raise SystemExit(
            "%s: located rectangle is %dx%d px (aspect %.2f), but the plate is "
            "%.0fx%.0f units (aspect %.2f) — that is not the backdrop, or the "
            "capture has non-square pixels."
            % (what, g.plate_w, g.plate_h, g.plate_w / g.plate_h,
               BACKDROP_W, BACKDROP_H, BACKDROP_W / BACKDROP_H))
    return g
