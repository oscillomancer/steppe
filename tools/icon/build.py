#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
"""Builds the application icon (assets/icon/) from the geometry below.

The mark is a quire stood on its spine with its leaves fanned open, which is
also an analog level meter: the leaves are the scale, the outer right leaf is
its red zone, and a clay needle swings from the sewn fold, its pivot.

Writes app.svg (the master, on Apple's icon grid, for app.icns), app-square.svg
(nearly full-bleed, for the Windows and Linux PNGs and app.ico) and
app-small.svg (fewer, broader leaves for 32 px and below everywhere), then the
PNGs, app.ico and app.icns. Python
standard library only; rasterizes with resvg (or rsvg-convert). The output is
deterministic: rerun it after changing the geometry and commit the results.

    python3 tools/icon/build.py [--out assets/icon]
"""
import argparse
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SIZE = 1024
# Colours from the stock plugin theme (plugins/suite/glue/include/supple/suite/
# script_theme.h) and the GUI theme (daw/gui/ui/src/theme.cpp).
WALNUT = "#251D17"  # plugin ground
RAISED = "#3E3229"  # plugin raised surface
LINE_LIT = "#6A584A"  # between the plugin lineLit and textFaint: the rim
PIVOT_GAP = "#2A211A"
CLAY = "#D97757"  # plugin accent
MASTER = "#C0634F"  # GUI master-strip red: the meter's red zone
LEAF_TONES = ["#BBAA96", "#D3C6B3", "#E6DCCB", "#F4ECDF"]  # textMuted .. parchment


def squircle(cx=512.0, cy=512.0, half=412.0, n=5.0, steps=240):
    """The tile: a superellipse on Apple's 824-of-1024 icon grid."""
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        pts.append((cx + half * math.copysign(abs(c) ** (2 / n), c),
                    cy + half * math.copysign(abs(s) ** (2 / n), s)))
    return "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts) + " Z"


def leaf(spine, angle, length, tip_width, fill):
    """One leaf seen end-on: tapering into the fold, bowing as paper does."""
    r = math.radians(angle)
    px, py = math.cos(r), math.sin(r)  # across the leaf
    ex, ey = spine[0] + length * math.sin(r), spine[1] - length * math.cos(r)
    c = math.radians(angle * 0.5)  # the bow: halfway between the leaf and upright
    cx, cy = spine[0] + length * 0.6 * math.sin(c), spine[1] - length * 0.6 * math.cos(c)
    k = tip_width * 0.35
    a = (ex + px * tip_width / 2, ey + py * tip_width / 2)
    b = (ex - px * tip_width / 2, ey - py * tip_width / 2)
    return (f'<path d="M{spine[0]:.1f},{spine[1]:.1f} Q{cx + px * k:.1f},{cy + py * k:.1f} '
            f'{a[0]:.1f},{a[1]:.1f} L{b[0]:.1f},{b[1]:.1f} Q{cx - px * k:.1f},{cy - py * k:.1f} '
            f'{spine[0]:.1f},{spine[1]:.1f} Z" fill="{fill}"/>')


def needle(spine, angle, length, width, fill):
    r = math.radians(angle)
    px, py = math.cos(r), math.sin(r)
    ex, ey = spine[0] + length * math.sin(r), spine[1] - length * math.cos(r)
    tip = width * 0.14
    pts = [(spine[0] + px * width / 2, spine[1] + py * width / 2), (ex + px * tip, ey + py * tip),
           (ex - px * tip, ey - py * tip), (spine[0] - px * width / 2, spine[1] - py * width / 2)]
    d = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts) + " Z"
    return f'<path d="{d}" fill="{fill}" stroke="{fill}" stroke-width="{tip * 1.2:.1f}" stroke-linejoin="round"/>'


def icon(leaves, tip_width, needle_width, pivot, needle_angle, rim, half=412.0, scale=1.0):
    """`half` sizes the tile; `scale` grows the mark about the tile's centre."""
    spine, reach, span = (512.0, 716.0), 416.0, 74.0
    angles = [-span + 2 * span * i / (leaves - 1) for i in range(leaves)]
    body = []
    for a in sorted(angles, key=lambda a: -abs(a)):  # outer leaves first, inner on top
        tone = LEAF_TONES[round((1 - abs(a) / span) * (len(LEAF_TONES) - 1))]
        body.append(leaf(spine, a, reach - abs(a) * 0.4, tip_width,
                         MASTER if a == angles[-1] else tone))
    body.append(f'<circle cx="{spine[0]:.0f}" cy="{spine[1]:.0f}" r="{pivot + 14:.0f}" fill="{PIVOT_GAP}"/>')
    body.append(needle(spine, needle_angle, reach + 18, needle_width, CLAY))
    body.append(f'<circle cx="{spine[0]:.0f}" cy="{spine[1]:.0f}" r="{pivot:.0f}" fill="{CLAY}"/>')
    tile = squircle(half=half)
    return "\n".join([
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}">',
        "<defs>",
        f'<linearGradient id="tile" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{RAISED}"/>'
        f'<stop offset="1" stop-color="{WALNUT}"/></linearGradient>',
        f'<clipPath id="clip"><path d="{tile}"/></clipPath>',
        "</defs>",
        f'<path d="{tile}" fill="url(#tile)"/>',
        *([f'<path d="{tile}" fill="none" stroke="{LINE_LIT}" stroke-width="{rim}"/>'] if rim else []),
        f'<g clip-path="url(#clip)" transform="translate(512 {512 + 40 * (scale - 1):.0f}) '
        f'scale({scale:g}) translate(-512 -512)">',
        *body,
        "</g>",
        "</svg>",
        "",
    ])


PNG_SIZES = [16, 24, 32, 48, 64, 128, 256, 512, 1024]
SMALL_MAX = 32  # sizes up to this use app-small.svg
ICO_SIZES = [16, 24, 32, 48, 64, 256]
# ICNS element types and the PNG size each holds.
ICNS_TYPES = [(b"icp4", 16), (b"icp5", 32), (b"ic07", 128), (b"ic08", 256), (b"ic09", 512),
              (b"ic10", 1024), (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512)]


def rasterize(svg: Path, png: Path, size: int):
    if shutil.which("resvg"):
        cmd = ["resvg", "-w", str(size), "-h", str(size), str(svg), str(png)]
    elif shutil.which("rsvg-convert"):
        cmd = ["rsvg-convert", "-w", str(size), "-h", str(size), "-o", str(png), str(svg)]
    else:
        sys.exit("build.py: needs resvg or rsvg-convert")
    subprocess.run(cmd, check=True)


def write_ico(path: Path, pngs: dict):
    # Every entry stored as PNG, which Windows Vista and later read at any size.
    header = struct.pack("<HHH", 0, 1, len(ICO_SIZES))
    entries, blobs, offset = b"", b"", 6 + 16 * len(ICO_SIZES)
    for size in ICO_SIZES:
        data = pngs[size]
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    path.write_bytes(header + entries + blobs)


def write_icns(path: Path, pngs: dict):
    body = b"".join(kind + struct.pack(">I", 8 + len(pngs[size])) + pngs[size]
                    for kind, size in ICNS_TYPES)
    path.write_bytes(b"icns" + struct.pack(">I", 8 + len(body)) + body)


def check_icns(path: Path):
    data = path.read_bytes()
    assert data[:4] == b"icns" and struct.unpack(">I", data[4:8])[0] == len(data)
    pos, seen = 8, []
    while pos < len(data):
        kind, length = data[pos:pos + 4], struct.unpack(">I", data[pos + 4:pos + 8])[0]
        assert data[pos + 8:pos + 16] == b"\x89PNG\r\n\x1a\n", kind
        seen.append(kind)
        pos += length
    assert pos == len(data) and seen == [k for k, _ in ICNS_TYPES]


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=root / "assets" / "icon")
    out = parser.parse_args().out
    (out / "png").mkdir(parents=True, exist_ok=True)
    master, square, small = out / "app.svg", out / "app-square.svg", out / "app-small.svg"
    # macOS: the tile on Apple's 824-of-1024 grid, so it sits right among other Dock icons.
    master.write_text(icon(leaves=7, tip_width=84, needle_width=36, pivot=34, needle_angle=12, rim=5))
    # Windows and Linux draw icons nearly to the edge; Apple's margin would make
    # Steppe's look a size smaller than its neighbours.
    square.write_text(icon(leaves=7, tip_width=84, needle_width=36, pivot=34, needle_angle=12, rim=6,
                           half=484, scale=1.14))
    # 32 px and below, every platform: fewer, broader leaves, full canvas.
    small.write_text(icon(leaves=5, tip_width=140, needle_width=80, pivot=60, needle_angle=14, rim=0,
                          half=496, scale=1.16))
    def render(svg, sizes):
        with tempfile.TemporaryDirectory() as tmp:
            out_pngs = {}
            for size in sizes:
                png = Path(tmp) / f"{size}.png"
                rasterize(small if size <= SMALL_MAX else svg, png, size)
                out_pngs[size] = png.read_bytes()
            return out_pngs
    desktop = render(square, PNG_SIZES)
    for size in PNG_SIZES:
        (out / "png" / f"app-{size}.png").write_bytes(desktop[size])
    write_ico(out / "app.ico", desktop)
    write_icns(out / "app.icns", render(master, sorted({s for _, s in ICNS_TYPES})))
    check_icns(out / "app.icns")


if __name__ == "__main__":
    main()
