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
        # Nominal row placement, refined by refine_row() below. These values are
        # the plate's own projection applied to the sphere row, which is only
        # correct for geometry COPLANAR with the plate. The spheres are not — see
        # refine_row's docstring — so treat these as a starting estimate, not as
        # where the spheres are.
        self.cy = y0 + self.plate_h / 2.0             # spheres sit at world y = 0
        self.r_px = RADIUS * self.ppu
        cx0 = x0 + self.plate_w / 2.0                 # world x = 0
        self.centres = [cx0 + (i - (STEPS - 1) / 2.0) * SPACING * self.ppu
                        for i in range(STEPS)]

    def refine_row(self, tile, what="capture"):
        """Replace the nominal row placement with one measured off the image.

        The nominal placement puts the sphere row at the plate's centre, at the
        plate's pixels-per-unit. Both are wrong, because the spheres sit at world
        z = 0 and the plate at z = -2.5:

          * DEPTH MAGNIFICATION. Both project through the same frustum, so the
            nearer row is magnified against the farther plate by a fixed ratio.
            Measured x1.063 on macOS/sim_display and x1.068 on Windows/Leia — and
            the pitch ratio and the radius ratio agree to within 0.3%, which is
            the mechanism's signature: spacing and radius lie in the same plane
            and must scale together. Purely geometric, so it is present on every
            platform and at every eye position.
          * OFF-AXIS EYE. A viewer that is not on the optical axis displaces the
            near row against the far plate. Additive on top of the magnification:
            25.5 px low on macOS with a fixed nominal eye, 50.5 px low on Windows
            with a live-tracked one.

        Sampling the nominal positions therefore lands partly or wholly on plate.
        That does not raise — it quietly returns residuals near zero, because
        plate matches the plate baseline by definition, which reads as a PASS on
        a build that has not been fixed. Both platforms hit this; macOS retained
        just enough overlap to keep the control's guard satisfied, Windows missed
        the spheres almost entirely and reported an opaque sphere at 3.6/255.

        So measure the row instead of predicting it. Spheres are the only thing
        inside the plate's bounds that departs from the per-row baseline, which
        makes them findable without segmenting on any material property — the
        approach that could not work here, since the spheres under test are the
        ones designed to be invisible.

        The acceptance capture is the awkward case: when the fix works, spheres
        1..STEPS-1 vanish by design and only the opaque control remains, so the
        pitch cannot be measured directly. Recover it from that one disc's
        RADIUS, which carries the same depth magnification.
        """
        base = self.row_baseline(tile)
        dev = np.abs(tile - base[:, None, :]).sum(axis=2)
        inside = np.zeros(dev.shape, dtype=bool)
        inside[self.y0 + 3:self.y1 - 2, self.x0 + 3:self.x1 - 2] = True
        # 40/255 summed over three channels: well above the plate's own dither
        # and JPEG-free PNG noise, well below any sphere's departure from it.
        blob = inside & (dev > 40.0)
        if blob.sum() < 200:
            raise SystemExit(
                "%s: found no geometry on the backdrop plate (%d departing pixels) — "
                "the opaque control sphere is never invisible, so an empty plate means "
                "the model did not load or the capture is not transmission_test."
                % (what, int(blob.sum())))

        rows = np.where(blob.sum(axis=1) > 3)[0]
        cy = (rows.min() + rows.max()) / 2.0
        half = max(4, int((rows.max() - rows.min()) * 0.6))
        band = blob[max(0, int(cy) - half):int(cy) + half + 1]

        cols = np.where(band.sum(axis=0) > 3)[0]
        runs, start, prev = [], cols[0], cols[0]
        for x in cols[1:]:
            if x - prev > 4:
                runs.append((start, prev))
                start = x
            prev = x
        runs.append((start, prev))
        runs = [r for r in runs if r[1] - r[0] > 12]
        if not runs:
            raise SystemExit(
                "%s: located a sphere row at y %.0f but could not resolve any disc in "
                "it — the row is there but not disc-shaped." % (what, cy))

        r_px = max(r[1] - r[0] for r in runs) / 2.0
        centres = [(r[0] + r[1]) / 2.0 for r in runs]
        if len(centres) == STEPS:
            # Fix pitch and origin from the extremes, which averages out any
            # single mis-segmented disc.
            pitch = (centres[-1] - centres[0]) / (STEPS - 1)
            exact = True
        else:
            # Fewer: the acceptance capture, where spheres 1..6 have vanished by
            # design and only the opaque control remains. More: something split a
            # disc or the plate carries something that is not a sphere — in which
            # case centres[-1] is NOT sphere 6 and the extremes fit would silently
            # divide the wrong span by STEPS-1. Both fall back to deriving the
            # pitch from the visible disc's RADIUS, which carries the same depth
            # magnification (see the ratio agreement in the docstring above).
            if len(centres) > STEPS:
                print("  WARNING: resolved %d discs on the sphere row, expected %d — "
                      "deriving pitch from radius instead of from the extremes"
                      % (len(centres), STEPS))
            pitch = SPACING * self.ppu * (r_px / (RADIUS * self.ppu))
            exact = False

        # GUARD: radius and pitch are locked by the scene. Both lie in the z = 0
        # plane, so whatever magnification the row carries against the plate
        # scales them identically and pitch / r_px must equal SPACING / RADIUS.
        # A fit that has picked up something which is not the sphere row — a
        # merged pair, an overlay, a reflection — generally breaks that ratio
        # even when it produces plausible-looking centres.
        #
        # Only meaningful on the extremes fit: in the radius-derived branch the
        # ratio holds by construction and the check is vacuous. That branch is
        # covered instead by the opaque control's absolute guard in
        # check_transmission_probe.py, and by the fact that a capture with only
        # one visible disc has, by definition, no other sphere to mislocate onto.
        ratio = (pitch / r_px) / (SPACING / RADIUS)
        if exact and abs(ratio - 1.0) > 0.10:
            raise SystemExit(
                "%s: sphere row is not self-consistent — pitch %.1f px against radius "
                "%.1f px is a ratio of %.2f, but the scene locks it at %.2f (%+.0f%%). "
                "Whatever was located is not the sphere row."
                % (what, pitch, r_px, pitch / r_px, SPACING / RADIUS, (ratio - 1.0) * 100))

        self.cy, self.r_px = cy, r_px
        self.centres = [centres[0] + i * pitch for i in range(STEPS)]
        return self

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

    # The plate fixes the frame of reference; it does not fix where the spheres
    # land in it, because they are not coplanar with it.
    return g.refine_row(tile, what)
