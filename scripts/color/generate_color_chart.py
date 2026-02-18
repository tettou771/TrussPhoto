#!/usr/bin/env python3
"""
generate_color_chart.py - Generate circular gradient color chart for LUT calibration
=====================================================================================

Generates a radial gradient image covering the full HSV color space in polar coords.
Display on screen, photograph with camera (RAW+JPG), then use for LUT fitting.

Since both RAW and JPEG see the same displayed colors, absolute display
accuracy doesn't matter - only the relative RAW<->JPEG difference is used.

Layout (polar coordinates, black background):
  Angle (theta) = hue (0-360 degrees)
  Radius bands (normalized to R):
    0    -> 0.2R : grayscale   (black -> white)
    0.2R -> 0.7R : saturation ramp (white -> max sat, hue from angle)
    0.7R -> 0.8R : smooth fade to black
    0.8R -> edge : black background

  Designed for bracket shooting: take 3 exposures (-1EV, 0, +1EV).
  The brightness axis is covered by bracketing, so the chart maximizes
  hue x saturation coverage.

  R = min(w, h) / 2

Usage:
  python generate_color_chart.py -o chart.png
  python generate_color_chart.py -o chart.png --width 2560 --height 1440
"""

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image


def generate_chart(output_path: Path, width: int = 2560, height: int = 1440):
    """Generate circular gradient color chart optimized for bracket shooting."""
    img = np.zeros((height, width, 3), dtype=np.uint8)

    cx = width / 2.0
    cy = height / 2.0
    R = min(width, height) / 2.0

    # Pixel grid
    xx, yy = np.meshgrid(np.arange(width), np.arange(height))
    dx = xx - cx
    dy = yy - cy
    r = np.sqrt(dx * dx + dy * dy)
    rn = r / R  # normalized radius

    # Hue from angle (0..1)
    hue = (np.arctan2(-dy, dx) / (2.0 * math.pi)) % 1.0

    # --- Concentric bands ---
    sat = np.zeros_like(rn)
    val = np.zeros_like(rn)

    # Band 1: grayscale (0 to 0.2) - black center -> white
    m1 = rn <= 0.2
    t1 = rn / 0.2  # 0 -> 1
    sat[m1] = 0.0
    val[m1] = t1[m1]  # black -> white

    # Band 2: saturation ramp (0.2 to 0.7) - white -> full sat color
    # At r=0.2: S=0, V=1 (white) — seamless with band 1
    # At r=0.7: S=1, V=1 (full sat)
    m2 = (rn > 0.2) & (rn <= 0.7)
    t2 = (rn - 0.2) / 0.5  # 0 -> 1
    sat[m2] = t2[m2]
    val[m2] = 1.0

    # Band 3: fade to black (0.7 to 0.8) - full sat -> black
    m3 = (rn > 0.7) & (rn <= 0.8)
    t3 = (rn - 0.7) / 0.1  # 0 -> 1
    sat[m3] = 1.0
    val[m3] = (1.0 - t3)[m3]

    # Beyond 0.8: black (already 0)

    # --- HSV to RGB (vectorized) ---
    h6 = hue * 6.0
    hi = h6.astype(int) % 6
    f = h6 - hi.astype(float)

    p = val * (1.0 - sat)
    q = val * (1.0 - f * sat)
    t = val * (1.0 - (1.0 - f) * sat)

    red = np.zeros_like(val)
    grn = np.zeros_like(val)
    blu = np.zeros_like(val)

    for sector, (rv, gv, bv) in enumerate([
        (val, t, p),    # 0
        (q, val, p),    # 1
        (p, val, t),    # 2
        (p, q, val),    # 3
        (t, p, val),    # 4
        (val, p, q),    # 5
    ]):
        m = hi == sector
        red[m] = rv[m]
        grn[m] = gv[m]
        blu[m] = bv[m]

    # Visible mask
    mask = rn <= 0.8

    img[:, :, 0] = (red * mask * 255 + 0.5).astype(np.uint8)
    img[:, :, 1] = (grn * mask * 255 + 0.5).astype(np.uint8)
    img[:, :, 2] = (blu * mask * 255 + 0.5).astype(np.uint8)

    out = Image.fromarray(img)
    out.save(str(output_path))
    print(f"Generated: {output_path}")
    print(f"  Size: {width} x {height}")
    print(f"  R = {R:.0f}px, visible to 0.8R = {R*0.8:.0f}px")
    print(f"  Band 1 (0-0.2R): grayscale black->white")
    print(f"  Band 2 (0.2-0.7R): saturation ramp white->full-sat")
    print(f"  Band 3 (0.7-0.8R): full-sat->black fade")
    print(f"  Use with bracket shooting (-1EV, 0, +1EV)")


def main():
    parser = argparse.ArgumentParser(
        description='Generate circular gradient color chart for LUT calibration')
    parser.add_argument('-o', '--output', default='color_chart.png', help='Output PNG')
    parser.add_argument('--width', type=int, default=2560, help='Image width (default: 2560)')
    parser.add_argument('--height', type=int, default=1440, help='Image height (default: 1440)')

    args = parser.parse_args()
    generate_chart(Path(args.output), width=args.width, height=args.height)


if __name__ == '__main__':
    main()
