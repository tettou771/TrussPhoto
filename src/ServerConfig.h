#pragma once

// =============================================================================
// ServerConfig.h - Server configuration with API key management
// =============================================================================
// Manages API key generation and persistence for --server mode.
// Config stored in tpDataPath/server_config.json.

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>

using namespace std;

namespace fs = std::filesystem;

class ServerConfig {
public:
    string apiKey;
    int port = 18730;

    // Load from file; returns true if newly created
    bool load(const string& path) {
        configPath_ = path;
        if (fs::exists(path)) {
            ifstream file(path);
            if (file) {
                try {
                    nlohmann::json j;
                    file >> j;
                    apiKey = j.value("apiKey", string(""));
                    port = j.value("port", 18730);
                    if (!apiKey.empty()) return false;
                } catch (...) {}
            }
        }
        return true; // needs key generation
    }

    void generateKeyIfMissing() {
        if (apiKey.empty()) {
            apiKey = generateRandomKey();
        }
    }

    void save() {
        if (configPath_.empty()) return;
        nlohmann::json j = {
            {"apiKey", apiKey},
            {"port", port}
        };
        ofstream file(configPath_);
        if (file) {
            file << j.dump(2);
        }
    }

    void save(const string& path) {
        configPath_ = path;
        save();
    }

private:
    string configPath_;

    static string generateRandomKey() {
        random_device rd;
        mt19937_64 gen(rd());
        uniform_int_distribution<uint64_t> dist;

        ostringstream oss;
        oss << hex << setfill('0');
        for (int i = 0; i < 4; i++) {
            oss << setw(16) << dist(gen);
        }
        return oss.str();
    }
};
