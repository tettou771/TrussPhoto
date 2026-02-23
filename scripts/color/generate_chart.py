#!/usr/bin/env python3
"""
generate_chart.py - Generate a discrete color patch chart for camera profiling
==============================================================================

Creates a grid of uniform color patches covering hue × saturation space,
plus grayscale steps. Designed to be displayed on a monitor and photographed.

Brightness variation is minimal (covered by bracket shooting instead).
Black borders between patches enable clean sigma-clipping of boundary pixels.

Usage:
  python generate_chart.py                    # default output: chart.png
  python generate_chart.py -o my_chart.png
  python generate_chart.py --patch-size 60    # larger patches
"""

import argparse
import colorsys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def generate_patches():
    """Generate list of (R, G, B) patches in 0-255.
    Covers hue × saturation with grayscale, prioritizing chromaticity coverage."""

    patches = []

    # --- Chromatic patches ---
    # Hues: 30 steps (12° each) for fine hue resolution
    hues = [i / 30.0 for i in range(30)]

    # Saturation levels: emphasize low saturation (skin tones, pastels)
    saturations = [0.08, 0.15, 0.25, 0.40, 0.60, 0.85, 1.0]

    # Single brightness level (bracketing covers the rest)
    value = 0.85

    for h in hues:
        for s in saturations:
            r, g, b = colorsys.hsv_to_rgb(h, s, value)
            patches.append((int(r * 255), int(g * 255), int(b * 255)))

    # --- Grayscale ---
    gray_steps = 12
    for i in range(gray_steps):
        v = int(255 * i / (gray_steps - 1))
        patches.append((v, v, v))

    return patches


def build_chart(patches, patch_size=48, border=14, cols=None, margin_pct=12):
    """Build square chart image from list of RGB tuples.
    Patches arranged in a square grid, centered with black margin.
    margin_pct: margin as percentage of total image size on each side.
    Returns PIL Image."""

    n = len(patches)
    if cols is None:
        # Square grid: find cols so rows ≈ cols
        cols = int(np.ceil(np.sqrt(n)))
    rows = int(np.ceil(n / cols))

    # Make grid square (pad rows if needed)
    if rows < cols:
        rows = cols

    # Grid content size
    cell = patch_size + border
    grid_w = cols * cell + border
    grid_h = rows * cell + border

    # Total image is square with margin
    content_size = max(grid_w, grid_h)
    # margin_pct on each side → content is (100 - 2*margin_pct)% of total
    total_size = int(content_size / (1.0 - 2.0 * margin_pct / 100.0))

    img = Image.new('RGB', (total_size, total_size), (0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Center the grid
    ox = (total_size - grid_w) // 2
    oy = (total_size - grid_h) // 2

    for idx, (r, g, b) in enumerate(patches):
        row = idx // cols
        col = idx % cols
        x0 = ox + border + col * cell
        y0 = oy + border + row * cell
        x1 = x0 + patch_size - 1
        y1 = y0 + patch_size - 1
        draw.rectangle([x0, y0, x1, y1], fill=(r, g, b))

    return img


def main():
    parser = argparse.ArgumentParser(
        description='Generate discrete color patch chart for camera profiling')
    parser.add_argument('-o', '--output', default='chart.png',
                        help='Output image path (default: chart.png)')
    parser.add_argument('--patch-size', type=int, default=48,
                        help='Patch size in pixels (default: 48)')
    parser.add_argument('--border', type=int, default=14,
                        help='Black border width between patches (default: 14)')
    parser.add_argument('--cols', type=int, default=None,
                        help='Number of columns (default: auto)')

    args = parser.parse_args()

    patches = generate_patches()

    # Shuffle with fixed seed for reproducibility
    rng = np.random.default_rng(42)
    indices = list(range(len(patches)))
    rng.shuffle(indices)
    patches = [patches[i] for i in indices]

    # Summary
    n_chromatic = 30 * 7
    n_gray = 12
    print(f"Patches: {len(patches)} total")
    print(f"  Chromatic: {n_chromatic} (30 hues × 7 saturations)")
    print(f"  Grayscale: {n_gray} steps")
    print(f"  Patch size: {args.patch_size}px, border: {args.border}px")

    img = build_chart(patches, args.patch_size, args.border, args.cols)
    print(f"  Chart size: {img.width} × {img.height}")

    out = Path(args.output)
    img.save(out)
    print(f"  Written: {out}")


if __name__ == '__main__':
    main()
