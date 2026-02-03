# How to Build TrussPhoto

## Prerequisites

### TrussC

This project depends on [TrussC](https://github.com/TrussC-org/TrussC). Clone it and note the path to `trussc/` directory.

### System Libraries (macOS)

Install via Homebrew:

```bash
brew install exiv2 lensfun pkg-config
```

| Library | Version | Purpose |
|---------|---------|---------|
| exiv2 | 0.28+ | EXIF/MakerNote metadata (Sony Creative Style, etc.) |
| lensfun | 0.3+ | Lens distortion/vignetting/CA correction |
| pkg-config | - | Used by CMake to find the above libraries |

### Addons (automatic)

These TrussC addons are listed in `addons.make` and resolved automatically during CMake configuration:

| Addon | Source | Purpose |
|-------|--------|---------|
| tcxCurl | system (libcurl) | HTTP client for server sync |
| tcxLibRaw | FetchContent | RAW image decoding |
| tcxLut | header-only | GPU LUT color grading |

## Build

### 1. Configure CMakePresets.json

`CMakePresets.json` contains an absolute path to TrussC (`TRUSSC_DIR`). Update it to match your environment, or regenerate with the TrussC Project Generator:

```bash
projectGenerator --update /path/to/TrussPhoto
```

### 2. Build

```bash
cmake --preset macos
cmake --build build-macos
```

The built app is output to `bin/TrussPhoto.app`.

## Project-Specific Dependencies

System libraries (exiv2, lensfun) are configured in `local.cmake` rather than as TrussC addons. This file is automatically loaded by `trussc_app()` during CMake configuration. See [BUILD_SYSTEM.md](../../TrussC/docs/BUILD_SYSTEM.md) for details on the `local.cmake` mechanism.
