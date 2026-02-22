#!/usr/bin/env python3
"""
generate_profiles.py - Auto-generate camera color profiles from RAW+JPG pairs
===============================================================================

Scans a directory for RAW files, reads EXIF to determine camera model and
color style, groups by (model, style), then builds a single .cube LUT per
group using all matching photos for maximum color coverage.

For each RAW file:
  1. Look for matching JPG (same stem); if not found, use embedded preview
  2. Read camera model and color style from EXIF/MakerNote
  3. Group by (model, style)

For each group:
  - Stack pixel correspondences from all photos
  - Fit a 3rd-order polynomial in chromaticity space (L,r,g) (20 terms with bias)
  - Write 64^3 .cube LUT to ~/.trussc/profiles/{Model}/{Style}.cube
  - Report color coverage gaps in HSV space

Usage:
  python generate_profiles.py /path/to/calibration/photos/
  python generate_profiles.py /path/to/photos/ --output-dir /custom/profiles/
  python generate_profiles.py /path/to/photos/ --dry-run   # just show grouping
"""

import argparse
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import rawpy
from PIL import Image

# --- RAW extensions ---
RAW_EXTENSIONS = {'.dng', '.arw', '.cr2', '.cr3', '.nef', '.raf', '.orf', '.rw2', '.pef'}
JPG_EXTENSIONS = {'.jpg', '.jpeg'}


# --- EXIF / MakerNote ---

def read_exif_fields(filepath: Path) -> dict:
    """Read camera model and color style via exiftool."""
    try:
        result = subprocess.run(
            ['exiftool', '-j', '-Model', '-Make',
             '-a', '-u', '-MakerUnknown:Unknown_0x003d',  # Sigma style
             '-PictureControlName',   # Nikon
             '-PictureStyle',         # Canon
             '-FilmMode',             # Fuji
             '-CreativeStyle',        # Sony
             str(filepath)],
            capture_output=True, text=True, timeout=10
        )
        import json
        data = json.loads(result.stdout)
        if data:
            return data[0]
    except (FileNotFoundError, subprocess.TimeoutExpired, Exception):
        pass
    return {}


def detect_camera_model(exif: dict) -> str:
    """Extract camera model from EXIF. Uses Model field as-is (spaces preserved)."""
    model = exif.get('Model', '').strip()
    if not model:
        return 'Unknown'
    # Only sanitize path-unsafe chars
    return model.replace('/', '_').replace('\\', '_')


def detect_color_style(exif: dict) -> str:
    """Detect camera color style from MakerNote tags."""
    # Try each maker-specific tag
    for key in ['Unknown_0x003d',  # Sigma
                'PictureControlName',  # Nikon
                'PictureStyle',  # Canon
                'FilmMode',  # Fuji
                'CreativeStyle']:  # Sony
        val = exif.get(key, '').strip()
        if val:
            return val

    return 'Default'


# --- Image loading ---

def process_raw(raw_path: Path) -> np.ndarray:
    """Process RAW → sRGB uint8 (H, W, 3)."""
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
    """Load JPG as (H, W, 3) uint8 RGB."""
    return np.array(Image.open(jpg_path).convert("RGB"))


def extract_embedded_preview(raw_path: Path) -> np.ndarray:
    """Extract embedded JPEG preview from RAW. Returns (H, W, 3) uint8 RGB."""
    from io import BytesIO
    with rawpy.imread(str(raw_path)) as raw:
        try:
            thumb = raw.extract_thumb()
        except rawpy.LibRawNoThumbnailError:
            raise RuntimeError(f"No embedded thumbnail in {raw_path}")
        if thumb.format == rawpy.ThumbFormat.JPEG:
            return np.array(Image.open(BytesIO(thumb.data)).convert("RGB"))
        elif thumb.format == rawpy.ThumbFormat.BITMAP:
            return thumb.data
        else:
            raise RuntimeError(f"Unknown thumbnail format: {thumb.format}")


def find_jpg_for_raw(raw_path: Path) -> Path | None:
    """Find matching JPG for a RAW file."""
    for ext in ['.JPG', '.jpg', '.JPEG', '.jpeg']:
        jpg = raw_path.with_suffix(ext)
        if jpg.exists():
            return jpg
    return None


def center_crop_square(img: np.ndarray, size: int) -> np.ndarray:
    """Center-crop to size x size square."""
    h, w = img.shape[:2]
    y0 = (h - size) // 2
    x0 = (w - size) // 2
    return img[y0:y0+size, x0:x0+size]


def load_pair_pixels(raw_path: Path, jpg_path: Path | None) -> tuple | None:
    """Load a RAW+JPG pair, crop, and return valid pixel arrays.
    Returns (src_pixels, tgt_pixels) as float32 [0,1], or None on failure."""
    try:
        raw_img = process_raw(raw_path)
        if jpg_path:
            jpg_img = load_jpg(jpg_path)
            source = "JPG"
        else:
            jpg_img = extract_embedded_preview(raw_path)
            source = "embedded"
    except Exception as e:
        print(f"    ERROR loading {raw_path.name}: {e}")
        return None

    # Match dimensions: when embedded preview is much smaller than RAW,
    # resize RAW down to preview size (they cover the same field of view).
    # For same-size JPG pairs, use the original center-crop approach.
    raw_h, raw_w = raw_img.shape[:2]
    jpg_h, jpg_w = jpg_img.shape[:2]

    if source == "embedded" and (raw_h > jpg_h * 1.5 or raw_w > jpg_w * 1.5):
        # Embedded preview covers full frame — resize RAW to match
        raw_resized = np.array(Image.fromarray(raw_img).resize(
            (jpg_w, jpg_h), Image.LANCZOS))
        h = jpg_h
    else:
        # Same-size pair: center-crop to smaller dimensions
        h = min(raw_h, jpg_h)
        w = min(raw_w, jpg_w)
        raw_resized = raw_img[(raw_h-h)//2:(raw_h-h)//2+h,
                              (raw_w-w)//2:(raw_w-w)//2+w]
        jpg_img = jpg_img[(jpg_h-h)//2:(jpg_h-h)//2+h,
                          (jpg_w-w)//2:(jpg_w-w)//2+w]

    # Center crop to h/2 square
    crop_size = h // 2
    raw_sq = center_crop_square(raw_resized, crop_size).astype(np.float32) / 255.0
    jpg_sq = center_crop_square(jpg_img, crop_size).astype(np.float32) / 255.0

    # Flatten and filter black background
    src_flat = raw_sq.reshape(-1, 3)
    tgt_flat = jpg_sq.reshape(-1, 3)
    brightness = src_flat.sum(axis=1)
    valid = brightness > 0.03
    src_valid = src_flat[valid]
    tgt_valid = tgt_flat[valid]

    print(f"    {raw_path.name} ({source}): {raw_img.shape[1]}x{raw_img.shape[0]}"
          f" → crop {crop_size}x{crop_size}, {valid.sum():,} valid px")

    return src_valid, tgt_valid


# --- Chromaticity-based polynomial fitting ---
# Input RGB is transformed to (L, r, g) where:
#   L = (R+G+B)/3          (luminance)
#   r = R/(R+G+B)          (red chromaticity)
#   g = G/(R+G+B)          (green chromaticity)
# Polynomial is built in this space, but output is still RGB.
# This avoids term degeneracy near the neutral axis (R≈G≈B).

EPS = 1e-6  # protect against division by zero (black pixels)


def rgb_to_lrg(rgb: np.ndarray) -> np.ndarray:
    """Convert (N,3) RGB to (N,3) [L, r, g] chromaticity space."""
    R, G, B = rgb[:, 0], rgb[:, 1], rgb[:, 2]
    S = R + G + B + EPS
    L = S / 3.0
    r = R / S
    g = G / S
    return np.column_stack([L, r, g])


def rgb_to_lrg_single(R, G, B) -> tuple:
    """Convert single RGB to (L, r, g)."""
    S = R + G + B + EPS
    return S / 3.0, R / S, G / S


def expand_poly(lrg: np.ndarray, order: int = 3) -> np.ndarray:
    """Expand (L, r, g) chromaticity to polynomial features with bias.
    order=2: 10 terms [1, L, r, g, L^2, r^2, g^2, Lr, Lg, rg]
    order=3: 20 terms (+ 10 cubic terms)
    order=4: 35 terms (+ 15 quartic terms)
    """
    n = lrg.shape[0]
    L, r, g = lrg[:, 0], lrg[:, 1], lrg[:, 2]
    terms = [np.ones(n), L, r, g, L*L, r*r, g*g, L*r, L*g, r*g]
    if order >= 3:
        terms += [L*L*L, r*r*r, g*g*g,
                  L*L*r, L*L*g, L*r*r, L*g*g, r*r*g, r*g*g, L*r*g]
    if order >= 4:
        L2, r2, g2 = L*L, r*r, g*g
        terms += [L2*L2, r2*r2, g2*g2,
                  L2*L*r, L2*L*g,
                  L*r2*r, r2*r*g,
                  L*g2*g, r*g2*g,
                  L2*r2, L2*g2, r2*g2,
                  L2*r*g, L*r2*g, L*r*g2]
    return np.column_stack(terms)


def expand_poly_single(L, r, g, order: int = 3) -> np.ndarray:
    """Expand single (L, r, g) to polynomial features with bias."""
    terms = [1.0, L, r, g, L*L, r*r, g*g, L*r, L*g, r*g]
    if order >= 3:
        terms += [L*L*L, r*r*r, g*g*g,
                  L*L*r, L*L*g, L*r*r, L*g*g, r*r*g, r*g*g, L*r*g]
    if order >= 4:
        L2, r2, g2 = L*L, r*r, g*g
        terms += [L2*L2, r2*r2, g2*g2,
                  L2*L*r, L2*L*g,
                  L*r2*r, r2*r*g,
                  L*g2*g, r*g2*g,
                  L2*r2, L2*g2, r2*g2,
                  L2*r*g, L*r2*g, L*r*g2]
    return np.array(terms)


def build_lut(src_all: np.ndarray, tgt_all: np.ndarray,
              lut_size: int = 64, sigma_clip: float = 1.5,
              clip_rounds: int = 3, min_survivor_pct: float = 10.0) -> tuple:
    """Build 3D LUT using chromaticity-space polynomial fit with sigma clipping.
    Iteratively removes outliers (misaligned pixels from lens distortion correction
    differences between RAW and camera JPEG) before final fit.
    min_survivor_pct guarantees at least N% of best-fitting pixels survive.
    Returns (lut, fit_info_dict)."""

    # Subsample to ~1M points
    n = src_all.shape[0]
    step = max(1, n // 1000000)
    src_sub = src_all[::step]
    tgt_sub = tgt_all[::step]
    n_initial = len(src_sub)
    min_survivors = max(100, int(n_initial * min_survivor_pct / 100.0))
    print(f"  Fitting samples: {n_initial:,} (step={step}), "
          f"min survivors: {min_survivors:,} ({min_survivor_pct:.0f}%)")

    # Convert to chromaticity space (L, r, g)
    src_lrg = rgb_to_lrg(src_sub)

    # 4th-order polynomial fit in chromaticity space
    use_order = 4

    # Iterative sigma clipping: fit → remove outliers → refit
    mask = np.ones(len(src_sub), dtype=bool)
    for clip_iter in range(clip_rounds):
        X = expand_poly(src_lrg[mask], use_order)
        M, _, _, _ = np.linalg.lstsq(X, tgt_sub[mask], rcond=None)

        # Compute residuals on ALL remaining points
        X_all = expand_poly(src_lrg[mask], use_order)
        pred = X_all @ M
        residuals = np.sqrt(np.sum((pred - tgt_sub[mask]) ** 2, axis=1))

        sigma = np.std(residuals)
        threshold = sigma_clip * sigma
        inlier_local = residuals <= threshold
        n_before = mask.sum()

        # Check if clipping would drop below minimum
        n_would_remain = inlier_local.sum()
        if n_would_remain < min_survivors:
            # Keep top min_survivors by residual instead
            indices = np.where(mask)[0]
            sorted_idx = np.argsort(residuals)
            keep = sorted_idx[:min_survivors]
            new_mask = np.zeros(len(src_sub), dtype=bool)
            new_mask[indices[keep]] = True
            mask = new_mask
            n_after = mask.sum()
            removed_pct = (1 - n_after / n_initial) * 100
            print(f"    Clip round {clip_iter+1}: σ={sigma:.4f}, "
                  f"threshold={threshold:.4f}, "
                  f"floor hit → kept best {n_after:,} ({removed_pct:.0f}% total removed)")
            break

        # Update global mask
        indices = np.where(mask)[0]
        mask[indices[~inlier_local]] = False
        n_after = mask.sum()
        removed_pct = (1 - n_after / n_initial) * 100

        print(f"    Clip round {clip_iter+1}: σ={sigma:.4f}, "
              f"threshold={threshold:.4f}, "
              f"removed {n_before - n_after:,} → "
              f"{n_after:,} remain ({removed_pct:.0f}% total removed)")

    # Final fit on clean data
    X = expand_poly(src_lrg[mask], use_order)
    n_terms = X.shape[1]
    M, _, _, _ = np.linalg.lstsq(X, tgt_sub[mask], rcond=None)

    pred = X @ M
    mae = np.mean(np.abs(pred - tgt_sub[mask]))
    rmse = np.sqrt(np.mean((pred - tgt_sub[mask]) ** 2))
    per_ch_mae = [np.mean(np.abs(pred[:, c] - tgt_sub[mask, c])) for c in range(3)]

    # Report bias (black point offset)
    bias = M[0]  # first row = constant term coefficients
    print(f"  Chromaticity polynomial: {n_terms} terms (order={use_order}, with bias)")
    print(f"  Black point offset: R={bias[0]:+.4f} G={bias[1]:+.4f} B={bias[2]:+.4f}")
    print(f"  Survivors: {mask.sum():,} / {n_initial:,} "
          f"({mask.sum()/n_initial*100:.0f}%)")

    # Build LUT (indexed by RGB, polynomial applied in chromaticity space)
    print(f"  Building {lut_size}^3 LUT...", end="", flush=True)
    scale = lut_size - 1
    lut = np.zeros((lut_size, lut_size, lut_size, 3), dtype=np.float32)
    for ri in range(lut_size):
        for gi in range(lut_size):
            for bi in range(lut_size):
                R, G, B = ri/scale, gi/scale, bi/scale
                L, cr, cg = rgb_to_lrg_single(R, G, B)
                features = expand_poly_single(L, cr, cg, use_order)
                lut[ri, gi, bi] = np.clip(features @ M, 0, 1)
    print(" done")

    info = {
        'mae': mae, 'rmse': rmse,
        'per_ch_mae': per_ch_mae,
        'n_samples': mask.sum(), 'n_total': n,
        'n_initial_sub': n_initial,
        'pct_removed': (1 - mask.sum() / n_initial) * 100,
    }
    return lut, info


def write_cube(lut: np.ndarray, output_path: Path, title: str = ""):
    """Write .cube LUT file."""
    size = lut.shape[0]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w') as f:
        if title:
            f.write(f'TITLE "{title}"\n')
        f.write(f'LUT_3D_SIZE {size}\n')
        f.write('DOMAIN_MIN 0.0 0.0 0.0\n')
        f.write('DOMAIN_MAX 1.0 1.0 1.0\n\n')
        for b in range(size):
            for g in range(size):
                for r in range(size):
                    val = lut[r, g, b]
                    f.write(f'{val[0]:.6f} {val[1]:.6f} {val[2]:.6f}\n')


# --- Color coverage analysis ---

HUE_NAMES = ['Red', 'Orange', 'Yellow', 'Chartreuse', 'Green', 'Spring',
             'Cyan', 'Azure', 'Blue', 'Violet', 'Magenta', 'Rose']

def analyze_coverage(src_pixels: np.ndarray) -> list[str]:
    """Analyze HSV color coverage and return suggestions."""
    import colorsys

    n = len(src_pixels)
    step = max(1, n // 200000)
    sample = src_pixels[::step]

    # Convert to HSV
    hsv = np.array([colorsys.rgb_to_hsv(r, g, b) for r, g, b in sample])
    H, S, V = hsv[:, 0], hsv[:, 1], hsv[:, 2]

    # Bin: 12 hue × 4 saturation × 4 value
    h_bins = 12
    s_bins = 4
    v_bins = 4
    hist = np.zeros((h_bins, s_bins, v_bins), dtype=int)

    h_idx = np.clip((H * h_bins).astype(int), 0, h_bins - 1)
    s_idx = np.clip((S * s_bins).astype(int), 0, s_bins - 1)
    v_idx = np.clip((V * v_bins).astype(int), 0, v_bins - 1)

    for i in range(len(sample)):
        hist[h_idx[i], s_idx[i], v_idx[i]] += 1

    # Find sparse bins (< 0.5% of expected uniform)
    expected = len(sample) / (h_bins * s_bins * v_bins)
    threshold = expected * 0.1  # very sparse
    suggestions = []

    # Check by hue (sum across sat and val)
    hue_totals = hist.sum(axis=(1, 2))
    for hi in range(h_bins):
        if hue_totals[hi] < expected * s_bins * v_bins * 0.2:
            suggestions.append(f"Low coverage: {HUE_NAMES[hi]} hues")

    # Check saturated darks and lights
    dark_total = hist[:, 2:, 0].sum()  # high sat, low val
    if dark_total < expected * h_bins * 2 * 0.2:
        suggestions.append("Low coverage: saturated dark tones")

    bright_total = hist[:, 2:, 3].sum()  # high sat, high val
    if bright_total < expected * h_bins * 2 * 0.2:
        suggestions.append("Low coverage: saturated bright tones")

    # Grayscale coverage
    gray_total = hist[:, 0, :].sum()  # low sat
    if gray_total < expected * h_bins * v_bins * 0.3:
        suggestions.append("Low coverage: neutral/grayscale tones")

    return suggestions


# --- Main ---

def scan_and_group(dir_path: Path, ignore_extensions: set = None,
                   camera_filter: str = None) -> dict:
    """Scan directory, read EXIF, group by (model, style).
    Returns {(model, style): [(raw_path, jpg_path|None), ...]}
    ignore_extensions: set of lowercase extensions to skip (e.g. {'.dng'})
    camera_filter: if set, only include files matching this camera model (case-insensitive)"""

    groups = defaultdict(list)
    skip_exts = ignore_extensions or set()

    raw_files = []
    for ext in RAW_EXTENSIONS:
        if ext in skip_exts:
            continue
        raw_files.extend(dir_path.rglob(f"*{ext}"))
        raw_files.extend(dir_path.rglob(f"*{ext.upper()}"))

    raw_files = sorted(set(raw_files))
    if not raw_files:
        print(f"No RAW files found in {dir_path}")
        if skip_exts:
            print(f"  (ignoring: {', '.join(skip_exts)})")
        return groups

    print(f"Found {len(raw_files)} RAW files")
    if skip_exts:
        print(f"  (ignoring: {', '.join(skip_exts)})")
    if camera_filter:
        print(f"  (camera filter: {camera_filter})")
    print()

    skipped_camera = 0
    for raw_path in raw_files:
        exif = read_exif_fields(raw_path)
        model = detect_camera_model(exif)

        if camera_filter and model.lower() != camera_filter.lower():
            skipped_camera += 1
            continue

        style = detect_color_style(exif)
        jpg_path = find_jpg_for_raw(raw_path)

        source = jpg_path.name if jpg_path else "(embedded)"
        print(f"  {raw_path.name} → {model} / {style}  [{source}]")

        groups[(model, style)].append((raw_path, jpg_path))

    if skipped_camera:
        print(f"\n  ({skipped_camera} files skipped by camera filter)")

    return groups


def main():
    parser = argparse.ArgumentParser(
        description='Auto-generate camera color profiles from RAW+JPG pairs')
    parser.add_argument('dir', help='Directory containing RAW (+JPG) files')
    parser.add_argument('--output-dir', default=None,
                        help='Output directory (default: ~/.trussc/profiles/)')
    parser.add_argument('--size', type=int, default=64, help='LUT grid size (default: 64)')
    parser.add_argument('--dry-run', action='store_true',
                        help='Show grouping without generating LUTs')
    parser.add_argument('--force', action='store_true',
                        help='Overwrite existing .cube files')
    parser.add_argument('--sigma-clip', type=float, default=1.5,
                        help='Sigma clipping threshold (default: 1.5)')
    parser.add_argument('--clip-rounds', type=int, default=3,
                        help='Number of sigma clipping iterations (default: 3)')
    parser.add_argument('--min-survivors', type=float, default=10.0,
                        help='Minimum survivor percentage floor (default: 10)')
    parser.add_argument('--ignore-dng', action='store_true',
                        help='Skip DNG files (useful when DNG previews are Lightroom-generated)')
    parser.add_argument('--camera', default=None,
                        help='Only process files from this camera model (case-insensitive)')

    args = parser.parse_args()
    dir_path = Path(args.dir)
    if not dir_path.is_dir():
        print(f"Error: {dir_path} is not a directory")
        sys.exit(1)

    output_dir = Path(args.output_dir) if args.output_dir else \
                 Path.home() / '.trussc' / 'profiles'

    # Build ignore set
    ignore_exts = set()
    if args.ignore_dng:
        ignore_exts.add('.dng')

    # Scan and group
    groups = scan_and_group(dir_path, ignore_extensions=ignore_exts,
                            camera_filter=args.camera)
    if not groups:
        sys.exit(1)

    # Summary
    print(f"\n{'='*60}")
    print(f"Groups: {len(groups)}")
    for (model, style), files in sorted(groups.items()):
        print(f"  {model} / {style}: {len(files)} file(s)")
    print(f"{'='*60}\n")

    if args.dry_run:
        return

    # Process each group
    results = []
    for (model, style), files in sorted(groups.items()):
        out_path = output_dir / model / f"{style}.cube"

        if out_path.exists() and not args.force:
            print(f"[SKIP] {model}/{style}.cube already exists (use --force)")
            results.append((model, style, len(files), 'skipped', {}))
            continue

        print(f"\n[BUILD] {model} / {style} ({len(files)} files)")

        # Load all pairs
        all_src = []
        all_tgt = []
        for raw_path, jpg_path in files:
            pair = load_pair_pixels(raw_path, jpg_path)
            if pair is not None:
                all_src.append(pair[0])
                all_tgt.append(pair[1])

        if not all_src:
            print(f"  No valid pairs for {model}/{style}")
            results.append((model, style, len(files), 'failed', {}))
            continue

        src_combined = np.vstack(all_src)
        tgt_combined = np.vstack(all_tgt)
        print(f"  Combined: {len(src_combined):,} valid pixels from {len(all_src)} files")

        # Color coverage analysis
        suggestions = analyze_coverage(src_combined)

        # Build LUT
        lut, info = build_lut(src_combined, tgt_combined, args.size,
                              sigma_clip=args.sigma_clip,
                              clip_rounds=args.clip_rounds,
                              min_survivor_pct=args.min_survivors)

        print(f"  Fit: MAE={info['mae']:.4f}  RMSE={info['rmse']:.4f}"
              f"  (R={info['per_ch_mae'][0]:.4f} G={info['per_ch_mae'][1]:.4f}"
              f" B={info['per_ch_mae'][2]:.4f})")

        if suggestions:
            print(f"  Coverage gaps:")
            for s in suggestions:
                print(f"    - {s}")

        # Write
        write_cube(lut, out_path, title=f"{model} {style}")
        print(f"  Written: {out_path}")

        results.append((model, style, len(all_src), 'ok', info))

    # Final summary
    print(f"\n{'='*60}")
    print(f"{'Model':<25} {'Style':<18} {'Files':>5} {'MAE':>7} {'Clip%':>6} {'Status'}")
    print(f"{'-'*25} {'-'*18} {'-'*5} {'-'*7} {'-'*6} {'-'*8}")
    for model, style, n_files, status, info in results:
        mae_str = f"{info['mae']:.4f}" if info else '-'
        clip_str = f"{info['pct_removed']:.0f}%" if info.get('pct_removed') is not None else '-'
        print(f"{model:<25} {style:<18} {n_files:>5} {mae_str:>7} {clip_str:>6} {status}")
    print(f"{'='*60}")


if __name__ == '__main__':
    main()
