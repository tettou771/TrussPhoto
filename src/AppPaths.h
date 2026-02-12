#pragma once

// =============================================================================
// AppPaths.h - Catalog-based path management for TrussPhoto
// =============================================================================
// All persistent data lives inside a user-chosen catalog folder.
// Only a minimal bootstrap config (lastCatalogPath) stays in OS-standard paths.

#include <TrussC.h>
#include <filesystem>
#include <string>

using namespace std;
using namespace tc;

namespace fs = std::filesystem;

namespace AppPaths {

// --- OS-standard bootstrap path (minimal: only app_config.json) ---

inline string appConfigDir() {
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

inline string appConfigPath() {
    return appConfigDir() + "/app_config.json";
}

inline string modelsDir() {
    return appConfigDir() + "/models";
}

// --- Legacy paths (for migration) ---

inline string legacyDataPath() {
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

inline string legacyCachePath() {
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

inline void ensureAppConfigDir() {
    fs::create_directories(appConfigDir());
    fs::create_directories(modelsDir());
}

inline void ensureCatalogDirectories(const string& catalogPath) {
    fs::create_directories(catalogPath);
    fs::create_directories(catalogPath + "/thumbnail_cache");
    fs::create_directories(catalogPath + "/smart_preview");
    fs::create_directories(catalogPath + "/originals");
    fs::create_directories(catalogPath + "/pending");
    logNotice() << "[AppPaths] Catalog: " << catalogPath;
}

// --- Migration from legacy paths to catalog ---
// Copies (not moves) for safety. Old files remain until manually deleted.

inline void migrateFromLegacy(const string& catalogPath) {
    string dp = legacyDataPath();
    string cp = legacyCachePath();
    string binData = getDataPath("");

    bool migrated = false;

    // Migrate library.db (legacy dataPath → catalog)
    string dbDest = catalogPath + "/library.db";
    string dbSrc = dp + "/library.db";
    // Also check bin/data/ as second fallback
    if (!fs::exists(dbDest)) {
        if (fs::exists(dbSrc)) {
            try {
                fs::copy_file(dbSrc, dbDest);
                logNotice() << "[AppPaths] Migrated library.db from legacy dataPath";
                migrated = true;
            } catch (const exception& e) {
                logWarning() << "[AppPaths] Failed to migrate library.db: " << e.what();
            }
        } else {
            string dbBin = binData + "library.db";
            if (fs::exists(dbBin)) {
                try {
                    fs::copy_file(dbBin, dbDest);
                    logNotice() << "[AppPaths] Migrated library.db from bin/data/";
                    migrated = true;
                } catch (const exception& e) {
                    logWarning() << "[AppPaths] Failed to migrate library.db: " << e.what();
                }
            }
        }
    }

    // Migrate settings.json → catalog.json (field rename)
    string catalogJsonDest = catalogPath + "/catalog.json";
    if (!fs::exists(catalogJsonDest)) {
        string settingsSrc = dp + "/settings.json";
        if (!fs::exists(settingsSrc)) {
            settingsSrc = binData + "settings.json";
        }
        if (fs::exists(settingsSrc)) {
            try {
                // Read old settings and convert field names
                ifstream inFile(settingsSrc);
                if (inFile) {
                    nlohmann::json oldJson;
                    inFile >> oldJson;
                    nlohmann::json newJson;
                    newJson["rawStoragePath"] = oldJson.value("libraryFolder", string(""));
                    newJson["serverUrl"] = oldJson.value("serverUrl", string(""));
                    newJson["apiKey"] = oldJson.value("apiKey", string(""));
                    ofstream outFile(catalogJsonDest);
                    if (outFile) {
                        outFile << newJson.dump(2);
                        logNotice() << "[AppPaths] Migrated settings.json → catalog.json";
                        migrated = true;
                    }
                }
            } catch (const exception& e) {
                logWarning() << "[AppPaths] Failed to migrate settings.json: " << e.what();
            }
        }
    }

    // Migrate server_config.json
    string scDest = catalogPath + "/server_config.json";
    string scSrc = dp + "/server_config.json";
    if (!fs::exists(scDest) && fs::exists(scSrc)) {
        try {
            fs::copy_file(scSrc, scDest);
            logNotice() << "[AppPaths] Migrated server_config.json";
            migrated = true;
        } catch (const exception& e) {
            logWarning() << "[AppPaths] Failed to migrate server_config.json: " << e.what();
        }
    }

    // Migrate thumbnail_cache directory (legacy cachePath → catalog)
    string thumbDest = catalogPath + "/thumbnail_cache";
    string thumbSrc = cp + "/thumbnail_cache";
    if (fs::exists(thumbSrc) && fs::is_directory(thumbSrc)) {
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
                migrated = true;
            } catch (const exception& e) {
                logWarning() << "[AppPaths] Failed to migrate thumbnail_cache: " << e.what();
            }
        }
    }

    // Migrate smart_preview directory (legacy dataPath → catalog)
    string spDest = catalogPath + "/smart_preview";
    string spSrc = dp + "/smart_preview";
    if (fs::exists(spSrc) && fs::is_directory(spSrc)) {
        bool destEmpty = true;
        if (fs::exists(spDest)) {
            for (auto& entry : fs::directory_iterator(spDest)) {
                (void)entry;
                destEmpty = false;
                break;
            }
        }
        if (destEmpty) {
            try {
                fs::copy(spSrc, spDest, fs::copy_options::recursive | fs::copy_options::skip_existing);
                logNotice() << "[AppPaths] Migrated smart_preview";
                migrated = true;
            } catch (const exception& e) {
                logWarning() << "[AppPaths] Failed to migrate smart_preview: " << e.what();
            }
        }
    }

    if (migrated) {
        logNotice() << "[AppPaths] Legacy migration complete (old files preserved)";
    }
}

} // namespace AppPaths
