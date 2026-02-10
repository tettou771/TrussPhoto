#pragma once

// =============================================================================
// AppConfig.h - Command-line argument parsing for TrussPhoto
// =============================================================================
// Supported flags:
//   --server           : Run in headless server mode (Crow HTTP)
//   --port N           : Server port (default 18730)
//   --library-dir PATH : Override library folder

#include <string>

using namespace std;

namespace AppConfig {

inline bool serverMode = false;
inline int serverPort = 18730;
inline string libraryDir;

inline void parse(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--server") {
            serverMode = true;
        } else if (arg == "--port" && i + 1 < argc) {
            serverPort = atoi(argv[++i]);
        } else if (arg == "--library-dir" && i + 1 < argc) {
            libraryDir = argv[++i];
        }
    }
}

} // namespace AppConfig
