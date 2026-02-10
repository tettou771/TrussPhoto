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
#include <sys/stat.h>

using namespace std;
using namespace tc;
using namespace tcx;

namespace fs = std::filesystem;

#include "PhotoEntry.h"
#include "PhotoDatabase.h"

// Photo provider - manages local + server photos
class PhotoProvider {
public:
    PhotoProvider() = default;

    // --- Configuration ---

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

    void setDatabasePath(const string& path) {
        databasePath_ = path;
    }

    void setJsonMigrationPath(const string& path) {
        jsonMigrationPath_ = path;
    }

    // Kept for backward compatibility but unused with SQLite
    void setLibraryPath(const string& path) {
        (void)path;
    }

    void setLibraryFolder(const string& folder) {
        libraryFolder_ = folder;
        fs::create_directories(folder);
    }

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

    // Scan library folder for unregistered files, returns count of added
    int scanLibraryFolder() {
        if (libraryFolder_.empty() || !fs::exists(libraryFolder_)) return 0;

        int addedCount = 0;
        vector<PhotoEntry> newEntries;
        for (const auto& entry : fs::recursive_directory_iterator(libraryFolder_)) {
            if (!entry.is_regular_file()) continue;
            if (!isSupportedImage(entry.path())) continue;

            string fname = entry.path().filename().string();
            uintmax_t fsize = entry.file_size();
            string id = fname + "_" + to_string(fsize);

            if (photos_.count(id)) continue;

            PhotoEntry photo;
            photo.id = id;
            photo.filename = fname;
            photo.fileSize = fsize;
            photo.localPath = entry.path().string();
            photo.isRaw = RawLoader::isRawFile(entry.path());
            photo.syncState = SyncState::LocalOnly;
            extractExifMetadata(photo.localPath, photo);
            photos_[id] = photo;
            newEntries.push_back(photo);
            addedCount++;
        }
        if (!newEntries.empty()) {
            db_.insertPhotos(newEntries);
        }
        return addedCount;
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
            if (!isSupportedImage(entry.path())) continue;

            string path = entry.path().string();
            auto size = entry.file_size();
            string fname = entry.path().filename().string();

            string id = fname + "_" + to_string(size);

            // Skip if already known
            if (photos_.count(id)) continue;

            // Register immediately with original path
            PhotoEntry photo;
            photo.id = id;
            photo.filename = fname;
            photo.fileSize = size;
            photo.localPath = path;
            photo.isRaw = RawLoader::isRawFile(entry.path());
            photo.syncState = SyncState::LocalOnly;

            // Extract metadata via exiv2
            extractExifMetadata(path, photo);

            photos_[id] = photo;
            newEntries.push_back(photo);
            added++;

            // Queue file copy if library folder is configured and source is outside it
            if (!libraryFolder_.empty()) {
                fs::path libPath = fs::canonical(fs::path(libraryFolder_));
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
        logNotice() << "[PhotoProvider] Found " << added << " new images (total: " << photos_.size() << ")";

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

    // Get sorted photo list (by filename)
    vector<string> getSortedIds() const {
        vector<string> ids;
        ids.reserve(photos_.size());
        for (const auto& [id, photo] : photos_) {
            ids.push_back(id);
        }
        sort(ids.begin(), ids.end(), [this](const string& a, const string& b) {
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
        if (libraryFolder_.empty()) {
            logWarning() << "[Consolidate] No library folder configured";
            return;
        }

        // Build task list on main thread
        vector<ConsolidateTask> tasks;
        fs::path libPath = fs::path(libraryFolder_);

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

                // Move RAW file
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

private:
    HttpClient client_;
    PhotoDatabase db_;
    unordered_map<string, PhotoEntry> photos_;
    string thumbnailCacheDir_;
    string databasePath_;
    string jsonMigrationPath_;
    string libraryFolder_;
    bool serverReachable_ = false;
    bool serverChecked_ = false;

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

    void startCopyThread() {
        lock_guard<mutex> lock(copyMutex_);
        if (pendingCopies_.empty() || copyThreadRunning_) return;

        copyThreadRunning_ = true;
        // Detach previous thread if any
        if (copyThread_.joinable()) copyThread_.join();

        // Take ownership of pending copies
        vector<CopyTask> tasks = std::move(pendingCopies_);
        pendingCopies_.clear();

        copyThread_ = thread([this, tasks = std::move(tasks)]() {
            for (const auto& task : tasks) {
                if (fs::exists(task.destPath)) {
                    // Already exists, just record it
                    lock_guard<mutex> lock(copyMutex_);
                    completedCopies_.push_back(task);
                    continue;
                }

                try {
                    fs::copy_file(task.srcPath, task.destPath);
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
        copyThread_.detach();
    }

    // Supported standard image extensions
    static inline const unordered_set<string> standardExtensions_ = {
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tga", ".psd", ".hdr"
    };

    bool isSupportedImage(const fs::path& path) const {
        string ext = path.extension().string();
        transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (standardExtensions_.count(ext)) return true;
        if (RawLoader::isRawFile(path)) return true;
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

            // Image dimensions from EXIF
            auto wIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelXDimension"));
            auto hIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelYDimension"));
            if (wIt != exif.end()) photo.width = (int)wIt->toInt64();
            if (hIt != exif.end()) photo.height = (int)hIt->toInt64();

            // Sony MakerNote: Creative Style
            string style = getString("Exif.Sony2.CreativeStyle");
            if (!style.empty()) {
                photo.creativeStyle = style;
            }
        } catch (...) {
            // exiv2 failed, leave metadata empty
        }
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
};
