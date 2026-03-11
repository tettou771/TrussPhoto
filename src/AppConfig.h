#pragma once

// =============================================================================
// AppConfig.h - Command-line argument parsing for TrussPhoto
// =============================================================================
// Supported flags:
//   --server                : Run in headless server mode (Crow HTTP)
//   --port N                : Server port (default 18730)
//   --catalog PATH          : Catalog folder path
//   --choose-catalog        : Force catalog selection dialog on launch
//   --raw-dir PATH          : Override RAW storage path (formerly --library-dir)
//   --import-lrcat PATH     : Import from Lightroom Classic catalog (.lrcat)
//   --generate-thumbnails   : Generate missing thumbnails on startup (server mode)
//   --generate-sp           : Generate missing smart previews on startup (server mode)
//   --consolidate           : Consolidate library on startup (copy originals)
//   --path-remap FROM=TO    : Remap file path prefix (repeatable)

#include <string>
#include <vector>
#include <utility>
#include <filesystem>

using namespace std;

namespace AppConfig {

inline bool serverMode = false;
inline bool chooseCatalog = false;
inline int serverPort = 18730;
inline string catalogDir;
inline string rawDir;
inline string importLrcatPath;
inline bool generateThumbnails = false;
inline bool generateSP = false;
inline bool consolidate = false;

// Path remap rules: {from_prefix, to_prefix}
// e.g. --path-remap "/Users/toru/Pictures=/mnt/photos"
inline vector<pair<string, string>> pathRemaps;

inline void parse(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--server") {
            serverMode = true;
        } else if (arg == "--port" && i + 1 < argc) {
            serverPort = atoi(argv[++i]);
        } else if (arg == "--catalog" && i + 1 < argc) {
            catalogDir = argv[++i];
        } else if (arg == "--choose-catalog") {
            chooseCatalog = true;
        } else if ((arg == "--raw-dir" || arg == "--library-dir") && i + 1 < argc) {
            rawDir = argv[++i];
        } else if (arg == "--import-lrcat" && i + 1 < argc) {
            importLrcatPath = argv[++i];
        } else if (arg == "--generate-thumbnails") {
            generateThumbnails = true;
        } else if (arg == "--generate-sp") {
            generateSP = true;
        } else if (arg == "--consolidate") {
            consolidate = true;
        } else if (arg == "--path-remap" && i + 1 < argc) {
            string val = argv[++i];
            auto eq = val.find('=');
            if (eq != string::npos) {
                pathRemaps.push_back({val.substr(0, eq), val.substr(eq + 1)});
            }
        }
    }
}

// Apply path remap rules to a file path.
// Returns the remapped path, or the original if no rule matches.
inline string remapPath(const string& path) {
    for (auto& [from, to] : pathRemaps) {
        if (path.size() >= from.size() && path.compare(0, from.size(), from) == 0) {
            return to + path.substr(from.size());
        }
    }
    return path;
}

// Check if path remapping is active
inline bool hasPathRemap() { return !pathRemaps.empty(); }

} // namespace AppConfig
