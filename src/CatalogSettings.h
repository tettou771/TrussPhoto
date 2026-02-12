#pragma once

// =============================================================================
// CatalogSettings.h - Per-catalog persistent settings (catalog.json)
// =============================================================================

#include <TrussC.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using namespace std;
using namespace tc;

namespace fs = std::filesystem;

class CatalogSettings {
public:
    string rawStoragePath;   // Where to store RAW originals (empty = catalog/originals/)
    string serverUrl;
    string apiKey;           // API key for server authentication

    // Load from catalog.json
    bool load(const string& path) {
        settingsPath_ = path;
        if (!fs::exists(path)) return false;

        ifstream file(path);
        if (!file) return false;

        try {
            nlohmann::json j;
            file >> j;
            rawStoragePath = j.value("rawStoragePath", string(""));
            serverUrl = j.value("serverUrl", string(""));
            apiKey = j.value("apiKey", string(""));
            return true;
        } catch (...) {
            return false;
        }
    }

    // Save to catalog.json
    void save() {
        if (settingsPath_.empty()) return;
        nlohmann::json j = {
            {"rawStoragePath", rawStoragePath},
            {"serverUrl", serverUrl},
            {"apiKey", apiKey}
        };
        ofstream file(settingsPath_);
        if (file) {
            file << j.dump(2);
            logNotice() << "[CatalogSettings] Saved to " << settingsPath_;
        }
    }

    // Check if server is configured
    bool hasServer() const {
        return !serverUrl.empty();
    }

private:
    string settingsPath_;
};

// =============================================================================
// AppBootstrap - Minimal bootstrap config in OS-standard path
// =============================================================================
// Only stores lastCatalogPath so app knows which catalog to open on next launch.

class AppBootstrap {
public:
    string lastCatalogPath;

    bool load(const string& path) {
        if (!fs::exists(path)) return false;

        ifstream file(path);
        if (!file) return false;

        try {
            nlohmann::json j;
            file >> j;
            lastCatalogPath = j.value("lastCatalogPath", string(""));
            return !lastCatalogPath.empty();
        } catch (...) {
            return false;
        }
    }

    void save(const string& path) {
        nlohmann::json j = {
            {"lastCatalogPath", lastCatalogPath}
        };
        ofstream file(path);
        if (file) {
            file << j.dump(2);
        }
    }
};
