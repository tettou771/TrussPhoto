#pragma once

// =============================================================================
// PhotoEntry - Photo metadata structure with JSON serialization
// =============================================================================

#include <string>
#include <array>
#include <cmath>
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
    bool faceScanned = false; // true=insightface detection has been run

    // GPS (0 = not available, use hasGps() to check)
    double latitude = 0;     // decimal degrees, positive=N
    double longitude = 0;    // decimal degrees, positive=E
    double altitude = 0;     // meters above sea level

    bool hasGps() const { return latitude != 0 || longitude != 0; }

    // Develop settings (per-photo)
    float chromaDenoise = 0.5f;  // 0-1, chroma noise reduction strength
    float lumaDenoise = 0.0f;    // 0-1, luma noise reduction strength
    float devExposure = 0.0f;    // EV stops (-3 to +3)
    float devWbTemp = 0.0f;      // temperature shift (-1 to +1)
    float devWbTint = 0.0f;      // tint shift (-1 to +1)
    float devContrast = 0.0f;    // -100 to +100
    float devHighlights = 0.0f;  // -100 to +100
    float devShadows = 0.0f;     // -100 to +100
    float devWhites = 0.0f;      // -100 to +100
    float devBlacks = 0.0f;      // -100 to +100
    float devVibrance = 0.0f;    // -100 to +100
    float devSaturation = 0.0f;  // -100 to +100

    // Lens correction (JSON: Sony EXIF spline, DNG polynomial, or Fuji MakerNote)
    string lensCorrectionParams;

    // Additional shooting info (available without RAW file)
    string exposureTime;       // "1/125" etc (human-readable string)
    float exposureBias = 0;    // EV
    int orientation = 1;       // EXIF orientation (1-8, 1=normal)
    string whiteBalance;       // "Auto", "Daylight" etc
    int focalLength35mm = 0;   // 35mm equivalent
    string offsetTime;         // timezone "+09:00"
    string bodySerial;         // camera body serial
    string lensSerial;         // lens serial
    float subjectDistance = 0; // meters
    string subsecTimeOriginal; // "625" etc (for pairing timestamp)
    string companionFiles;     // JSON array: companion paths (future use)

    // User crop (normalized 0-1 relative to developed FBO output)
    float userCropX = 0.0f;
    float userCropY = 0.0f;
    float userCropW = 1.0f;
    float userCropH = 1.0f;

    // User rotation
    float userAngle = 0.0f;   // Fine rotation (radians, ±TAU/8)
    int userRotation90 = 0;    // 90° steps (0-3, counterclockwise)

    // Perspective / shear correction (tilt angles in degrees)
    float userPerspV = 0.0f;   // vertical tilt (degrees, ±45)
    float userPerspH = 0.0f;   // horizontal tilt (degrees, ±45)
    float userShear = 0.0f;    // rolling shutter shear (-1 to +1)

    bool hasCrop() const {
        return userCropX != 0.0f || userCropY != 0.0f ||
               userCropW != 1.0f || userCropH != 1.0f;
    }

    bool hasRotation() const {
        return userAngle != 0.0f || userRotation90 != 0;
    }

    bool hasPerspective() const {
        return userPerspV != 0.0f || userPerspH != 0.0f || userShear != 0.0f;
    }

    float totalRotation() const {
        constexpr float kHalfPi = 1.5707963267948966f;
        return userRotation90 * kHalfPi + userAngle;
    }

    // Forward transform: source UV → warped position (shear + homography)
    // Uses proper projective geometry with focal length.
    pair<float,float> forwardWarp(float u, float v) const {
        // 1. Shear
        float u2 = u + userShear * (v - 0.5f);
        float v2 = v;

        if (userPerspV == 0 && userPerspH == 0) return {u2, v2};

        // Normalized focal length (default 28mm ≈ iPhone standard)
        float focal = (focalLength35mm > 0) ? (float)focalLength35mm : 28.0f;
        float fx = focal / 36.0f;
        float fy = focal / 24.0f;

        // Tilt angles → radians
        constexpr float kDeg2Rad = 3.14159265f / 180.0f;
        float tv = userPerspV * kDeg2Rad;
        float th = userPerspH * kDeg2Rad;
        float cosV = cosf(tv), sinV = sinf(tv);
        float cosH = cosf(th), sinH = sinf(th);

        // Source pixel → 3D ray
        float rx = (u2 - 0.5f) / fx;
        float ry = (v2 - 0.5f) / fy;

        // Horizontal tilt correction (Y-axis rotation by -θ_h)
        float x1 = cosH * rx + sinH;
        float y1 = ry;
        float z1 = -sinH * rx + cosH;

        // Vertical tilt correction (X-axis rotation by -θ_v)
        float xf = x1;
        float yf = cosV * y1 - sinV * z1;
        float zf = sinV * y1 + cosV * z1;

        // Project back (clamp denominator to prevent singularity)
        if (zf < 0.001f) zf = 0.001f;
        float wu = fx * xf / zf + 0.5f;
        float wv = fy * yf / zf + 0.5f;
        return {wu, wv};
    }

    // Inverse transform: warped position → source UV
    // Analytical inverse of the homography (inverse rotation = transpose)
    pair<float,float> inverseWarp(float wu, float wv) const {
        if (!hasPerspective()) return {wu, wv};

        float focal = (focalLength35mm > 0) ? (float)focalLength35mm : 28.0f;
        float fx = focal / 36.0f;
        float fy = focal / 24.0f;

        constexpr float kDeg2Rad = 3.14159265f / 180.0f;
        float tv = userPerspV * kDeg2Rad;
        float th = userPerspH * kDeg2Rad;
        float cosV = cosf(tv), sinV = sinf(tv);
        float cosH = cosf(th), sinH = sinf(th);

        // Warped pixel → 3D ray
        float rx = (wu - 0.5f) / fx;
        float ry = (wv - 0.5f) / fy;

        // Inverse vertical (X-axis rotation by +θ_v)
        float x1 = rx;
        float y1 = cosV * ry + sinV;
        float z1 = -sinV * ry + cosV;

        // Inverse horizontal (Y-axis rotation by +θ_h)
        float xf = cosH * x1 - sinH * z1;
        float yf = y1;
        float zf = sinH * x1 + cosH * z1;

        if (zf < 0.001f) zf = 0.001f;
        float u = fx * xf / zf + 0.5f;
        float v = fy * yf / zf + 0.5f;

        // Inverse shear
        u = u - userShear * (v - 0.5f);
        return {u, v};
    }

    // Compute bounding box of the warped+rotated image (in source pixels)
    pair<float,float> computeBB(int srcW, int srcH) const {
        float totalRot = totalRotation();

        if (!hasPerspective()) {
            float cosA = fabs(cos(totalRot)), sinA = fabs(sin(totalRot));
            return {srcW * cosA + srcH * sinA, srcW * sinA + srcH * cosA};
        }

        // Warp 4 source corners through forward transform, then rotate
        float cosR = cos(totalRot), sinR = sin(totalRot);
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        float corners[4][2] = {{0,0},{1,0},{1,1},{0,1}};
        for (auto& c : corners) {
            auto [wu, wv] = forwardWarp(c[0], c[1]);
            // To pixel coords centered at origin
            float px = (wu - 0.5f) * srcW;
            float py = (wv - 0.5f) * srcH;
            // Rotate
            float rx = px * cosR - py * sinR;
            float ry = px * sinR + py * cosR;
            minX = min(minX, rx); maxX = max(maxX, rx);
            minY = min(minY, ry); maxY = max(maxY, ry);
        }
        // Also sample edge midpoints for better BB estimate with perspective
        float edges[4][2] = {{0.5f,0},{1,0.5f},{0.5f,1},{0,0.5f}};
        for (auto& e : edges) {
            auto [wu, wv] = forwardWarp(e[0], e[1]);
            float px = (wu - 0.5f) * srcW;
            float py = (wv - 0.5f) * srcH;
            float rx = px * cosR - py * sinR;
            float ry = px * sinR + py * cosR;
            minX = min(minX, rx); maxX = max(maxX, rx);
            minY = min(minY, ry); maxY = max(maxY, ry);
        }
        return {maxX - minX, maxY - minY};
    }

    // Compute 4-corner UV coordinates for crop+rotation+perspective export
    // Returns {u0,v0, u1,v1, u2,v2, u3,v3} (TL, TR, BR, BL)
    array<float, 8> getCropQuad(int srcW, int srcH) const {
        if (!hasPerspective()) {
            // Fast path: rotation only
            float totalRot = totalRotation();
            float cosA = fabs(cos(totalRot)), sinA = fabs(sin(totalRot));
            float bbW = srcW * cosA + srcH * sinA;
            float bbH = srcW * sinA + srcH * cosA;
            auto toUV = [&](float bx, float by) -> pair<float,float> {
                float dx = (bx - 0.5f) * bbW, dy = (by - 0.5f) * bbH;
                float cosR = cos(-totalRot), sinR = sin(-totalRot);
                return {(dx*cosR - dy*sinR) / srcW + 0.5f,
                        (dx*sinR + dy*cosR) / srcH + 0.5f};
            };
            auto [u0,v0] = toUV(userCropX, userCropY);
            auto [u1,v1] = toUV(userCropX + userCropW, userCropY);
            auto [u2,v2] = toUV(userCropX + userCropW, userCropY + userCropH);
            auto [u3,v3] = toUV(userCropX, userCropY + userCropH);
            return {u0,v0, u1,v1, u2,v2, u3,v3};
        }

        // With perspective: BB-norm crop → BB pixel → inv rotation → inv perspective → source UV
        auto [bbW, bbH] = computeBB(srcW, srcH);
        float totalRot = totalRotation();

        auto bbToSourceUV = [&](float bx, float by) -> pair<float,float> {
            // BB-norm → BB pixel (centered)
            float dx = (bx - 0.5f) * bbW;
            float dy = (by - 0.5f) * bbH;
            // Inverse rotation → image pixel (centered)
            float cosR = cos(-totalRot), sinR = sin(-totalRot);
            float ix = dx * cosR - dy * sinR;
            float iy = dx * sinR + dy * cosR;
            // Image pixel → source UV (0-1)
            float wu = ix / srcW + 0.5f;
            float wv = iy / srcH + 0.5f;
            // Inverse warp → source UV
            return inverseWarp(wu, wv);
        };

        auto [u0,v0] = bbToSourceUV(userCropX, userCropY);
        auto [u1,v1] = bbToSourceUV(userCropX + userCropW, userCropY);
        auto [u2,v2] = bbToSourceUV(userCropX + userCropW, userCropY + userCropH);
        auto [u3,v3] = bbToSourceUV(userCropX, userCropY + userCropH);
        return {u0,v0, u1,v1, u2,v2, u3,v3};
    }

    // Per-pixel inverse: output normalized (0-1) → source UV
    // For perspective export where bilinear quad interpolation is insufficient
    pair<float,float> getCropUV(float tx, float ty, int srcW, int srcH) const {
        auto [bbW, bbH] = computeBB(srcW, srcH);
        float totalRot = totalRotation();

        // Output (tx,ty) in 0-1 → BB-norm crop position
        float bx = userCropX + tx * userCropW;
        float by = userCropY + ty * userCropH;

        // BB-norm → BB pixel (centered)
        float dx = (bx - 0.5f) * bbW;
        float dy = (by - 0.5f) * bbH;

        // Inverse rotation
        float cosR = cos(-totalRot), sinR = sin(-totalRot);
        float ix = dx * cosR - dy * sinR;
        float iy = dx * sinR + dy * cosR;

        // Image pixel → warped UV
        float wu = ix / srcW + 0.5f;
        float wv = iy / srcH + 0.5f;

        // Inverse warp → source UV
        return inverseWarp(wu, wv);
    }

    // Compute output pixel dimensions for crop+rotation+perspective
    pair<int,int> getCropOutputSize(int srcW, int srcH) const {
        auto [bbW, bbH] = computeBB(srcW, srcH);
        return {max(1, (int)round(userCropW * bbW)),
                max(1, (int)round(userCropH * bbH))};
    }

    // Stacking (RAW+JPG, Live Photo grouping)
    string stackId;              // same value = same stack ("" = not stacked)
    bool stackPrimary = false;   // true = visible in grid, false = hidden companion

    // State
    SyncState syncState = SyncState::LocalOnly;

    // Parse "YYYY:MM:DD HH:MM:SS" → epoch seconds (0 on failure)
    static int64_t parseDateTimeOriginal(const string& dt) {
        if (dt.size() < 19) return 0;
        try {
            tm t = {};
            t.tm_year = stoi(dt.substr(0, 4)) - 1900;
            t.tm_mon = stoi(dt.substr(5, 2)) - 1;
            t.tm_mday = stoi(dt.substr(8, 2));
            t.tm_hour = stoi(dt.substr(11, 2));
            t.tm_min = stoi(dt.substr(14, 2));
            t.tm_sec = stoi(dt.substr(17, 2));
            return (int64_t)mktime(&t);
        } catch (...) {
            return 0;
        }
    }
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
        {"isManaged", e.isManaged},
        {"chromaDenoise", e.chromaDenoise},
        {"lumaDenoise", e.lumaDenoise},
        {"devExposure", e.devExposure},
        {"devWbTemp", e.devWbTemp},
        {"devWbTint", e.devWbTint},
        {"devContrast", e.devContrast},
        {"devHighlights", e.devHighlights},
        {"devShadows", e.devShadows},
        {"devWhites", e.devWhites},
        {"devBlacks", e.devBlacks},
        {"devVibrance", e.devVibrance},
        {"devSaturation", e.devSaturation},
        {"lensCorrectionParams", e.lensCorrectionParams},
        {"exposureTime", e.exposureTime},
        {"exposureBias", e.exposureBias},
        {"orientation", e.orientation},
        {"whiteBalance", e.whiteBalance},
        {"focalLength35mm", e.focalLength35mm},
        {"offsetTime", e.offsetTime},
        {"bodySerial", e.bodySerial},
        {"lensSerial", e.lensSerial},
        {"subjectDistance", e.subjectDistance},
        {"subsecTimeOriginal", e.subsecTimeOriginal},
        {"companionFiles", e.companionFiles},
        {"stackId", e.stackId},
        {"stackPrimary", e.stackPrimary},
        {"userCropX", e.userCropX},
        {"userCropY", e.userCropY},
        {"userCropW", e.userCropW},
        {"userCropH", e.userCropH},
        {"userAngle", e.userAngle},
        {"userRotation90", e.userRotation90},
        {"userPerspV", e.userPerspV},
        {"userPerspH", e.userPerspH},
        {"userShear", e.userShear}
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
    e.chromaDenoise = j.value("chromaDenoise", 0.5f);
    e.lumaDenoise = j.value("lumaDenoise", 0.0f);
    e.devExposure = j.value("devExposure", 0.0f);
    e.devWbTemp = j.value("devWbTemp", 0.0f);
    e.devWbTint = j.value("devWbTint", 0.0f);
    e.devContrast = j.value("devContrast", 0.0f);
    e.devHighlights = j.value("devHighlights", 0.0f);
    e.devShadows = j.value("devShadows", 0.0f);
    e.devWhites = j.value("devWhites", 0.0f);
    e.devBlacks = j.value("devBlacks", 0.0f);
    e.devVibrance = j.value("devVibrance", 0.0f);
    e.devSaturation = j.value("devSaturation", 0.0f);

    e.lensCorrectionParams = j.value("lensCorrectionParams", string(""));
    e.exposureTime = j.value("exposureTime", string(""));
    e.exposureBias = j.value("exposureBias", 0.0f);
    e.orientation = j.value("orientation", 1);
    e.whiteBalance = j.value("whiteBalance", string(""));
    e.focalLength35mm = j.value("focalLength35mm", 0);
    e.offsetTime = j.value("offsetTime", string(""));
    e.bodySerial = j.value("bodySerial", string(""));
    e.lensSerial = j.value("lensSerial", string(""));
    e.subjectDistance = j.value("subjectDistance", 0.0f);
    e.subsecTimeOriginal = j.value("subsecTimeOriginal", string(""));
    e.companionFiles = j.value("companionFiles", string(""));
    e.stackId = j.value("stackId", string(""));
    e.stackPrimary = j.value("stackPrimary", false);
    e.userCropX = j.value("userCropX", 0.0f);
    e.userCropY = j.value("userCropY", 0.0f);
    e.userCropW = j.value("userCropW", 1.0f);
    e.userCropH = j.value("userCropH", 1.0f);
    e.userAngle = j.value("userAngle", 0.0f);
    e.userRotation90 = j.value("userRotation90", 0);
    e.userPerspV = j.value("userPerspV", 0.0f);
    e.userPerspH = j.value("userPerspH", 0.0f);
    e.userShear = j.value("userShear", 0.0f);

    int state = j.value("syncState", 0);
    e.syncState = static_cast<SyncState>(state);
    // Syncing state doesn't survive restart - revert to LocalOnly
    if (e.syncState == SyncState::Syncing) {
        e.syncState = SyncState::LocalOnly;
    }
}
