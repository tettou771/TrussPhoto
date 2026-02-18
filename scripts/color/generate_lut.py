#!/usr/bin/env python3
"""
generate_lut.py - Generate .cube 3D LUT from RAW+JPG pairs (gradient chart)
============================================================================

Creates a color lookup table that transforms LibRaw's default RAW rendering
to match the camera's JPEG output. Uses pixel-level correspondence from
a photographed gradient color chart.

Center-crops both RAW and JPG to (h/2)x(h/2) square, then fits a 2nd-order
polynomial color transform from millions of pixel correspondences.

Usage:
  # Process all pairs in a directory (auto-detect style from Sigma MakerNote)
  python generate_lut.py --dir /path/to/gradient/ -o /output/dir/

  # Single pair
  python generate_lut.py BF_08009.DNG BF_08009.JPG -o Standard.cube

  # Specify LUT size (default 33)
  python generate_lut.py --dir /path/to/gradient/ -o /output/dir/ --size 64
"""

import argparse
import sys
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
            no_auto_bright=True,
            user_flip=0,
        )
    return rgb


def load_jpg(jpg_path: Path) -> np.ndarray:
    """Load JPG file as numpy array (H, W, 3) uint8."""
    img = Image.open(jpg_path).convert("RGB")
    return np.array(img)


def center_crop_square(img: np.ndarray, size: int) -> np.ndarray:
    """Center-crop image to size x size square."""
    h, w = img.shape[:2]
    y0 = (h - size) // 2
    x0 = (w - size) // 2
    return img[y0:y0+size, x0:x0+size]


def align_and_crop(raw_img: np.ndarray, jpg_img: np.ndarray) -> tuple:
    """Align RAW and JPG to same dimensions, then center-crop to (h/2)x(h/2).
    Returns (src, tgt) as float32 [0,1]."""
    # Match dimensions first (center-crop to smaller of the two)
    h = min(raw_img.shape[0], jpg_img.shape[0])
    w = min(raw_img.shape[1], jpg_img.shape[1])

    raw_c = center_crop_square.__wrapped__(raw_img, h, w) if False else \
            raw_img[(raw_img.shape[0]-h)//2:(raw_img.shape[0]-h)//2+h,
                    (raw_img.shape[1]-w)//2:(raw_img.shape[1]-w)//2+w]
    jpg_c = jpg_img[(jpg_img.shape[0]-h)//2:(jpg_img.shape[0]-h)//2+h,
                    (jpg_img.shape[1]-w)//2:(jpg_img.shape[1]-w)//2+w]

    # If JPG is different resolution, resize to match RAW
    if raw_c.shape[:2] != jpg_c.shape[:2]:
        jpg_pil = Image.fromarray(jpg_c).resize(
            (raw_c.shape[1], raw_c.shape[0]), Image.LANCZOS)
        jpg_c = np.array(jpg_pil)

    # Center-crop to h/2 x h/2 square
    crop_size = h // 2
    raw_sq = center_crop_square(raw_c, crop_size)
    jpg_sq = center_crop_square(jpg_c, crop_size)

    print(f"  Crop: {raw_img.shape[1]}x{raw_img.shape[0]} → {crop_size}x{crop_size}")

    return raw_sq.astype(np.float32) / 255.0, jpg_sq.astype(np.float32) / 255.0


def expand_poly(rgb: np.ndarray, order: int = 2) -> np.ndarray:
    """Expand RGB to polynomial features.
    order=2: 9 terms [R, G, B, R^2, G^2, B^2, RG, RB, GB]
    """
    R, G, B = rgb[:, 0], rgb[:, 1], rgb[:, 2]
    terms = [R, G, B, R*R, G*G, B*B, R*G, R*B, G*B]
    if order >= 3:
        terms += [
            R*R*R, G*G*G, B*B*B,
            R*R*G, R*R*B, R*G*G, G*G*B, R*B*B, G*B*B, R*G*B,
        ]
    return np.column_stack(terms)


def expand_poly_single(r, g, b, order: int = 2) -> np.ndarray:
    """Expand a single RGB triplet to polynomial features."""
    terms = [r, g, b, r*r, g*g, b*b, r*g, r*b, g*b]
    if order >= 3:
        terms += [
            r*r*r, g*g*g, b*b*b,
            r*r*g, r*r*b, r*g*g, g*g*b, r*b*b, g*b*b, r*g*b,
        ]
    return np.array(terms)


def build_3d_lut(src: np.ndarray, tgt: np.ndarray, lut_size: int = 33) -> np.ndarray:
    """Build 3D LUT using 2nd-order polynomial fit from pixel correspondences.

    Args:
        src: Source (RAW) float32 [0,1], shape (H, W, 3)
        tgt: Target (JPG) float32 [0,1], shape (H, W, 3)
        lut_size: LUT grid size

    Returns:
        (lut_size, lut_size, lut_size, 3) float32 LUT
    """
    src_flat = src.reshape(-1, 3)
    tgt_flat = tgt.reshape(-1, 3)
    n_pixels = src_flat.shape[0]

    # Skip near-black pixels (outside the gradient circle = black background)
    brightness = src_flat.sum(axis=1)
    valid = brightness > 0.03  # ~1% threshold
    src_valid = src_flat[valid]
    tgt_valid = tgt_flat[valid]
    print(f"  Valid pixels: {valid.sum():,} / {n_pixels:,} ({valid.sum()*100/n_pixels:.1f}%)")

    # Subsample for polynomial fitting (500k points is plenty)
    n = src_valid.shape[0]
    step = max(1, n // 500000)
    src_sub = src_valid[::step]
    tgt_sub = tgt_valid[::step]
    print(f"  Fitting samples: {len(src_sub):,} (step={step})")

    # 3rd-order polynomial fit: tgt = poly(src) @ M
    use_order = 3
    X = expand_poly(src_sub, use_order)
    M, residuals, rank, sv = np.linalg.lstsq(X, tgt_sub, rcond=None)

    # Report fit quality
    pred = X @ M
    mae = np.mean(np.abs(pred - tgt_sub))
    rmse = np.sqrt(np.mean((pred - tgt_sub) ** 2))
    print(f"  Fit quality: MAE={mae:.4f}, RMSE={rmse:.4f}")

    # Check per-channel
    for ch, name in enumerate(['R', 'G', 'B']):
        ch_mae = np.mean(np.abs(pred[:, ch] - tgt_sub[:, ch]))
        print(f"    {name}: MAE={ch_mae:.4f}")

    # Build 3D LUT
    print(f"  Building {lut_size}^3 LUT...", end="", flush=True)
    scale = lut_size - 1
    lut = np.zeros((lut_size, lut_size, lut_size, 3), dtype=np.float32)

    for ri in range(lut_size):
        for gi in range(lut_size):
            for bi in range(lut_size):
                r = ri / scale
                g = gi / scale
                b = bi / scale
                features = expand_poly_single(r, g, b, use_order)
                out = features @ M
                lut[ri, gi, bi] = np.clip(out, 0, 1)

    print(" done")
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
        # .cube format: R fastest, B slowest
        for b in range(size):
            for g in range(size):
                for r in range(size):
                    val = lut[r, g, b]
                    f.write(f'{val[0]:.6f} {val[1]:.6f} {val[2]:.6f}\n')
    print(f"  Written: {output_path} ({size}x{size}x{size})")


def read_sigma_style(filepath: Path) -> str:
    """Read Sigma color mode from MakerNote tag 0x003d."""
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
    print(f"\nProcessing: {raw_path.name}")

    # Load RAW
    print("  RAW → sRGB...", end="", flush=True)
    raw_img = process_raw(raw_path)
    print(f" {raw_img.shape[1]}x{raw_img.shape[0]}")

    # Load target JPG
    if embedded:
        print("  Extracting embedded preview...", end="", flush=True)
        from io import BytesIO
        with rawpy.imread(str(raw_path)) as raw:
            thumb = raw.extract_thumb()
            if thumb.format == rawpy.ThumbFormat.JPEG:
                jpg_img = np.array(Image.open(BytesIO(thumb.data)).convert("RGB"))
            else:
                jpg_img = thumb.data
        print(f" {jpg_img.shape[1]}x{jpg_img.shape[0]}")
    else:
        print(f"  Loading {jpg_path.name}...", end="", flush=True)
        jpg_img = load_jpg(jpg_path)
        print(f" {jpg_img.shape[1]}x{jpg_img.shape[0]}")

    # Align and center-crop to h/2 square
    src, tgt = align_and_crop(raw_img, jpg_img)

    # Build LUT
    lut = build_3d_lut(src, tgt, lut_size)

    # Write
    write_cube(lut, output_path, title=title)


def process_directory(dir_path: Path, output_dir: Path, lut_size: int = 33,
                      embedded: bool = False):
    """Process all RAW+JPG pairs in a directory."""
    raw_files = sorted(dir_path.glob("*.DNG")) + sorted(dir_path.glob("*.dng"))
    if not raw_files:
        raw_files = sorted(dir_path.glob("*.ARW")) + sorted(dir_path.glob("*.arw"))
    if not raw_files:
        print("No RAW files found in directory")
        return

    output_dir.mkdir(parents=True, exist_ok=True)

    for raw_path in raw_files:
        style = read_sigma_style(raw_path)
        if not style:
            style = raw_path.stem

        out_name = f"{style}.cube"
        out_path = output_dir / out_name

        if out_path.exists():
            print(f"Skipping {raw_path.name} → {out_name} (already exists)")
            continue

        if embedded:
            process_pair(raw_path, embedded=True, output_path=out_path,
                        lut_size=lut_size, title=style)
        else:
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
        description='Generate .cube 3D LUT from RAW+JPG pairs (gradient chart)')
    parser.add_argument('raw', nargs='?', help='RAW file path')
    parser.add_argument('jpg', nargs='?', help='JPG file path')
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
