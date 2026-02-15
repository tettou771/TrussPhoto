#pragma once

// =============================================================================
// LrcatImporter - Read-only importer for Lightroom Classic catalog (.lrcat)
// =============================================================================
// Reads photos, metadata, keywords, and develop settings from lrcat SQLite DB.
// Does NOT modify the lrcat file (opened READONLY).

#include <sqlite3.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <TrussC.h>
#include "PhotoEntry.h"

using namespace std;
using namespace tc;

namespace fs = std::filesystem;

class LrcatImporter {
public:
    struct Stats {
        int totalImages = 0;
        int skippedVirtual = 0;
        int missingFile = 0;
        int imported = 0;
        int faces = 0;
        int namedFaces = 0;
        int persons = 0;
    };

    struct FaceEntry {
        string photoId;       // filename_filesize
        string personName;    // "" if unnamed
        float x, y, w, h;    // normalized 0-1 (top-left + size)
        int lrClusterId = 0;
    };

    struct Result {
        vector<PhotoEntry> entries;
        vector<FaceEntry> faces;
        Stats stats;
    };

    static Result import(const string& lrcatPath) {
        Result result;

        if (!fs::exists(lrcatPath)) {
            logError() << "[LrcatImport] File not found: " << lrcatPath;
            return result;
        }

        sqlite3* db = nullptr;
        int rc = sqlite3_open_v2(lrcatPath.c_str(), &db,
                                  SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK) {
            logError() << "[LrcatImport] Failed to open: " << sqlite3_errmsg(db);
            sqlite3_close(db);
            return result;
        }

        logNotice() << "[LrcatImport] Opened: " << lrcatPath;

        // Step 1: Build keyword map (image id_local -> tags)
        auto keywordMap = loadKeywords(db);
        logNotice() << "[LrcatImport] Keywords loaded for " << keywordMap.size() << " images";

        // Step 2: Load all photos (also builds imageIdMap for face import)
        unordered_map<int64_t, string> imageIdMap;
        result = loadPhotos(db, keywordMap, imageIdMap);

        // Step 3: Load faces
        result.faces = loadFaces(db, imageIdMap, result.stats);
        logNotice() << "[LrcatImport] Faces: " << result.stats.faces
                    << " (named: " << result.stats.namedFaces
                    << ", persons: " << result.stats.persons << ")";

        sqlite3_close(db);

        logNotice() << "[LrcatImport] Done: total=" << result.stats.totalImages
                    << " imported=" << result.stats.imported
                    << " missing=" << result.stats.missingFile
                    << " faces=" << result.stats.faces;
        return result;
    }

private:
    // Load keywords: image id_local -> vector of keyword names
    static unordered_map<int64_t, vector<string>> loadKeywords(sqlite3* db) {
        unordered_map<int64_t, vector<string>> result;

        const char* sql =
            "SELECT ki.image, k.name "
            "FROM AgLibraryKeywordImage ki "
            "JOIN AgLibraryKeyword k ON ki.tag = k.id_local "
            "WHERE k.includeOnExport = 1 "
            "ORDER BY ki.image";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            logWarning() << "[LrcatImport] Keyword query failed: " << sqlite3_errmsg(db);
            return result;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t imageId = sqlite3_column_int64(stmt, 0);
            const char* name = (const char*)sqlite3_column_text(stmt, 1);
            if (name) {
                result[imageId].push_back(name);
            }
        }
        sqlite3_finalize(stmt);
        return result;
    }

    static Result loadPhotos(sqlite3* db,
                             const unordered_map<int64_t, vector<string>>& keywordMap,
                             unordered_map<int64_t, string>& imageIdMap) {
        Result result;

        const char* sql =
            "SELECT "
            "  lf.baseName, lf.extension, "
            "  CAST(COALESCE(pih.fileSize, 0) AS INTEGER), "
            "  rf.absolutePath, f.pathFromRoot, "
            "  ai.captureTime, "
            "  CAST(COALESCE(ai.rating, 0) AS INTEGER), "
            "  CAST(COALESCE(ai.pick, 0) AS INTEGER), "
            "  COALESCE(ai.colorLabels, ''), "
            "  CAST(COALESCE(ai.fileWidth, 0) AS INTEGER), "
            "  CAST(COALESCE(ai.fileHeight, 0) AS INTEGER), "
            "  COALESCE(cm.value, ''), "
            "  COALESCE(ln.value, ''), "
            "  COALESCE(ex.aperture, 0), "
            "  COALESCE(ex.focalLength, 0), "
            "  COALESCE(ex.isoSpeedRating, 0), "
            "  COALESCE(ex.gpsLatitude, 0), COALESCE(ex.gpsLongitude, 0), "
            "  COALESCE(ex.hasGPS, 0), "
            "  COALESCE(ds.text, ''), "
            "  ai.id_local "
            "FROM Adobe_images ai "
            "JOIN AgLibraryFile lf ON ai.rootFile = lf.id_local "
            "JOIN AgLibraryFolder f ON lf.folder = f.id_local "
            "JOIN AgLibraryRootFolder rf ON f.rootFolder = rf.id_local "
            "LEFT JOIN AgParsedImportHash pih ON lf.id_global = pih.id_global "
            "LEFT JOIN AgHarvestedExifMetadata ex ON ex.image = ai.id_local "
            "LEFT JOIN AgInternedExifCameraModel cm ON ex.cameraModelRef = cm.id_local "
            "LEFT JOIN AgInternedExifLens ln ON ex.lensRef = ln.id_local "
            "LEFT JOIN Adobe_imageDevelopSettings ds ON ds.image = ai.id_local "
            "WHERE ai.masterImage IS NULL";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            logError() << "[LrcatImport] Photo query failed: " << sqlite3_errmsg(db);
            return result;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            result.stats.totalImages++;

            // Columns
            string baseName = safeText(stmt, 0);
            string extension = safeText(stmt, 1);
            int64_t fileSize = sqlite3_column_int64(stmt, 2);
            string absPath = safeText(stmt, 3);
            string pathFromRoot = safeText(stmt, 4);
            string captureTime = safeText(stmt, 5);
            int rating = sqlite3_column_int(stmt, 6);
            int pick = sqlite3_column_int(stmt, 7);
            string colorLabels = safeText(stmt, 8);
            int width = sqlite3_column_int(stmt, 9);
            int height = sqlite3_column_int(stmt, 10);
            string cameraModel = safeText(stmt, 11);
            string lensModel = safeText(stmt, 12);
            double aperture = sqlite3_column_double(stmt, 13);
            double focalLength = sqlite3_column_double(stmt, 14);
            double iso = sqlite3_column_double(stmt, 15);
            double gpsLat = sqlite3_column_double(stmt, 16);
            double gpsLon = sqlite3_column_double(stmt, 17);
            int hasGPS = sqlite3_column_int(stmt, 18);
            string developText = safeText(stmt, 19);
            int64_t imageIdLocal = sqlite3_column_int64(stmt, 20);

            // Build filename
            string filename = baseName + "." + extension;

            // Build local path
            string localPath = buildLocalPath(absPath, pathFromRoot, filename);

            // Build photo ID (filename_filesize)
            // If fileSize is 0, try to get it from the file system
            if (fileSize == 0 && !localPath.empty() && fs::exists(localPath)) {
                try { fileSize = (int64_t)fs::file_size(localPath); } catch (...) {}
            }

            string id = filename + "_" + to_string(fileSize);

            // Map LR image id_local -> our photo ID (for face import)
            imageIdMap[imageIdLocal] = id;

            // Check if file exists (skip if not accessible)
            if (!fs::exists(localPath)) {
                result.stats.missingFile++;
                // Still import — the file might be on a disconnected volume
            }

            // Build entry
            PhotoEntry e;
            e.id = id;
            e.filename = filename;
            e.fileSize = (uintmax_t)fileSize;
            e.localPath = localPath;
            e.dateTimeOriginal = captureTimeToExif(captureTime);
            e.width = width;
            e.height = height;
            e.camera = cameraModel;
            e.cameraMake = inferCameraMake(cameraModel);
            e.lens = lensModel;
            e.focalLength = (float)focalLength;
            e.aperture = (float)aperture;
            e.iso = (float)iso;
            e.isRaw = isRawExtension(extension);
            e.isVideo = isVideoExtension(extension);
            e.rating = clamp(rating, 0, 5);
            e.flag = clamp(pick, -1, 1);
            e.colorLabel = mapColorLabel(colorLabels);
            e.developSettings = developText;
            e.isManaged = false;
            e.syncState = SyncState::LocalOnly;

            // GPS
            if (hasGPS && (gpsLat != 0 || gpsLon != 0)) {
                e.latitude = gpsLat;
                e.longitude = gpsLon;
            }

            // Keywords → tags JSON array
            auto kwIt = keywordMap.find(imageIdLocal);
            if (kwIt != keywordMap.end() && !kwIt->second.empty()) {
                nlohmann::json tagArr = nlohmann::json::array();
                for (const auto& kw : kwIt->second) {
                    tagArr.push_back(kw);
                }
                e.tags = tagArr.dump();
            }

            result.entries.push_back(std::move(e));
            result.stats.imported++;
        }

        sqlite3_finalize(stmt);
        return result;
    }

    // Load face data from lrcat (AgLibraryFace + AgLibraryKeywordFace + AgLibraryKeyword)
    static vector<FaceEntry> loadFaces(sqlite3* db,
                                        const unordered_map<int64_t, string>& imageIdMap,
                                        Stats& stats) {
        vector<FaceEntry> result;

        const char* sql =
            "SELECT f.image, f.tl_x, f.tl_y, f.br_x, f.br_y, "
            "  CAST(COALESCE(f.cluster, 0) AS INTEGER), "
            "  k.name AS person_name "
            "FROM AgLibraryFace f "
            "LEFT JOIN AgLibraryKeywordFace kf ON kf.face = f.id_local AND kf.userPick = 1 "
            "LEFT JOIN AgLibraryKeyword k ON kf.tag = k.id_local "
            "WHERE f.regionType = 1.0";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            logWarning() << "[LrcatImport] Face query failed: " << sqlite3_errmsg(db);
            return result;
        }

        unordered_set<string> personNames;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t imageId = sqlite3_column_int64(stmt, 0);

            // Map LR image id to our photo ID
            auto it = imageIdMap.find(imageId);
            if (it == imageIdMap.end()) continue;

            float tlX = (float)sqlite3_column_double(stmt, 1);
            float tlY = (float)sqlite3_column_double(stmt, 2);
            float brX = (float)sqlite3_column_double(stmt, 3);
            float brY = (float)sqlite3_column_double(stmt, 4);
            int clusterId = sqlite3_column_int(stmt, 5);
            const char* personName = (const char*)sqlite3_column_text(stmt, 6);

            FaceEntry face;
            face.photoId = it->second;
            face.x = tlX;
            face.y = tlY;
            face.w = brX - tlX;
            face.h = brY - tlY;
            face.lrClusterId = clusterId;
            if (personName && personName[0] != '\0') {
                face.personName = personName;
                personNames.insert(face.personName);
                stats.namedFaces++;
            }

            result.push_back(std::move(face));
            stats.faces++;
        }

        sqlite3_finalize(stmt);
        stats.persons = (int)personNames.size();
        return result;
    }

    // --- Helper functions ---

    static string safeText(sqlite3_stmt* stmt, int col) {
        const char* txt = (const char*)sqlite3_column_text(stmt, col);
        return txt ? string(txt) : "";
    }

    // Build absolute local path from LR root folder + relative path + filename
    static string buildLocalPath(const string& absolutePath,
                                  const string& pathFromRoot,
                                  const string& filename) {
        // absolutePath: "/Volumes/PhotosSSD/LightroomClassic/"
        // pathFromRoot: "2024/05/03/"
        // filename: "DSC00001.ARW"
        string path = absolutePath;
        if (!path.empty() && path.back() != '/') path += '/';
        path += pathFromRoot;
        if (!path.empty() && path.back() != '/') path += '/';
        path += filename;
        return path;
    }

    // Convert LR captureTime to EXIF format
    // LR: "2024-05-03T18:43:45" → EXIF: "2024:05:03 18:43:45"
    static string captureTimeToExif(const string& ct) {
        if (ct.size() < 19) return ct;
        string result = ct;
        // Replace dashes with colons in date part
        if (result[4] == '-') result[4] = ':';
        if (result[7] == '-') result[7] = ':';
        // Replace T with space
        if (result[10] == 'T') result[10] = ' ';
        return result.substr(0, 19);
    }

    // Infer camera make from model string
    static string inferCameraMake(const string& model) {
        if (model.empty()) return "";

        // Sony: ILCE-*, ILCA-*, DSC-*, SLT-*, NEX-*
        if (model.substr(0, 4) == "ILCE" || model.substr(0, 4) == "ILCA" ||
            model.substr(0, 3) == "DSC" || model.substr(0, 3) == "SLT" ||
            model.substr(0, 3) == "NEX") {
            return "SONY";
        }
        // Canon
        if (model.find("Canon") != string::npos || model.substr(0, 3) == "EOS") {
            return "Canon";
        }
        // Nikon
        if (model.find("NIKON") != string::npos || model.substr(0, 5) == "NIKON") {
            return "NIKON CORPORATION";
        }
        // Fujifilm
        if (model.find("X-") == 0 || model.find("GFX") == 0 ||
            model.find("FUJIFILM") != string::npos) {
            return "FUJIFILM";
        }
        // Panasonic / Lumix
        if (model.find("DC-") == 0 || model.find("DMC-") == 0 ||
            model.find("Lumix") != string::npos) {
            return "Panasonic";
        }
        // Olympus
        if (model.find("E-M") == 0 || model.find("E-P") == 0 ||
            model.find("OM-") == 0) {
            return "OLYMPUS";
        }
        // Leica
        if (model.find("LEICA") != string::npos) {
            return "LEICA";
        }
        // Ricoh / Pentax
        if (model.find("PENTAX") != string::npos || model.find("GR") == 0) {
            return "RICOH IMAGING COMPANY, LTD.";
        }
        return "";
    }

    // Check if extension is a RAW format
    static bool isRawExtension(const string& ext) {
        static const unordered_set<string> rawExts = {
            "ARW", "arw", "CR2", "cr2", "CR3", "cr3",
            "NEF", "nef", "NRW", "nrw",
            "ORF", "orf", "RAF", "raf",
            "RW2", "rw2", "PEF", "pef",
            "DNG", "dng", "SRW", "srw",
            "3FR", "3fr", "IIQ", "iiq",
            "ERF", "erf", "MEF", "mef",
            "MOS", "mos", "KDC", "kdc",
            "DCR", "dcr"
        };
        return rawExts.count(ext) > 0;
    }

    // Check if extension is a video format
    static bool isVideoExtension(const string& ext) {
        static const unordered_set<string> videoExts = {
            "MP4", "mp4", "MOV", "mov", "AVI", "avi",
            "MKV", "mkv", "MTS", "mts", "M2TS", "m2ts",
            "MPG", "mpg", "MPEG", "mpeg", "WMV", "wmv",
            "FLV", "flv", "WEBM", "webm", "M4V", "m4v",
            "3GP", "3gp"
        };
        return videoExts.count(ext) > 0;
    }

    // Map LR color label string to standard label
    static string mapColorLabel(const string& label) {
        if (label.empty()) return "";
        // LR stores: "Red", "Yellow", "Green", "Blue", "Purple"
        // These match our format directly
        if (label == "Red" || label == "Yellow" || label == "Green" ||
            label == "Blue" || label == "Purple") {
            return label;
        }
        return "";
    }
};
