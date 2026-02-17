#!/usr/bin/env python3
"""
generate_lut.py - Generate .cube 3D LUT from RAW+JPG pairs
============================================================

Creates a color lookup table that transforms LibRaw's default RAW rendering
to match the camera's JPEG output (with its color profile/style applied).

Supports two modes:
  1. RAW + external JPG pair
  2. RAW with embedded JPEG preview (for cameras you no longer have)

Usage:
  # From RAW + JPG pair
  python generate_lut.py BF_07993.DNG BF_07993.JPG -o Standard.cube

  # From RAW with embedded preview
  python generate_lut.py BF_07993.DNG --embedded -o Standard.cube

  # Process all pairs in a directory (auto-detect style from Sigma MakerNote)
  python generate_lut.py --dir /path/to/calibration/ -o /output/dir/

  # Specify LUT size (default 33)
  python generate_lut.py BF_07993.DNG BF_07993.JPG -o Standard.cube --size 64
"""

import argparse
import sys
import struct
from pathlib import Path

import numpy as np
import rawpy
from PIL import Image


def process_raw(raw_path: Path) -> np.ndarray:
    """Process RAW file with rawpy (LibRaw) using default settings matching TrussPhoto.
    Returns sRGB uint8 image as numpy array (H, W, 3)."""
    with rawpy.imread(str(raw_path)) as raw:
        rgb = raw.postprocess(
            use_camera_wb=True,
            output_color=rawpy.ColorSpace.sRGB,
            output_bps=8,
            no_auto_bright=False,
            user_flip=0,  # no EXIF rotation - match embedded preview orientation
        )
    return rgb


def load_jpg(jpg_path: Path) -> np.ndarray:
    """Load JPG file as numpy array (H, W, 3) uint8."""
    img = Image.open(jpg_path).convert("RGB")
    return np.array(img)


def extract_embedded_preview(raw_path: Path) -> np.ndarray:
    """Extract the largest embedded JPEG preview from a RAW file.
    Returns numpy array (H, W, 3) uint8."""
    with rawpy.imread(str(raw_path)) as raw:
        # Get the largest thumbnail
        try:
            thumb = raw.extract_thumb()
        except rawpy.LibRawNoThumbnailError:
            raise RuntimeError(f"No embedded thumbnail in {raw_path}")

        if thumb.format == rawpy.ThumbFormat.JPEG:
            from io import BytesIO
            img = Image.open(BytesIO(thumb.data)).convert("RGB")
            return np.array(img)
        elif thumb.format == rawpy.ThumbFormat.BITMAP:
            return thumb.data
        else:
            raise RuntimeError(f"Unknown thumbnail format: {thumb.format}")


def align_images(src: np.ndarray, target: np.ndarray) -> tuple:
    """Resize images to match dimensions (use the smaller of the two).
    Returns (src_resized, target_resized) as float32 [0,1]."""
    # Use the smaller dimensions
    h = min(src.shape[0], target.shape[0])
    w = min(src.shape[1], target.shape[1])

    # Center crop both to same size
    def center_crop(img, th, tw):
        y0 = (img.shape[0] - th) // 2
        x0 = (img.shape[1] - tw) // 2
        return img[y0:y0+th, x0:x0+tw]

    src_c = center_crop(src, h, w)
    tgt_c = center_crop(target, h, w)

    # If sizes still differ significantly, resize target to match src
    if abs(src_c.shape[0] - tgt_c.shape[0]) > 2 or abs(src_c.shape[1] - tgt_c.shape[1]) > 2:
        tgt_pil = Image.fromarray(tgt_c).resize((src_c.shape[1], src_c.shape[0]), Image.LANCZOS)
        tgt_c = np.array(tgt_pil)

    return src_c.astype(np.float32) / 255.0, tgt_c.astype(np.float32) / 255.0


def build_3d_lut(src: np.ndarray, target: np.ndarray, lut_size: int = 33) -> np.ndarray:
    """Build a 3D LUT using per-channel tone curves + 3x3 color matrix.

    Two-stage approach for robustness:
      1. Fit a 3x3 color matrix via least-squares (cross-channel color shifts)
      2. Build per-channel 1D tone curves from residuals (tone/contrast)
      3. Combine into a 3D LUT

    Args:
        src: Source image (RAW rendered) as float32 [0,1], shape (H, W, 3)
        target: Target image (JPG) as float32 [0,1], shape (H, W, 3)
        lut_size: Number of nodes per axis (typically 33 or 64)

    Returns:
        3D LUT array of shape (lut_size, lut_size, lut_size, 3)
    """
    src_flat = src.reshape(-1, 3)
    tgt_flat = target.reshape(-1, 3)

    # Subsample for matrix fitting (full image is too large)
    n = src_flat.shape[0]
    step = max(1, n // 500000)
    src_sub = src_flat[::step]
    tgt_sub = tgt_flat[::step]

    # Stage 1: Fit 3x3 color matrix (least squares: tgt = src @ M.T)
    # Solve: M = (src^T src)^-1 src^T tgt
    M, _, _, _ = np.linalg.lstsq(src_sub, tgt_sub, rcond=None)
    print(f"    Color matrix:\n{M}")

    # Stage 2: Per-channel 1D tone curves on the residual
    # Apply matrix to source, then build 1D curves for remaining correction
    src_corrected = src_flat @ M
    src_corrected = np.clip(src_corrected, 0, 1)

    scale = lut_size - 1
    curves = np.zeros((3, lut_size), dtype=np.float64)
    counts = np.zeros((3, lut_size), dtype=np.float64)

    for ch in range(3):
        idx = np.clip((src_corrected[:, ch] * scale).astype(np.int32), 0, int(scale))
        np.add.at(curves[ch], idx, tgt_flat[:, ch])
        np.add.at(counts[ch], idx, 1)

    # Average and fill empty bins with identity
    for ch in range(3):
        for i in range(lut_size):
            if counts[ch, i] > 0:
                curves[ch, i] /= counts[ch, i]
            else:
                curves[ch, i] = i / scale

    # Enforce monotonicity
    for ch in range(3):
        for i in range(1, lut_size):
            if curves[ch, i] < curves[ch, i - 1]:
                curves[ch, i] = curves[ch, i - 1]

    # Stage 3: Combine into 3D LUT
    # For each input (r,g,b): apply matrix, then look up tone curves
    lut = np.zeros((lut_size, lut_size, lut_size, 3), dtype=np.float32)
    for ri in range(lut_size):
        for gi in range(lut_size):
            for bi in range(lut_size):
                rgb_in = np.array([ri / scale, gi / scale, bi / scale])
                # Apply color matrix
                rgb_mat = rgb_in @ M
                rgb_mat = np.clip(rgb_mat, 0, 1)
                # Apply per-channel tone curves (linear interpolation)
                out = np.zeros(3)
                for ch in range(3):
                    v = rgb_mat[ch] * scale
                    lo = int(v)
                    hi = min(lo + 1, int(scale))
                    frac = v - lo
                    out[ch] = curves[ch, lo] * (1 - frac) + curves[ch, hi] * frac
                lut[ri, gi, bi] = np.clip(out, 0, 1)

    return lut


def write_cube(lut: np.ndarray, output_path: Path, title: str = ""):
    """Write 3D LUT in .cube format."""
    size = lut.shape[0]
    with open(output_path, 'w') as f:
        if title:
            f.write(f'TITLE "{title}"\n')
        f.write(f'LUT_3D_SIZE {size}\n')
        f.write(f'DOMAIN_MIN 0.0 0.0 0.0\n')
        f.write(f'DOMAIN_MAX 1.0 1.0 1.0\n')
        f.write('\n')

        # .cube format: R changes fastest, then G, then B (slowest)
        for b in range(size):
            for g in range(size):
                for r in range(size):
                    val = lut[r, g, b]
                    f.write(f'{val[0]:.6f} {val[1]:.6f} {val[2]:.6f}\n')

    print(f"  Written: {output_path} ({size}x{size}x{size})")


def read_sigma_style(filepath: Path) -> str:
    """Read Sigma color mode from MakerNote tag 0x003d.
    Works by parsing the TIFF IFD structure in the MakerNote."""
    try:
        import subprocess
        result = subprocess.run(
            ['exiftool', '-a', '-u', '-s3', '-MakerUnknown:Unknown_0x003d', str(filepath)],
            capture_output=True, text=True, timeout=10
        )
        style = result.stdout.strip()
        if style:
            return style
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return ""


def process_pair(raw_path: Path, jpg_path: Path = None, embedded: bool = False,
                 output_path: Path = None, lut_size: int = 33, title: str = ""):
    """Process a single RAW+JPG pair and generate .cube LUT."""

    print(f"Processing: {raw_path.name}")

    # Get RAW rendering
    print("  RAW → sRGB...", end="", flush=True)
    raw_img = process_raw(raw_path)
    print(f" {raw_img.shape[1]}x{raw_img.shape[0]}")

    # Get target JPG
    if embedded:
        print("  Extracting embedded preview...", end="", flush=True)
        jpg_img = extract_embedded_preview(raw_path)
        print(f" {jpg_img.shape[1]}x{jpg_img.shape[0]}")
    else:
        print(f"  Loading {jpg_path.name}...", end="", flush=True)
        jpg_img = load_jpg(jpg_path)
        print(f" {jpg_img.shape[1]}x{jpg_img.shape[0]}")

    # Align
    print("  Aligning...", end="", flush=True)
    src, tgt = align_images(raw_img, jpg_img)
    print(f" {src.shape[1]}x{src.shape[0]}")

    # Build LUT
    print(f"  Building {lut_size}x{lut_size}x{lut_size} LUT...", end="", flush=True)
    lut = build_3d_lut(src, tgt, lut_size)
    print(" done")

    # Write
    write_cube(lut, output_path, title=title)


def process_directory(dir_path: Path, output_dir: Path, lut_size: int = 33,
                      embedded: bool = False):
    """Process all RAW+JPG pairs in a directory."""
    # Find DNG files
    raw_files = sorted(dir_path.glob("*.DNG")) + sorted(dir_path.glob("*.dng"))
    if not raw_files:
        raw_files = sorted(dir_path.glob("*.ARW")) + sorted(dir_path.glob("*.arw"))
    if not raw_files:
        print("No RAW files found in directory")
        return

    output_dir.mkdir(parents=True, exist_ok=True)

    for raw_path in raw_files:
        # Try to read Sigma style name
        style = read_sigma_style(raw_path)
        if not style:
            style = raw_path.stem  # fallback to filename

        # Determine output filename
        out_name = f"{style}.cube"
        out_path = output_dir / out_name

        # Skip if already exists
        if out_path.exists():
            print(f"Skipping {raw_path.name} → {out_name} (already exists)")
            continue

        if embedded:
            process_pair(raw_path, embedded=True, output_path=out_path,
                        lut_size=lut_size, title=style)
        else:
            # Find matching JPG
            jpg_path = raw_path.with_suffix('.JPG')
            if not jpg_path.exists():
                jpg_path = raw_path.with_suffix('.jpg')
            if not jpg_path.exists():
                print(f"Skipping {raw_path.name} (no matching JPG)")
                continue

            process_pair(raw_path, jpg_path, output_path=out_path,
                        lut_size=lut_size, title=style)


def main():
    parser = argparse.ArgumentParser(
        description='Generate .cube 3D LUT from RAW+JPG pairs')
    parser.add_argument('raw', nargs='?', help='RAW file path')
    parser.add_argument('jpg', nargs='?', help='JPG file path (omit with --embedded)')
    parser.add_argument('-o', '--output', required=True,
                        help='Output .cube file or directory (with --dir)')
    parser.add_argument('--embedded', action='store_true',
                        help='Use embedded JPEG preview instead of external JPG')
    parser.add_argument('--dir', help='Process all RAW+JPG pairs in directory')
    parser.add_argument('--size', type=int, default=33,
                        help='LUT grid size (default: 33)')

    args = parser.parse_args()

    if args.dir:
        process_directory(Path(args.dir), Path(args.output),
                         lut_size=args.size, embedded=args.embedded)
    elif args.raw:
        raw_path = Path(args.raw)
        if not raw_path.exists():
            print(f"Error: {raw_path} not found")
            sys.exit(1)

        if args.embedded:
            process_pair(raw_path, embedded=True,
                        output_path=Path(args.output),
                        lut_size=args.size, title=raw_path.stem)
        else:
            if not args.jpg:
                print("Error: JPG path required (or use --embedded)")
                sys.exit(1)
            jpg_path = Path(args.jpg)
            if not jpg_path.exists():
                print(f"Error: {jpg_path} not found")
                sys.exit(1)
            process_pair(raw_path, jpg_path,
                        output_path=Path(args.output),
                        lut_size=args.size, title=raw_path.stem)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
