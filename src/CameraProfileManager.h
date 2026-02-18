#pragma once

// =============================================================================
// CameraProfileManager - Manages camera color profiles (.cube LUT files)
// =============================================================================
// Profile directory structure:
//   ~/.trussc/profiles/
//     Sony_ILCE-7CM2/
//       Standard.cube      <- Creative Style name
//       Vivid.cube
//       _default.cube      <- Fallback when style is unknown
//     SIGMA_BF/
//       Standard.cube
// =============================================================================

#include <TrussC.h>
#include <filesystem>
#include <unordered_map>
using namespace std;
using namespace tc;

namespace fs = std::filesystem;

class CameraProfileManager {
public:
    // Set the profile root directory and scan for profiles
    void setProfileDir(const string& dir) {
        profileDir_ = dir;
        scanProfiles();
    }

    // Scan profileDir_ for .cube files
    void scanProfiles() {
        profiles_.clear();
        if (profileDir_.empty() || !fs::exists(profileDir_)) return;

        for (const auto& cameraDir : fs::directory_iterator(profileDir_)) {
            if (!cameraDir.is_directory()) continue;
            string cameraKey = cameraDir.path().filename().string();

            for (const auto& file : fs::directory_iterator(cameraDir.path())) {
                if (!file.is_regular_file()) continue;
                string ext = file.path().extension().string();
                if (ext != ".cube" && ext != ".CUBE") continue;

                string styleName = file.path().stem().string();
                string key = cameraKey + "/" + styleName;
                profiles_[key] = file.path().string();
            }
        }

        if (!profiles_.empty()) {
            logNotice() << "[ProfileManager] Found " << profiles_.size() << " profiles";
        }
    }

    // Find profile .cube path for a given camera model and creative style
    // Camera model should match directory name exactly (from Exif.Image.Model)
    // Search order: exact style match -> _default -> empty
    string findProfile(const string& cameraModel, const string& style = "") const {
        string cameraKey = sanitize(cameraModel);

        // 1. Try exact style match
        if (!style.empty()) {
            auto it = profiles_.find(cameraKey + "/" + style);
            if (it != profiles_.end()) return it->second;
        }

        // 2. Fallback to _default
        {
            auto it = profiles_.find(cameraKey + "/_default");
            if (it != profiles_.end()) return it->second;
        }

        return "";
    }

    bool hasProfile(const string& cameraModel) const {
        string cameraKey = sanitize(cameraModel);
        for (const auto& [key, path] : profiles_) {
            if (key.substr(0, cameraKey.size()) == cameraKey &&
                key.size() > cameraKey.size() && key[cameraKey.size()] == '/') {
                return true;
            }
        }
        return false;
    }

    const string& getProfileDir() const { return profileDir_; }

private:
    string profileDir_;
    unordered_map<string, string> profiles_;  // "CameraKey/StyleName" -> path

    // Sanitize camera model name for directory matching
    // Spaces are preserved to match directory names exactly
    static string sanitize(const string& name) {
        return name;
    }
};
