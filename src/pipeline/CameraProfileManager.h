#pragma once

// =============================================================================
// CameraProfileManager - Manages camera color profiles (.dcp and .cube files)
// =============================================================================
// Profile directory structure:
//   ~/.trussc/profiles/
//     Sony ILCE-7CM2/           <- Camera model name (spaces preserved)
//       Camera ST.dcp           <- DCP profile (preferred)
//       Standard.cube           <- .cube LUT (fallback)
//       _default.cube           <- Fallback when style is unknown
//     SIGMA_BF/
//       Standard.cube
//
// Priority: .dcp > .cube (DCP has camera-specific color science)
// =============================================================================

#include <TrussC.h>
#include <filesystem>
#include <unordered_map>
using namespace std;
using namespace tc;

namespace fs = std::filesystem;

enum class ProfileType { None, DCP, CubeLUT };

struct ProfileInfo {
    string path;
    ProfileType type = ProfileType::None;
};

class CameraProfileManager {
public:
    // Set the profile root directory and scan for profiles
    void setProfileDir(const string& dir) {
        profileDir_ = dir;
        scanProfiles();
    }

    // Scan profileDir_ for .dcp and .cube files
    void scanProfiles() {
        profiles_.clear();
        if (profileDir_.empty() || !fs::exists(profileDir_)) return;

        for (const auto& cameraDir : fs::directory_iterator(profileDir_)) {
            if (!cameraDir.is_directory()) continue;
            string cameraKey = cameraDir.path().filename().string();

            for (const auto& file : fs::directory_iterator(cameraDir.path())) {
                if (!file.is_regular_file()) continue;
                string ext = file.path().extension().string();

                ProfileType type = ProfileType::None;
                if (ext == ".dcp" || ext == ".DCP") type = ProfileType::DCP;
                else if (ext == ".cube" || ext == ".CUBE") type = ProfileType::CubeLUT;
                else continue;

                string styleName = file.path().stem().string();
                string key = cameraKey + "/" + styleName;

                // DCP takes priority over .cube with same name
                auto it = profiles_.find(key);
                if (it == profiles_.end() || type == ProfileType::DCP) {
                    profiles_[key] = {file.path().string(), type};
                }
            }
        }

        if (!profiles_.empty()) {
            int dcpCount = 0, cubeCount = 0;
            for (auto& [k, v] : profiles_) {
                if (v.type == ProfileType::DCP) dcpCount++;
                else cubeCount++;
            }
            logNotice() << "[ProfileManager] Found " << profiles_.size()
                        << " profiles (" << dcpCount << " DCP, " << cubeCount << " cube)";
        }
    }

    // Find profile for a given camera model and creative style
    // Returns path and type. Search order: exact style → _default → empty
    ProfileInfo findProfileInfo(const string& cameraModel, const string& style = "") const {
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

        return {"", ProfileType::None};
    }

    // Legacy API: returns path string (backward compatible)
    string findProfile(const string& cameraModel, const string& style = "") const {
        return findProfileInfo(cameraModel, style).path;
    }

    bool hasProfile(const string& cameraModel) const {
        string cameraKey = sanitize(cameraModel);
        for (const auto& [key, info] : profiles_) {
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
    unordered_map<string, ProfileInfo> profiles_;  // "CameraKey/StyleName" -> info

    // Sanitize camera model name for directory matching
    static string sanitize(const string& name) {
        return name;
    }
};
