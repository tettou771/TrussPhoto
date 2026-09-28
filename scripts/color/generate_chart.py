#!/usr/bin/env python3
"""
generate_chart.py - Generate a discrete color patch chart for camera profiling
==============================================================================

Creates a grid of uniform color patches sampled via Vogel spiral in HS space,
plus grayscale steps. Two brightness variants (white-start, gray-start) ensure
coverage across the luminance axis when combined with bracket shooting.

Black borders between patches enable clean sigma-clipping of boundary pixels.

Usage:
  python generate_chart.py                    # default output: chart.png
  python generate_chart.py -o my_chart.png
  python generate_chart.py --patch-size 60    # larger patches
  python generate_chart.py --shuffle          # randomize patch order
"""

import argparse
import colorsys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

GOLDEN_ANGLE_DEG = 137.50776405003785


def generate_patches():
    """Generate list of (R, G, B) patches in 0-255.
    Uses Vogel spiral in HS plane for uniform chromaticity coverage.
    Two brightness variants: white-start (V=0.9) and gray-start (V=0.45)."""

    patches = []

    # --- Chromatic patches via Vogel spiral ---
    # Two variants: bright and dark (bracket shooting covers intermediate)
    n_per_variant = 105

    for vi, value in enumerate([0.9, 0.45]):
        # Offset dark variant by half golden angle to avoid overlap
        angle_offset = 180.0 if vi == 1 else 0.0
        for i in range(n_per_variant):
            # Vogel spiral: golden angle rotation, sqrt radius for uniform area density
            # More absolute points at larger radius = higher saturation
            r = ((i + 0.5) / n_per_variant) ** 0.5  # saturation 0→1
            theta_deg = i * GOLDEN_ANGLE_DEG + angle_offset
            h = (theta_deg % 360) / 360.0
            s = r

            rgb = colorsys.hsv_to_rgb(h, s, value)
            patches.append(tuple(int(c * 255) for c in rgb))

    # --- Pure RGB primaries + CMY ---
    patches.append((255, 0, 0))
    patches.append((0, 255, 0))
    patches.append((0, 0, 255))
    patches.append((0, 255, 255))
    patches.append((255, 0, 255))
    patches.append((255, 255, 0))

    # --- Grayscale ---
    gray_steps = 9
    for i in range(gray_steps):
        v = int(255 * i / (gray_steps - 1))
        patches.append((v, v, v))

    return patches


def disperse_patches(patches, cols):
    """Arrange patches so adjacent cells have maximally different colors.
    Random initial placement + iterative swap refinement."""

    n = len(patches)
    rows = int(np.ceil(n / cols))
    rgb = np.array(patches, dtype=np.float32)

    # Random initial placement
    rng = np.random.default_rng(42)
    grid = list(rng.permutation(n))

    def min_neighbor_dist(pos, grid, rgb, cols, rows):
        row, col = divmod(pos, cols)
        neighbors = []
        for dr, dc in [(-1,-1),(-1,0),(-1,1),(0,-1),(0,1),(1,-1),(1,0),(1,1)]:
            nr, nc = row + dr, col + dc
            if 0 <= nr < rows and 0 <= nc < cols:
                nb = grid[nr * cols + nc]
                if nb is not None:
                    neighbors.append(nb)
        if not neighbors:
            return float('inf')
        p = rgb[grid[pos]]
        return min(np.sqrt(np.sum((p - rgb[nb]) ** 2)) for nb in neighbors)

    for _ in range(5000):
        a, b = rng.integers(0, n, size=2)
        if a == b:
            continue
        # Current worst of the two
        da = min_neighbor_dist(a, grid, rgb, cols, rows)
        db = min_neighbor_dist(b, grid, rgb, cols, rows)
        old_worst = min(da, db)
        # Try swap
        grid[a], grid[b] = grid[b], grid[a]
        da2 = min_neighbor_dist(a, grid, rgb, cols, rows)
        db2 = min_neighbor_dist(b, grid, rgb, cols, rows)
        new_worst = min(da2, db2)
        if new_worst <= old_worst:
            # Revert
            grid[a], grid[b] = grid[b], grid[a]

    return [patches[grid[i]] for i in range(n)]


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
    parser.add_argument('--shuffle', action='store_true',
                        help='Shuffle patch order (recommended for shooting)')

    args = parser.parse_args()

    patches = generate_patches()

    if args.shuffle:
        patches = disperse_patches(patches, int(np.ceil(np.sqrt(len(patches)))))

    # Summary
    n_per_variant = 105
    print(f"Patches: {len(patches)} total")
    print(f"  Chromatic: {n_per_variant * 2} (Vogel spiral, {n_per_variant}/variant × 2 brightness)")
    print(f"  Pure RGB + CMY: 6")
    print(f"  Grayscale: 9 steps")
    print(f"  Patch size: {args.patch_size}px, border: {args.border}px")
    if args.shuffle:
        print(f"  Shuffled (seed=42)")

    img = build_chart(patches, args.patch_size, args.border, args.cols)
    print(f"  Chart size: {img.width} × {img.height}")

    out = Path(args.output)
    img.save(out)
    print(f"  Written: {out}")


if __name__ == '__main__':
    main()
