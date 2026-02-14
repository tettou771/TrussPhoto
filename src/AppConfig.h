#pragma once

// =============================================================================
// AppConfig.h - Command-line argument parsing for TrussPhoto
// =============================================================================
// Supported flags:
//   --server           : Run in headless server mode (Crow HTTP)
//   --port N           : Server port (default 18730)
//   --catalog PATH     : Catalog folder path
//   --choose-catalog   : Force catalog selection dialog on launch
//   --raw-dir PATH     : Override RAW storage path (formerly --library-dir)
//   --import-lrcat PATH: Import from Lightroom Classic catalog (.lrcat)

#include <string>

using namespace std;

namespace AppConfig {

inline bool serverMode = false;
inline bool chooseCatalog = false;
inline int serverPort = 18730;
inline string catalogDir;
inline string rawDir;
inline string importLrcatPath;

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
        }
    }
}

} // namespace AppConfig
