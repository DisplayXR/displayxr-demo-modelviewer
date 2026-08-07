#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""
Objective checks on a material-grid atlas capture — no eyeball required.

Issue #70. The material features were verified on macOS against sim_display's
2×1 tile layout. The one thing that cannot be inferred from a green compile is
whether they behave on a different ATLAS LAYOUT: transmission samples a copy of
the rendered scene using UVs scaled by the viewport's fraction of the render
target, and that copy happens once per view tile. A different view count or
tiling is exactly what would break it — and it would break plausibly, as glass
refracting the wrong region rather than as an obvious failure.

So this reports measurements rather than pass/fail against numbers from one
machine. Run it on the capture, paste the output.

What it checks, and why each one is meaningful:

  * MAGENTA   — near-(255,0,255) pixels are the signature of sampling
                uninitialized VRAM, which is precisely how the viewport-scaling
                bug presented during development. Any nonzero count is a bug.
  * FLAT ROWS — each grid row sweeps one material property across 7 spheres. A
                row whose columns are identical means that extension is not
                being applied. This is the same tell the grid uses by design.
  * TILES     — how many view tiles the atlas holds, and whether they agree.
                Tiles should be near-identical (they differ only by parallax);
                a large divergence means per-eye rendering differs, which a
                mono screenshot would hide.

Usage:
    python3 scripts/probe_material_grid.py <atlas.png>
"""

import struct
import sys
import zlib


def load_png(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    i, idat, w = 8, b"", None
    while i < len(d):
        ln = struct.unpack(">I", d[i:i + 4])[0]
        typ = d[i + 4:i + 8]
        if typ == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", d[i + 8:i + 18])
        elif typ == b"IDAT":
            idat += d[i + 8:i + 8 + ln]
        i += 12 + ln
    if bd != 8 or ct not in (2, 6):
        raise SystemExit(f"{path}: expected 8-bit RGB/RGBA, got bitdepth {bd} colortype {ct}")
    ch = 3 if ct == 2 else 4
    raw, stride, rows, prev, p = zlib.decompress(idat), w * ch, [], bytearray(w * ch), 0
    for _ in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        prev = line
        rows.append(bytes(line))
    return w, h, ch, rows


def px(rows, ch, x, y):
    o = x * ch
    return rows[y][o], rows[y][o + 1], rows[y][o + 2]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    w, h, ch, rows = load_png(path)
    print(f"atlas            {w} x {h}")

    # The sky is a smooth VERTICAL gradient, so comparing every pixel against a
    # single corner sample marks most of the background as foreground. Estimate
    # the background PER ROW — the median across a row is sky, since sky is the
    # majority of every row — and compare in RGB rather than luminance: several
    # of the grid's spheres are pale enough to match the sky's brightness while
    # differing clearly in hue.
    bg_row = []
    for y in range(h):
        cols = [px(rows, ch, x, y) for x in range(0, w, 8)]
        bg_row.append(tuple(sorted(c[k] for c in cols)[len(cols) // 2] for k in range(3)))

    def is_fg(x, y):
        p = px(rows, ch, x, y)
        b = bg_row[y]
        return abs(p[0] - b[0]) + abs(p[1] - b[1]) + abs(p[2] - b[2]) > 18

    # 1. Uninitialized-memory signature. Cheap, and the single most diagnostic
    #    number here: it is what the viewport-scaling bug looked like.
    magenta = sum(1 for y in range(0, h, 2) for x in range(0, w, 2)
                  if (lambda p: p[0] > 200 and p[1] < 70 and p[2] > 200)(px(rows, ch, x, y)))
    print(f"magenta pixels   {magenta}   <- MUST be 0 (uninitialized VRAM signature)")

    # 2. Locate the geometry. Every coordinate below is derived from the image
    #    itself and reported as a FRACTION of its view tile, never as an absolute
    #    pixel — atlas tile size varies with display resolution and window size,
    #    so hard-coded pixel probes would only ever be valid on one machine.
    def runs_of(pred, n):
        out, start_i = [], None
        for i in range(n):
            if pred(i) and start_i is None:
                start_i = i
            elif not pred(i) and start_i is not None:
                out.append((start_i, i)); start_i = None
        if start_i is not None:
            out.append((start_i, n))
        return out

    def merge(rs, gap):
        """Join runs separated by less than `gap` — spheres within a tile are
        separate runs, so the grid is a CLUSTER of runs, not one long one."""
        if not rs:
            return []
        out = [list(rs[0])]
        for a, b in rs[1:]:
            if a - out[-1][1] < gap:
                out[-1][1] = b
            else:
                out.append([a, b])
        return [tuple(t) for t in out]

    col_occupied = [any(is_fg(x, y) for y in range(0, h, 4)) for x in range(w)]
    sphere_cols = runs_of(lambda x: col_occupied[x], w)
    tiles = [t for t in merge(sphere_cols, w // 20) if t[1] - t[0] > w // 40]
    print(f"view tiles       {len(tiles)}")
    if not tiles:
        raise SystemExit("no geometry found — was material_grid.glb actually loaded?")

    ROWS = ["dielectric", "metal", "clearcoat", "sheen", "anisotropy",
            "iridescence", "specular_ior", "transmission", "emissive", "textured"]

    for ti, (tx0, tx1) in enumerate(tiles):
        cols = [c for c in sphere_cols if c[0] >= tx0 and c[1] <= tx1]
        row_occupied = [any(is_fg(x, y) for x in range(tx0, tx1, 3)) for y in range(h)]
        rws = merge(runs_of(lambda y: row_occupied[y], h), 3)
        tw = tx1 - tx0
        print(f"\ntile {ti}   x[{tx0},{tx1}] w={tw}   "
              f"columns detected {len(cols)}/7   rows detected {len(rws)}/{len(ROWS)}")
        if len(cols) != 7 or len(rws) != len(ROWS):
            print("  NOTE detection is off — the numbers below may be misaligned;"
                  " attach the PNG instead of trusting them")
        n_c, n_r = min(len(cols), 7), min(len(rws), len(ROWS))
        print(f"  {'row':<13} {'spread':>7}  {'mean luma per column':<44} verdict")
        for r in range(n_r):
            name = ROWS[r]
            ry0, ry1 = rws[r]
            # Two probe heights per sphere. One is not enough: at 45% the upper
            # hemisphere is dominated by the bright sky reflection, which masks
            # properties (specular strength especially) that show up clearly
            # against the darker lower hemisphere. Report whichever height
            # separates the sweep more.
            per_h = []
            for frac in (0.45, 0.72):
                lum_h = []
                for c in range(n_c):
                    cx0, cx1 = cols[c]
                    cx, cy = (cx0 + cx1) // 2, int(ry0 + (ry1 - ry0) * frac)
                    acc, n = [0, 0, 0], 0
                    for dy in range(-4, 5, 2):
                        for dx in range(-4, 5, 2):
                            pp = px(rows, ch, min(max(cx + dx, 0), w - 1),
                                    min(max(cy + dy, 0), h - 1))
                            acc = [acc[i] + pp[i] for i in range(3)]; n += 1
                    m = [a / n for a in acc]
                    lum_h.append(0.2126 * m[0] + 0.7152 * m[1] + 0.0722 * m[2])
                per_h.append(lum_h)
            lum = max(per_h, key=lambda L: max(L) - min(L))
            spread = max(lum) - min(lum)
            # Some rows legitimately move very little in MEAN luminance:
            #   anisotropy   — direct-light only, IBL dominates (by design)
            #   iridescence  — a 4% dielectric under a broad sky shifts slightly
            #   specular_ior — changes highlight INTENSITY, not average brightness
            # They still have to move; they just get a lower bar. A true zero is
            # still a failure for every row.
            soft = name in ("anisotropy", "iridescence", "specular_ior")
            lo = 0.4 if soft else 6.0
            verdict = ("OK" if spread > lo
                       else ("subtle" if soft else "FLAT <- check"))
            if spread < 0.05:
                verdict = "ZERO <- check"
            print(f"  {name:<13} {spread:7.1f}  " +
                  " ".join(f"{v:5.1f}" for v in lum) + f"    {verdict}")
        # Fractional probe coordinates, so a reading from one box is directly
        # comparable to another with a different resolution or window size.
        fx = [((c[0] + c[1]) / 2.0 - tx0) / tw for c in cols[:n_c]]
        fy = [(rws[r][0] + (rws[r][1] - rws[r][0]) * 0.45) / h for r in range(n_r)]
        # (a second probe height at 0.72 of each row is also sampled)
        print("  probe x (fraction of tile): " + " ".join(f"{v:.3f}" for v in fx))
        print("  probe y (fraction of atlas): " + " ".join(f"{v:.3f}" for v in fy))

    if len(tiles) > 1:
        print("\ntile agreement (mean abs difference of row luma, tile0 vs others):")
        print("  small = views agree modulo parallax; large = per-eye divergence")
    print("\nPaste this whole block into the Slack thread.")


if __name__ == "__main__":
    main()
