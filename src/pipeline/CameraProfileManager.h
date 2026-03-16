#pragma once

// =============================================================================
// CameraProfileManager - Manages camera color profiles (.dcp and .cube files)
// =============================================================================
// Profile directory structure:
//   ~/.trussc/profiles/
//     SONY_ILCE-7CM2/               <- Camera dir (vendor_model or just model)
//       Sony ILCE-7CM2 Camera ST.dcp  <- Adobe DCP (preferred)
//       Standard.cube               <- .cube LUT (fallback)
//       _default.cube               <- Fallback when style is unknown
//
// DCP file naming: "{Vendor} {Model} Camera {StyleAbbrev}.dcp"
//   StyleAbbrev: ST=Standard, PT=Portrait, VV=Vivid, NT=Neutral,
//                FL=Flat, IN=Light, SH=Deep, BW=Monochrome, VV2=Vivid2
//
// Camera model matching: directory name must contain the EXIF model string
//   e.g. dir "SONY_ILCE-7CM2" contains "ILCE-7CM2" → match
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
        profileDirs_.clear();
        profileDirs_.push_back(dir);
        scanProfiles();
    }

    // Add an additional profile directory (scanned after the primary)
    void addProfileDir(const string& dir) {
        profileDirs_.push_back(dir);
        scanProfiles();
    }

    // Scan all profile directories for .dcp and .cube files
    void scanProfiles() {
        profiles_.clear();
        cameraDirs_.clear();

        for (const auto& profileDir : profileDirs_) {
        if (profileDir.empty() || !fs::exists(profileDir)) continue;

        for (const auto& cameraDir : fs::directory_iterator(profileDir)) {
            if (!cameraDir.is_directory()) continue;
            string dirName = cameraDir.path().filename().string();

            for (const auto& file : fs::directory_iterator(cameraDir.path())) {
                if (!file.is_regular_file()) continue;
                string ext = file.path().extension().string();

                ProfileType type = ProfileType::None;
                if (ext == ".dcp" || ext == ".DCP") type = ProfileType::DCP;
                else if (ext == ".cube" || ext == ".CUBE") type = ProfileType::CubeLUT;
                else {
                    logNotice() << "[ProfileManager] Skipping unknown ext: " << ext
                                << " file: " << file.path().filename().string();
                    continue;
                }

                string stem = file.path().stem().string();

                // Extract style name from DCP filename
                // Pattern: "Vendor Model Camera XX" → style = "XX"
                string styleName;
                if (type == ProfileType::DCP) {
                    styleName = extractDcpStyle(stem);
                } else {
                    styleName = stem;  // .cube: filename IS the style name
                }

                // Key: "dirName/styleName"
                string key = dirName + "/" + styleName;

                // DCP takes priority over .cube with same style
                auto it = profiles_.find(key);
                if (it == profiles_.end() || type == ProfileType::DCP) {
                    profiles_[key] = {file.path().string(), type};
                }
            }

            // Remember directory name for model matching (avoid duplicates across dirs)
            if (find(cameraDirs_.begin(), cameraDirs_.end(), dirName) == cameraDirs_.end()) {
                cameraDirs_.push_back(dirName);
            }
        }
        } // end for profileDirs_

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
    // cameraModel: EXIF string e.g. "ILCE-7CM2"
    // style: Creative Style e.g. "Portrait", "Standard", "Vivid", etc.
    ProfileInfo findProfileInfo(const string& cameraModel, const string& style = "") const {
        // Step 1: Find matching camera directory
        string dirName = findCameraDir(cameraModel);
        if (dirName.empty()) return {"", ProfileType::None};

        // Step 2: Map creative style name to DCP abbreviation
        string abbrev = styleToAbbrev(style);

        logNotice() << "[ProfileManager] findProfileInfo: camera=" << cameraModel
                     << " style=" << style << " → dir=" << dirName << " abbrev=" << abbrev;

        // Try DCP-style abbreviation first (e.g. "ST", "PT")
        if (!abbrev.empty()) {
            auto it = profiles_.find(dirName + "/" + abbrev);
            if (it != profiles_.end()) return it->second;
        }

        // Try full style name (for .cube files like "Standard.cube")
        if (!style.empty()) {
            auto it = profiles_.find(dirName + "/" + style);
            if (it != profiles_.end()) return it->second;
        }

        // Try "Camera ST" as default for DCP (Adobe Camera Standard)
        {
            auto it = profiles_.find(dirName + "/ST");
            if (it != profiles_.end()) return it->second;
        }

        // Try _default (for .cube)
        {
            auto it = profiles_.find(dirName + "/_default");
            if (it != profiles_.end()) return it->second;
        }

        // Return first available profile for this camera
        string prefix = dirName + "/";
        for (const auto& [key, info] : profiles_) {
            if (key.substr(0, prefix.size()) == prefix) {
                return info;
            }
        }

        return {"", ProfileType::None};
    }

    // Legacy API: returns path string (backward compatible)
    string findProfile(const string& cameraModel, const string& style = "") const {
        return findProfileInfo(cameraModel, style).path;
    }

    bool hasProfile(const string& cameraModel) const {
        return !findCameraDir(cameraModel).empty();
    }

    string getProfileDir() const { return profileDirs_.empty() ? "" : profileDirs_[0]; }

private:
    vector<string> profileDirs_;                    // profile root directories
    unordered_map<string, ProfileInfo> profiles_;   // "dirName/styleName" -> info
    vector<string> cameraDirs_;                     // list of camera directory names

    // Find camera directory that contains the EXIF model string
    // e.g. "ILCE-7CM2" matches dir "SONY_ILCE-7CM2"
    // If multiple dirs match, prefer the one with DCP files
    string findCameraDir(const string& cameraModel) const {
        if (cameraModel.empty()) return "";

        string modelUpper = toUpper(cameraModel);

        // Collect all matching dirs, then pick the best one
        vector<string> candidates;

        // Exact match
        for (const auto& dir : cameraDirs_) {
            if (toUpper(dir) == modelUpper) candidates.push_back(dir);
        }

        // Substring match: dir contains model
        for (const auto& dir : cameraDirs_) {
            if (toUpper(dir).find(modelUpper) != string::npos) {
                // Avoid duplicates
                if (find(candidates.begin(), candidates.end(), dir) == candidates.end())
                    candidates.push_back(dir);
            }
        }

        // Reverse: model contains dir
        for (const auto& dir : cameraDirs_) {
            if (modelUpper.find(toUpper(dir)) != string::npos) {
                if (find(candidates.begin(), candidates.end(), dir) == candidates.end())
                    candidates.push_back(dir);
            }
        }

        if (candidates.empty()) return "";
        if (candidates.size() == 1) return candidates[0];

        // Multiple matches — prefer directory that has DCP files
        for (const auto& dir : candidates) {
            string prefix = dir + "/";
            for (const auto& [key, info] : profiles_) {
                if (key.substr(0, prefix.size()) == prefix && info.type == ProfileType::DCP) {
                    return dir;
                }
            }
        }

        return candidates[0];
    }

    // Extract style abbreviation from DCP filename stem
    // "Sony ILCE-7CM2 Camera ST" → "ST"
    // "Canon EOS R5 Camera Standard" → "Standard"
    static string extractDcpStyle(const string& stem) {
        // Find "Camera " prefix — everything after it is the style
        size_t pos = stem.rfind("Camera ");
        if (pos != string::npos) {
            return stem.substr(pos + 7);  // 7 = strlen("Camera ")
        }
        // No "Camera " prefix — use the whole stem
        return stem;
    }

    // Map Sony/general creative style names to Adobe DCP abbreviations
    static string styleToAbbrev(const string& style) {
        if (style.empty()) return "";

        string s = toLower(style);

        if (s == "standard")    return "ST";
        if (s == "portrait")    return "PT";
        if (s == "vivid")       return "VV";
        if (s == "neutral")     return "NT";
        if (s == "flat")        return "FL";
        if (s == "light")       return "IN";  // Adobe "Instant" ≈ Sony "Light"
        if (s == "deep")        return "SH";  // Adobe "Shade" ≈ Sony "Deep"
        if (s == "monochrome" || s == "b&w" || s == "bw") return "BW";
        if (s == "vivid2")      return "VV2";

        // Already an abbreviation?
        string su = toUpper(style);
        if (su == "ST" || su == "PT" || su == "VV" || su == "NT" ||
            su == "FL" || su == "IN" || su == "SH" || su == "BW" || su == "VV2") {
            return su;
        }

        return style;  // pass through as-is
    }

    static string toUpper(const string& s) {
        string r = s;
        for (auto& c : r) c = toupper(c);
        return r;
    }

    static string toLower(const string& s) {
        string r = s;
        for (auto& c : r) c = tolower(c);
        return r;
    }
};
