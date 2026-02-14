# How to Build TrussPhoto

## Quick Start (macOS)

```bash
# 1. Install system dependencies
brew install exiv2 jpeg-xl onnxruntime pkg-config

# 2. Configure & build
cmake --preset macos
cmake --build build-macos

# 3. (Recommended) Set up LINE CLIP Japanese v2 for high-quality Japanese search
python3 -m venv .venv && source .venv/bin/activate
pip install transformers torch sentencepiece protobuf ftfy onnx onnxruntime pillow timm
python scripts/export_japanese_clip.py

# 4. Run
./bin/TrussPhoto.app/Contents/MacOS/TrussPhoto
```

---

## Prerequisites

### TrussC Framework

TrussPhoto is built on [TrussC](https://github.com/nicotoru/TrussC), a lightweight creative coding framework. Clone TrussC and ensure the directory structure looks like this:

```
TrussC/
├── trussc/              <- Core framework
├── addons/
│   ├── tcxCrow/         <- HTTP server (Crow)
│   ├── tcxCurl/         <- HTTP client (libcurl)
│   ├── tcxLibRaw/       <- RAW image decoding (LibRaw)
│   ├── tcxLut/          <- GPU LUT color grading
│   └── tcxIME/          <- IME text input
└── apps/
    └── TrussPhoto/      <- This project
```

`CMakePresets.json` contains an absolute path to TrussC (`TRUSSC_DIR`). Update it to match your environment, or regenerate with the TrussC Project Generator:

```bash
projectGenerator --update /path/to/TrussPhoto
```

### System Libraries (macOS)

Install via Homebrew:

```bash
brew install exiv2 jpeg-xl onnxruntime pkg-config
```

| Library | brew package | Version | Purpose |
|---------|-------------|---------|---------|
| exiv2 | `exiv2` | 0.28+ | EXIF / MakerNote / XMP metadata |
| libjxl | `jpeg-xl` | - | JPEG XL smart preview encode/decode |
| ONNX Runtime | `onnxruntime` | 1.20+ | CLIP embedding inference (vision + text) |
| pkg-config | `pkg-config` | - | Used by CMake to find the above libraries |

These are linked as **dynamic libraries** (dylib) via `pkg-config` in `local.cmake`.

### Addons (automatic)

Listed in `addons.make`, resolved automatically during CMake configuration:

| Addon | Source | Purpose |
|-------|--------|---------|
| tcxCurl | system libcurl | HTTP client for server sync |
| tcxCrow | header-only (Crow) | HTTP server (`--server` mode) |
| tcxLibRaw | FetchContent (LibRaw) | RAW image decoding (OpenMP parallel demosaic) |
| tcxLut | header-only | GPU LUT color grading shader |
| tcxIME | platform-native | IME text input for search bar |

### Auto-fetched (CMake FetchContent)

| Library | Purpose |
|---------|---------|
| SQLite amalgamation | Embedded photo database |
| LibRaw | RAW image processing (via tcxLibRaw) |
| SentencePiece | Tokenizer for Japanese Stable CLIP text encoder |

### Bundled Data

| Path | Content | License |
|------|---------|---------|
| `bin/data/lensfun/` | Lens correction database (XML) | CC-BY-SA 3.0 |

---

## Build

### macOS

```bash
cmake --preset macos
cmake --build build-macos
```

Output: `bin/TrussPhoto.app`

### Xcode

```bash
cmake --preset xcode
open xcode/TrussPhoto.xcodeproj
```

### Adding / Removing Source Files

TrussPhoto uses `GLOB`-based source collection. After adding or removing `.h` / `.cpp` files in `src/`, re-run cmake configure:

```bash
cmake --preset macos
```

---

## Runtime Setup

### First Launch

On first launch, TrussPhoto will prompt you to choose or create a **catalog folder**. All app data (database, thumbnails, smart previews, originals) is stored inside this folder.

```
MyCatalog/
├── library.db              # SQLite database
├── catalog.json            # Catalog settings
├── thumbnail_cache/        # JPEG thumbnail cache
├── smart_preview/          # JPEG XL smart previews
├── originals/              # RAW file storage (if copying enabled)
└── pending/                # Upload queue (server mode)
```

A minimal bootstrap config is stored in the OS-standard path:

| OS | Path |
|----|------|
| macOS | `~/Library/Application Support/TrussPhoto/app_config.json` |
| Linux | `~/.local/share/TrussPhoto/app_config.json` |
| Windows | `%APPDATA%/TrussPhoto/app_config.json` |

### LINE CLIP Japanese v2 (recommended)

For high-quality Japanese + English search, export the LINE CLIP Japanese Base v2 model (Apache 2.0, no login required):

```bash
cd /path/to/TrussPhoto
python3 -m venv .venv
source .venv/bin/activate

# Install Python dependencies
pip install transformers torch sentencepiece protobuf ftfy onnx onnxruntime pillow timm

# Export vision + text ONNX models and tokenizer
python scripts/export_japanese_clip.py
```

This produces three files in `~/Library/Application Support/TrussPhoto/models/`:

| File | Size | Description |
|------|------|-------------|
| `line-clip-ja-v2-vision.onnx` | ~350 MB | EVA02-B/16 vision encoder -> 256-dim |
| `line-clip-ja-v2-text.onnx` | ~350 MB | rinna RoBERTa text encoder -> 256-dim |
| `line-clip-ja-v2-spiece.model` | ~800 KB | SentencePiece tokenizer model |

On next launch, TrussPhoto will detect these files and use LINE CLIP for both vision and text encoding. All existing image embeddings will be automatically regenerated with the new model.

Log output: `[CLIP] Ready (line-clip-ja-v2, 256-dim)` + `[TextEncoder] LINE CLIP Japanese v2 mode ready`

### Fallback: CLIP ViT-B/32 (auto-downloaded)

If the Japanese CLIP ONNX files are not present, TrussPhoto automatically falls back to CLIP ViT-B/32:

| Model | Size | Location |
|-------|------|----------|
| CLIP ViT-B/32 vision | ~338 MB | `models/clip-vit-b32-vision.onnx` |
| CLIP ViT-B/32 text (English) | ~254 MB | `models/clip-vit-b32-text.onnx` |
| BPE vocab + merges | ~2 MB | `models/clip-vit-b32-vocab.json`, `models/clip-vit-b32-merges.txt` |

Download happens in the background. The app is fully usable during download.

### Multilingual Search (legacy, optional)

For multilingual search without Japanese CLIP, you can still use the DistilBERT multilingual model (produces 512-dim vectors in the ViT-B/32 embedding space):

```bash
pip install sentence-transformers torch onnx onnxscript onnxruntime
python scripts/export_multilingual_clip.py
```

> **Note:** If LINE CLIP Japanese v2 files are present, they take priority over multilingual CLIP.

### Camera Profiles (optional)

Custom camera profiles (.cube LUT files) can be placed in:

```
~/.trussc/profiles/
  SONY_ILCE-7CM2/           <- Camera model (spaces -> underscores)
    Portrait.cube
    Standard.cube
    _default.cube            <- Fallback when style is unknown
```

---

## Command-Line Arguments

```bash
# GUI mode (default)
./bin/TrussPhoto.app/Contents/MacOS/TrussPhoto

# Specify catalog
./bin/TrussPhoto.app/Contents/MacOS/TrussPhoto --catalog /path/to/MyCatalog

# Headless server mode
./bin/TrussPhoto.app/Contents/MacOS/TrussPhoto --server --catalog /path/to/MyCatalog

# Server with options
./bin/TrussPhoto.app/Contents/MacOS/TrussPhoto --server --catalog /path/to/catalog --port 8080 --raw-dir /photos
```

| Flag | Description | Default |
|------|-------------|---------|
| `--server` | Headless REST API server mode | GUI mode |
| `--port N` | Server port | 18730 |
| `--catalog PATH` | Catalog folder | Last used / dialog |
| `--raw-dir PATH` | RAW storage path override | `catalog.json` value or `originals/` |

---

## Distribution Notes

Currently TrussPhoto depends on Homebrew dylibs at runtime:

- `/opt/homebrew/opt/exiv2/lib/libexiv2.28.dylib`
- `/opt/homebrew/opt/jpeg-xl/lib/libjxl.0.dylib`
- `/opt/homebrew/opt/onnxruntime/lib/libonnxruntime.*.dylib`

For distribution to machines without Homebrew, these dylibs need to be bundled inside the `.app` bundle using `install_name_tool` to rewrite rpaths, or the libraries need to be built from source as static libraries. This is planned for a future release.

---

## Troubleshooting

### `pkg_check_modules` fails for exiv2 / libjxl / onnxruntime

Make sure Homebrew is installed and the packages are present:
```bash
brew list exiv2 jpeg-xl onnxruntime pkg-config
```

If using Apple Silicon, ensure `/opt/homebrew/bin` is in your PATH.

### Library not loaded: libexiv2.28.dylib (runtime)

The app can't find the dylib at runtime. Either install via `brew install exiv2`, or set `DYLD_LIBRARY_PATH` to the directory containing the dylib.

### CLIP model download stalls

The ViT-B/32 vision model (~338 MB) is downloaded via `curl` in the background. Check network connectivity. Partial downloads are cleaned up automatically. You can also download manually:

```bash
cd ~/Library/Application\ Support/TrussPhoto/models/
curl -L -o clip-vit-b32-vision.onnx \
  https://huggingface.co/Qdrant/clip-ViT-B-32-vision/resolve/main/model.onnx
```

### LINE CLIP export fails

1. Ensure sufficient disk space (~2 GB for model download + export)
2. Required Python packages:
```bash
pip install transformers torch sentencepiece protobuf ftfy onnx onnxruntime pillow timm
```

### multilingual-clip-text.onnx export fails

Ensure you have sufficient disk space (~1 GB for model download + export) and the correct Python packages:
```bash
python3 -m venv .venv && source .venv/bin/activate
pip install sentence-transformers torch onnx onnxscript onnxruntime
```

The script downloads `sentence-transformers/clip-ViT-B-32-multilingual-v1` from HuggingFace on first run (~1 GB download).
