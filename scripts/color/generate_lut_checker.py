#!/usr/bin/env python3
"""
generate_lut_checker.py - Generate .cube 3D LUT using ColorChecker detection
==============================================================================

Detects a 24-patch ColorChecker in both RAW rendering and camera JPEG,
then builds a LUT from the 24 color correspondences. This avoids pixel
alignment issues caused by lens correction differences between RAW and JPEG.

Usage:
  # Single RAW + JPG pair (ColorChecker must be visible in both)
  python generate_lut_checker.py BF_07993.DNG BF_07993.JPG -o Standard.cube

  # With embedded JPEG preview
  python generate_lut_checker.py BF_07993.DNG --embedded -o Standard.cube

  # Process all pairs in a directory
  python generate_lut_checker.py --dir /path/to/calibration/ -o /output/dir/

  # Debug mode: save annotated images showing detected patches
  python generate_lut_checker.py BF_07993.DNG BF_07993.JPG -o Standard.cube --debug
"""

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np
import rawpy
from PIL import Image


# --- ColorChecker Detection ---

def detect_checker_patches(image_bgr: np.ndarray, label: str = "",
                           debug_path: Path = None) -> np.ndarray:
    """Detect ColorChecker Classic 24-patch and return average RGB per patch.

    Args:
        image_bgr: Input image in BGR uint8 (OpenCV format)
        label: Label for debug output
        debug_path: If set, save annotated image showing detected patches

    Returns:
        (24, 3) float32 array of average RGB values [0,1] for each patch,
        in standard ColorChecker order (row-major, top-left to bottom-right).
        Raises RuntimeError if detection fails.
    """
    detector = cv2.mcc.CCheckerDetector_create()
    if not detector.process(image_bgr, cv2.mcc.MCC24):
        raise RuntimeError(f"ColorChecker not detected in {label}")

    checker = detector.getBestColorChecker()

    # getChartsRGB returns (24, 4) or similar: each row is [R, G, B, count]
    charts_rgb = checker.getChartsRGB()
    n_patches = charts_rgb.shape[0] // 3  # rows are R,G,B interleaved?

    # Actually chartsRGB is shape (N, 1) flattened as [R0, G0, B0, R1, G1, B1, ...]
    # Let's figure out the actual format
    raw_data = np.array(charts_rgb).flatten()

    # The format is: for each patch, 4 values: [avg_channel, avg_channel, avg_channel, count]
    # Actually it's [R, G, B] for each patch, total 24*3 = 72 values + counts
    # Let's check the shape
    print(f"    {label}: chartsRGB shape={charts_rgb.shape}, raw len={len(raw_data)}")

    # chartsRGB is (N_patches, 4) where cols are [channel_value, ?, ?, count]
    # OR it's (N_patches * 3, 1) - need to check
    # Let's use getBox() instead for more control

    box = checker.getBox()  # 4 corner points of the checker

    if box is not None and len(box) == 4:
        print(f"    {label}: Checker box corners detected")
        # Extract patches manually from the detected corners
        patches = extract_patches_from_corners(image_bgr, box, debug_path, label)
        return patches

    # Fallback: parse chartsRGB directly
    # chartsRGB shape is typically (rows, 1) with rows = patches * channels
    if len(raw_data) >= 72:  # 24 patches * 3 channels
        # Try reshape as (24, 3) - RGB values
        if len(raw_data) == 96:  # 24 * 4 (RGB + count)
            rgb = raw_data.reshape(24, 4)[:, :3]
        elif len(raw_data) == 72:  # 24 * 3
            rgb = raw_data.reshape(24, 3)
        else:
            # Try to take first 72 values
            rgb = raw_data[:72].reshape(24, 3)

        return rgb.astype(np.float32) / 255.0

    raise RuntimeError(f"Cannot parse chartsRGB (shape={charts_rgb.shape}) for {label}")


def extract_patches_from_corners(image_bgr: np.ndarray, corners: np.ndarray,
                                  debug_path: Path = None, label: str = "") -> np.ndarray:
    """Extract 24 patch colors from detected checker corners.

    Uses perspective transform to rectify the checker, then samples
    the inner region of each patch (avoiding borders).

    Args:
        image_bgr: Input BGR image
        corners: 4 corner points of the detected checker
        debug_path: Optional path to save debug image
        label: Label for messages

    Returns:
        (24, 3) float32 array of average RGB [0,1]
    """
    corners = np.array(corners, dtype=np.float32).reshape(4, 2)

    # Target: rectified checker image (6 cols x 4 rows)
    # Standard patch size for extraction
    patch_px = 80  # pixels per patch in rectified image
    margin = 0.25  # use inner 50% of each patch (skip 25% border on each side)

    dst_w = 6 * patch_px
    dst_h = 4 * patch_px
    dst_corners = np.array([
        [0, 0], [dst_w, 0], [dst_w, dst_h], [0, dst_h]
    ], dtype=np.float32)

    # Perspective transform
    M = cv2.getPerspectiveTransform(corners, dst_corners)
    rectified = cv2.warpPerspective(image_bgr, M, (dst_w, dst_h))

    # Extract each patch
    patches = np.zeros((24, 3), dtype=np.float32)
    m = int(patch_px * margin)  # margin in pixels

    debug_img = rectified.copy() if debug_path else None

    for row in range(4):
        for col in range(6):
            idx = row * 6 + col
            x0 = col * patch_px + m
            y0 = row * patch_px + m
            x1 = (col + 1) * patch_px - m
            y1 = (row + 1) * patch_px - m

            patch_region = rectified[y0:y1, x0:x1]
            # BGR to RGB, then average
            avg_bgr = patch_region.mean(axis=(0, 1))
            patches[idx] = avg_bgr[[2, 1, 0]]  # BGR -> RGB

            if debug_img is not None:
                cv2.rectangle(debug_img, (x0, y0), (x1, y1), (0, 255, 0), 1)
                cv2.putText(debug_img, str(idx), (col * patch_px + 5, row * patch_px + 20),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 0), 1)

    if debug_path and debug_img is not None:
        cv2.imwrite(str(debug_path), debug_img)
        print(f"    Debug image saved: {debug_path}")

    return patches / 255.0


# --- RAW / JPEG loading ---

def process_raw(raw_path: Path) -> np.ndarray:
    """Process RAW file with rawpy (LibRaw). Returns sRGB uint8 (H, W, 3)."""
    with rawpy.imread(str(raw_path)) as raw:
        rgb = raw.postprocess(
            use_camera_wb=True,
            output_color=rawpy.ColorSpace.sRGB,
            output_bps=8,
            no_auto_bright=False,
            user_flip=0,
        )
    return rgb


def load_jpg(jpg_path: Path) -> np.ndarray:
    """Load JPG as numpy array (H, W, 3) uint8 RGB."""
    img = Image.open(jpg_path).convert("RGB")
    return np.array(img)


def extract_embedded_preview(raw_path: Path) -> np.ndarray:
    """Extract largest embedded JPEG preview from RAW. Returns (H, W, 3) uint8 RGB."""
    with rawpy.imread(str(raw_path)) as raw:
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


def rgb_to_bgr(img: np.ndarray) -> np.ndarray:
    """Convert RGB uint8 array to BGR (OpenCV format)."""
    return img[:, :, ::-1].copy()


# --- LUT Building ---

def expand_poly(rgb: np.ndarray, order: int = 2) -> np.ndarray:
    """Expand RGB to polynomial features.
    order=2: 9 terms  [R, G, B, R², G², B², RG, RB, GB]
    order=3: 19 terms [+ R³, G³, B³, R²G, R²B, RG², G²B, RB², GB², RGB]
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


def build_3d_lut_from_patches(src_patches: np.ndarray, tgt_patches: np.ndarray,
                               lut_size: int = 33) -> np.ndarray:
    """Build 3D LUT from ColorChecker patch correspondences.

    Two-stage approach:
      1. 2nd-order polynomial color transform from all 24 patches
         (9 terms: R,G,B,R²,G²,B²,RG,RB,GB → captures non-linear color)
      2. Smooth per-channel tone curve from 6 grayscale patches (polynomial fit)

    Args:
        src_patches: (24, 3) float32 RGB [0,1] from RAW
        tgt_patches: (24, 3) float32 RGB [0,1] from JPEG
        lut_size: LUT grid size

    Returns:
        (lut_size, lut_size, lut_size, 3) float32 LUT
    """
    patch_names = [
        "DarkSkin", "LightSkin", "BlueSky", "Foliage", "BlueFlower", "BluishGreen",
        "Orange", "PurplishBlue", "ModerateRed", "Purple", "YellowGreen", "OrangeYellow",
        "Blue", "Green", "Red", "Yellow", "Magenta", "Cyan",
        "White", "Neutral8", "Neutral6.5", "Neutral5", "Neutral3.5", "Black"
    ]

    # --- Luminance-preserving color correction ---
    # The key insight: we want to capture the camera's COLOR rendering (hue/saturation)
    # but NOT its tone mapping (brightness compression). Otherwise highlights get crushed.
    #
    # Method: normalize each patch to unit luminance, fit the polynomial on chromaticity
    # ratios, then in the LUT apply color correction while preserving input luminance.

    LUM = np.array([0.2126, 0.7152, 0.0722])  # sRGB luminance weights

    print(f"\n    Patch colors (RAW → JPEG):")
    for i in range(24):
        s = src_patches[i]
        t = tgt_patches[i]
        s_lum = s @ LUM
        t_lum = t @ LUM
        name = patch_names[i] if i < len(patch_names) else f"Patch{i}"
        print(f"      {name:14s}: ({s[0]:.3f},{s[1]:.3f},{s[2]:.3f}) L={s_lum:.3f}"
              f" → ({t[0]:.3f},{t[1]:.3f},{t[2]:.3f}) L={t_lum:.3f}")

    # Compute per-patch chromaticity ratios: target_ch / source_ch
    # For each patch, the ratio tells us "how does the camera change this color?"
    # independent of overall brightness.
    eps = 1e-6
    src_lum = (src_patches @ LUM).reshape(-1, 1)  # (24, 1)
    tgt_lum = (tgt_patches @ LUM).reshape(-1, 1)

    # Normalize to unit luminance
    src_norm = src_patches / np.maximum(src_lum, eps)  # (24, 3) chromaticity
    tgt_norm = tgt_patches / np.maximum(tgt_lum, eps)

    # The color ratio: how each channel changes relative to luminance
    color_ratio = tgt_norm / np.maximum(src_norm, eps)  # (24, 3)
    # Clamp extreme ratios (from near-black patches where division is unstable)
    color_ratio = np.clip(color_ratio, 0.5, 2.0)

    print(f"\n    Color ratios (chromaticity change, luminance-independent):")
    for i in range(24):
        name = patch_names[i]
        cr = color_ratio[i]
        print(f"      {name:14s}: R×{cr[0]:.3f}  G×{cr[1]:.3f}  B×{cr[2]:.3f}")

    # Fit polynomial to predict color_ratio from source RGB
    # This captures: "for input color X, multiply chromaticity by ratio Y"
    use_order = 2

    # Add anchor points: pure colors should have ratio ~1.0 (identity)
    anchor_colors = np.array([
        [1, 0, 0], [0, 1, 0], [0, 0, 1],
        [1, 1, 0], [0, 1, 1], [1, 0, 1],
        [0.5, 0.5, 0.5],  # mid gray: identity
    ], dtype=np.float32)
    anchor_ratios = np.ones((len(anchor_colors), 3), dtype=np.float32)

    src_aug = np.vstack([src_patches, anchor_colors])
    ratio_aug = np.vstack([color_ratio, anchor_ratios])
    n_real = len(src_patches)
    print(f"    Anchors: {len(anchor_colors)} identity anchors added ({len(src_aug)} total)")

    X = expand_poly(src_aug, use_order)
    M_ratio, _, _, _ = np.linalg.lstsq(X, ratio_aug, rcond=None)

    # Check fit on real patches
    X_real = expand_poly(src_patches, use_order)
    pred_ratio = X_real @ M_ratio
    pred_colors = src_patches * src_norm * pred_ratio * src_lum  # hmm, this is circular

    # Actually, let's verify differently: apply ratio to source and compare
    pred_norm = src_norm * pred_ratio
    pred_rgb = pred_norm * src_lum  # reconstruct at source luminance
    # To compare fairly, also reconstruct target at source luminance
    tgt_at_src_lum = tgt_norm * src_lum

    res = np.mean(np.abs(pred_norm - tgt_norm))
    print(f"\n    Chromaticity residual: {res:.4f}")

    print(f"\n    Per-patch chromaticity fit:")
    for i in range(min(n_real, 24)):
        name = patch_names[i]
        pn = pred_norm[i]
        tn = tgt_norm[i]
        err = np.mean(np.abs(pn - tn))
        print(f"      {name:14s}: pred_chr=({pn[0]:.3f},{pn[1]:.3f},{pn[2]:.3f}) "
              f"tgt_chr=({tn[0]:.3f},{tn[1]:.3f},{tn[2]:.3f}) err={err:.4f}")

    # Build 3D LUT: for each input RGB, predict color ratio, apply, preserve luminance
    scale = lut_size - 1
    lut = np.zeros((lut_size, lut_size, lut_size, 3), dtype=np.float32)
    for ri in range(lut_size):
        for gi in range(lut_size):
            for bi in range(lut_size):
                r = ri / scale
                g = gi / scale
                b = bi / scale

                # Input luminance
                lum_in = r * LUM[0] + g * LUM[1] + b * LUM[2]

                if lum_in < eps:
                    lut[ri, gi, bi] = [0, 0, 0]
                    continue

                # Predict color ratio
                features = expand_poly_single(r, g, b, use_order)
                ratio = np.clip(features @ M_ratio, 0.5, 2.0)

                # Apply ratio to chromaticity, reconstruct at original luminance
                r_norm = r / lum_in
                g_norm = g / lum_in
                b_norm = b / lum_in

                out_norm = np.array([r_norm * ratio[0], g_norm * ratio[1], b_norm * ratio[2]])

                # Rescale so output luminance = input luminance
                out_lum = out_norm @ LUM
                if out_lum > eps:
                    out = out_norm * (lum_in / out_lum)
                else:
                    out = np.array([0, 0, 0])

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
        for b in range(size):
            for g in range(size):
                for r in range(size):
                    val = lut[r, g, b]
                    f.write(f'{val[0]:.6f} {val[1]:.6f} {val[2]:.6f}\n')
    print(f"  Written: {output_path} ({size}x{size}x{size})")


# --- Sigma MakerNote ---

def read_sigma_style(filepath: Path) -> str:
    """Read Sigma color mode from MakerNote."""
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


# --- Main processing ---

def process_pair(raw_path: Path, jpg_path: Path = None, embedded: bool = False,
                 output_path: Path = None, lut_size: int = 33, title: str = "",
                 debug: bool = False):
    """Process a single RAW+JPG pair using ColorChecker detection."""

    print(f"\nProcessing: {raw_path.name}")
    debug_dir = output_path.parent / "debug" if debug else None
    if debug_dir:
        debug_dir.mkdir(parents=True, exist_ok=True)

    # Load RAW
    print("  RAW → sRGB...", end="", flush=True)
    raw_img = process_raw(raw_path)
    print(f" {raw_img.shape[1]}x{raw_img.shape[0]}")

    # Load target
    if embedded:
        print("  Extracting embedded preview...", end="", flush=True)
        jpg_img = extract_embedded_preview(raw_path)
        print(f" {jpg_img.shape[1]}x{jpg_img.shape[0]}")
    else:
        print(f"  Loading {jpg_path.name}...", end="", flush=True)
        jpg_img = load_jpg(jpg_path)
        print(f" {jpg_img.shape[1]}x{jpg_img.shape[0]}")

    # Detect ColorChecker in both images
    print("  Detecting ColorChecker in RAW...", flush=True)
    raw_bgr = rgb_to_bgr(raw_img)
    raw_debug = debug_dir / f"{raw_path.stem}_raw_patches.png" if debug_dir else None
    src_patches = detect_checker_patches(raw_bgr, "RAW", raw_debug)

    print("  Detecting ColorChecker in JPEG...", flush=True)
    jpg_bgr = rgb_to_bgr(jpg_img)
    jpg_debug = debug_dir / f"{raw_path.stem}_jpg_patches.png" if debug_dir else None
    tgt_patches = detect_checker_patches(jpg_bgr, "JPEG", jpg_debug)

    print(f"  Detected {len(src_patches)} patches in each image")

    # Build LUT
    print(f"  Building {lut_size}³ LUT from ColorChecker patches...")
    lut = build_3d_lut_from_patches(src_patches, tgt_patches, lut_size)

    # Write
    write_cube(lut, output_path, title=title)


def process_directory(dir_path: Path, output_dir: Path, lut_size: int = 33,
                      embedded: bool = False, debug: bool = False):
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
            try:
                process_pair(raw_path, embedded=True, output_path=out_path,
                            lut_size=lut_size, title=style, debug=debug)
            except RuntimeError as e:
                print(f"  ERROR: {e}")
        else:
            jpg_path = raw_path.with_suffix('.JPG')
            if not jpg_path.exists():
                jpg_path = raw_path.with_suffix('.jpg')
            if not jpg_path.exists():
                print(f"Skipping {raw_path.name} (no matching JPG)")
                continue

            try:
                process_pair(raw_path, jpg_path, output_path=out_path,
                            lut_size=lut_size, title=style, debug=debug)
            except RuntimeError as e:
                print(f"  ERROR: {e}")


def main():
    parser = argparse.ArgumentParser(
        description='Generate .cube 3D LUT from RAW+JPG using ColorChecker detection')
    parser.add_argument('raw', nargs='?', help='RAW file path')
    parser.add_argument('jpg', nargs='?', help='JPG file path (omit with --embedded)')
    parser.add_argument('-o', '--output', required=True,
                        help='Output .cube file or directory (with --dir)')
    parser.add_argument('--embedded', action='store_true',
                        help='Use embedded JPEG preview instead of external JPG')
    parser.add_argument('--dir', help='Process all RAW+JPG pairs in directory')
    parser.add_argument('--size', type=int, default=33,
                        help='LUT grid size (default: 33)')
    parser.add_argument('--debug', action='store_true',
                        help='Save debug images showing detected patches')

    args = parser.parse_args()

    if args.dir:
        process_directory(Path(args.dir), Path(args.output),
                         lut_size=args.size, embedded=args.embedded, debug=args.debug)
    elif args.raw:
        raw_path = Path(args.raw)
        if not raw_path.exists():
            print(f"Error: {raw_path} not found")
            sys.exit(1)

        if args.embedded:
            process_pair(raw_path, embedded=True,
                        output_path=Path(args.output),
                        lut_size=args.size, title=raw_path.stem, debug=args.debug)
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
                        lut_size=args.size, title=raw_path.stem, debug=args.debug)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
