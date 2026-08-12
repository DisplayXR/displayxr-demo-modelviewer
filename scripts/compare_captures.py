#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Diff two atlas captures. Answers "did this change the render, and where?".

Captures from scripts/capture_reference.sh are bit-exact reproducible — the same
build captured twice gives mean 0.000 — so this is a real equality test, not a
similarity score. A mean above --tol is a genuine change; 0.000 is a genuine
identity. That property is what makes it worth having a tool at all.

It exists because it kept being needed and kept being rebuilt by hand. Every
number in issues #79 and #81 ("mean 38.2 across 99.99 % of pixels", "0.000")
came from an ad-hoc script that was not kept, so none of those claims could be
re-checked by the next person. Now they can:

    ./scripts/build_macos.sh
    SIM_DISPLAY_OUTPUT=2d ./scripts/capture_reference.sh assets/material_grid.glb /tmp/cap-before --wait 16
    ...make the change, rebuild...
    SIM_DISPLAY_OUTPUT=2d ./scripts/capture_reference.sh assets/material_grid.glb /tmp/cap-after --wait 16
    python3 scripts/compare_captures.py /tmp/cap-before/material_grid.png \
                                        /tmp/cap-after/material_grid.png --per-row

SIM_DISPLAY_OUTPUT=2d IS REQUIRED for measurement. The app defaults to the first
3D mode, and anaglyph combines the views per colour channel — it destroys
exactly the thing being measured.

--per-row attributes the diff to the material_grid.glb sweep rows, so "the coat
change moved the clearcoat row and nothing else" is a direct reading rather than
an inference from a whole-image mean. Row geometry comes from
probe_material_grid.py, which already locates it; this does not re-derive it.

Usage:
    python3 scripts/compare_captures.py <before.png> <after.png> [--per-row] [--tol 0.01]

Exit code is 1 if the mean absolute difference exceeds --tol (default 0.01), so
this can gate a "must be an exact identity" step in a bisect.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_material_grid as pmg   # noqa: E402  (path set above)

try:
    import numpy as np
except ImportError:
    raise SystemExit("numpy required: python3 -m pip install numpy")


def load_rgb(path):
    """Return an (h, w, 3) uint8 array.

    Pillow when available (fast, and check_transmission_probe.py already depends
    on it); otherwise the pure-stdlib decoder in probe_material_grid.py, so this
    still runs on a box with numpy but no Pillow.
    """
    try:
        from PIL import Image
        return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
    except ImportError:
        w, h, ch, rows = pmg.load_png(path)
        a = np.frombuffer(b"".join(rows), dtype=np.uint8).reshape(h, w, ch)
        return np.ascontiguousarray(a[:, :, :3])


def stats(a, b):
    """Mean/max absolute difference and the count of differing channels.

    int16 rather than uint8: the difference of two uint8s wraps, which would
    report a 1-level change as 255.
    """
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return float(d.mean()), int(d.max()), int(np.count_nonzero(d)), d.size


def main():
    args = [a for a in sys.argv[1:]]
    if "-h" in args or "--help" in args:
        raise SystemExit(__doc__)
    per_row = "--per-row" in args
    if per_row:
        args.remove("--per-row")
    tol = 0.01
    if "--tol" in args:
        i = args.index("--tol")
        tol = float(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    before, after = args

    a, b = load_rgb(before), load_rgb(after)
    if a.shape != b.shape:
        raise SystemExit(f"shape mismatch: {before} is {a.shape}, {after} is {b.shape} — "
                         "captures must come from the same window size to be comparable")

    mean, mx, nz, total = stats(a, b)
    print(f"before  {before}")
    print(f"after   {after}")
    print(f"atlas   {a.shape[1]} x {a.shape[0]}")
    print()
    print(f"mean |diff|          {mean:.3f}")
    print(f"max  |diff|          {mx}")
    print(f"channels differing   {nz} / {total} ({100.0 * nz / total:.2f} %)")

    if per_row:
        # Locate the grid on the BEFORE image. Captures are deterministic, so the
        # geometry is shared; using one image's bands for both keeps the rows
        # aligned even when the change under test is large enough to perturb
        # detection on the after image.
        w, h, ch, rows = pmg.load_png(before)
        is_fg, sx, sy = pmg.foreground(w, h, ch, rows)
        tiles, sphere_cols, xb, yb = pmg.detect_tiles(w, h, is_fg, sy)
        if not tiles:
            print("\nper-row: no geometry found — is this a material_grid capture?")
            return 1 if mean > tol else 0
        tile_data = pmg.detect_rows(tiles, sphere_cols, is_fg)
        print(f"\nper-row ({len(tiles)} view tile(s), {len(xb)} cols x {len(yb)} rows)")
        for ti, (tx0, tx1, ty0, ty1, cols, rws, raw_n) in enumerate(tile_data):
            print(f"  tile {ti}   x[{tx0},{tx1}] y[{ty0},{ty1}]"
                  f"   rows detected {raw_n}/{len(pmg.ROWS)}")
            print(f"    {'row':<13} {'mean':>8} {'max':>5} {'% chan':>8}")
            for r in range(min(len(rws), len(pmg.ROWS))):
                ry0, ry1 = rws[r]
                sa, sb = a[ry0:ry1, tx0:tx1], b[ry0:ry1, tx0:tx1]
                if sa.size == 0:
                    continue
                rm, rx, rnz, rtot = stats(sa, sb)
                print(f"    {pmg.ROWS[r]:<13} {rm:8.3f} {rx:5d} {100.0 * rnz / rtot:7.2f}%")

    if mean > tol:
        print(f"\nCHANGED — mean {mean:.3f} exceeds tol {tol}")
        return 1
    print(f"\nIDENTICAL within tol {tol}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
