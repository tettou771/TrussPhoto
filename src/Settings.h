#pragma once

// =============================================================================
// Settings.h - Persistent app settings (server URL, library folder, etc.)
// =============================================================================

#include <TrussC.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include "AppPaths.h"

using namespace std;
using namespace tc;

namespace fs = std::filesystem;

class Settings {
public:
    string serverUrl;
    string apiKey;           // API key for server authentication
    string libraryFolder;   // Where to copy/store photos

    // Load from settings.json
    bool load() {
        string path = getSettingsPath();
        if (!fs::exists(path)) return false;

        ifstream file(path);
        if (!file) return false;

        try {
            nlohmann::json j;
            file >> j;
            serverUrl = j.value("serverUrl", serverUrl);
            apiKey = j.value("apiKey", string(""));
            libraryFolder = j.value("libraryFolder", string(""));
            return true;
        } catch (...) {
            return false;
        }
    }

    // Save to settings.json
    void save() {
        string path = getSettingsPath();
        nlohmann::json j = {
            {"serverUrl", serverUrl},
            {"apiKey", apiKey},
            {"libraryFolder", libraryFolder}
        };
        ofstream file(path);
        if (file) {
            file << j.dump(2);
            logNotice() << "[Settings] Saved to " << path;
        }
    }

    // Check if first run (no library folder set)
    bool isFirstRun() const {
        return libraryFolder.empty();
    }

    // Check if server is configured
    bool hasServer() const {
        return !serverUrl.empty();
    }

private:
    static string getSettingsPath() {
        return AppPaths::dataPath() + "/settings.json";
    }
};
