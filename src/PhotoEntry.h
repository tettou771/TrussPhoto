#pragma once

// =============================================================================
// PhotoEntry - Photo metadata structure with JSON serialization
// =============================================================================

#include <string>
#include <nlohmann/json.hpp>
#include "Constants.h"

using namespace std;

// Photo entry with sync awareness
struct PhotoEntry {
    // Identity (filename + size + dateTime)
    string id;
    string filename;
    uintmax_t fileSize = 0;
    string dateTimeOriginal;

    // Paths
    string localPath;            // Local file path (RAW or standard)
    string localThumbnailPath;   // Cached thumbnail path
    string localSmartPreviewPath; // Smart preview JPEG XL path (tpDataPath/smart_preview/)

    // Metadata
    string cameraMake;
    string camera;
    string lens;
    string lensMake;
    int width = 0;
    int height = 0;
    bool isRaw = false;
    bool isVideo = false;
    string creativeStyle;
    float focalLength = 0;
    float aperture = 0;
    float iso = 0;

    // User-editable metadata
    int rating = 0;              // 0-5 (0=unrated)
    string colorLabel;           // "", "Red", "Yellow", "Green", "Blue", "Purple"
    int flag = 0;                // 0=none, 1=pick, -1=reject
    string memo;                 // Markdown freetext
    string tags;                 // JSON array string: '["travel","sunrise"]'

    // Field-level updatedAt (ms since epoch, 0=never)
    int64_t ratingUpdatedAt = 0;
    int64_t colorLabelUpdatedAt = 0;
    int64_t flagUpdatedAt = 0;
    int64_t memoUpdatedAt = 0;
    int64_t tagsUpdatedAt = 0;

    // Develop settings (LR import)
    string developSettings;  // LR develop settings text blob
    bool isManaged = true;   // true=originals/ managed, false=external reference

    // GPS (0 = not available, use hasGps() to check)
    double latitude = 0;     // decimal degrees, positive=N
    double longitude = 0;    // decimal degrees, positive=E
    double altitude = 0;     // meters above sea level

    bool hasGps() const { return latitude != 0 || longitude != 0; }

    // State
    SyncState syncState = SyncState::LocalOnly;
};

// JSON serialization for PhotoEntry
inline void to_json(nlohmann::json& j, const PhotoEntry& e) {
    j = nlohmann::json{
        {"id", e.id},
        {"filename", e.filename},
        {"fileSize", e.fileSize},
        {"dateTimeOriginal", e.dateTimeOriginal},
        {"localPath", e.localPath},
        {"localThumbnailPath", e.localThumbnailPath},
        {"localSmartPreviewPath", e.localSmartPreviewPath},
        {"cameraMake", e.cameraMake},
        {"camera", e.camera},
        {"lens", e.lens},
        {"lensMake", e.lensMake},
        {"width", e.width},
        {"height", e.height},
        {"isRaw", e.isRaw},
        {"isVideo", e.isVideo},
        {"creativeStyle", e.creativeStyle},
        {"focalLength", e.focalLength},
        {"aperture", e.aperture},
        {"iso", e.iso},
        {"syncState", static_cast<int>(e.syncState)},
        {"rating", e.rating},
        {"colorLabel", e.colorLabel},
        {"flag", e.flag},
        {"memo", e.memo},
        {"tags", e.tags},
        {"ratingUpdatedAt", e.ratingUpdatedAt},
        {"colorLabelUpdatedAt", e.colorLabelUpdatedAt},
        {"flagUpdatedAt", e.flagUpdatedAt},
        {"memoUpdatedAt", e.memoUpdatedAt},
        {"tagsUpdatedAt", e.tagsUpdatedAt},
        {"latitude", e.latitude},
        {"longitude", e.longitude},
        {"altitude", e.altitude},
        {"developSettings", e.developSettings},
        {"isManaged", e.isManaged}
    };
}

inline void from_json(const nlohmann::json& j, PhotoEntry& e) {
    e.id = j.value("id", string(""));
    e.filename = j.value("filename", string(""));
    e.fileSize = j.value("fileSize", (uintmax_t)0);
    e.dateTimeOriginal = j.value("dateTimeOriginal", string(""));
    e.localPath = j.value("localPath", string(""));
    e.localThumbnailPath = j.value("localThumbnailPath", string(""));
    e.localSmartPreviewPath = j.value("localSmartPreviewPath", string(""));
    e.cameraMake = j.value("cameraMake", string(""));
    e.camera = j.value("camera", string(""));
    e.lens = j.value("lens", string(""));
    e.lensMake = j.value("lensMake", string(""));
    e.width = j.value("width", 0);
    e.height = j.value("height", 0);
    e.isRaw = j.value("isRaw", false);
    e.isVideo = j.value("isVideo", false);
    e.creativeStyle = j.value("creativeStyle", string(""));
    e.focalLength = j.value("focalLength", 0.0f);
    e.aperture = j.value("aperture", 0.0f);
    e.iso = j.value("iso", 0.0f);

    e.rating = j.value("rating", 0);
    e.colorLabel = j.value("colorLabel", string(""));
    e.flag = j.value("flag", 0);
    e.memo = j.value("memo", string(""));
    e.tags = j.value("tags", string(""));
    e.ratingUpdatedAt = j.value("ratingUpdatedAt", (int64_t)0);
    e.colorLabelUpdatedAt = j.value("colorLabelUpdatedAt", (int64_t)0);
    e.flagUpdatedAt = j.value("flagUpdatedAt", (int64_t)0);
    e.memoUpdatedAt = j.value("memoUpdatedAt", (int64_t)0);
    e.tagsUpdatedAt = j.value("tagsUpdatedAt", (int64_t)0);

    e.latitude = j.value("latitude", 0.0);
    e.longitude = j.value("longitude", 0.0);
    e.altitude = j.value("altitude", 0.0);

    e.developSettings = j.value("developSettings", string(""));
    e.isManaged = j.value("isManaged", true);

    int state = j.value("syncState", 0);
    e.syncState = static_cast<SyncState>(state);
    // Syncing state doesn't survive restart - revert to LocalOnly
    if (e.syncState == SyncState::Syncing) {
        e.syncState = SyncState::LocalOnly;
    }
}
