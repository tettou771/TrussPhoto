#pragma once

// =============================================================================
// AppPaths.h - OS-standard persistent data and cache paths
// =============================================================================
// tpDataPath: permanent data (library.db, settings.json, server_config.json)
// tpCachePath: regenerable cache (thumbnail_cache/)
// bin/data/: read-only bundle resources (lensfun DB, etc.)

#include <TrussC.h>
#include <filesystem>
#include <string>

using namespace std;
using namespace tc;

namespace fs = std::filesystem;

namespace AppPaths {

// --- Platform-specific base paths ---

inline string dataPath() {
#if defined(__APPLE__)
    string home = getenv("HOME") ? getenv("HOME") : ".";
    return home + "/Library/Application Support/TrussPhoto";
#elif defined(_WIN32)
    string appdata = getenv("APPDATA") ? getenv("APPDATA") : ".";
    return appdata + "/TrussPhoto";
#else
    string home = getenv("HOME") ? getenv("HOME") : ".";
    return home + "/.local/share/TrussPhoto";
#endif
}

inline string cachePath() {
#if defined(__APPLE__)
    string home = getenv("HOME") ? getenv("HOME") : ".";
    return home + "/Library/Caches/TrussPhoto";
#elif defined(_WIN32)
    string localAppData = getenv("LOCALAPPDATA") ? getenv("LOCALAPPDATA") : ".";
    return localAppData + "/TrussPhoto";
#else
    string home = getenv("HOME") ? getenv("HOME") : ".";
    return home + "/.cache/TrussPhoto";
#endif
}

// --- Directory creation ---

inline void ensureDirectories() {
    fs::create_directories(dataPath());
    fs::create_directories(cachePath());
    fs::create_directories(dataPath() + "/smart_preview");
    fs::create_directories(cachePath() + "/thumbnail_cache");
    logNotice() << "[AppPaths] Data: " << dataPath();
    logNotice() << "[AppPaths] Cache: " << cachePath();
}

// --- Migration from bin/data/ to OS-standard paths ---
// Copies (not moves) for safety. Old files remain until manually deleted.

inline void migrateFromBinData() {
    string dp = dataPath();
    string cp = cachePath();
    string binData = getDataPath("");

    // Migrate library.db
    string dbDest = dp + "/library.db";
    string dbSrc = binData + "library.db";
    if (!fs::exists(dbDest) && fs::exists(dbSrc)) {
        try {
            fs::copy_file(dbSrc, dbDest);
            logNotice() << "[AppPaths] Migrated library.db";
        } catch (const exception& e) {
            logWarning() << "[AppPaths] Failed to migrate library.db: " << e.what();
        }
    }

    // Migrate settings.json
    string settingsDest = dp + "/settings.json";
    string settingsSrc = binData + "settings.json";
    if (!fs::exists(settingsDest) && fs::exists(settingsSrc)) {
        try {
            fs::copy_file(settingsSrc, settingsDest);
            logNotice() << "[AppPaths] Migrated settings.json";
        } catch (const exception& e) {
            logWarning() << "[AppPaths] Failed to migrate settings.json: " << e.what();
        }
    }

    // Migrate thumbnail_cache directory
    string thumbDest = cp + "/thumbnail_cache";
    string thumbSrc = binData + "thumbnail_cache";
    if (fs::exists(thumbSrc) && fs::is_directory(thumbSrc)) {
        // Only migrate if dest is empty or doesn't have subdirs yet
        bool destEmpty = true;
        if (fs::exists(thumbDest)) {
            for (auto& entry : fs::directory_iterator(thumbDest)) {
                (void)entry;
                destEmpty = false;
                break;
            }
        }
        if (destEmpty) {
            try {
                fs::copy(thumbSrc, thumbDest, fs::copy_options::recursive | fs::copy_options::skip_existing);
                logNotice() << "[AppPaths] Migrated thumbnail_cache";
            } catch (const exception& e) {
                logWarning() << "[AppPaths] Failed to migrate thumbnail_cache: " << e.what();
            }
        }
    }
}

} // namespace AppPaths
