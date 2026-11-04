#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
"""Builds the application icon (assets/icon/) from the geometry below.

The mark is steppe grass bent by the wind on a flat horizon, under a pale
moon. The blades grow taller from left to right, so the grass is also a level
meter, and the last, tallest blade is clay: its red zone.

Writes app.svg (the master, on Apple's icon grid, for app.icns), app-square.svg
(nearly full-bleed, for the Windows and Linux PNGs and app.ico) and
app-small.svg (fewer, broader blades and no moon for 32 px and below
everywhere), then the PNGs, app.ico and app.icns. Python
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
# script_theme.h): walnut, the raised surface, parchment, clay and sage.
WALNUT = "#251D17"  # plugin ground
RAISED = "#3E3229"  # plugin raised surface
LINE_LIT = "#6A584A"  # between the plugin lineLit and textFaint: the rim and the horizon
CLAY = "#D97757"  # plugin accent: the meter's red zone
MOON = "#E6DCCB"  # a parchment tone
SAGE_TONES = ["#6F7B52", "#8A9866", "#A3B27C", "#BFCB9A"]  # darker .. lighter than sage (accent2)


def squircle(cx=512.0, cy=512.0, half=412.0, n=5.0, steps=240):
    """The tile: a superellipse on Apple's 824-of-1024 icon grid."""
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        pts.append((cx + half * math.copysign(abs(c) ** (2 / n), c),
                    cy + half * math.copysign(abs(s) ** (2 / n), s)))
    return "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts) + " Z"


def blade(x, base, height, width, bend, fill):
    """One blade of grass: wide at the root, pointed, bowed by the wind."""
    tip = (x + bend, base - height)
    c1 = (x - width * 0.5 + bend * 0.15, base - height * 0.55)
    c2 = (x + width * 0.5 + bend * 0.55, base - height * 0.5)
    return (f'<path d="M{x - width / 2:.1f},{base:.1f} Q{c1[0]:.1f},{c1[1]:.1f} {tip[0]:.1f},{tip[1]:.1f} '
            f'Q{c2[0]:.1f},{c2[1]:.1f} {x + width / 2:.1f},{base:.1f} Z" fill="{fill}"/>')


def icon(blades, width, low, high, left, right, base, horizon, moon, rim, half=412.0, scale=1.0):
    """`half` sizes the tile; `scale` grows the mark about the tile's centre;
    `moon` is (x, y, r) or None."""
    body = [f'<rect x="40" y="{base:.0f}" width="944" height="{horizon:.0f}" fill="{LINE_LIT}"/>']
    if moon:
        body.insert(0, f'<circle cx="{moon[0]:.0f}" cy="{moon[1]:.0f}" r="{moon[2]:.0f}" fill="{MOON}"/>')
    for i in range(blades):
        t = i / (blades - 1)
        height = low + (high - low) * t ** 1.3
        tone = SAGE_TONES[round(t * (len(SAGE_TONES) - 1))]
        body.append(blade(left + (right - left) * t, base + 1, height, width, height * 0.3,
                          CLAY if i == blades - 1 else tone))
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
        f'<g clip-path="url(#clip)"><g transform="translate(512 512) scale({scale:g}) translate(-512 -512)">',
        *body,
        "</g></g>",
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
    grass = dict(blades=9, width=50, low=130, high=470, left=210, right=690, base=724, horizon=10,
                 moon=(318, 318, 62))
    # macOS: the tile on Apple's 824-of-1024 grid, so it sits right among other Dock icons.
    master.write_text(icon(**grass, rim=5))
    # Windows and Linux draw icons nearly to the edge; Apple's margin would make
    # Steppe's look a size smaller than its neighbours.
    square.write_text(icon(**grass, rim=6, half=484, scale=1.12))
    # 32 px and below, every platform: fewer, broader blades, no moon, full canvas.
    small.write_text(icon(blades=6, width=96, low=170, high=560, left=230, right=690, base=772,
                          horizon=26, moon=None, rim=0, half=496, scale=1.14))
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
