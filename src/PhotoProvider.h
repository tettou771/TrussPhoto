#pragma once

// =============================================================================
// PhotoProvider - Abstraction layer for photo access (local + server)
// =============================================================================

#include <TrussC.h>
#include <tcxLibRaw.h>
#include <tcxCurl.h>
#include <nlohmann/json.hpp>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <sys/stat.h>
#include <vecLib/vDSP.h>

using namespace std;
using namespace tc;
using namespace tcx;

namespace fs = std::filesystem;

#include "PhotoEntry.h"
#include "PhotoDatabase.h"
#include "pipeline/SmartPreview.h"
#include "ai/ClipEmbedder.h"
#include "ai/ClipTextEncoder.h"
#include "ai/FaceDetector.h"
#include "ai/FaceRecognizer.h"
#include "pipeline/LrcatImporter.h"

// Photo provider - manages local + server photos
class PhotoProvider {
public:
    PhotoProvider() = default;

    // --- Configuration ---

    // Set all paths from a single catalog directory
    void setCatalogDir(const string& catalogPath) {
        catalogDir_        = catalogPath;
        thumbnailCacheDir_ = catalogPath + "/thumbnail_cache";
        smartPreviewDir_   = catalogPath + "/smart_preview";
        databasePath_      = catalogPath + "/library.db";
        pendingDir_        = catalogPath + "/pending";
        fs::create_directories(thumbnailCacheDir_);
        fs::create_directories(smartPreviewDir_);
        fs::create_directories(pendingDir_);
    }

    const string& getCatalogDir() const { return catalogDir_; }

    void setRawStoragePath(const string& path) {
        rawStoragePath_ = path;
        if (!path.empty()) fs::create_directories(path);
    }

    void setServerUrl(const string& url) {
        client_.setBaseUrl(url);
        serverChecked_ = false;
    }

    void setApiKey(const string& key) {
        client_.setBearerToken(key);
    }

    void setThumbnailCacheDir(const string& dir) {
        thumbnailCacheDir_ = dir;
        fs::create_directories(dir);
    }

    void setSmartPreviewDir(const string& dir) {
        smartPreviewDir_ = dir;
        fs::create_directories(dir);
    }

    void setDatabasePath(const string& path) {
        databasePath_ = path;
    }

    void setJsonMigrationPath(const string& path) {
        jsonMigrationPath_ = path;
    }

    const string& getRawStoragePath() const { return rawStoragePath_; }

    // --- Server connectivity ---

    bool isServerReachable() {
        if (!serverChecked_) {
            serverReachable_ = client_.isReachable();
            serverChecked_ = true;
            if (serverReachable_) {
                logNotice() << "[PhotoProvider] Server connected";
            }
        }
        return serverReachable_;
    }

    void resetServerCheck() { serverChecked_ = false; }
    bool isServerConnected() const { return serverReachable_; }

    // --- Library persistence (SQLite) ---

    void saveLibrary() {
        // No-op: SQLite writes are immediate
    }

    bool loadLibrary() {
        if (databasePath_.empty()) return false;

        if (!db_.open(databasePath_)) return false;

        // Auto-migrate from JSON if DB is empty and JSON exists
        if (!jsonMigrationPath_.empty() && fs::exists(jsonMigrationPath_)) {
            db_.migrateFromJson(jsonMigrationPath_);
        }

        // Load all entries into memory
        auto entries = db_.loadAll();
        for (auto& entry : entries) {
            photos_[entry.id] = std::move(entry);
        }
        logNotice() << "[PhotoProvider] Library loaded from DB: " << photos_.size() << " photos";

        // Rebuild stack index from persisted stack data
        rebuildStackIndex();

        // Load face name cache
        loadFaceCache();
        loadFaceEmbeddingCache();

        return !photos_.empty();
    }

    void markDirty() {
        // No-op: SQLite writes are immediate
    }

    void saveIfDirty() {
        // No-op: SQLite writes are immediate
    }

    // --- Library validation ---

    // Check all entries for missing local files, returns count of newly missing/changed
    int validateLibrary() {
        int changedCount = 0;
        for (auto& [id, photo] : photos_) {
            // ServerOnly has no local file by design
            if (photo.syncState == SyncState::ServerOnly) continue;

            if (photo.localPath.empty() || !fs::exists(photo.localPath)) {
                if (photo.syncState == SyncState::Synced) {
                    photo.syncState = SyncState::ServerOnly;
                    db_.updateSyncState(id, photo.syncState);
                    changedCount++;
                } else if (photo.syncState != SyncState::Missing) {
                    photo.syncState = SyncState::Missing;
                    db_.updateSyncState(id, photo.syncState);
                    changedCount++;
                }
            } else {
                if (photo.syncState == SyncState::Missing) {
                    photo.syncState = SyncState::LocalOnly;
                    db_.updateSyncState(id, photo.syncState);
                    changedCount++;
                }
            }
        }
        return changedCount;
    }

    // Relink a single photo to a new file path
    bool relinkPhoto(const string& id, const string& newPath) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;

        it->second.localPath = newPath;
        db_.updateLocalPath(id, newPath);

        if (it->second.syncState == SyncState::Missing) {
            it->second.syncState = SyncState::LocalOnly;
            db_.updateSyncState(id, SyncState::LocalOnly);
        }

        logNotice() << "[Relink] " << it->second.filename << " -> " << newPath;
        return true;
    }

    // Relink missing photos by scanning a folder for matching files (by filename+filesize ID)
    int relinkFromFolder(const string& folderPath) {
        fs::path folder(folderPath);
        if (!fs::exists(folder) || !fs::is_directory(folder)) return 0;

        // Build a set of missing photo IDs for fast lookup
        unordered_map<string, string*> missingById;  // id -> pointer to localPath
        for (auto& [id, photo] : photos_) {
            if (photo.localPath.empty() || !fs::exists(photo.localPath)) {
                missingById[id] = &photo.localPath;
            }
        }
        if (missingById.empty()) return 0;

        int relinked = 0;
        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            if (!isSupportedFile(entry.path())) continue;

            string fname = entry.path().filename().string();
            uintmax_t fsize = entry.file_size();
            string id = fname + "_" + to_string(fsize);

            auto it = missingById.find(id);
            if (it == missingById.end()) continue;

            string newPath = entry.path().string();
            *(it->second) = newPath;
            db_.updateLocalPath(id, newPath);

            // Restore sync state from Missing to LocalOnly
            auto photoIt = photos_.find(id);
            if (photoIt != photos_.end() && photoIt->second.syncState == SyncState::Missing) {
                photoIt->second.syncState = SyncState::LocalOnly;
                db_.updateSyncState(id, SyncState::LocalOnly);
            }

            missingById.erase(it);
            relinked++;
            logNotice() << "[Relink] " << fname << " -> " << newPath;
        }
        return relinked;
    }

    // Re-extract creativeStyle for photos that have empty style and accessible files
    int refreshCreativeStyles() {
        int updated = 0;
        for (auto& [id, photo] : photos_) {
            if (!photo.creativeStyle.empty()) continue;
            if (photo.localPath.empty() || !fs::exists(photo.localPath)) continue;

            PhotoEntry temp;
            extractExifMetadata(photo.localPath, temp);
            if (!temp.creativeStyle.empty()) {
                photo.creativeStyle = temp.creativeStyle;
                db_.updatePhoto(photo);
                updated++;
            }
        }
        if (updated > 0) {
            logNotice() << "[RefreshStyles] Updated " << updated << " photos";
        }
        return updated;
    }

    // Scan library folder for unregistered files, returns count of added
    int scanLibraryFolder() {
        if (rawStoragePath_.empty() || !fs::exists(rawStoragePath_)) return 0;

        int addedCount = 0;
        vector<PhotoEntry> newEntries;
        for (const auto& entry : fs::recursive_directory_iterator(rawStoragePath_)) {
            if (!entry.is_regular_file()) continue;
            if (!isSupportedFile(entry.path())) continue;

            string fname = entry.path().filename().string();
            uintmax_t fsize = entry.file_size();
            string id = fname + "_" + to_string(fsize);

            if (photos_.count(id)) continue;

            bool video = isVideoFile(entry.path());

            PhotoEntry photo;
            photo.id = id;
            photo.filename = fname;
            photo.fileSize = fsize;
            photo.localPath = entry.path().string();
            photo.isVideo = video;
            photo.isRaw = video ? false : RawLoader::isRawFile(entry.path());
            photo.syncState = SyncState::LocalOnly;
            if (!video) {
                extractExifMetadata(photo.localPath, photo);
                extractXmpMetadata(photo.localPath, photo);
            }
            photos_[id] = photo;
            newEntries.push_back(photo);
            addedCount++;
        }
        if (!newEntries.empty()) {
            db_.insertPhotos(newEntries);
        }
        return addedCount;
    }

    // Check if a file path is a supported file (image or video)
    bool isSupportedFile(const string& path) const {
        return isSupportedFile(fs::path(path));
    }

    // Import individual files (non-blocking, copies happen in background)
    int importFiles(const vector<string>& filePaths) {
        int added = 0;
        vector<PhotoEntry> newEntries;

        for (const auto& filePath : filePaths) {
            fs::path p(filePath);
            if (!fs::exists(p) || !fs::is_regular_file(p)) continue;
            if (!isSupportedFile(p)) continue;

            string fname = p.filename().string();
            uintmax_t fsize = fs::file_size(p);
            string id = fname + "_" + to_string(fsize);

            if (photos_.count(id)) continue;

            bool video = isVideoFile(p);

            PhotoEntry photo;
            photo.id = id;
            photo.filename = fname;
            photo.fileSize = fsize;
            photo.localPath = filePath;
            photo.isVideo = video;
            photo.isRaw = video ? false : RawLoader::isRawFile(p);
            photo.syncState = SyncState::LocalOnly;
            if (!video) {
                extractExifMetadata(filePath, photo);
                extractXmpMetadata(filePath, photo);
            }

            photos_[id] = photo;
            newEntries.push_back(photo);
            added++;

            // Queue file copy if library folder configured and source is outside it
            if (!rawStoragePath_.empty()) {
                fs::path libPath = fs::canonical(fs::path(rawStoragePath_));
                fs::path srcCanonical = fs::canonical(p);
                string srcStr = srcCanonical.string();
                string libStr = libPath.string();
                if (srcStr.substr(0, libStr.size()) != libStr) {
                    string subdir = dateToSubdir(photo.dateTimeOriginal, filePath);
                    fs::path destDir = libPath / subdir;
                    fs::create_directories(destDir);
                    fs::path destPath = resolveDestPath(destDir, fname);
                    lock_guard<mutex> lock(copyMutex_);
                    pendingCopies_.push_back({id, filePath, destPath.string()});
                }
            }
        }

        if (!newEntries.empty()) {
            db_.insertPhotos(newEntries);
        }
        if (added > 0) {
            logNotice() << "[PhotoProvider] Imported " << added << " files (total: " << photos_.size() << ")";
            startCopyThread();
        }
        return added;
    }

    // --- Scan and import ---

    // Scan local folder for image files (non-blocking, copies happen in background)
    void scanFolder(const string& folderPath) {
        fs::path folder(folderPath);
        if (!fs::exists(folder) || !fs::is_directory(folder)) {
            logWarning() << "[PhotoProvider] Not a valid directory: " << folderPath;
            return;
        }

        logNotice() << "[PhotoProvider] Scanning folder: " << folderPath;
        int added = 0;
        vector<PhotoEntry> newEntries;

        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            if (!isSupportedFile(entry.path())) continue;

            string path = entry.path().string();
            auto size = entry.file_size();
            string fname = entry.path().filename().string();

            string id = fname + "_" + to_string(size);

            // Skip if already known
            if (photos_.count(id)) continue;

            bool video = isVideoFile(entry.path());

            // Register immediately with original path
            PhotoEntry photo;
            photo.id = id;
            photo.filename = fname;
            photo.fileSize = size;
            photo.localPath = path;
            photo.isVideo = video;
            photo.isRaw = video ? false : RawLoader::isRawFile(entry.path());
            photo.syncState = SyncState::LocalOnly;

            // Extract metadata (skip exiv2/XMP for video — may crash)
            if (!video) {
                extractExifMetadata(path, photo);
                extractXmpMetadata(path, photo);
            }

            photos_[id] = photo;
            newEntries.push_back(photo);
            added++;

            // Queue file copy if library folder is configured and source is outside it
            if (!rawStoragePath_.empty()) {
                fs::path libPath = fs::canonical(fs::path(rawStoragePath_));
                fs::path srcCanonical = fs::canonical(fs::path(path));
                string srcStr = srcCanonical.string();
                string libStr = libPath.string();
                // Skip if source is already inside library folder
                if (srcStr.substr(0, libStr.size()) != libStr) {
                    string subdir = dateToSubdir(photo.dateTimeOriginal, path);
                    fs::path destDir = libPath / subdir;
                    fs::create_directories(destDir);
                    fs::path destPath = resolveDestPath(destDir, fname);
                    lock_guard<mutex> lock(copyMutex_);
                    pendingCopies_.push_back({id, path, destPath.string()});
                }
            }
        }

        if (!newEntries.empty()) {
            db_.insertPhotos(newEntries);
        }
        logNotice() << "[PhotoProvider] Found " << added << " new files (total: " << photos_.size() << ")";

        // Start background copy if there are pending copies
        startCopyThread();
    }

    // --- Server sync ---

    void syncWithServer() {
        if (!isServerReachable()) return;

        auto res = client_.get("/api/photos");
        if (!res.ok()) return;

        // Collect server-side IDs
        unordered_set<string> serverIds;
        auto data = res.json();
        vector<PhotoEntry> newServerPhotos;

        for (const auto& p : data["photos"]) {
            string id = p.value("id", string(""));
            if (id.empty()) continue;
            serverIds.insert(id);

            if (photos_.count(id)) {
                auto& state = photos_[id].syncState;
                if (state == SyncState::LocalOnly) {
                    state = SyncState::Synced;
                    db_.updateSyncState(id, state);
                } else if (state == SyncState::Missing) {
                    state = SyncState::ServerOnly;
                    db_.updateSyncState(id, state);
                }
            } else {
                PhotoEntry photo;
                photo.id = id;
                photo.filename = p.value("filename", string(""));
                photo.fileSize = p.value("fileSize", (uintmax_t)0);
                photo.width = p.value("width", 0);
                photo.height = p.value("height", 0);
                photo.syncState = SyncState::ServerOnly;
                photos_[id] = photo;
                newServerPhotos.push_back(photo);
            }
        }

        if (!newServerPhotos.empty()) {
            db_.insertPhotos(newServerPhotos);
        }

        // Revert Synced photos not found on server back to LocalOnly
        for (auto& [id, photo] : photos_) {
            if (photo.syncState == SyncState::Synced && !serverIds.count(id)) {
                photo.syncState = SyncState::LocalOnly;
                db_.updateSyncState(id, photo.syncState);
            }
        }
    }

    // --- Thumbnail resolution ---

    // Priority: local cache -> server -> local decode
    bool getThumbnail(const string& id, Pixels& outPixels) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        auto& photo = it->second;

        // Video thumbnail: extract frame via AVFoundation
        if (photo.isVideo) {
            // 1. Check local cache
            if (!photo.localThumbnailPath.empty() && fs::exists(photo.localThumbnailPath)) {
                if (outPixels.load(photo.localThumbnailPath)) return true;
            }
            // 2. Extract frame from video file
            if (!photo.localPath.empty() && fs::exists(photo.localPath)) {
                if (VideoPlayer::extractFrame(photo.localPath, outPixels, 1.0f)) {
                    int w = outPixels.getWidth();
                    int h = outPixels.getHeight();
                    if (w > THUMBNAIL_MAX_SIZE || h > THUMBNAIL_MAX_SIZE) {
                        float scale = (float)THUMBNAIL_MAX_SIZE / max(w, h);
                        resizePixels(outPixels, (int)(w * scale), (int)(h * scale));
                    }
                    saveThumbnailCache(id, photo, outPixels);
                    return true;
                }
            }
            return false;
        }

        // 1. Check local thumbnail cache
        if (!photo.localThumbnailPath.empty() && fs::exists(photo.localThumbnailPath)) {
            if (outPixels.load(photo.localThumbnailPath)) {
                return true;
            }
        }

        // 2. Try server thumbnail (skip if server not reachable)
        if (isServerReachable()) {
            auto res = client_.get("/api/photos/" + id + "/thumbnail");
            if (res.ok() && !res.body.empty()) {
                string subdir = dateToSubdir(photo.dateTimeOriginal, photo.localPath);
                string cacheDir = thumbnailCacheDir_ + "/" + subdir;
                fs::create_directories(cacheDir);
                string cachePath = cacheDir + "/" + id + ".jpg";
                ofstream file(cachePath, ios::binary);
                if (file) {
                    file.write(res.body.data(), res.body.size());
                    file.close();
                    photo.localThumbnailPath = cachePath;
                    db_.updateThumbnailPath(id, cachePath);

                    if (outPixels.load(cachePath)) {
                        return true;
                    }
                }
            }
        }

        // 2.5. If RAW/Video primary with JPG/HEIF companion, use companion for thumbnail
        if (!photo.stackId.empty() && photo.stackPrimary && (photo.isRaw || photo.isVideo)) {
            auto sit = stackIndex_.find(photo.stackId);
            if (sit != stackIndex_.end()) {
                for (auto& cid : sit->second) {
                    if (cid == id) continue;
                    auto cit = photos_.find(cid);
                    if (cit == photos_.end()) continue;
                    auto& companion = cit->second;
                    if (companion.isRaw || companion.isVideo) continue;
                    if (companion.localPath.empty() || !fs::exists(companion.localPath)) continue;
                    // Found a JPG/HEIF companion - load it
                    Pixels full;
                    if (full.load(companion.localPath)) {
                        int w = full.getWidth();
                        int h = full.getHeight();
                        if (w > THUMBNAIL_MAX_SIZE || h > THUMBNAIL_MAX_SIZE) {
                            float scale = (float)THUMBNAIL_MAX_SIZE / max(w, h);
                            resizePixels(full, (int)(w * scale), (int)(h * scale));
                        }
                        outPixels = std::move(full);
                        saveThumbnailCache(id, photo, outPixels);
                        return true;
                    }
                }
            }
        }

        // 3. Fallback: decode from local file
        if (!photo.localPath.empty() && fs::exists(photo.localPath)) {
            if (photo.isRaw) {
                // Try embedded JPEG first (fast), fallback to RAW decode
                bool loaded = RawLoader::loadEmbeddedPreview(photo.localPath, outPixels);
                if (loaded) {
                    // Embedded preview returns float RGBA — convert to 8-bit for thumbnails
                    convertF32ToU8(outPixels);
                }
                if (!loaded) {
                    loaded = RawLoader::loadWithMaxSize(photo.localPath, outPixels, THUMBNAIL_MAX_SIZE);
                }
                if (loaded) {
                    // Resize if needed (embedded preview may be larger)
                    int w = outPixels.getWidth();
                    int h = outPixels.getHeight();
                    if (w > THUMBNAIL_MAX_SIZE || h > THUMBNAIL_MAX_SIZE) {
                        float scale = (float)THUMBNAIL_MAX_SIZE / max(w, h);
                        resizePixels(outPixels, (int)(w * scale), (int)(h * scale));
                    }
                    saveThumbnailCache(id, photo, outPixels);
                    return true;
                }
            } else {
                Pixels full;
                if (full.load(photo.localPath)) {
                    int w = full.getWidth();
                    int h = full.getHeight();
                    if (w > THUMBNAIL_MAX_SIZE || h > THUMBNAIL_MAX_SIZE) {
                        float scale = (float)THUMBNAIL_MAX_SIZE / max(w, h);
                        resizePixels(full, (int)(w * scale), (int)(h * scale));
                    }
                    outPixels = std::move(full);
                    saveThumbnailCache(id, photo, outPixels);
                    return true;
                }
            }
        }

        return false;
    }

    // --- Upload ---

    bool uploadToServer(const string& id) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        auto& photo = it->second;

        if (photo.localPath.empty()) return false;

        photo.syncState = SyncState::Syncing;

        auto res = client_.post("/api/import", {{"path", photo.localPath}});
        if (res.ok()) {
            photo.syncState = SyncState::Synced;
            db_.updateSyncState(id, photo.syncState);
            return true;
        }

        photo.syncState = SyncState::LocalOnly;
        return false;
    }

    // --- Rich metadata setters ---

    static int64_t nowMs() {
        return chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();
    }

    bool setGps(const string& id, double lat, double lon) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        it->second.latitude = lat;
        it->second.longitude = lon;
        db_.updateGps(id, lat, lon);
        return true;
    }

    bool setRating(const string& id, int rating) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        rating = clamp(rating, 0, 5);
        auto ts = nowMs();
        it->second.rating = rating;
        it->second.ratingUpdatedAt = ts;
        db_.updateRating(id, rating, ts);
        writeXmpSidecarIfLocal(it->second);
        return true;
    }

    bool setDenoise(const string& id, float chroma, float luma) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        it->second.chromaDenoise = chroma;
        it->second.lumaDenoise = luma;
        db_.updateDenoise(id, chroma, luma);
        return true;
    }

    bool setDevelop(const string& id, float exposure, float wbTemp, float wbTint,
                    float contrast, float highlights, float shadows,
                    float whites, float blacks,
                    float vibrance, float saturation,
                    float chroma, float luma) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        it->second.devExposure = exposure;
        it->second.devWbTemp = wbTemp;
        it->second.devWbTint = wbTint;
        it->second.devContrast = contrast;
        it->second.devHighlights = highlights;
        it->second.devShadows = shadows;
        it->second.devWhites = whites;
        it->second.devBlacks = blacks;
        it->second.devVibrance = vibrance;
        it->second.devSaturation = saturation;
        it->second.chromaDenoise = chroma;
        it->second.lumaDenoise = luma;
        db_.updateDevelop(id, exposure, wbTemp, wbTint,
                          contrast, highlights, shadows, whites, blacks,
                          vibrance, saturation, chroma, luma);
        return true;
    }

    bool setUserCrop(const string& id, float x, float y, float w, float h) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        it->second.userCropX = x;
        it->second.userCropY = y;
        it->second.userCropW = w;
        it->second.userCropH = h;
        db_.updateUserCrop(id, x, y, w, h);
        return true;
    }

    bool setUserRotation(const string& id, float angle, int rot90) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        it->second.userAngle = angle;
        it->second.userRotation90 = rot90;
        db_.updateUserRotation(id, angle, rot90);
        return true;
    }

    bool setUserPerspective(const string& id, float perspV, float perspH, float shear) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        it->second.userPerspV = perspV;
        it->second.userPerspH = perspH;
        it->second.userShear = shear;
        db_.updateUserPerspective(id, perspV, perspH, shear);
        return true;
    }

    bool setColorLabel(const string& id, const string& label) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        auto ts = nowMs();
        it->second.colorLabel = label;
        it->second.colorLabelUpdatedAt = ts;
        db_.updateColorLabel(id, label, ts);
        writeXmpSidecarIfLocal(it->second);
        return true;
    }

    bool setFlag(const string& id, int flag) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        flag = clamp(flag, -1, 1);
        auto ts = nowMs();
        it->second.flag = flag;
        it->second.flagUpdatedAt = ts;
        db_.updateFlag(id, flag, ts);
        writeXmpSidecarIfLocal(it->second);
        return true;
    }

    bool setMemo(const string& id, const string& memo) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        auto ts = nowMs();
        it->second.memo = memo;
        it->second.memoUpdatedAt = ts;
        db_.updateMemo(id, memo, ts);
        writeXmpSidecarIfLocal(it->second);
        return true;
    }

    bool setTags(const string& id, const string& tags) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        auto ts = nowMs();
        it->second.tags = tags;
        it->second.tagsUpdatedAt = ts;
        db_.updateTags(id, tags, ts);
        writeXmpSidecarIfLocal(it->second);
        return true;
    }

    // Update lens correction params JSON (e.g., to add intW/intH after first RAW load)
    bool updateLensCorrectionParams(const string& id, const string& params) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        it->second.lensCorrectionParams = params;
        db_.updateLensCorrectionParams(id, params);
        return true;
    }

    // --- Folder tree ---

    // Folder info for tree display
    struct FolderInfo {
        string path;           // absolute path
        string displayName;    // directory name (leaf)
        int photoCount = 0;    // photos in this exact folder
        bool exists = true;    // folder exists on disk
    };

    // Build folder list from DB entries' localPath
    // Includes intermediate directories so the tree hierarchy is complete
    vector<FolderInfo> buildFolderList() const {
        unordered_map<string, FolderInfo> folders;

        // Collect direct parent directories of all photos
        for (const auto& [id, photo] : photos_) {
            if (photo.localPath.empty()) continue;
            string dir = fs::path(photo.localPath).parent_path().string();
            auto& info = folders[dir];
            info.path = dir;
            info.displayName = fs::path(dir).filename().string();
            info.photoCount++;
        }

        // Add intermediate directories between leaf folders and their root
        // so the tree hierarchy can be properly built.
        // For managed photos: stop at rawStoragePath parent.
        // For external references: stop at volume/mount root.
        vector<string> leafPaths;
        for (const auto& [path, _] : folders) {
            leafPaths.push_back(path);
        }

        // Detect stop boundary for each leaf path
        auto shouldStop = [this](const string& leafPath, const string& pstr) -> bool {
            // If leaf is under rawStoragePath, stop at rawStoragePath parent
            if (!rawStoragePath_.empty() &&
                leafPath.size() > rawStoragePath_.size() &&
                leafPath.substr(0, rawStoragePath_.size()) == rawStoragePath_) {
                fs::path rawParent = fs::path(rawStoragePath_).parent_path();
                return pstr.size() < rawParent.string().size();
            }
            // External path: stop at depth 2 (/Volumes/X, /Users/X, /mnt/X)
            int slashes = 0;
            for (char c : pstr) { if (c == '/') slashes++; }
            return slashes < 2;
        };

        for (const auto& leafPath : leafPaths) {
            fs::path p = fs::path(leafPath).parent_path();
            while (!p.empty() && p.string() != "/" && p != p.root_path()) {
                string pstr = p.string();
                if (shouldStop(leafPath, pstr)) break;
                if (folders.count(pstr)) break;
                auto& info = folders[pstr];
                info.path = pstr;
                info.displayName = p.filename().string();
                info.photoCount = 0;
                p = p.parent_path();
            }
        }

        // Check existence
        for (auto& [path, info] : folders) {
            info.exists = fs::exists(path);
        }
        // Sort by path
        vector<FolderInfo> result;
        for (auto& [_, info] : folders) result.push_back(std::move(info));
        sort(result.begin(), result.end(), [](const FolderInfo& a, const FolderInfo& b) {
            return a.path < b.path;
        });
        return result;
    }

    // --- Collections ---

    void loadCollections() {
        collections_ = db_.loadCollections();
    }

    const vector<Collection>& getCollections() const { return collections_; }

    vector<string> getCollectionPhotoIds(int collectionId) {
        return db_.getCollectionPhotoIds(collectionId);
    }

    int createCollection(const string& name, int parentId = 0) {
        int id = db_.insertCollection(name, parentId, Collection::Regular);
        loadCollections();
        return id;
    }

    bool renameCollection(int id, const string& newName) {
        bool ok = db_.renameCollection(id, newName);
        if (ok) loadCollections();
        return ok;
    }

    bool deleteCollection(int id) {
        bool ok = db_.deleteCollection(id);
        if (ok) loadCollections();
        return ok;
    }

    bool addToCollection(int collectionId, const vector<string>& photoIds) {
        vector<pair<string, int>> photos;
        for (int i = 0; i < (int)photoIds.size(); i++)
            photos.push_back({photoIds[i], i});
        bool ok = db_.insertCollectionPhotos(collectionId, photos);
        if (ok) loadCollections();
        return ok;
    }

    // Import collections from LrcatImporter results into DB
    void importCollections(const vector<LrcatImporter::CollectionEntry>& entries,
                           const vector<LrcatImporter::CollectionImageEntry>& images) {
        if (entries.empty()) return;

        // Clear existing collections before re-import
        db_.clearCollections();

        // Map LR id -> our DB id for parent resolution
        unordered_map<int64_t, int> lrIdToDbId;

        // First pass: insert groups and top-level collections (parentId == 0)
        // Second pass: insert children using mapped parent IDs
        // Since LR uses its own id_local, we need to map them.

        // Sort: groups first, then others, to ensure parents exist before children.
        // But LR allows arbitrary nesting, so we do multi-pass.
        vector<const LrcatImporter::CollectionEntry*> pending;
        for (const auto& e : entries) pending.push_back(&e);

        int maxPasses = 10;
        while (!pending.empty() && maxPasses-- > 0) {
            vector<const LrcatImporter::CollectionEntry*> next;
            for (auto* e : pending) {
                int dbParentId = 0;
                if (e->lrParentId != 0) {
                    auto it = lrIdToDbId.find(e->lrParentId);
                    if (it == lrIdToDbId.end()) {
                        // Parent not yet inserted, defer
                        next.push_back(e);
                        continue;
                    }
                    dbParentId = it->second;
                }
                int dbId = db_.insertCollection(
                    e->name, dbParentId, e->type, e->rules, "", "", 0);
                if (dbId > 0) {
                    lrIdToDbId[e->lrId] = dbId;
                }
            }
            pending = std::move(next);
        }

        // Insert collection-photo associations
        // Group by collection for batch insert
        unordered_map<int, vector<pair<string, int>>> collPhotos;
        for (const auto& img : images) {
            auto it = lrIdToDbId.find(img.lrCollectionId);
            if (it == lrIdToDbId.end()) continue;
            collPhotos[it->second].push_back({img.photoId, img.position});
        }
        for (auto& [collId, photos] : collPhotos) {
            db_.insertCollectionPhotos(collId, photos);
        }

        // Reload cache
        loadCollections();

        int totalPhotos = 0;
        for (const auto& c : collections_) totalPhotos += c.photoCount;
        logNotice() << "[PhotoProvider] importCollections: " << collections_.size()
                    << " collections, " << totalPhotos << " photo associations";
    }

    // --- Accessors ---

    void setSyncState(const string& id, SyncState state) {
        auto it = photos_.find(id);
        if (it != photos_.end()) {
            it->second.syncState = state;
            db_.updateSyncState(id, state);
        }
    }

    const unordered_map<string, PhotoEntry>& photos() const { return photos_; }
    size_t getCount() const { return photos_.size(); }

    PhotoEntry* getPhoto(const string& id) {
        auto it = photos_.find(id);
        return (it != photos_.end()) ? &it->second : nullptr;
    }

    // Remove a photo from memory and database
    bool removePhoto(const string& id) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        db_.deletePhoto(id);
        photos_.erase(it);
        return true;
    }

    // Fully delete photos: local file + thumbnail + DB + memory
    int deletePhotos(const vector<string>& ids) {
        int deleted = 0;
        for (const auto& id : ids) {
            auto it = photos_.find(id);
            if (it == photos_.end()) continue;

            auto& photo = it->second;

            // Delete local RAW/image file + XMP sidecar (only if managed)
            if (photo.isManaged && !photo.localPath.empty() && fs::exists(photo.localPath)) {
                // Delete XMP sidecar
                string xmpPath = xmpWritePath(photo.localPath);
                if (!xmpPath.empty() && fs::exists(xmpPath)) {
                    try { fs::remove(xmpPath); } catch (...) {}
                }
                try {
                    fs::remove(photo.localPath);
                    logNotice() << "[Delete] Removed file: " << photo.localPath;
                } catch (const exception& e) {
                    logWarning() << "[Delete] Failed to remove file: " << e.what();
                }
            }

            // Delete thumbnail cache
            if (!photo.localThumbnailPath.empty() && fs::exists(photo.localThumbnailPath)) {
                try { fs::remove(photo.localThumbnailPath); } catch (...) {}
            }

            // Delete smart preview
            if (!photo.localSmartPreviewPath.empty() && fs::exists(photo.localSmartPreviewPath)) {
                try { fs::remove(photo.localSmartPreviewPath); } catch (...) {}
            }

            // Delete embeddings, DB entry, and memory
            db_.deleteEmbeddings(id);
            db_.deletePhoto(id);
            photos_.erase(it);
            deleted++;
        }
        return deleted;
    }

    // --- Stacking (RAW+JPG, Live Photo grouping) ---

    // Resolve stacks: group entries by (dir, stem, dateTimeOriginal)
    // and assign stack_id / stack_primary accordingly
    int resolveStacks() {
        // Group by (dir, lowercase_stem)
        struct GroupKey {
            string dir;
            string stem;
            bool operator==(const GroupKey& o) const { return dir == o.dir && stem == o.stem; }
        };
        struct GroupKeyHash {
            size_t operator()(const GroupKey& k) const {
                size_t h1 = hash<string>{}(k.dir);
                size_t h2 = hash<string>{}(k.stem);
                return h1 ^ (h2 << 1);
            }
        };

        unordered_map<GroupKey, vector<string>, GroupKeyHash> groups;

        for (auto& [id, photo] : photos_) {
            if (photo.localPath.empty()) continue;
            fs::path p(photo.localPath);
            string dir = p.parent_path().string();
            string stem = p.stem().string();
            transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            groups[{dir, stem}].push_back(id);
        }

        int stackCount = 0;
        vector<pair<string, pair<string, bool>>> updates; // id -> (stackId, primary)

        for (auto& [key, ids] : groups) {
            if (ids.size() < 2) continue;

            // Sub-group by dateTimeOriginal (must match exactly)
            unordered_map<string, vector<string>> byDate;
            for (auto& id : ids) {
                auto& dt = photos_[id].dateTimeOriginal;
                if (dt.empty()) continue; // skip entries without timestamp
                byDate[dt].push_back(id);
            }

            for (auto& [dt, dateIds] : byDate) {
                if (dateIds.size() < 2) continue;

                // Category: RAW=2, Video=1, Image(JPG/HEIF/PNG)=0
                // Same-category groups are NOT stacked
                auto typeCategory = [this](const string& id) -> int {
                    auto& p = photos_[id];
                    if (p.isRaw) return 2;
                    if (p.isVideo) return 1;
                    return 0; // JPG, HEIF, PNG, etc. — all same category
                };

                // Check if all entries are the same category
                int firstCat = typeCategory(dateIds[0]);
                bool allSame = true;
                for (size_t i = 1; i < dateIds.size(); i++) {
                    if (typeCategory(dateIds[i]) != firstCat) {
                        allSame = false;
                        break;
                    }
                }
                if (allSame) continue; // e.g. RAW+RAW or JPG+JPG — don't stack

                // Sort by priority: RAW > Video > Image, then by fileSize
                sort(dateIds.begin(), dateIds.end(), [&](const string& a, const string& b) {
                    int ca = typeCategory(a), cb = typeCategory(b);
                    if (ca != cb) return ca > cb;
                    return photos_[a].fileSize > photos_[b].fileSize;
                });

                string primaryId = dateIds[0];
                string stackId = primaryId; // use primary's ID as stack_id

                for (size_t i = 0; i < dateIds.size(); i++) {
                    bool isPrimary = (i == 0);
                    updates.push_back({dateIds[i], {stackId, isPrimary}});
                }
                stackCount++;
            }
        }

        // Apply updates to memory + DB
        if (!updates.empty()) {
            for (auto& [id, val] : updates) {
                auto& photo = photos_[id];
                photo.stackId = val.first;
                photo.stackPrimary = val.second;
                db_.updateStackId(id, val.first, val.second);
            }
            // Build stack index
            rebuildStackIndex();
            logNotice() << "[PhotoProvider] Resolved " << stackCount << " stacks ("
                        << updates.size() << " entries)";
        }
        return stackCount;
    }

    // Get companion IDs for a stacked photo
    vector<string> getStackCompanions(const string& id) const {
        auto it = photos_.find(id);
        if (it == photos_.end() || it->second.stackId.empty()) return {};

        auto sit = stackIndex_.find(it->second.stackId);
        if (sit == stackIndex_.end()) return {};

        vector<string> result;
        for (auto& cid : sit->second) {
            if (cid != id) result.push_back(cid);
        }
        return result;
    }

    // Get stack size for a photo (0 = not stacked)
    int getStackSize(const string& id) const {
        auto it = photos_.find(id);
        if (it == photos_.end() || it->second.stackId.empty()) return 0;
        auto sit = stackIndex_.find(it->second.stackId);
        return sit != stackIndex_.end() ? (int)sit->second.size() : 0;
    }

    // Get sorted photo list (by dateTimeOriginal descending, newest first)
    // Stacked non-primary entries are excluded from the result
    vector<string> getSortedIds() const {
        vector<string> ids;
        ids.reserve(photos_.size());
        for (const auto& [id, photo] : photos_) {
            // Filter out non-primary stacked entries
            if (!photo.stackId.empty() && !photo.stackPrimary) continue;
            ids.push_back(id);
        }
        sort(ids.begin(), ids.end(), [this](const string& a, const string& b) {
            const auto& da = photos_.at(a).dateTimeOriginal;
            const auto& db = photos_.at(b).dateTimeOriginal;
            // Empty dates sort to end
            if (da.empty() != db.empty()) return !da.empty();
            if (da != db) return da > db;  // newest first
            return photos_.at(a).filename < photos_.at(b).filename;
        });
        return ids;
    }

    // Get all LocalOnly photo IDs (for upload queue)
    vector<pair<string, string>> getLocalOnlyPhotos() const {
        vector<pair<string, string>> result;
        for (const auto& [id, photo] : photos_) {
            if (photo.syncState == SyncState::LocalOnly && !photo.localPath.empty()) {
                result.push_back({id, photo.localPath});
            }
        }
        return result;
    }

    // --- Reference import (lrcat etc.) ---

    // Import pre-built entries as external references, then enrich with EXIF + XMP
    int importReferences(vector<PhotoEntry>& entries) {
        int added = 0;
        vector<PhotoEntry> newEntries;

        for (auto& e : entries) {
            if (photos_.count(e.id)) continue;  // skip duplicates

            // Extract full EXIF from file (lens correction, creative style, etc.)
            if (!e.localPath.empty() && !e.isVideo && fs::exists(e.localPath)) {
                extractExifMetadata(e.localPath, e);
                extractXmpMetadata(e.localPath, e);
            }

            photos_[e.id] = e;
            newEntries.push_back(e);
            added++;
        }

        if (!newEntries.empty()) {
            db_.insertPhotos(newEntries);
        }
        logNotice() << "[PhotoProvider] importReferences: " << added
                    << " added (total: " << photos_.size() << ")";
        return added;
    }

    // --- Faces ---

    // Import face entries from LrcatImporter into DB
    int importFaces(const vector<LrcatImporter::FaceEntry>& faces) {
        if (faces.empty()) return 0;

        // Collect unique person names
        vector<string> names;
        {
            unordered_set<string> nameSet;
            for (const auto& f : faces) {
                if (!f.personName.empty()) nameSet.insert(f.personName);
            }
            names.assign(nameSet.begin(), nameSet.end());
        }

        // Insert persons and get name->id mapping
        auto personMap = db_.insertPersons(names);

        // Build FaceRow list for batch insert
        vector<PhotoDatabase::FaceRow> rows;
        rows.reserve(faces.size());
        for (const auto& f : faces) {
            // Only import faces for photos we actually have
            if (!photos_.count(f.photoId)) continue;

            PhotoDatabase::FaceRow row;
            row.photoId = f.photoId;
            row.x = f.x;
            row.y = f.y;
            row.w = f.w;
            row.h = f.h;
            row.source = "lightroom";
            row.lrClusterId = f.lrClusterId;

            if (!f.personName.empty()) {
                auto it = personMap.find(f.personName);
                if (it != personMap.end()) {
                    row.personId = it->second;
                }
            }

            rows.push_back(std::move(row));
        }

        int inserted = db_.insertFaces(rows);
        logNotice() << "[PhotoProvider] importFaces: " << inserted
                    << " faces, " << names.size() << " persons";

        // Rebuild cache after import
        loadFaceCache();
        return inserted;
    }

    // Load photo_id -> person names cache from DB
    void loadFaceCache() {
        faceNameCache_ = db_.loadPersonNamesByPhoto();
        logNotice() << "[PhotoProvider] Face cache: "
                    << faceNameCache_.size() << " photos with faces";
    }

    // Load face embedding cache from DB (face DB id → embedding)
    void loadFaceEmbeddingCache() {
        faceEmbeddingCache_ = db_.loadFaceEmbeddings();
        logNotice() << "[PhotoProvider] Face embedding cache: "
                    << faceEmbeddingCache_.size() << " face embeddings";
    }

    // Get person names for a photo (empty if none)
    const vector<string>* getPersonNames(const string& photoId) const {
        auto it = faceNameCache_.find(photoId);
        return it != faceNameCache_.end() ? &it->second : nullptr;
    }

    // Check if two photos share any person
    bool sharesPerson(const string& id1, const string& id2) const {
        auto* names1 = getPersonNames(id1);
        auto* names2 = getPersonNames(id2);
        if (!names1 || !names2) return false;
        for (const auto& n : *names1) {
            for (const auto& m : *names2) {
                if (n == m) return true;
            }
        }
        return false;
    }

    // Search photos by person name (case-insensitive partial match)
    vector<string> searchByPersonName(const string& query) const {
        if (query.empty() || faceNameCache_.empty()) return {};

        string lq = query;
        transform(lq.begin(), lq.end(), lq.begin(), ::tolower);

        vector<string> result;
        for (const auto& [photoId, names] : faceNameCache_) {
            for (const auto& name : names) {
                string ln = name;
                transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
                if (ln.find(lq) != string::npos) {
                    result.push_back(photoId);
                    break;
                }
            }
        }
        return result;
    }

    // Search photos by text field matching (filename, camera, lens, tags, memo, person names)
    // Returns photo IDs that match any text field (case-insensitive partial match)
    vector<string> searchByTextFields(const string& query) const {
        if (query.empty()) return {};

        string lq = query;
        transform(lq.begin(), lq.end(), lq.begin(), ::tolower);

        auto contains = [&lq](const string& field) {
            if (field.empty()) return false;
            string lf = field;
            transform(lf.begin(), lf.end(), lf.begin(), ::tolower);
            return lf.find(lq) != string::npos;
        };

        vector<string> result;
        for (const auto& [id, photo] : photos_) {
            if (contains(fs::path(photo.filename).stem().string()) ||
                contains(photo.camera) || contains(photo.cameraMake) ||
                contains(photo.lens) || contains(photo.lensMake) ||
                contains(photo.memo) || contains(photo.colorLabel) ||
                contains(photo.creativeStyle) || contains(photo.dateTimeOriginal) ||
                contains(photo.tags)) {
                result.push_back(id);
                continue;
            }
            // Check person names
            auto it = faceNameCache_.find(id);
            if (it != faceNameCache_.end()) {
                for (const auto& name : it->second) {
                    if (contains(name)) {
                        result.push_back(id);
                        break;
                    }
                }
            }
        }
        return result;
    }

    // --- People View / Clustering ---

    struct FaceCluster {
        int clusterId = 0;           // negative: auto cluster, positive: person_id
        string name;                 // person name or "" (unnamed)
        string suggestedName;        // embedding-based name suggestion
        int personId = 0;            // DB person_id (0 = unnamed cluster)
        vector<int> faceIds;         // member face IDs
        int photoCount = 0;          // unique photo count
        string repPhotoId;           // representative photo ID
        float repFaceX = 0, repFaceY = 0, repFaceW = 0, repFaceH = 0;  // rep face bbox
    };

    struct ClusterResult {
        vector<FaceCluster> clusters;
        int processedUnnamed = 0;    // number of unnamed faces actually clustered
        int totalUnnamed = 0;        // total unnamed faces in DB
    };

    // Pre-load face data from DB (call on main thread, returns data for clustering)
    struct FaceClusterInput {
        vector<PhotoDatabase::FaceInfo> allFaces;
        unordered_map<int, string> personNames;
    };

    FaceClusterInput loadFaceClusterData() {
        FaceClusterInput input;
        input.allFaces = db_.loadAllFacesWithEmbeddings();
        input.personNames = loadPersonIdToName();
        return input;
    }

    // Build clusters from pre-loaded data (thread-safe: no DB access)
    // maxFaces: limit unnamed faces to process (0 = unlimited)
    static ClusterResult clusterFaces(
            const vector<PhotoDatabase::FaceInfo>& allFaces,
            const unordered_map<int, string>& personNames,
            float threshold = 0.60f,
            int maxFaces = 0) {
        if (allFaces.empty()) return {};

        // Step 1: Group named faces by person_id
        unordered_map<int, vector<int>> namedGroups;  // person_id -> face indices
        vector<int> unnamedIndices;

        for (int i = 0; i < (int)allFaces.size(); i++) {
            if (allFaces[i].personId > 0) {
                namedGroups[allFaces[i].personId].push_back(i);
            } else {
                unnamedIndices.push_back(i);
            }
        }

        int totalUnnamed = (int)unnamedIndices.size();

        // Limit unnamed faces if maxFaces specified
        if (maxFaces > 0 && (int)unnamedIndices.size() > maxFaces) {
            unnamedIndices.resize(maxFaces);
        }
        int processedUnnamed = (int)unnamedIndices.size();

        vector<FaceCluster> clusters;

        // Step 2: Build named clusters
        unordered_map<int, vector<float>> namedCentroids;  // person_id -> centroid
        for (auto& [pid, indices] : namedGroups) {
            FaceCluster c;
            c.personId = pid;
            auto nameIt = personNames.find(pid);
            c.name = nameIt != personNames.end() ? nameIt->second : "";
            c.clusterId = pid;

            unordered_set<string> photoSet;
            vector<float> centroid;
            for (int idx : indices) {
                auto& fi = allFaces[idx];
                c.faceIds.push_back(fi.faceId);
                photoSet.insert(fi.photoId);
                if (centroid.empty()) {
                    centroid = fi.embedding;
                } else {
                    for (int d = 0; d < (int)centroid.size(); d++)
                        centroid[d] += fi.embedding[d];
                }
            }
            c.photoCount = (int)photoSet.size();

            // Normalize centroid
            if (!centroid.empty()) {
                float norm = 0;
                for (float v : centroid) norm += v * v;
                norm = sqrtf(norm);
                if (norm > 0) for (float& v : centroid) v /= norm;
                namedCentroids[pid] = centroid;
            }

            // Representative face: first face
            if (!indices.empty()) {
                auto& rep = allFaces[indices[0]];
                c.repPhotoId = rep.photoId;
                c.repFaceX = rep.x;
                c.repFaceY = rep.y;
                c.repFaceW = rep.w;
                c.repFaceH = rep.h;
            }

            clusters.push_back(std::move(c));
        }

        // Sort named clusters by photo count descending
        sort(clusters.begin(), clusters.end(),
             [](const FaceCluster& a, const FaceCluster& b) {
                 return a.photoCount > b.photoCount;
             });

        // Step 3: Greedy centroid clustering for unnamed faces
        // Full pairwise for small counts, windowed for large
        static constexpr int CLUSTER_WINDOW = 600;

        int nextClusterId = -1;
        int unnamedCount = (int)unnamedIndices.size();
        vector<bool> assigned(unnamedCount, false);

        // Determine embedding dimension
        int embDim = 0;
        for (int idx : unnamedIndices) {
            if (!allFaces[idx].embedding.empty()) {
                embDim = (int)allFaces[idx].embedding.size();
                break;
            }
        }

        struct UnnamedCluster {
            vector<int> memberIndices;  // indices into unnamedIndices
            vector<float> centroid;
        };
        vector<UnnamedCluster> unnamedClusters;

        if (embDim > 0 && unnamedCount > 0) {
            // Sort unnamed faces by embedding projection for spatial locality
            // Use sum of first 8 components as a simple projection
            auto projection = [&](int unnamedIdx) -> float {
                auto& emb = allFaces[unnamedIndices[unnamedIdx]].embedding;
                if (emb.empty()) return 0.0f;
                float sum = 0;
                int dims = min(8, (int)emb.size());
                for (int d = 0; d < dims; d++) sum += emb[d];
                return sum;
            };

            // Create sorted index array
            vector<int> sortedOrder(unnamedCount);
            iota(sortedOrder.begin(), sortedOrder.end(), 0);
            sort(sortedOrder.begin(), sortedOrder.end(),
                 [&](int a, int b) { return projection(a) < projection(b); });

            // Remap assigned array to sorted order
            // sortedOrder[i] = original index into unnamedIndices
            // We need assigned to track by sorted position

            // Use full pairwise for small counts (<=1000), windowed for large
            int window = (unnamedCount <= 1000) ? unnamedCount : CLUSTER_WINDOW;
            logNotice() << "[Clustering] " << unnamedCount
                        << " unnamed faces, window=" << window;

            int assignedCount = 0;
            for (int si = 0; si < unnamedCount; si++) {
                int origI = sortedOrder[si];
                if (assigned[origI]) continue;
                auto& seed = allFaces[unnamedIndices[origI]];
                if (seed.embedding.empty()) { assigned[origI] = true; assignedCount++; continue; }

                UnnamedCluster uc;
                uc.centroid = seed.embedding;
                uc.memberIndices.push_back(origI);
                assigned[origI] = true;
                assignedCount++;

                // Only check within the window in sorted order
                int windowEnd = min(si + window, unnamedCount);
                for (int sj = si + 1; sj < windowEnd; sj++) {
                    int origJ = sortedOrder[sj];
                    if (assigned[origJ]) continue;
                    auto& cand = allFaces[unnamedIndices[origJ]];
                    if (cand.embedding.empty()) continue;

                    float sim = cosineSimilarity(uc.centroid, cand.embedding);
                    if (sim > threshold) {
                        uc.memberIndices.push_back(origJ);
                        assigned[origJ] = true;
                        assignedCount++;

                        // Update centroid (running average + normalize)
                        int n = (int)uc.memberIndices.size();
                        for (int d = 0; d < embDim; d++) {
                            uc.centroid[d] = uc.centroid[d] * (n - 1) / n +
                                             cand.embedding[d] / n;
                        }
                        float norm = 0;
                        for (float v : uc.centroid) norm += v * v;
                        norm = sqrtf(norm);
                        if (norm > 0) for (float& v : uc.centroid) v /= norm;
                    }
                }

                unnamedClusters.push_back(std::move(uc));

                if (unnamedClusters.size() % 5000 == 0) {
                    logNotice() << "[Clustering] progress: " << unnamedClusters.size()
                                << " clusters, " << assignedCount << "/" << unnamedCount;
                }
            }
        }

        // Step 4: Build FaceCluster objects for unnamed clusters (skip size=1)
        for (auto& uc : unnamedClusters) {
            if ((int)uc.memberIndices.size() < 2) continue;  // skip singletons

            FaceCluster c;
            c.clusterId = nextClusterId--;
            c.personId = 0;

            unordered_set<string> photoSet;
            for (int mi : uc.memberIndices) {
                auto& fi = allFaces[unnamedIndices[mi]];
                c.faceIds.push_back(fi.faceId);
                photoSet.insert(fi.photoId);
            }
            c.photoCount = (int)photoSet.size();

            // Representative face
            auto& rep = allFaces[unnamedIndices[uc.memberIndices[0]]];
            c.repPhotoId = rep.photoId;
            c.repFaceX = rep.x;
            c.repFaceY = rep.y;
            c.repFaceW = rep.w;
            c.repFaceH = rep.h;

            // Suggest name by comparing centroid to named centroids
            float bestSim = 0;
            string bestName;
            for (auto& [pid, nc] : namedCentroids) {
                float sim = cosineSimilarity(uc.centroid, nc);
                if (sim > bestSim) {
                    bestSim = sim;
                    auto nit = personNames.find(pid);
                    bestName = nit != personNames.end() ? nit->second : "";
                }
            }
            if (bestSim > threshold && !bestName.empty()) {
                c.suggestedName = bestName;
            }

            clusters.push_back(std::move(c));
        }

        logNotice() << "[Clustering] " << clusters.size() << " clusters ("
                    << namedGroups.size() << " named, "
                    << (clusters.size() - namedGroups.size()) << " unnamed) from "
                    << allFaces.size() << " faces"
                    << " [processed " << processedUnnamed << "/" << totalUnnamed << " unnamed]";
        return {std::move(clusters), processedUnnamed, totalUnnamed};
    }

    // Convenience: load data + cluster in one call (main thread only)
    ClusterResult buildFaceClusters(float threshold = 0.60f, int maxFaces = 0) {
        auto input = loadFaceClusterData();
        return clusterFaces(input.allFaces, input.personNames, threshold, maxFaces);
    }

    // Assign a name to an unnamed cluster (batch update face person_id)
    int assignNameToCluster(const FaceCluster& cluster, const string& name) {
        int personId = db_.getOrCreatePerson(name);
        if (personId <= 0) return 0;
        db_.batchUpdateFacePersonId(cluster.faceIds, personId);
        loadFaceCache();
        return personId;
    }

    // Get face briefs (id, photo_id, bbox) for gallery display
    vector<PhotoDatabase::FaceBrief> getFaceBriefs(const vector<int>& faceIds) {
        return db_.getFaceBriefs(faceIds);
    }

    // Get photo IDs for a set of face IDs
    vector<string> getPhotoIdsForFaceIds(const vector<int>& faceIds) {
        return db_.getPhotoIdsForFaceIds(faceIds);
    }

    // Merge two persons
    bool mergePersons(int targetId, int sourceId) {
        bool ok = db_.mergePersons(targetId, sourceId);
        if (ok) loadFaceCache();
        return ok;
    }

    // Unassign faces from their person (set person_id = NULL)
    bool unassignFaces(const vector<int>& faceIds) {
        bool ok = db_.unassignFaces(faceIds);
        if (ok) loadFaceCache();
        return ok;
    }

    // Assign faces to an existing person
    bool assignFacesToPerson(const vector<int>& faceIds, int personId) {
        bool ok = db_.batchUpdateFacePersonId(faceIds, personId);
        if (ok) loadFaceCache();
        return ok;
    }

    // Rename a person
    bool renamePerson(int personId, const string& newName) {
        bool ok = db_.renamePerson(personId, newName);
        if (ok) loadFaceCache();
        return ok;
    }

    // --- Face Detection Pipeline ---

    // Initialize face detection models (SCRFD + ArcFace)
    void initFaceModels(const string& detModelPath, const string& recModelPath) {
        if (faceDetector_.load(detModelPath) && faceRecognizer_.load(recModelPath)) {
            faceModelsReady_ = true;
        }
    }

    bool isFaceModelsReady() const { return faceModelsReady_; }

    // Queue all photos that have SP but haven't been face-scanned yet
    int queueAllMissingFaceDetections() {
        if (!faceModelsReady_) return 0;

        vector<string> ids;
        for (const auto& [id, photo] : photos_) {
            if (photo.isVideo) continue;
            if (photo.faceScanned) continue;
            // Need SP to exist
            if (photo.localSmartPreviewPath.empty() || !fs::exists(photo.localSmartPreviewPath)) continue;
            ids.push_back(id);
        }

        if (ids.empty()) return 0;

        {
            lock_guard<mutex> lock(faceMutex_);
            pendingFaceDetections_ = std::move(ids);
            faceTotalCount_ = (int)pendingFaceDetections_.size();
            faceCompletedCount_ = 0;
        }

        startFaceDetectionThread();
        logNotice() << "[FaceDetection] Queued " << faceTotalCount_.load() << " photos";
        return faceTotalCount_;
    }

    // Process completed face detection results (call from main thread)
    int processFaceDetectionResults() {
        vector<FaceDetResult> results;
        {
            lock_guard<mutex> lock(faceMutex_);
            if (completedFaceDetections_.empty()) return 0;
            results.swap(completedFaceDetections_);
        }

        int totalInserted = 0;
        for (auto& result : results) {
            auto* photo = getPhoto(result.photoId);

            // No faces found — just mark as scanned
            if (result.faces.empty()) {
                db_.updateFaceScanned(result.photoId, true);
                if (photo) photo->faceScanned = true;
                continue;
            }

            // Load existing Lightroom faces for this photo
            auto existingFaces = db_.getFacesForPhoto(result.photoId);
            if (!photo) continue;

            vector<PhotoDatabase::FaceRow> newRows;
            for (size_t i = 0; i < result.faces.size(); i++) {
                auto& det = result.faces[i];

                // Normalize bbox to 0-1 (det coords are already normalized by SP dimensions)
                float nx = det.x1, ny = det.y1;
                float nw = det.x2 - det.x1, nh = det.y2 - det.y1;

                // Try to match with existing Lightroom faces by overlap
                int matchedPersonId = 0;
                float bestOverlap = 0;
                for (const auto& existing : existingFaces) {
                    if (existing.source != "lightroom") continue;
                    float overlap = faceOverlap(existing, nx, ny, nw, nh);
                    if (overlap > bestOverlap && overlap > 0.5f) {
                        bestOverlap = overlap;
                        matchedPersonId = existing.personId;
                    }
                }

                PhotoDatabase::FaceRow row;
                row.photoId = result.photoId;
                row.personId = matchedPersonId;
                row.x = nx;
                row.y = ny;
                row.w = nw;
                row.h = nh;
                row.source = "insightface";
                if (i < result.embeddings.size()) {
                    row.embedding = std::move(result.embeddings[i]);
                }
                newRows.push_back(std::move(row));
            }

            int inserted = db_.insertFaces(newRows);
            totalInserted += inserted;

            // Mark photo as face-scanned
            db_.updateFaceScanned(result.photoId, true);
            if (photo) photo->faceScanned = true;
        }

        if (totalInserted > 0) {
            loadFaceCache();
            loadFaceEmbeddingCache();
        }

        return totalInserted;
    }

    bool isFaceDetectionRunning() const { return faceThreadRunning_; }
    int getFaceDetectionTotalCount() const { return faceTotalCount_; }
    int getFaceDetectionCompletedCount() const { return faceCompletedCount_; }

    // --- Smart Preview ---

    // Compute smart preview path for a photo
    string smartPreviewPath(const PhotoEntry& photo) const {
        if (smartPreviewDir_.empty()) return "";
        string subdir = dateToSubdir(photo.dateTimeOriginal, photo.localPath);
        return smartPreviewDir_ + "/" + subdir + "/" + photo.id + ".jxl";
    }

    // Generate smart preview from F32 pixels (call after RAW decode)
    bool generateSmartPreview(const string& id, const Pixels& rawPixelsF32) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        auto& photo = it->second;

        string spPath = smartPreviewPath(photo);
        if (spPath.empty()) return false;

        if (SmartPreview::encode(rawPixelsF32, spPath)) {
            photo.localSmartPreviewPath = spPath;
            db_.updateSmartPreviewPath(id, spPath);
            return true;
        }
        return false;
    }

    // Load smart preview to F32 pixels
    bool loadSmartPreview(const string& id, Pixels& outF32) {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        auto& photo = it->second;

        if (photo.localSmartPreviewPath.empty() || !fs::exists(photo.localSmartPreviewPath)) {
            return false;
        }
        return SmartPreview::decode(photo.localSmartPreviewPath, outF32);
    }

    // Check if photo has a smart preview
    bool hasSmartPreview(const string& id) const {
        auto it = photos_.find(id);
        if (it == photos_.end()) return false;
        return !it->second.localSmartPreviewPath.empty() &&
               fs::exists(it->second.localSmartPreviewPath);
    }

    // Queue photos for background SP generation (RAW + JPEG)
    void queueSmartPreviewGeneration(const vector<string>& ids) {
        lock_guard<mutex> lock(spMutex_);
        for (const auto& id : ids) {
            auto it = photos_.find(id);
            if (it == photos_.end()) continue;
            auto& photo = it->second;
            if (photo.isVideo) continue;
            if (!photo.localSmartPreviewPath.empty() && fs::exists(photo.localSmartPreviewPath)) continue;
            if (photo.localPath.empty() || !fs::exists(photo.localPath)) continue;
            pendingSPGenerations_.push_back(id);
        }
        startSPGenerationThread();
    }

    // Queue all photos without smart preview (RAW + JPEG)
    int queueAllMissingSP() {
        vector<string> ids;
        for (const auto& [id, photo] : photos_) {
            if (photo.isVideo) continue;
            if (!photo.localSmartPreviewPath.empty() && fs::exists(photo.localSmartPreviewPath)) continue;
            if (photo.localPath.empty() || !fs::exists(photo.localPath)) continue;
            ids.push_back(id);
        }
        if (!ids.empty()) {
            queueSmartPreviewGeneration(ids);
        }
        return (int)ids.size();
    }

    // Process completed SP generation results (call from main thread)
    void processSPResults() {
        lock_guard<mutex> lock(spMutex_);
        for (const auto& result : completedSPGenerations_) {
            auto it = photos_.find(result.photoId);
            if (it != photos_.end() && !result.spPath.empty()) {
                it->second.localSmartPreviewPath = result.spPath;
                db_.updateSmartPreviewPath(result.photoId, result.spPath);
            }
        }
        completedSPGenerations_.clear();
    }

    bool isSPGenerationRunning() const { return spThreadRunning_; }
    int getSPPendingCount() const {
        lock_guard<mutex> lock(spMutex_);
        return (int)pendingSPGenerations_.size();
    }
    int getSPCompletedCount() const { return spCompletedCount_; }
    int getSPTotalCount() const { return spTotalCount_; }

    // --- EXIF Backfill (v9 fields) ---

    int queueAllMissingExifData() {
        if (exifBackfillRunning_) return 0;
        vector<string> ids;
        for (const auto& [id, photo] : photos_) {
            if (photo.localPath.empty() || !fs::exists(photo.localPath)) continue;
            // Queue if v9 fields are all at defaults (never extracted)
            bool needsV9 = photo.lensCorrectionParams.empty() &&
                           photo.exposureTime.empty() &&
                           photo.orientation == 1 && photo.focalLength35mm == 0;
            // Also queue if basic metadata is missing (e.g. DNG dimension issue)
            bool needsBasic = (photo.width == 0 || photo.height == 0);
            // RAW files with no lens correction data may need re-extraction
            // (e.g. new format support added: Fuji, etc.)
            bool needsLensCorr = photo.isRaw && photo.lensCorrectionParams.empty();
            if (needsV9 || needsBasic || needsLensCorr) {
                ids.push_back(id);
            }
        }
        if (!ids.empty()) {
            startExifBackfillThread(ids);
        }
        return (int)ids.size();
    }

    void processExifBackfillResults() {
        lock_guard<mutex> lock(exifBackfillMutex_);
        for (const auto& result : completedExifBackfills_) {
            auto it = photos_.find(result.photoId);
            if (it == photos_.end()) continue;
            auto& photo = it->second;
            const auto& r = result.updatedFields;

            // Basic metadata: only overwrite if currently missing
            if (photo.width == 0 && r.width > 0) photo.width = r.width;
            if (photo.height == 0 && r.height > 0) photo.height = r.height;
            if (photo.cameraMake.empty() && !r.cameraMake.empty()) photo.cameraMake = r.cameraMake;
            if (photo.camera.empty() && !r.camera.empty()) photo.camera = r.camera;
            if (photo.lens.empty() && !r.lens.empty()) photo.lens = r.lens;
            if (photo.focalLength == 0 && r.focalLength > 0) photo.focalLength = r.focalLength;
            if (photo.aperture == 0 && r.aperture > 0) photo.aperture = r.aperture;
            if (photo.iso == 0 && r.iso > 0) photo.iso = r.iso;
            if (photo.dateTimeOriginal.empty() && !r.dateTimeOriginal.empty()) photo.dateTimeOriginal = r.dateTimeOriginal;
            if (photo.creativeStyle.empty() && !r.creativeStyle.empty()) photo.creativeStyle = r.creativeStyle;

            // v9 fields: always overwrite (backfill is the authority)
            photo.lensCorrectionParams = r.lensCorrectionParams;
            photo.exposureTime = r.exposureTime;
            photo.exposureBias = r.exposureBias;
            photo.orientation = r.orientation;
            photo.whiteBalance = r.whiteBalance;
            photo.focalLength35mm = r.focalLength35mm;
            photo.offsetTime = r.offsetTime;
            photo.bodySerial = r.bodySerial;
            photo.lensSerial = r.lensSerial;
            photo.subjectDistance = r.subjectDistance;
            photo.subsecTimeOriginal = r.subsecTimeOriginal;
            photo.companionFiles = r.companionFiles;
            db_.updateExifData(photo);
        }
        completedExifBackfills_.clear();
    }

    bool isExifBackfillRunning() const { return exifBackfillRunning_; }

    // Backfill develop parameters from LR develop_settings text blob.
    // Targets photos with non-empty developSettings but all 7 tone/color params at 0.
    int backfillDevelopSettings() {
        int count = 0;
        for (auto& [id, photo] : photos_) {
            if (photo.developSettings.empty()) continue;
            // Skip if any param is already non-zero (already parsed)
            if (photo.devContrast != 0 || photo.devHighlights != 0 || photo.devShadows != 0 ||
                photo.devWhites != 0 || photo.devBlacks != 0 ||
                photo.devVibrance != 0 || photo.devSaturation != 0) continue;

            LrcatImporter::parseDevelopSettings(photo.developSettings, photo);

            // Check if anything was actually extracted
            if (photo.devContrast != 0 || photo.devHighlights != 0 || photo.devShadows != 0 ||
                photo.devWhites != 0 || photo.devBlacks != 0 ||
                photo.devVibrance != 0 || photo.devSaturation != 0 ||
                photo.devExposure != 0) {
                db_.updateDevelop(id, photo.devExposure, photo.devWbTemp, photo.devWbTint,
                                  photo.devContrast, photo.devHighlights, photo.devShadows,
                                  photo.devWhites, photo.devBlacks,
                                  photo.devVibrance, photo.devSaturation,
                                  photo.chromaDenoise, photo.lumaDenoise);
                count++;
            }
        }
        if (count > 0) {
            logNotice() << "[DevelopBackfill] Updated " << count << " photos";
        }
        return count;
    }

    // --- CLIP Embedding ---

    // Initialize CLIP embedder + text encoder in background
    void initEmbedder(const string& modelsDir) {
        // Eagerly create shared Ort::Env on main thread before background threads
        // (ONNX Runtime's Env init is not thread-safe)
        getSharedOrtEnv();
        clipEmbedder_.loadAsync(modelsDir);
        textEncoder_.loadAsync(modelsDir);
    }

    bool isEmbedderReady() const { return clipEmbedder_.isReady(); }
    bool isEmbedderInitializing() const { return clipEmbedder_.isInitializing(); }
    const string& getEmbedderStatus() const { return clipEmbedder_.getStatusText(); }
    bool isTextEncoderReady() const { return textEncoder_.isReady(); }

    // Unload vision model to free ~340MB memory (after all embeddings generated)
    void unloadVisionModel() {
        clipEmbedder_.unload();
    }

    // Load all image embeddings from DB into memory cache
    void loadEmbeddingCache() {
        auto ids = getSortedIds();
        int loaded = 0;
        for (const auto& id : ids) {
            auto vec = db_.getEmbedding(id, clipEmbedder_.MODEL_NAME);
            if (!vec.empty()) {
                embeddingCache_[id] = std::move(vec);
                loaded++;
            }
        }
        logNotice() << "[EmbeddingCache] Loaded " << loaded << " embeddings";
    }

    // Search result struct
    struct SearchResult {
        string photoId;
        float score;  // cosine similarity
    };

    // Get cached embedding (nullptr if not available)
    const vector<float>* getCachedEmbedding(const string& id) const {
        auto it = embeddingCache_.find(id);
        return it != embeddingCache_.end() ? &it->second : nullptr;
    }

    // Find top-N similar photos by CLIP embedding
    vector<SearchResult> findSimilar(const string& id, int topN = 20) const {
        auto* ref = getCachedEmbedding(id);
        if (!ref) return {};
        vector<SearchResult> results;
        for (const auto& [otherId, otherEmb] : embeddingCache_) {
            if (otherId == id) continue;
            float score = cosineSimilarity(*ref, otherEmb);
            results.push_back({otherId, score});
        }
        if ((int)results.size() > topN) {
            partial_sort(results.begin(), results.begin() + topN, results.end(),
                [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
            results.resize(topN);
        } else {
            sort(results.begin(), results.end(),
                [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
        }
        return results;
    }

    // Haversine distance between two GPS coordinates (km)
    static double haversine(double lat1, double lon1, double lat2, double lon2) {
        double R = 6371.0;
        double dLat = (lat2 - lat1) * M_PI / 180.0;
        double dLon = (lon2 - lon1) * M_PI / 180.0;
        double a = sin(dLat / 2) * sin(dLat / 2) +
                   cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
                   sin(dLon / 2) * sin(dLon / 2);
        return R * 2 * atan2(sqrt(a), sqrt(1 - a));
    }

    // Find nearby photos by GPS distance (within maxKm), sorted by distance
    vector<SearchResult> findNearby(const string& id, double maxKm = 5.0, int limit = 20) const {
        auto it = photos_.find(id);
        if (it == photos_.end() || !it->second.hasGps()) return {};
        const auto& ref = it->second;

        vector<pair<string, double>> nearby;
        for (const auto& [otherId, entry] : photos_) {
            if (otherId == id || !entry.hasGps()) continue;
            double dist = haversine(ref.latitude, ref.longitude,
                                    entry.latitude, entry.longitude);
            if (dist <= maxKm) {
                nearby.push_back({otherId, dist});
            }
        }

        sort(nearby.begin(), nearby.end(),
             [](const auto& a, const auto& b) { return a.second < b.second; });
        if ((int)nearby.size() > limit) nearby.resize(limit);

        // Convert distance to score: closer = higher (0~1 range)
        vector<SearchResult> results;
        for (const auto& [pid, dist] : nearby) {
            float score = 1.0f / (1.0f + (float)dist / 2.0f);
            results.push_back({pid, score});
        }
        return results;
    }

    // Semantic search: text query → sorted results (descending by similarity)
    // Uses dynamic threshold: keeps items within 15% of top score,
    // but if spread is tiny (< 0.03) returns all sorted by relevance.
    vector<SearchResult> searchByText(const string& query) {
        if (!textEncoder_.isReady()) return {};

        // Encode text query
        auto textEmb = textEncoder_.encode(query);
        if (textEmb.empty()) return {};

        // Compare with all cached image embeddings
        vector<SearchResult> all;
        all.reserve(embeddingCache_.size());
        for (const auto& [id, imgEmb] : embeddingCache_) {
            float score = cosineSimilarity(textEmb, imgEmb);
            all.push_back({id, score});
        }

        if (all.empty()) return {};

        // Sort by score descending
        sort(all.begin(), all.end(),
             [](const SearchResult& a, const SearchResult& b) {
                 return a.score > b.score;
             });

        float topScore = all.front().score;
        float botScore = all.back().score;
        float spread = topScore - botScore;

        // Dynamic filtering: spread-based cutoff (model-agnostic)
        // If scores are well-spread, keep only the top cluster.
        // If scores are clustered (no clear match), return all sorted.
        vector<SearchResult> results;
        if (spread > 0.03f) {
            float cutoff = topScore - spread * 0.35f;
            for (const auto& r : all) {
                if (r.score >= cutoff) results.push_back(r);
            }
        } else {
            // Scores too clustered — return all, sorted by relevance
            results = std::move(all);
        }

        logNotice() << "[Search] query=\"" << query
                    << "\" results: " << results.size()
                    << "/" << embeddingCache_.size()
                    << " top=" << topScore << " spread=" << spread;
        return results;
    }

    // Queue all photos that don't have embeddings yet
    int queueAllMissingEmbeddings() {
        if (!clipEmbedder_.isReady()) return 0;
        auto ids = db_.getPhotosWithoutEmbedding(clipEmbedder_.MODEL_NAME);
        if (ids.empty()) return 0;

        lock_guard<mutex> lock(embMutex_);
        for (const auto& id : ids) {
            auto it = photos_.find(id);
            if (it != photos_.end() && it->second.isVideo) continue;
            pendingEmbeddings_.push_back(id);
        }
        startEmbeddingThread();
        return (int)ids.size();
    }

    // Queue specific photos for embedding
    void queueEmbeddings(const vector<string>& ids) {
        if (!clipEmbedder_.isReady() || ids.empty()) return;
        lock_guard<mutex> lock(embMutex_);
        for (const auto& id : ids) {
            if (!db_.hasEmbedding(id, clipEmbedder_.MODEL_NAME)) {
                pendingEmbeddings_.push_back(id);
            }
        }
        startEmbeddingThread();
    }

    // Process completed embeddings (call from main thread)
    int processEmbeddingResults() {
        lock_guard<mutex> lock(embMutex_);
        int count = (int)completedEmbeddings_.size();
        for (const auto& result : completedEmbeddings_) {
            db_.insertEmbedding(result.photoId, clipEmbedder_.MODEL_NAME,
                                "image", result.embedding);
            // Update in-memory cache
            embeddingCache_[result.photoId] = result.embedding;
        }
        completedEmbeddings_.clear();
        return count;
    }

    bool isEmbeddingRunning() const { return embThreadRunning_; }
    int getEmbeddingTotalCount() const { return embTotalCount_; }
    int getEmbeddingCompletedCount() const { return embCompletedCount_; }

    // Process completed file copies (call from main thread in update)
    void processCopyResults() {
        lock_guard<mutex> lock(copyMutex_);
        for (const auto& result : completedCopies_) {
            auto it = photos_.find(result.photoId);
            if (it != photos_.end() && !result.destPath.empty()) {
                it->second.localPath = result.destPath;
                db_.updateLocalPath(result.photoId, result.destPath);
            }
        }
        completedCopies_.clear();
    }

    bool hasPendingCopies() const {
        lock_guard<mutex> lock(copyMutex_);
        return !pendingCopies_.empty() || copyThreadRunning_;
    }

    // --- Library consolidation ---

    // Move all files into date-based directory structure (background)
    void consolidateLibrary() {
        if (consolidateRunning_) {
            logWarning() << "[Consolidate] Already running";
            return;
        }
        if (rawStoragePath_.empty()) {
            logWarning() << "[Consolidate] No library folder configured";
            return;
        }

        // Build task list on main thread
        vector<ConsolidateTask> tasks;
        fs::path libPath = fs::path(rawStoragePath_);

        for (auto& [id, photo] : photos_) {
            if (photo.localPath.empty() || !fs::exists(photo.localPath)) continue;

            // Re-extract EXIF if dateTimeOriginal is missing
            if (photo.dateTimeOriginal.empty()) {
                extractExifMetadata(photo.localPath, photo);
                db_.updatePhoto(photo);
            }

            string subdir = dateToSubdir(photo.dateTimeOriginal, photo.localPath);

            // Check if RAW file needs moving
            fs::path expectedDir = libPath / subdir;
            fs::path currentDir = fs::path(photo.localPath).parent_path();
            string newRawPath;
            string oldRawPath = photo.localPath;

            bool needsRawMove = false;
            try {
                needsRawMove = (fs::canonical(currentDir) != fs::canonical(expectedDir));
            } catch (...) {
                needsRawMove = (currentDir != expectedDir);
            }

            if (needsRawMove) {
                fs::create_directories(expectedDir);
                fs::path dest = resolveDestPath(expectedDir, photo.filename);
                newRawPath = dest.string();
            }

            // Check if thumbnail needs moving
            string newThumbPath;
            string oldThumbPath = photo.localThumbnailPath;
            if (!photo.localThumbnailPath.empty() && fs::exists(photo.localThumbnailPath) &&
                !thumbnailCacheDir_.empty()) {
                fs::path expectedThumbDir = fs::path(thumbnailCacheDir_) / subdir;
                fs::path currentThumbDir = fs::path(photo.localThumbnailPath).parent_path();

                bool needsThumbMove = false;
                try {
                    needsThumbMove = (fs::canonical(currentThumbDir) != fs::canonical(expectedThumbDir));
                } catch (...) {
                    needsThumbMove = (currentThumbDir != expectedThumbDir);
                }

                if (needsThumbMove) {
                    fs::create_directories(expectedThumbDir);
                    string thumbFilename = fs::path(photo.localThumbnailPath).filename().string();
                    newThumbPath = (expectedThumbDir / thumbFilename).string();
                }
            }

            if (!newRawPath.empty() || !newThumbPath.empty()) {
                tasks.push_back({
                    id,
                    newRawPath.empty() ? "" : oldRawPath,
                    newRawPath,
                    newThumbPath.empty() ? "" : oldThumbPath,
                    newThumbPath
                });
            }
        }

        if (tasks.empty()) {
            logNotice() << "[Consolidate] All files already in correct location";
            return;
        }

        consolidateTotal_ = (int)tasks.size();
        consolidateProgress_ = 0;
        consolidateRunning_ = true;

        if (consolidateThread_.joinable()) consolidateThread_.join();

        consolidateThread_ = thread([this, tasks = std::move(tasks)]() {
            int progress = 0;
            for (const auto& task : tasks) {
                bool rawOk = true;
                bool thumbOk = true;

                // Move RAW file + XMP sidecar
                if (!task.oldPath.empty() && !task.newPath.empty()) {
                    try {
                        fs::rename(task.oldPath, task.newPath);
                    } catch (...) {
                        try {
                            fs::copy_file(task.oldPath, task.newPath);
                            fs::remove(task.oldPath);
                        } catch (const exception& e) {
                            logWarning() << "[Consolidate] Failed: " << task.oldPath << " -> " << e.what();
                            rawOk = false;
                        }
                    }
                    // Move XMP sidecar alongside RAW
                    if (rawOk) {
                        string oldXmp = xmpWritePath(task.oldPath);
                        string newXmp = xmpWritePath(task.newPath);
                        if (!oldXmp.empty() && fs::exists(oldXmp)) {
                            try { fs::rename(oldXmp, newXmp); } catch (...) {
                                try { fs::copy_file(oldXmp, newXmp); fs::remove(oldXmp); } catch (...) {}
                            }
                        }
                    }
                }

                // Move thumbnail
                if (!task.oldThumbnailPath.empty() && !task.newThumbnailPath.empty()) {
                    try {
                        fs::rename(task.oldThumbnailPath, task.newThumbnailPath);
                    } catch (...) {
                        try {
                            fs::copy_file(task.oldThumbnailPath, task.newThumbnailPath);
                            fs::remove(task.oldThumbnailPath);
                        } catch (const exception& e) {
                            logWarning() << "[Consolidate] Thumb failed: " << e.what();
                            thumbOk = false;
                        }
                    }
                }

                if (rawOk || thumbOk) {
                    ConsolidateTask completed = task;
                    if (!rawOk) { completed.oldPath.clear(); completed.newPath.clear(); }
                    if (!thumbOk) { completed.oldThumbnailPath.clear(); completed.newThumbnailPath.clear(); }
                    lock_guard<mutex> lock(consolidateMutex_);
                    completedConsolidates_.push_back(completed);
                }

                consolidateProgress_ = ++progress;
            }

            consolidateRunning_ = false;
            logNotice() << "[Consolidate] Done: " << progress << " files processed";
        });
    }

    // Process completed consolidation results (call from main thread)
    void processConsolidateResults() {
        lock_guard<mutex> lock(consolidateMutex_);
        for (const auto& result : completedConsolidates_) {
            auto it = photos_.find(result.photoId);
            if (it == photos_.end()) continue;

            bool changed = false;
            if (!result.newPath.empty()) {
                it->second.localPath = result.newPath;
                changed = true;
            }
            if (!result.newThumbnailPath.empty()) {
                it->second.localThumbnailPath = result.newThumbnailPath;
                changed = true;
            }
            if (changed) {
                db_.updateLocalAndThumbnailPaths(result.photoId,
                    it->second.localPath, it->second.localThumbnailPath);
            }
        }
        completedConsolidates_.clear();
    }

    bool isConsolidateRunning() const { return consolidateRunning_; }
    int getConsolidateTotal() const { return consolidateTotal_; }
    int getConsolidateProgress() const { return consolidateProgress_; }
    void joinConsolidate() {
        if (consolidateThread_.joinable()) consolidateThread_.join();
    }

    // Graceful shutdown: signal all threads to stop, then join
    void shutdown() {
        stopping_ = true;
        // Wake up any waiting preprocess/inference threads
        prepQueueCV_.notify_all();
        prepQueueSpaceCV_.notify_all();
        for (auto& t : prepThreads_) {
            if (t.joinable()) t.join();
        }
        prepThreads_.clear();
        for (auto& t : embThreads_) {
            if (t.joinable()) t.join();
        }
        embThreads_.clear();
        if (faceThread_.joinable()) faceThread_.join();
        for (auto& t : spThreads_) {
            if (t.joinable()) t.join();
        }
        spThreads_.clear();
        if (copyThread_.joinable()) copyThread_.join();
        if (consolidateThread_.joinable()) consolidateThread_.join();
        if (exifBackfillThread_.joinable()) exifBackfillThread_.join();
    }

private:
    void rebuildStackIndex() {
        stackIndex_.clear();
        for (auto& [id, photo] : photos_) {
            if (!photo.stackId.empty()) {
                stackIndex_[photo.stackId].push_back(id);
            }
        }
    }

    // Load person id->name mapping from DB
    unordered_map<int, string> loadPersonIdToName() {
        return db_.loadPersonIdToName();
    }

    HttpClient client_;
    PhotoDatabase db_;
    unordered_map<string, PhotoEntry> photos_;
    unordered_map<string, vector<string>> stackIndex_; // stackId -> member ids
    vector<Collection> collections_;
    string catalogDir_;
    string thumbnailCacheDir_;
    string databasePath_;
    string jsonMigrationPath_;
    string rawStoragePath_;
    string smartPreviewDir_;
    string pendingDir_;
    bool serverReachable_ = false;
    bool serverChecked_ = false;
    atomic<bool> stopping_{false};

    // Background file copy
    struct CopyTask {
        string photoId;
        string srcPath;
        string destPath;
    };

    mutable mutex copyMutex_;
    vector<CopyTask> pendingCopies_;
    vector<CopyTask> completedCopies_;
    bool copyThreadRunning_ = false;
    thread copyThread_;

    // Background consolidation
    struct ConsolidateTask {
        string photoId;
        string oldPath;
        string newPath;
        string oldThumbnailPath;
        string newThumbnailPath;
    };

    mutable mutex consolidateMutex_;
    vector<ConsolidateTask> completedConsolidates_;
    atomic<bool> consolidateRunning_{false};
    atomic<int> consolidateTotal_{0};
    atomic<int> consolidateProgress_{0};
    thread consolidateThread_;

    // Background SP generation
    struct SPResult {
        string photoId;
        string spPath;
    };

    mutable mutex spMutex_;
    vector<string> pendingSPGenerations_;
    vector<SPResult> completedSPGenerations_;
    atomic<bool> spThreadRunning_{false};
    atomic<int> spCompletedCount_{0};
    atomic<int> spTotalCount_{0};
    static constexpr int SP_WORKERS = 2;
    vector<thread> spThreads_;

    // EXIF backfill
    struct ExifBackfillResult {
        string photoId;
        PhotoEntry updatedFields;  // Only v9 fields populated
    };
    mutable mutex exifBackfillMutex_;
    vector<ExifBackfillResult> completedExifBackfills_;
    atomic<bool> exifBackfillRunning_{false};
    thread exifBackfillThread_;
    static constexpr int EXIF_BACKFILL_WORKERS = 2;

    // CLIP embedding
    ClipEmbedder clipEmbedder_;
    ClipTextEncoder textEncoder_;
    unordered_map<string, vector<float>> embeddingCache_;

    // Face name cache (photo_id -> person names)
    unordered_map<string, vector<string>> faceNameCache_;

    // Face detection pipeline
    FaceDetector faceDetector_;
    FaceRecognizer faceRecognizer_;
    bool faceModelsReady_ = false;

    struct FaceDetResult {
        string photoId;
        vector<DetectedFace> faces;
        vector<vector<float>> embeddings;
    };

    mutable mutex faceMutex_;
    vector<string> pendingFaceDetections_;
    vector<FaceDetResult> completedFaceDetections_;
    atomic<bool> faceThreadRunning_{false};
    atomic<int> faceCompletedCount_{0};
    atomic<int> faceTotalCount_{0};
    thread faceThread_;

    // Face embedding cache (face DB id → embedding)
    unordered_map<int, vector<float>> faceEmbeddingCache_;

    static float cosineSimilarity(const vector<float>& a, const vector<float>& b) {
        if (a.size() != b.size() || a.empty()) return 0;
        float dot = 0;
#ifdef __APPLE__
        vDSP_dotpr(a.data(), 1, b.data(), 1, &dot, (vDSP_Length)a.size());
#else
        for (size_t i = 0; i < a.size(); i++) dot += a[i] * b[i];
#endif
        // Both vectors are already L2-normalized, so dot product = cosine similarity
        return dot;
    }

    struct EmbeddingResult {
        string photoId;
        vector<float> embedding;
    };

    // Preprocessed tensor ready for inference
    struct PreparedTensor {
        string photoId;
        vector<float> tensor;
    };

    mutable mutex embMutex_;
    vector<string> pendingEmbeddings_;
    vector<EmbeddingResult> completedEmbeddings_;
    atomic<bool> embThreadRunning_{false};
    atomic<int> embCompletedCount_{0};
    atomic<int> embTotalCount_{0};
    vector<thread> embThreads_;
    vector<thread> prepThreads_;

    // Queue between preprocess workers and inference thread
    mutex prepQueueMutex_;
    condition_variable prepQueueCV_;
    deque<PreparedTensor> prepQueue_;
    atomic<int> prepDoneCount_{0};  // how many preprocess workers have finished
    int prepWorkerCount_ = 0;
    static constexpr int PREP_QUEUE_MAX = 32;    // limit memory usage
    static constexpr int INFER_THREAD_COUNT = 4; // parallel inference threads
    atomic<int> inferThreadsRunning_{0};          // track active inference threads
    condition_variable prepQueueSpaceCV_;       // signal when queue has space

    void startEmbeddingThread() {
        // Called with embMutex_ already held
        if (pendingEmbeddings_.empty() || embThreadRunning_) return;

        embThreadRunning_ = true;
        embCompletedCount_ = 0;
        for (auto& t : embThreads_) {
            if (t.joinable()) t.join();
        }
        embThreads_.clear();
        for (auto& t : prepThreads_) {
            if (t.joinable()) t.join();
        }
        prepThreads_.clear();

        vector<string> ids = std::move(pendingEmbeddings_);
        pendingEmbeddings_.clear();
        embTotalCount_ = (int)ids.size();

        // Clear prep queue state
        {
            lock_guard<mutex> lock(prepQueueMutex_);
            prepQueue_.clear();
            prepDoneCount_ = 0;
        }

        // Split IDs among preprocess workers
        const int workerCount = min(8, max(1, (int)ids.size()));
        prepWorkerCount_ = workerCount;
        int chunkSize = ((int)ids.size() + workerCount - 1) / workerCount;

        for (int w = 0; w < workerCount; w++) {
            int start = w * chunkSize;
            int end = min(start + chunkSize, (int)ids.size());
            if (start >= end) {
                prepDoneCount_++;
                continue;
            }

            vector<string> chunk(ids.begin() + start, ids.begin() + end);
            prepThreads_.emplace_back([this, chunk = std::move(chunk)]() {
                int skipped = 0;
                for (const auto& id : chunk) {
                    if (stopping_) break;

                    // Load thumbnail
                    auto it = photos_.find(id);
                    if (it == photos_.end()) continue;
                    auto& photo = it->second;

                    Pixels thumbPixels;
                    string thumbPath = photo.localThumbnailPath;
                    bool loaded = false;

                    if (!thumbPath.empty() && fs::exists(thumbPath)) {
                        loaded = thumbPixels.load(thumbPath);
                    }
                    if (!loaded) {
                        loaded = getThumbnail(id, thumbPixels);
                    }
                    if (!loaded) { skipped++; continue; }

                    // Preprocess to float tensor (thread-safe)
                    auto tensor = clipEmbedder_.preprocessPixels(thumbPixels);
                    if (tensor.empty()) continue;

                    // Push to inference queue (bounded)
                    {
                        unique_lock<mutex> lock(prepQueueMutex_);
                        prepQueueSpaceCV_.wait(lock, [this]() {
                            return (int)prepQueue_.size() < PREP_QUEUE_MAX || stopping_;
                        });
                        if (stopping_) break;
                        prepQueue_.push_back({id, std::move(tensor)});
                    }
                    prepQueueCV_.notify_one();
                }
                if (skipped > 0) {
                    logNotice() << "[CLIP] Preprocess worker skipped " << skipped << " (no thumbnail)";
                }
                prepDoneCount_++;
                prepQueueCV_.notify_one();  // wake inference thread to check done
            });
        }

        // Inference threads: parallel ONNX inference (Session::Run is thread-safe)
        logNotice() << "[CLIP] Pipeline started: "
                    << prepWorkerCount_ << " preprocess workers + "
                    << INFER_THREAD_COUNT << " inference threads";

        inferThreadsRunning_ = INFER_THREAD_COUNT;
        for (int t = 0; t < INFER_THREAD_COUNT; t++) {
            embThreads_.emplace_back([this]() {
                while (true) {
                    PreparedTensor item;
                    {
                        unique_lock<mutex> lock(prepQueueMutex_);
                        prepQueueCV_.wait(lock, [this]() {
                            return !prepQueue_.empty() || (prepDoneCount_ >= prepWorkerCount_) || stopping_;
                        });
                        if (stopping_) break;
                        if (prepQueue_.empty()) {
                            if (prepDoneCount_ >= prepWorkerCount_) break;
                            continue;
                        }
                        item = std::move(prepQueue_.front());
                        prepQueue_.pop_front();
                    }
                    prepQueueSpaceCV_.notify_one();

                    auto embedding = clipEmbedder_.infer(item.tensor);
                    if (embedding.empty()) continue;

                    {
                        lock_guard<mutex> lock(embMutex_);
                        completedEmbeddings_.push_back({item.photoId, std::move(embedding)});
                    }
                    embCompletedCount_++;
                }
                if (--inferThreadsRunning_ == 0) {
                    logNotice() << "[CLIP] Embedding done: " << embCompletedCount_.load()
                                << "/" << embTotalCount_.load();
                    embThreadRunning_ = false;
                }
            });
        }
    }

    // Face overlap: ratio of intersection to smaller face area
    static float faceOverlap(const PhotoDatabase::FaceRow& a,
                             float bx, float by, float bw, float bh) {
        float ix1 = max(a.x, bx);
        float iy1 = max(a.y, by);
        float ix2 = min(a.x + a.w, bx + bw);
        float iy2 = min(a.y + a.h, by + bh);
        float iw = max(0.0f, ix2 - ix1);
        float ih = max(0.0f, iy2 - iy1);
        float inter = iw * ih;
        float areaA = a.w * a.h;
        float areaB = bw * bh;
        float smaller = min(areaA, areaB);
        return smaller > 0 ? inter / smaller : 0;
    }

    void startFaceDetectionThread() {
        lock_guard<mutex> lock(faceMutex_);
        if (pendingFaceDetections_.empty() || faceThreadRunning_) return;

        faceThreadRunning_ = true;
        if (faceThread_.joinable()) faceThread_.join();

        // Collect SP paths on main thread to avoid data race on photos_
        struct FaceJob { string id; string spPath; };
        vector<FaceJob> jobs;
        jobs.reserve(pendingFaceDetections_.size());
        for (const auto& id : pendingFaceDetections_) {
            auto it = photos_.find(id);
            if (it == photos_.end()) continue;
            jobs.push_back({id, it->second.localSmartPreviewPath});
        }
        pendingFaceDetections_.clear();

        faceThread_ = thread([this, jobs = std::move(jobs)]() {
            logNotice() << "[FaceDetection] Starting for " << jobs.size() << " photos";
            int done = 0;

            for (const auto& job : jobs) {
                if (stopping_) break;

                // Decode smart preview to get pixel data
                Pixels spF32;
                if (!SmartPreview::decode(job.spPath, spF32)) {
                    logWarning() << "[FaceDetection] Failed to decode SP: " << job.id;
                    faceCompletedCount_++;
                    done++;
                    continue;
                }

                int spW = spF32.getWidth();
                int spH = spF32.getHeight();
                int ch = spF32.getChannels();

                // Convert F32 → RGB uint8 for face detector
                vector<uint8_t> rgb(spW * spH * 3);
                const float* src = spF32.getDataF32();
                for (int i = 0; i < spW * spH; i++) {
                    for (int c = 0; c < 3; c++) {
                        int srcIdx = (ch == 4) ? (i * 4 + c) : (i * 3 + c);
                        rgb[i * 3 + c] = (uint8_t)(clamp(src[srcIdx], 0.0f, 1.0f) * 255.0f);
                    }
                }

                // Detect faces
                auto faces = faceDetector_.detect(rgb.data(), spW, spH);

                FaceDetResult result;
                result.photoId = job.id;

                if (!faces.empty()) {
                    // Get embeddings for each face (using pixel-space coords before normalization)
                    vector<vector<float>> embeddings;
                    embeddings.reserve(faces.size());
                    for (auto& face : faces) {
                        auto emb = faceRecognizer_.getEmbedding(rgb.data(), spW, spH, face);
                        embeddings.push_back(std::move(emb));
                    }

                    // Normalize face coordinates to 0-1
                    for (auto& face : faces) {
                        face.normalize(spW, spH);
                    }

                    result.faces = std::move(faces);
                    result.embeddings = std::move(embeddings);
                }
                // Empty faces → result.faces stays empty, main thread sets faceScanned

                {
                    lock_guard<mutex> lk(faceMutex_);
                    completedFaceDetections_.push_back(std::move(result));
                }
                faceCompletedCount_++;
                done++;
            }

            logNotice() << "[FaceDetection] Done: " << done << "/" << jobs.size();
            faceThreadRunning_ = false;
        });
    }

    void startSPGenerationThread() {
        // Called with spMutex_ already held
        if (pendingSPGenerations_.empty() || spThreadRunning_) return;

        spThreadRunning_ = true;
        for (auto& t : spThreads_) {
            if (t.joinable()) t.join();
        }
        spThreads_.clear();

        // Pre-collect paths on main thread (avoid data race on photos_)
        struct SPJob { string id; string localPath; string spPath; bool isRaw; };
        auto jobs = make_shared<vector<SPJob>>();
        jobs->reserve(pendingSPGenerations_.size());
        for (const auto& id : pendingSPGenerations_) {
            auto it = photos_.find(id);
            if (it == photos_.end()) continue;
            string lp = it->second.localPath;
            string sp = smartPreviewPath(it->second);
            if (!lp.empty() && !sp.empty()) {
                jobs->push_back({id, std::move(lp), std::move(sp), it->second.isRaw});
            }
        }
        pendingSPGenerations_.clear();
        spTotalCount_ = (int)jobs->size();
        spCompletedCount_ = 0;

        auto workIdx = make_shared<atomic<int>>(0);
        auto doneCount = make_shared<atomic<int>>(0);
        int totalJobs = (int)jobs->size();

        logNotice() << "[SmartPreview] Starting generation for " << totalJobs
                    << " photos (" << SP_WORKERS << " workers)";

        auto finishedWorkers = make_shared<atomic<int>>(0);

        for (int w = 0; w < SP_WORKERS; w++) {
            spThreads_.emplace_back([this, jobs, workIdx, doneCount, totalJobs, finishedWorkers]() {
                while (!stopping_) {
                    int idx = workIdx->fetch_add(1);
                    if (idx >= totalJobs) break;

                    const auto& job = (*jobs)[idx];

                    if (!fs::exists(job.localPath)) {
                        spCompletedCount_++;
                        continue;
                    }

                    // Already exists?
                    if (fs::exists(job.spPath)) {
                        lock_guard<mutex> lock(spMutex_);
                        completedSPGenerations_.push_back({job.id, job.spPath});
                        (*doneCount)++;
                        spCompletedCount_++;
                        continue;
                    }

                    // Load image as F32 pixels
                    Pixels rawF32;
                    if (job.isRaw) {
                        // RAW: half resolution decode (skips demosaic, ~10x faster)
                        if (!RawLoader::loadFloatPreview(job.localPath, rawF32)) {
                            spCompletedCount_++;
                            continue;
                        }
                    } else {
                        // JPEG/other: load via stb_image (U8) and convert to F32
                        Pixels u8;
                        if (!u8.load(job.localPath)) {
                            spCompletedCount_++;
                            continue;
                        }
                        int w = u8.getWidth(), h = u8.getHeight(), ch = u8.getChannels();
                        rawF32.allocate(w, h, ch, PixelFormat::F32);
                        const unsigned char* src8 = u8.getData();
                        float* dst = rawF32.getDataF32();
                        for (int i = 0; i < w * h * ch; i++) {
                            dst[i] = src8[i] / 255.0f;
                        }
                    }

                    // Encode to JPEG XL (XYB float16)
                    if (SmartPreview::encode(rawF32, job.spPath)) {
                        lock_guard<mutex> lock(spMutex_);
                        completedSPGenerations_.push_back({job.id, job.spPath});
                        (*doneCount)++;
                    }
                    spCompletedCount_++;
                }

                // Last worker to finish marks pipeline done
                if (finishedWorkers->fetch_add(1) + 1 == SP_WORKERS) {
                    logNotice() << "[SmartPreview] Generation done: " << doneCount->load()
                                << "/" << jobs->size();
                    spThreadRunning_ = false;
                }
            });
        }
    }

    void startCopyThread() {
        lock_guard<mutex> lock(copyMutex_);
        if (pendingCopies_.empty() || copyThreadRunning_) return;

        copyThreadRunning_ = true;
        if (copyThread_.joinable()) copyThread_.join();

        // Take ownership of pending copies
        vector<CopyTask> tasks = std::move(pendingCopies_);
        pendingCopies_.clear();

        copyThread_ = thread([this, tasks = std::move(tasks)]() {
            for (const auto& task : tasks) {
                if (stopping_) break;

                if (fs::exists(task.destPath)) {
                    // Already exists, just record it
                    lock_guard<mutex> lock(copyMutex_);
                    completedCopies_.push_back(task);
                    continue;
                }

                try {
                    fs::copy_file(task.srcPath, task.destPath);
                    // Copy XMP sidecar if exists
                    string srcXmp = findXmpSidecar(task.srcPath);
                    if (!srcXmp.empty()) {
                        string destXmp = xmpWritePath(task.destPath);
                        if (!destXmp.empty() && !fs::exists(destXmp)) {
                            try { fs::copy_file(srcXmp, destXmp); } catch (...) {}
                        }
                    }
                    logNotice() << "[PhotoProvider] Copied: "
                                << fs::path(task.srcPath).filename().string();
                    lock_guard<mutex> lock(copyMutex_);
                    completedCopies_.push_back(task);
                } catch (const exception& e) {
                    logWarning() << "[PhotoProvider] Copy failed: " << e.what();
                }
            }

            lock_guard<mutex> lock(copyMutex_);
            copyThreadRunning_ = false;
        });
    }

    // Supported standard image extensions
    static inline const unordered_set<string> standardExtensions_ = {
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tga", ".psd", ".hdr",
        ".heic", ".heif"
    };

    // Supported video extensions
    static inline const unordered_set<string> videoExtensions_ = {
        ".mp4", ".mov", ".avi", ".mkv", ".mts", ".m2ts",
        ".mpg", ".mpeg", ".wmv", ".webm", ".m4v", ".3gp"
    };

    bool isSupportedImage(const fs::path& path) const {
        string ext = path.extension().string();
        transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (standardExtensions_.count(ext)) return true;
        if (RawLoader::isRawFile(path)) return true;
        return false;
    }

    static bool isVideoFile(const fs::path& path) {
        string ext = path.extension().string();
        transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return videoExtensions_.count(ext) > 0;
    }

    bool isSupportedFile(const fs::path& path) const {
        if (isSupportedImage(path)) return true;
        if (isVideoFile(path)) return true;
        return false;
    }

    void saveThumbnailCache(const string& id, PhotoEntry& photo, Pixels& pixels) {
        if (thumbnailCacheDir_.empty()) return;
        string subdir = dateToSubdir(photo.dateTimeOriginal, photo.localPath);
        string dir = thumbnailCacheDir_ + "/" + subdir;
        fs::create_directories(dir);
        string cachePath = dir + "/" + id + ".jpg";

        // Convert to 8-bit if float, or clone if already 8-bit
        Pixels savePixels;
        if (pixels.getFormat() == PixelFormat::F32) {
            savePixels.allocate(pixels.getWidth(), pixels.getHeight(), pixels.getChannels());
            const float* src = pixels.getDataF32();
            unsigned char* dst = savePixels.getData();
            int count = pixels.getWidth() * pixels.getHeight() * pixels.getChannels();
            for (int i = 0; i < count; i++) {
                dst[i] = (unsigned char)(clamp(src[i], 0.0f, 1.0f) * 255.0f);
            }
        } else {
            savePixels = pixels.clone();
        }

        // Save with low quality for small file size
        stbi_write_jpg(cachePath.c_str(),
            savePixels.getWidth(), savePixels.getHeight(),
            savePixels.getChannels(), savePixels.getData(),
            THUMBNAIL_JPEG_QUALITY);

        photo.localThumbnailPath = cachePath;
        db_.updateThumbnailPath(photo.id, cachePath);
    }

    // Parse XMP GPS string like "35,41.25894N" or "139,50.14254E" to decimal degrees
    // Lightroom writes GPS in deg,min.fracDir format
    static double parseXmpGpsCoord(const string& str) {
        if (str.empty()) return 0;

        // Extract direction letter (last char: N/S/E/W)
        char dir = str.back();
        string numPart = str.substr(0, str.size() - 1);

        // Split by comma: "35,41.25894"
        auto commaPos = numPart.find(',');
        if (commaPos == string::npos) return 0;

        double deg = 0, min = 0;
        try {
            deg = stod(numPart.substr(0, commaPos));
            min = stod(numPart.substr(commaPos + 1));
        } catch (...) {
            return 0;
        }

        double result = deg + min / 60.0;
        if (dir == 'S' || dir == 'W') result = -result;
        return result;
    }

    // Convert EXIF GPS DMS (Rational x3) to decimal degrees
    static double exifGpsToDecimal(const Exiv2::Value& value) {
        if (value.count() < 3) return 0;
        auto r0 = value.toRational(0);
        auto r1 = value.toRational(1);
        auto r2 = value.toRational(2);
        if (r0.second == 0 || r1.second == 0 || r2.second == 0) return 0;
        double deg = (double)r0.first / (double)r0.second;
        double min = (double)r1.first / (double)r1.second;
        double sec = (double)r2.first / (double)r2.second;
        return deg + min / 60.0 + sec / 3600.0;
    }

    // Parse Sigma MakerNote to extract color mode (tag 0x003d)
    // Sigma MakerNote: "SIGMA\0" (6B) + 4B unknown + IFD (little-endian)
    // IFD value offsets are TIFF-absolute, not MakerNote-relative.
    // We calculate the base offset from the min external valOff.
    static string extractSigmaColorMode(const Exiv2::ExifData& exif) {
        auto it = exif.findKey(Exiv2::ExifKey("Exif.Photo.MakerNote"));
        if (it == exif.end()) return "";

        auto& value = it->value();
        size_t size = value.size();
        if (size < 14) return "";

        vector<uint8_t> data(size);
        value.copy((Exiv2::byte*)data.data(), Exiv2::invalidByteOrder);

        if (memcmp(data.data(), "SIGMA", 5) != 0) return "";

        uint16_t numEntries = data[10] | (data[11] << 8);
        if (numEntries == 0 || numEntries > 500) return "";
        size_t ifdStart = 12;
        size_t ifdEnd = ifdStart + (size_t)numEntries * 12;
        if (ifdEnd > size) return "";

        // Calculate base offset: IFD value offsets are TIFF-absolute.
        // The data area starts right after the IFD entries at MakerNote byte ifdEnd.
        // Find the minimum external valOff to determine the TIFF base.
        uint32_t minExtVal = UINT32_MAX;
        static const uint8_t typeSizes[] = {0,1,1,2,4,8,1,1,2,4,8};
        for (int i = 0; i < numEntries; i++) {
            size_t off = ifdStart + i * 12;
            uint16_t typ = data[off + 2] | (data[off + 3] << 8);
            uint32_t cnt = data[off + 4] | (data[off + 5] << 8) |
                           (data[off + 6] << 16) | (data[off + 7] << 24);
            uint32_t ts = (typ < sizeof(typeSizes)) ? typeSizes[typ] : 1;
            if ((uint64_t)cnt * ts > 4) {
                uint32_t vo = data[off + 8] | (data[off + 9] << 8) |
                              (data[off + 10] << 16) | (data[off + 11] << 24);
                if (vo < minExtVal) minExtVal = vo;
            }
        }
        // base = minExtVal - ifdEnd (MakerNote's TIFF offset)
        uint32_t baseOff = (minExtVal != UINT32_MAX && minExtVal >= ifdEnd)
                           ? minExtVal - (uint32_t)ifdEnd : 0;

        // Now find tag 0x003d
        for (int i = 0; i < numEntries; i++) {
            size_t off = ifdStart + i * 12;
            uint16_t tag = data[off] | (data[off + 1] << 8);
            if (tag != 0x003d) continue;

            uint16_t type = data[off + 2] | (data[off + 3] << 8);
            uint32_t count = data[off + 4] | (data[off + 5] << 8) |
                             (data[off + 6] << 16) | (data[off + 7] << 24);
            if (type != 2 || count == 0) break;

            const char* str;
            if (count <= 4) {
                str = (const char*)&data[off + 8];
            } else {
                uint32_t valOff = data[off + 8] | (data[off + 9] << 8) |
                                  (data[off + 10] << 16) | (data[off + 11] << 24);
                // Convert TIFF-absolute to MakerNote-relative
                size_t mnOff = (valOff >= baseOff) ? valOff - baseOff : 0;
                if (mnOff + count > size) break;
                str = (const char*)&data[mnOff];
            }
            string result(str, strnlen(str, count));
            logNotice() << "[Sigma] ColorMode: \"" << result << "\"";
            return result;
        }
        return "";
    }

    // Extract EXIF/MakerNote metadata using exiv2
    static void extractExifMetadata(const string& path, PhotoEntry& photo) {
        try {
            auto image = Exiv2::ImageFactory::open(path);
            image->readMetadata();
            auto& exif = image->exifData();

            auto getString = [&](const char* key) -> string {
                auto it = exif.findKey(Exiv2::ExifKey(key));
                if (it != exif.end()) return it->print(&exif);
                return "";
            };

            auto getFloat = [&](const char* key) -> float {
                auto it = exif.findKey(Exiv2::ExifKey(key));
                if (it != exif.end()) return it->toFloat();
                return 0;
            };

            photo.cameraMake = getString("Exif.Image.Make");
            photo.camera = getString("Exif.Image.Model");
            photo.lens = getString("Exif.Photo.LensModel");
            photo.lensMake = getString("Exif.Photo.LensMake");
            photo.focalLength = getFloat("Exif.Photo.FocalLength");
            photo.aperture = getFloat("Exif.Photo.FNumber");
            photo.iso = getFloat("Exif.Photo.ISOSpeedRatings");
            photo.dateTimeOriginal = getString("Exif.Photo.DateTimeOriginal");

            // Image dimensions from EXIF (try multiple tags)
            auto wIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelXDimension"));
            auto hIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelYDimension"));
            if (wIt != exif.end()) photo.width = (int)wIt->toInt64();
            if (hIt != exif.end()) photo.height = (int)hIt->toInt64();

            // Fallback: Exif.Image.ImageWidth/ImageLength
            if (photo.width == 0 || photo.height == 0) {
                auto w2 = exif.findKey(Exiv2::ExifKey("Exif.Image.ImageWidth"));
                auto h2 = exif.findKey(Exiv2::ExifKey("Exif.Image.ImageLength"));
                if (w2 != exif.end() && photo.width == 0) photo.width = (int)w2->toInt64();
                if (h2 != exif.end() && photo.height == 0) photo.height = (int)h2->toInt64();
            }

            // Fallback: read from image file header (JPEG SOF, PNG IHDR, etc.)
            if (photo.width == 0 || photo.height == 0) {
                int w = 0, h = 0, ch = 0;
                if (stbi_info(path.c_str(), &w, &h, &ch)) {
                    if (photo.width == 0) photo.width = w;
                    if (photo.height == 0) photo.height = h;
                }
            }

            // Creative Style / Color Mode / Film Simulation
            // Sony: parsed by exiv2 natively
            string style = getString("Exif.Sony2.CreativeStyle");
            // Sigma: exiv2 doesn't parse Sigma MakerNote, read tag 0x003d from raw binary
            if (style.empty()) {
                style = extractSigmaColorMode(exif);
            }
            // Fujifilm: Film Simulation
            if (style.empty()) {
                style = getString("Exif.Fujifilm.FilmMode");
            }
            if (!style.empty()) {
                photo.creativeStyle = style;
            }

            // GPS coordinates
            // Use toString() for Ref (returns "N"/"S"/"E"/"W"), not print() which returns "North"/"West"
            auto getRawString = [&](const char* key) -> string {
                auto it = exif.findKey(Exiv2::ExifKey(key));
                if (it != exif.end()) return it->toString();
                return "";
            };

            auto latIt = exif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitude"));
            string latRef = getRawString("Exif.GPSInfo.GPSLatitudeRef");
            if (latIt != exif.end() && !latRef.empty()) {
                photo.latitude = exifGpsToDecimal(latIt->value());
                if (latRef == "S") photo.latitude = -photo.latitude;
            }

            auto lonIt = exif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLongitude"));
            string lonRef = getRawString("Exif.GPSInfo.GPSLongitudeRef");
            if (lonIt != exif.end() && !lonRef.empty()) {
                photo.longitude = exifGpsToDecimal(lonIt->value());
                if (lonRef == "W") photo.longitude = -photo.longitude;
            }

            auto altIt = exif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSAltitude"));
            if (altIt != exif.end()) {
                auto altR = altIt->value().toRational(0);
                if (altR.second != 0) {
                    photo.altitude = (double)altR.first / (double)altR.second;
                }
                // Check altitude ref (1 = below sea level)
                auto altRefIt = exif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSAltitudeRef"));
                if (altRefIt != exif.end() && altRefIt->toInt64() == 1) {
                    photo.altitude = -photo.altitude;
                }
            }

            // --- Extended shooting info (v9) ---
            {
                // ExposureTime: "1/125 s" -> "1/125"
                string et = getString("Exif.Photo.ExposureTime");
                if (!et.empty()) {
                    auto sp = et.find(' ');
                    photo.exposureTime = (sp != string::npos) ? et.substr(0, sp) : et;
                }
                photo.exposureBias = getFloat("Exif.Photo.ExposureBiasValue");

                auto oriIt = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
                if (oriIt != exif.end()) photo.orientation = (int)oriIt->toInt64();

                photo.whiteBalance = getString("Exif.Photo.WhiteBalance");

                auto fl35It = exif.findKey(Exiv2::ExifKey("Exif.Photo.FocalLengthIn35mmFilm"));
                if (fl35It != exif.end()) photo.focalLength35mm = (int)fl35It->toInt64();

                photo.offsetTime = getString("Exif.Photo.OffsetTime");
                photo.bodySerial = getString("Exif.Photo.BodySerialNumber");
                photo.lensSerial = getString("Exif.Photo.LensSerialNumber");
                photo.subjectDistance = getFloat("Exif.Photo.SubjectDistance");
                photo.subsecTimeOriginal = getString("Exif.Photo.SubSecTimeOriginal");
            }

            // --- Lens correction parameters (Sony EXIF / DNG OpcodeList / Fuji MakerNote) ---
            extractLensCorrectionParams(exif, photo);

        } catch (...) {
            // exiv2 failed, leave metadata empty
        }
    }

    // Extract Sony, DNG, or Fuji lens correction data from EXIF and store as JSON
    static void extractLensCorrectionParams(const Exiv2::ExifData& exif, PhotoEntry& photo) {
        // Try Sony SubImage1 correction params first
        auto distIt = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DistortionCorrParams"));
        if (distIt != exif.end()) {
            extractSonyLensCorrection(exif, photo);
        } else {
            // Try DNG OpcodeList
            auto opIt = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.OpcodeList3"));
            if (opIt != exif.end()) {
                extractDngLensCorrection(exif, photo);
            } else {
                // Try Fujifilm MakerNote
                auto fujiIt = exif.findKey(Exiv2::ExifKey("Exif.Fujifilm.GeometricDistortionParams"));
                if (fujiIt != exif.end()) {
                    extractFujiLensCorrection(exif, photo);
                }
            }
        }

        // Append DefaultCropOrigin/Size + orientation to lens correction JSON.
        // Crop coords are stored in EXIF's native landscape orientation here.
        // They get transformed to pixel-space (rotation-aware) when the RAW
        // is first displayed and written back via processRawLoadCompletion.
        if (!photo.lensCorrectionParams.empty()) {
            try {
                auto j = nlohmann::json::parse(photo.lensCorrectionParams);
                // Only append DefaultCrop for Sony/DNG (Fuji doesn't use it)
                string type = j.value("type", string(""));
                if (type != "fuji") {
                    auto cropOrig = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DefaultCropOrigin"));
                    auto cropSize = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DefaultCropSize"));
                    if (cropOrig != exif.end() && cropSize != exif.end() &&
                        cropOrig->count() >= 2 && cropSize->count() >= 2) {
                        j["cropX"] = (int)cropOrig->toInt64(0);
                        j["cropY"] = (int)cropOrig->toInt64(1);
                        j["cropW"] = (int)cropSize->toInt64(0);
                        j["cropH"] = (int)cropSize->toInt64(1);
                        j["orient"] = photo.orientation;
                        photo.lensCorrectionParams = j.dump();
                    }
                }
            } catch (...) {}
        }
    }

    // Decode Sony SubImage1 EXIF lens correction -> JSON
    static void extractSonyLensCorrection(const Exiv2::ExifData& exif, PhotoEntry& photo) {
        try {
            auto readKnots = [&](const char* key) -> vector<int16_t> {
                auto it = exif.findKey(Exiv2::ExifKey(key));
                if (it == exif.end()) return {};
                vector<int16_t> vals;
                for (int i = 0; i < (int)it->count(); i++) {
                    vals.push_back((int16_t)it->toInt64(i));
                }
                return vals;
            };

            auto distRaw = readKnots("Exif.SubImage1.DistortionCorrParams");
            auto caRaw = readKnots("Exif.SubImage1.ChromaticAberrationCorrParams");
            auto vigRaw = readKnots("Exif.SubImage1.VignettingCorrParams");

            if (distRaw.empty() && caRaw.empty() && vigRaw.empty()) return;

            nlohmann::json j;
            j["type"] = "sony";

            // Distortion: raw * 2^(-14) + 1
            if (!distRaw.empty()) {
                j["nc"] = (int)distRaw.size();
                nlohmann::json dist = nlohmann::json::array();
                for (auto v : distRaw) dist.push_back(v * (1.0 / 16384.0) + 1.0);
                j["dist"] = dist;
            }

            // Chromatic aberration: raw * 2^(-21) + 1
            // First half = R channel, second half = B channel
            if (!caRaw.empty()) {
                int half = (int)caRaw.size() / 2;
                nlohmann::json caR = nlohmann::json::array();
                nlohmann::json caB = nlohmann::json::array();
                for (int i = 0; i < half; i++)
                    caR.push_back(caRaw[i] * (1.0 / 2097152.0) + 1.0);
                for (int i = half; i < (int)caRaw.size(); i++)
                    caB.push_back(caRaw[i] * (1.0 / 2097152.0) + 1.0);
                j["caR"] = caR;
                j["caB"] = caB;
            }

            // Vignetting: gain = 2^(0.5 - 2^(raw * 2^(-13) - 1))
            // darktable v2 formula (no squaring of inner or outer term)
            if (!vigRaw.empty()) {
                nlohmann::json vig = nlohmann::json::array();
                for (auto v : vigRaw) {
                    double x = v * (1.0 / 8192.0) - 1.0;
                    double gain = pow(2.0, 0.5 - pow(2.0, x));
                    vig.push_back(gain);
                }
                j["vig"] = vig;
            }

            photo.lensCorrectionParams = j.dump();
        } catch (...) {}
    }

    // Decode DNG OpcodeList -> JSON
    static void extractDngLensCorrection(const Exiv2::ExifData& exif, PhotoEntry& photo) {
        try {
            nlohmann::json j;
            j["type"] = "dng";

            // OpcodeList3: WarpRectilinear (distortion + TCA)
            auto op3It = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.OpcodeList3"));
            if (op3It != exif.end()) {
                auto& val = op3It->value();
                size_t sz = val.size();
                vector<uint8_t> buf(sz);
                val.copy((Exiv2::byte*)buf.data(), Exiv2::invalidByteOrder);
                parseDngWarpRectilinear(buf, j);
            }

            // OpcodeList2: GainMap (vignetting)
            auto op2It = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.OpcodeList2"));
            if (op2It != exif.end()) {
                auto& val = op2It->value();
                size_t sz = val.size();
                vector<uint8_t> buf(sz);
                val.copy((Exiv2::byte*)buf.data(), Exiv2::invalidByteOrder);
                parseDngGainMap(buf, j);
            }

            if (j.contains("warp") || j.contains("gain")) {
                photo.lensCorrectionParams = j.dump();
            }
        } catch (...) {}
    }

    // Big-endian readers
    static uint32_t readBE32(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    static double readBE64f(const uint8_t* p) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
        double d;
        memcpy(&d, &v, 8);
        return d;
    }
    static float readBE32f(const uint8_t* p) {
        uint32_t v = readBE32(p);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }

    // Parse WarpRectilinear opcode from OpcodeList3
    static void parseDngWarpRectilinear(const vector<uint8_t>& buf, nlohmann::json& j) {
        if (buf.size() < 4) return;
        uint32_t numOps = readBE32(buf.data());
        size_t pos = 4;

        for (uint32_t op = 0; op < numOps && pos + 16 <= buf.size(); op++) {
            uint32_t opcodeId = readBE32(buf.data() + pos);
            // uint32_t dngVersion = readBE32(buf.data() + pos + 4);
            // uint32_t flags = readBE32(buf.data() + pos + 8);
            uint32_t paramBytes = readBE32(buf.data() + pos + 12);
            pos += 16;

            if (opcodeId == 1 && pos + paramBytes <= buf.size()) {
                // WarpRectilinear: nPlanes(4) + [6 doubles per plane] + center(cx, cy)
                const uint8_t* d = buf.data() + pos;
                uint32_t nPlanes = readBE32(d);
                if (nPlanes >= 1 && nPlanes <= 3) {
                    nlohmann::json warp;
                    warp["planes"] = nPlanes;
                    nlohmann::json coeffs = nlohmann::json::array();
                    size_t off = 4;
                    for (uint32_t p = 0; p < nPlanes; p++) {
                        nlohmann::json plane = nlohmann::json::array();
                        for (int k = 0; k < 6; k++) {
                            plane.push_back(readBE64f(d + off));
                            off += 8;
                        }
                        coeffs.push_back(plane);
                    }
                    warp["cx"] = readBE64f(d + off); off += 8;
                    warp["cy"] = readBE64f(d + off);
                    j["warp"] = warp;
                }
            }
            pos += paramBytes;
        }
    }

    // Parse GainMap opcode from OpcodeList2
    static void parseDngGainMap(const vector<uint8_t>& buf, nlohmann::json& j) {
        if (buf.size() < 4) return;
        uint32_t numOps = readBE32(buf.data());
        size_t pos = 4;

        for (uint32_t op = 0; op < numOps && pos + 16 <= buf.size(); op++) {
            uint32_t opcodeId = readBE32(buf.data() + pos);
            uint32_t paramBytes = readBE32(buf.data() + pos + 12);
            pos += 16;

            if (opcodeId == 9 && pos + paramBytes <= buf.size()) {
                // GainMap: top(4) left(4) bottom(4) right(4) plane(4) planes(4)
                //          rowPitch(4) colPitch(4) mapPointsV(4) mapPointsH(4)
                //          mapSpacingV(8) mapSpacingH(8) mapOriginV(8) mapOriginH(8)
                //          mapPlanes(4) + mapPoints float32[]
                const uint8_t* d = buf.data() + pos;
                if (paramBytes < 72) { pos += paramBytes; continue; }
                // uint32_t top = readBE32(d);
                // uint32_t left = readBE32(d + 4);
                // uint32_t bottom = readBE32(d + 8);
                // uint32_t right = readBE32(d + 12);
                // uint32_t plane = readBE32(d + 16);
                // uint32_t planes = readBE32(d + 20);
                uint32_t rowPitch = readBE32(d + 24);
                uint32_t colPitch = readBE32(d + 28);
                uint32_t rows = readBE32(d + 32);
                uint32_t cols = readBE32(d + 36);
                // double spacingV = readBE64f(d + 40);
                // double spacingH = readBE64f(d + 48);
                // double originV = readBE64f(d + 56);
                // double originH = readBE64f(d + 64);
                uint32_t mapPlanes = readBE32(d + 72);

                size_t headerSize = 76;
                uint32_t totalPoints = rows * cols * mapPlanes;
                if (headerSize + totalPoints * 4 <= paramBytes && totalPoints > 0 && totalPoints < 100000) {
                    nlohmann::json gain;
                    gain["rows"] = rows;
                    gain["cols"] = cols;
                    gain["mapPlanes"] = mapPlanes;
                    gain["rowPitch"] = rowPitch;
                    gain["colPitch"] = colPitch;
                    nlohmann::json data = nlohmann::json::array();
                    for (uint32_t i = 0; i < totalPoints; i++) {
                        data.push_back(readBE32f(d + headerSize + i * 4));
                    }
                    gain["data"] = data;
                    j["gain"] = gain;
                }
            }
            pos += paramBytes;
        }
    }

    // Decode Fujifilm MakerNote lens correction params -> JSON
    // Pre-computes values to Sony-compatible format:
    //   distortion: factor = value/100 + 1 (darktable formula)
    //   CA: factor = value + 1
    //   vignetting: factor = value/100 (fractional brightness)
    static void extractFujiLensCorrection(const Exiv2::ExifData& exif, PhotoEntry& photo) {
        try {
            auto distIt = exif.findKey(Exiv2::ExifKey("Exif.Fujifilm.GeometricDistortionParams"));
            if (distIt == exif.end()) return;

            int count = (int)distIt->count();
            int nc;
            if (count == 19) nc = 9;        // X-Trans IV/V
            else if (count == 23) nc = 11;   // X-Trans I/II/III
            else return;

            nlohmann::json j;
            j["type"] = "fuji";
            j["nc"] = nc;

            // Knot positions (values[1..nc])
            nlohmann::json knots = nlohmann::json::array();
            for (int i = 0; i < nc; i++)
                knots.push_back(distIt->toFloat(1 + i));
            j["knots"] = knots;

            // Distortion: values[nc+1..2*nc], factor = value/100 + 1
            nlohmann::json dist = nlohmann::json::array();
            for (int i = 0; i < nc; i++)
                dist.push_back(distIt->toFloat(1 + nc + i) / 100.0f + 1.0f);
            j["dist"] = dist;

            // Chromatic Aberration (R + B channels)
            auto caIt = exif.findKey(Exiv2::ExifKey("Exif.Fujifilm.ChromaticAberrationParams"));
            if (caIt != exif.end()) {
                int caCount = (int)caIt->count();
                // IV/V: 29 = 1 header + 9 knots + 9 R + 9 B + 1 trailer
                // I/II/III: 31 = 1 header + 10 knots + 10 R + 10 B (+ 1?)
                if ((nc == 9 && caCount == 29) || (nc == 11 && caCount >= 31)) {
                    nlohmann::json caR = nlohmann::json::array();
                    nlohmann::json caB = nlohmann::json::array();
                    for (int i = 0; i < nc; i++) {
                        caR.push_back(caIt->toFloat(1 + nc + i) + 1.0f);
                        caB.push_back(caIt->toFloat(1 + nc * 2 + i) + 1.0f);
                    }
                    j["caR"] = caR;
                    j["caB"] = caB;
                }
            }

            // Vignetting: factor = value/100 (fractional brightness)
            auto vigIt = exif.findKey(Exiv2::ExifKey("Exif.Fujifilm.VignettingParams"));
            if (vigIt != exif.end() && (int)vigIt->count() == count) {
                nlohmann::json vig = nlohmann::json::array();
                for (int i = 0; i < nc; i++)
                    vig.push_back(vigIt->toFloat(1 + nc + i) / 100.0f);
                j["vig"] = vig;
            }

            photo.lensCorrectionParams = j.dump();
        } catch (...) {}
    }

    // Convert "YYYY:MM:DD HH:MM:SS" to "YYYY/MM/DD", fallback to mtime or "unknown"
    static string dateToSubdir(const string& dateTimeOriginal, const string& filePath = "") {
        // Try parsing EXIF date format
        if (dateTimeOriginal.size() >= 10 && dateTimeOriginal[4] == ':' && dateTimeOriginal[7] == ':') {
            string y = dateTimeOriginal.substr(0, 4);
            string m = dateTimeOriginal.substr(5, 2);
            string d = dateTimeOriginal.substr(8, 2);
            // Validate numeric
            if (y.find_first_not_of("0123456789") == string::npos &&
                m.find_first_not_of("0123456789") == string::npos &&
                d.find_first_not_of("0123456789") == string::npos) {
                return y + "/" + m + "/" + d;
            }
        }
        // Fallback: file modification time via stat
        if (!filePath.empty() && fs::exists(filePath)) {
            try {
                struct stat st;
                if (stat(filePath.c_str(), &st) == 0) {
                    tm ltm;
                    localtime_r(&st.st_mtime, &ltm);
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%04d/%02d/%02d",
                             ltm.tm_year + 1900, ltm.tm_mon + 1, ltm.tm_mday);
                    return string(buf);
                }
            } catch (...) {}
        }
        return "unknown";
    }

    // Resolve destination path, adding -1, -2 suffix if file already exists
    static fs::path resolveDestPath(const fs::path& dir, const string& filename) {
        fs::path dest = dir / filename;
        if (!fs::exists(dest)) return dest;

        string stem = fs::path(filename).stem().string();
        string ext = fs::path(filename).extension().string();
        for (int i = 1; i < 10000; i++) {
            dest = dir / (stem + "-" + to_string(i) + ext);
            if (!fs::exists(dest)) return dest;
        }
        return dest; // unlikely fallback
    }

    // Convert float RGBA pixels to 8-bit RGBA in-place
    static void convertF32ToU8(Pixels& pixels) {
        if (pixels.getFormat() != PixelFormat::F32) return;
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        Pixels u8;
        u8.allocate(w, h, ch);
        const float* src = pixels.getDataF32();
        unsigned char* dst = u8.getData();
        int count = w * h * ch;
        for (int i = 0; i < count; i++) {
            dst[i] = (unsigned char)(clamp(src[i], 0.0f, 1.0f) * 255.0f);
        }
        pixels = std::move(u8);
    }

    static void resizePixels(Pixels& src, int newW, int newH) {
        int srcW = src.getWidth();
        int srcH = src.getHeight();
        int channels = src.getChannels();

        Pixels dst;
        dst.allocate(newW, newH, channels);

        const unsigned char* srcData = src.getData();
        unsigned char* dstData = dst.getData();

        for (int y = 0; y < newH; y++) {
            int srcY = y * srcH / newH;
            for (int x = 0; x < newW; x++) {
                int srcX = x * srcW / newW;
                int srcIdx = (srcY * srcW + srcX) * channels;
                int dstIdx = (y * newW + x) * channels;
                for (int c = 0; c < channels; c++) {
                    dstData[dstIdx + c] = srcData[srcIdx + c];
                }
            }
        }

        src = std::move(dst);
    }

    // --- XMP sidecar ---

    // Find XMP sidecar path for a given file (Lightroom: .xmp, darktable: .ARW.xmp)
    static string findXmpSidecar(const string& localPath) {
        if (localPath.empty()) return "";
        fs::path p(localPath);
        // Lightroom style: replace extension with .xmp
        fs::path lr = p;
        lr.replace_extension(".xmp");
        if (fs::exists(lr)) return lr.string();
        // darktable style: append .xmp
        fs::path dt = p;
        dt += ".xmp";
        if (fs::exists(dt)) return dt.string();
        return "";
    }

    // Get sidecar write path (Lightroom style: replace extension)
    static string xmpWritePath(const string& localPath) {
        if (localPath.empty()) return "";
        fs::path p(localPath);
        p.replace_extension(".xmp");
        return p.string();
    }

    // Extract metadata from XMP sidecar (called after extractExifMetadata)
    static void extractXmpMetadata(const string& localPath, PhotoEntry& photo) {
        string xmpPath = findXmpSidecar(localPath);
        if (xmpPath.empty()) return;

        try {
            auto image = Exiv2::ImageFactory::open(xmpPath);
            image->readMetadata();
            auto& xmp = image->xmpData();

            // Rating
            auto rIt = xmp.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
            if (rIt != xmp.end()) {
                photo.rating = clamp((int)rIt->toInt64(), 0, 5);
            }

            // Color label
            auto lIt = xmp.findKey(Exiv2::XmpKey("Xmp.xmp.Label"));
            if (lIt != xmp.end()) {
                photo.colorLabel = lIt->toString();
            }

            // Description -> memo (lang-alt, take first value)
            auto dIt = xmp.findKey(Exiv2::XmpKey("Xmp.dc.description"));
            if (dIt != xmp.end()) {
                photo.memo = dIt->toString();
            }

            // Subject -> tags (bag of strings -> JSON array)
            nlohmann::json tagArr = nlohmann::json::array();
            for (auto it = xmp.begin(); it != xmp.end(); ++it) {
                if (it->key().substr(0, 15) == "Xmp.dc.subject") {
                    string val = it->toString();
                    if (!val.empty()) tagArr.push_back(val);
                }
            }
            if (!tagArr.empty()) {
                photo.tags = tagArr.dump();
            }

            // GPS from XMP (Lightroom writes exif:GPSLatitude/GPSLongitude)
            if (!photo.hasGps()) {
                auto gpsLatIt = xmp.findKey(Exiv2::XmpKey("Xmp.exif.GPSLatitude"));
                auto gpsLonIt = xmp.findKey(Exiv2::XmpKey("Xmp.exif.GPSLongitude"));
                if (gpsLatIt != xmp.end() && gpsLonIt != xmp.end()) {
                    photo.latitude = parseXmpGpsCoord(gpsLatIt->toString());
                    photo.longitude = parseXmpGpsCoord(gpsLonIt->toString());
                }

                auto gpsAltIt = xmp.findKey(Exiv2::XmpKey("Xmp.exif.GPSAltitude"));
                if (gpsAltIt != xmp.end()) {
                    // Altitude is stored as rational string like "59000/10000"
                    string altStr = gpsAltIt->toString();
                    auto slashPos = altStr.find('/');
                    if (slashPos != string::npos) {
                        try {
                            double num = stod(altStr.substr(0, slashPos));
                            double den = stod(altStr.substr(slashPos + 1));
                            if (den != 0) photo.altitude = num / den;
                        } catch (...) {}
                    } else {
                        try { photo.altitude = stod(altStr); } catch (...) {}
                    }

                    // Check altitude ref
                    auto altRefIt = xmp.findKey(Exiv2::XmpKey("Xmp.exif.GPSAltitudeRef"));
                    if (altRefIt != xmp.end() && altRefIt->toString() == "1") {
                        photo.altitude = -photo.altitude;
                    }
                }
            }

            // Use sidecar mtime as updatedAt
            struct stat st;
            if (stat(xmpPath.c_str(), &st) == 0) {
                int64_t mtime = (int64_t)st.st_mtime * 1000;
                if (photo.rating != 0) photo.ratingUpdatedAt = mtime;
                if (!photo.colorLabel.empty()) photo.colorLabelUpdatedAt = mtime;
                if (!photo.memo.empty()) photo.memoUpdatedAt = mtime;
                if (!tagArr.empty()) photo.tagsUpdatedAt = mtime;
            }

            logNotice() << "[XMP] Read sidecar: " << xmpPath
                        << " rating=" << photo.rating;
        } catch (const exception& e) {
            logWarning() << "[XMP] Failed to read: " << xmpPath << " - " << e.what();
        }
    }

    // Write XMP sidecar (Lightroom-compatible)
    static void writeXmpSidecar(const string& localPath, const PhotoEntry& photo) {
        string xmpPath = xmpWritePath(localPath);
        if (xmpPath.empty()) return;

        try {
            Exiv2::XmpData xmp;

            // If sidecar already exists, read it first to preserve other fields
            if (fs::exists(xmpPath)) {
                auto existing = Exiv2::ImageFactory::open(xmpPath);
                existing->readMetadata();
                xmp = existing->xmpData();
            }

            // Rating
            xmp["Xmp.xmp.Rating"] = photo.rating;

            // Color label
            if (!photo.colorLabel.empty()) {
                xmp["Xmp.xmp.Label"] = photo.colorLabel;
            } else {
                auto it = xmp.findKey(Exiv2::XmpKey("Xmp.xmp.Label"));
                if (it != xmp.end()) xmp.erase(it);
            }

            // Description (lang-alt)
            if (!photo.memo.empty()) {
                xmp["Xmp.dc.description"] = photo.memo;
            } else {
                auto it = xmp.findKey(Exiv2::XmpKey("Xmp.dc.description"));
                if (it != xmp.end()) xmp.erase(it);
            }

            // Subject (bag)
            // Remove existing subjects first
            while (true) {
                auto it = xmp.findKey(Exiv2::XmpKey("Xmp.dc.subject"));
                if (it == xmp.end()) break;
                xmp.erase(it);
            }
            if (!photo.tags.empty()) {
                try {
                    auto tagArr = nlohmann::json::parse(photo.tags);
                    if (tagArr.is_array()) {
                        for (const auto& t : tagArr) {
                            Exiv2::Value::UniquePtr val = Exiv2::Value::create(Exiv2::xmpBag);
                            val->read(t.get<string>());
                            xmp.add(Exiv2::XmpKey("Xmp.dc.subject"), val.get());
                        }
                    }
                } catch (...) {}
            }

            // Write XMP file
            string xmpPacket;
            Exiv2::XmpParser::encode(xmpPacket, xmp);

            ofstream out(xmpPath);
            if (out) {
                out << xmpPacket;
                logNotice() << "[XMP] Wrote sidecar: " << xmpPath;
            }
        } catch (const exception& e) {
            logWarning() << "[XMP] Failed to write: " << xmpPath << " - " << e.what();
        }
    }

    // Write XMP only if photo has a local path and is managed
    static void writeXmpSidecarIfLocal(const PhotoEntry& photo) {
        if (photo.localPath.empty() || !fs::exists(photo.localPath)) return;
        if (!photo.isManaged) return;  // don't write XMP for external references
        writeXmpSidecar(photo.localPath, photo);
    }

    // EXIF backfill thread: re-read EXIF from local files for v9 fields
    void startExifBackfillThread(const vector<string>& ids) {
        if (exifBackfillRunning_) return;
        exifBackfillRunning_ = true;

        if (exifBackfillThread_.joinable()) exifBackfillThread_.join();

        auto jobs = make_shared<vector<pair<string, string>>>();
        jobs->reserve(ids.size());
        for (const auto& id : ids) {
            auto it = photos_.find(id);
            if (it != photos_.end() && !it->second.localPath.empty()) {
                jobs->push_back({id, it->second.localPath});
            }
        }

        logNotice() << "[ExifBackfill] Starting " << jobs->size() << " jobs with "
                     << EXIF_BACKFILL_WORKERS << " workers";

        exifBackfillThread_ = thread([this, jobs]() {
            // Suppress exiv2 warnings/errors during bulk backfill
            // (Sony2/Olympus MakerNote parse errors are harmless but noisy)
            auto prevLevel = Exiv2::LogMsg::level();
            Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);

            atomic<int> nextIdx{0};
            int total = (int)jobs->size();

            auto worker = [&]() {
                while (!stopping_) {
                    int idx = nextIdx.fetch_add(1);
                    if (idx >= total) break;

                    const auto& [id, path] = (*jobs)[idx];
                    PhotoEntry temp;
                    temp.id = id;

                    // Re-read EXIF data using extractExifMetadata (which now populates v9 fields)
                    try {
                        auto image = Exiv2::ImageFactory::open(path);
                        image->readMetadata();
                        auto& exif = image->exifData();

                        auto getString = [&](const char* key) -> string {
                            auto it = exif.findKey(Exiv2::ExifKey(key));
                            if (it != exif.end()) return it->print(&exif);
                            return "";
                        };
                        auto getFloat = [&](const char* key) -> float {
                            auto it = exif.findKey(Exiv2::ExifKey(key));
                            if (it != exif.end()) return it->toFloat();
                            return 0;
                        };

                        // Basic metadata (may be missing for some formats)
                        temp.cameraMake = getString("Exif.Image.Make");
                        temp.camera = getString("Exif.Image.Model");
                        temp.lens = getString("Exif.Photo.LensModel");
                        temp.focalLength = getFloat("Exif.Photo.FocalLength");
                        temp.aperture = getFloat("Exif.Photo.FNumber");
                        temp.iso = getFloat("Exif.Photo.ISOSpeedRatings");
                        temp.dateTimeOriginal = getString("Exif.Photo.DateTimeOriginal");

                        // Image dimensions
                        auto wIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelXDimension"));
                        auto hIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelYDimension"));
                        if (wIt != exif.end()) temp.width = (int)wIt->toInt64();
                        if (hIt != exif.end()) temp.height = (int)hIt->toInt64();
                        if (temp.width == 0 || temp.height == 0) {
                            auto w2 = exif.findKey(Exiv2::ExifKey("Exif.Image.ImageWidth"));
                            auto h2 = exif.findKey(Exiv2::ExifKey("Exif.Image.ImageLength"));
                            if (w2 != exif.end() && temp.width == 0) temp.width = (int)w2->toInt64();
                            if (h2 != exif.end() && temp.height == 0) temp.height = (int)h2->toInt64();
                        }
                        // Fallback: SubImage1 (DNG main image)
                        if (temp.width == 0 || temp.height == 0) {
                            auto w3 = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.ImageWidth"));
                            auto h3 = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.ImageLength"));
                            if (w3 != exif.end() && temp.width == 0) temp.width = (int)w3->toInt64();
                            if (h3 != exif.end() && temp.height == 0) temp.height = (int)h3->toInt64();
                        }

                        // Extended shooting info
                        string et = getString("Exif.Photo.ExposureTime");
                        if (!et.empty()) {
                            auto sp = et.find(' ');
                            temp.exposureTime = (sp != string::npos) ? et.substr(0, sp) : et;
                        }
                        temp.exposureBias = getFloat("Exif.Photo.ExposureBiasValue");
                        auto oriIt = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
                        if (oriIt != exif.end()) temp.orientation = (int)oriIt->toInt64();
                        temp.whiteBalance = getString("Exif.Photo.WhiteBalance");
                        auto fl35It = exif.findKey(Exiv2::ExifKey("Exif.Photo.FocalLengthIn35mmFilm"));
                        if (fl35It != exif.end()) temp.focalLength35mm = (int)fl35It->toInt64();
                        temp.offsetTime = getString("Exif.Photo.OffsetTime");
                        temp.bodySerial = getString("Exif.Photo.BodySerialNumber");
                        temp.lensSerial = getString("Exif.Photo.LensSerialNumber");
                        temp.subjectDistance = getFloat("Exif.Photo.SubjectDistance");
                        temp.subsecTimeOriginal = getString("Exif.Photo.SubSecTimeOriginal");

                        // Creative Style / Film Simulation
                        string style = getString("Exif.Sony2.CreativeStyle");
                        if (style.empty()) style = extractSigmaColorMode(exif);
                        if (style.empty()) style = getString("Exif.Fujifilm.FilmMode");
                        temp.creativeStyle = style;

                        // Lens correction
                        extractLensCorrectionParams(exif, temp);
                    } catch (...) {}

                    {
                        lock_guard<mutex> lock(exifBackfillMutex_);
                        completedExifBackfills_.push_back({id, std::move(temp)});
                    }

                    if (total > 0 && (idx + 1) % (max(1, total / 4)) == 0) {
                        logNotice() << "[ExifBackfill] " << (idx + 1) << "/" << total;
                    }
                }
            };

            vector<thread> workers;
            for (int i = 0; i < EXIF_BACKFILL_WORKERS; i++) {
                workers.emplace_back(worker);
            }
            for (auto& w : workers) w.join();

            // Restore exiv2 log level
            Exiv2::LogMsg::setLevel(prevLevel);

            logNotice() << "[ExifBackfill] Complete: " << total << " photos processed";
            exifBackfillRunning_ = false;
        });
    }
};
