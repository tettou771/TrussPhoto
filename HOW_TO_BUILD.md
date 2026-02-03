# How to Build TrussPhoto

## Prerequisites

### TrussC

This project depends on [TrussC](https://github.com/TrussC-org/TrussC). Clone it and note the path to `trussc/` directory.

### System Libraries (macOS)

Install via Homebrew:

```bash
brew install exiv2 pkg-config
```

| Library | Version | Purpose |
|---------|---------|---------|
| exiv2 | 0.28+ | EXIF/MakerNote metadata (Sony Creative Style, etc.) |
| pkg-config | - | Used by CMake to find exiv2 |

### Addons (automatic)

These TrussC addons are listed in `addons.make` and resolved automatically during CMake configuration:

| Addon | Source | Purpose |
|-------|--------|---------|
| tcxCurl | system (libcurl) | HTTP client for server sync |
| tcxLibRaw | FetchContent | RAW image decoding |
| tcxLut | header-only | GPU LUT color grading |

### Bundled Data

Lens correction data (from [lensfun](https://github.com/lensfun/lensfun), CC-BY-SA 3.0) is bundled in `bin/data/lensfun/`. No additional installation needed.

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

exiv2 is configured in `local.cmake` rather than as a TrussC addon. This file is automatically loaded by `trussc_app()` during CMake configuration. See [BUILD_SYSTEM.md](../../TrussC/docs/BUILD_SYSTEM.md) for details on the `local.cmake` mechanism.

Lens correction uses a self-contained implementation (`src/LensCorrector.h`) that parses lensfun XML data directly with pugixml. No lensfun library dependency.
