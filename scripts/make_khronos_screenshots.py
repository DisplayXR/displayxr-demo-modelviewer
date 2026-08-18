#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Turn a viewer atlas capture into the screenshot Khronos asks a model to ship.

glTF-Sample-Assets CONTRIBUTING wants a screenshot "roughly 150 to 500 pixels"
wide and "not extremely elongated in one direction". What the viewer's 'I' key
writes is neither: it is the multi-view ATLAS, tile_cols x tile_rows tiles side
by side (2x1 here), so a raw capture is ~3.6:1 and shows the same scene twice.

So: take ONE tile, trim it to the content, cap the aspect, and downscale.

Taking one tile is the correct reduction rather than a crop of convenience. The
tiles are per-eye views of the same scene; a reader wants the scene, and two
near-identical copies side by side would just read as a rendering artefact to
anyone outside this project.

Usage:
    python scripts/make_khronos_screenshots.py [--outdir DIR]

Reads the newest matching capture per model from the viewer's capture folder
(%USERPROFILE%/Pictures/DisplayXR by default, --captures to override) and writes
<outdir>/Models/<Name>/screenshot/{screenshot.png,screenshot_large.png}.
"""

import argparse
import os
import sys
from pathlib import Path

from PIL import Image

MODELS = [
    "CoatParameterSweep",
    "FuzzParameterSweep",
    "DiffuseRoughnessParameterSweep",
]

TARGET_W = 480          # inside Khronos' 150-500 guidance, with headroom
LARGE_W = 1280          # the "additional (larger) screenshot" the README body can use
MAX_ASPECT = 2.0        # "not extremely elongated"; pad rather than crop to reach it
MARGIN = 0.06           # fraction of the content box added back as breathing room
SKY_TOL = 10            # per-channel deviation from the row's sky reference


def content_bbox(img):
    """Bounding box of everything that is not the procedural sky.

    The sky is a smooth VERTICAL gradient, so a single background colour does not
    describe it — a global median would clip the top or the bottom depending on
    which end it landed near. Instead each row gets its own reference, taken from
    that row's own left and right edges, which are sky in every framing we emit.
    """
    px = img.convert("RGB").load()
    w, h = img.size
    edge = max(4, w // 40)
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        ref = []
        for x in list(range(edge)) + list(range(w - edge, w)):
            ref.append(px[x, y])
        ref_r = sorted(c[0] for c in ref)[len(ref) // 2]
        ref_g = sorted(c[1] for c in ref)[len(ref) // 2]
        ref_b = sorted(c[2] for c in ref)[len(ref) // 2]
        for x in range(w):
            r, g, b = px[x, y]
            if (abs(r - ref_r) > SKY_TOL or abs(g - ref_g) > SKY_TOL
                    or abs(b - ref_b) > SKY_TOL):
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if maxx < 0:
        return None
    return minx, miny, maxx, maxy


def frame(img):
    """One tile -> a trimmed, aspect-capped crop."""
    w, h = img.size
    box = content_bbox(img)
    if box is None:
        return img
    minx, miny, maxx, maxy = box
    cw, ch = maxx - minx + 1, maxy - miny + 1
    mx, my = int(cw * MARGIN), int(ch * MARGIN)
    minx, maxx = minx - mx, maxx + mx
    miny, maxy = miny - my, maxy + my
    cw, ch = maxx - minx + 1, maxy - miny + 1

    # Cap the aspect by GROWING the short axis, never by cutting the long one:
    # cropping content out of a conformance asset to hit a ratio would be lying
    # about what the asset contains.
    if cw > ch * MAX_ASPECT:
        want = cw / MAX_ASPECT
        grow = int((want - ch) / 2)
        miny, maxy = miny - grow, maxy + grow
    elif ch > cw * MAX_ASPECT:
        want = ch / MAX_ASPECT
        grow = int((want - cw) / 2)
        minx, maxx = minx - grow, maxx + grow

    # Clamp back inside the tile; the sky beyond the content is a fine backdrop.
    minx, miny = max(0, minx), max(0, miny)
    maxx, maxy = min(w - 1, maxx), min(h - 1, maxy)
    return img.crop((minx, miny, maxx + 1, maxy + 1))


def newest_capture(capdir, name):
    hits = sorted(Path(capdir).glob(name + "-*_atlas_*.png"),
                  key=lambda p: p.stat().st_mtime, reverse=True)
    return hits[0] if hits else None


def main():
    repo = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default=str(repo / "khronos_submission"))
    ap.add_argument("--captures",
                    default=str(Path(os.environ.get("USERPROFILE", Path.home()))
                                / "Pictures" / "DisplayXR"))
    args = ap.parse_args()

    rc = 0
    for name in MODELS:
        cap = newest_capture(args.captures, name)
        if cap is None:
            print("MISSING capture for %s in %s" % (name, args.captures))
            rc = 1
            continue
        atlas = Image.open(cap)
        aw, ah = atlas.size
        # The capture is the CONTENT REGION: tile_cols*view_w by tile_rows*view_h.
        # Layout is 2x1, so one tile is the left half.
        tile = atlas.crop((0, 0, aw // 2, ah))
        shot = frame(tile)

        outdir = Path(args.outdir) / "Models" / name / "screenshot"
        outdir.mkdir(parents=True, exist_ok=True)

        sw, sh = shot.size
        small = shot.resize((TARGET_W, max(1, round(sh * TARGET_W / sw))),
                            Image.LANCZOS)
        small.save(outdir / "screenshot.png", optimize=True)
        large = shot.resize((LARGE_W, max(1, round(sh * LARGE_W / sw))),
                            Image.LANCZOS)
        large.save(outdir / "screenshot_large.png", optimize=True)

        print("%-32s %s  tile=%dx%d  crop=%dx%d  ->  %dx%d" %
              (name, cap.name, aw // 2, ah, sw, sh, small.size[0], small.size[1]))
    return rc


if __name__ == "__main__":
    sys.exit(main())
