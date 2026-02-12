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
#include "SmartPreview.h"
#include "ClipEmbedder.h"

// Photo provider - manages local + server photos
class PhotoProvider {
public:
    PhotoProvider() = default;

    // --- Configuration ---

    // Set all paths from a single catalog directory
    void setCatalogDir(const string& catalogPath) {
        thumbnailCacheDir_ = catalogPath + "/thumbnail_cache";
        smartPreviewDir_   = catalogPath + "/smart_preview";
        databasePath_      = catalogPath + "/library.db";
        pendingDir_        = catalogPath + "/pending";
        fs::create_directories(thumbnailCacheDir_);
        fs::create_directories(smartPreviewDir_);
        fs::create_directories(pendingDir_);
    }

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
            if (!isSupportedImage(entry.path())) continue;

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

    // Scan library folder for unregistered files, returns count of added
    int scanLibraryFolder() {
        if (rawStoragePath_.empty() || !fs::exists(rawStoragePath_)) return 0;

        int addedCount = 0;
        vector<PhotoEntry> newEntries;
        for (const auto& entry : fs::recursive_directory_iterator(rawStoragePath_)) {
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
            extractXmpMetadata(photo.localPath, photo);
            photos_[id] = photo;
            newEntries.push_back(photo);
            addedCount++;
        }
        if (!newEntries.empty()) {
            db_.insertPhotos(newEntries);
        }
        return addedCount;
    }

    // Check if a file path is a supported image
    bool isSupportedFile(const string& path) const {
        return isSupportedImage(fs::path(path));
    }

    // Import individual files (non-blocking, copies happen in background)
    int importFiles(const vector<string>& filePaths) {
        int added = 0;
        vector<PhotoEntry> newEntries;

        for (const auto& filePath : filePaths) {
            fs::path p(filePath);
            if (!fs::exists(p) || !fs::is_regular_file(p)) continue;
            if (!isSupportedImage(p)) continue;

            string fname = p.filename().string();
            uintmax_t fsize = fs::file_size(p);
            string id = fname + "_" + to_string(fsize);

            if (photos_.count(id)) continue;

            PhotoEntry photo;
            photo.id = id;
            photo.filename = fname;
            photo.fileSize = fsize;
            photo.localPath = filePath;
            photo.isRaw = RawLoader::isRawFile(p);
            photo.syncState = SyncState::LocalOnly;
            extractExifMetadata(filePath, photo);
            extractXmpMetadata(filePath, photo);

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

            // Extract metadata via exiv2 + XMP sidecar
            extractExifMetadata(path, photo);
            extractXmpMetadata(path, photo);

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

    // --- Rich metadata setters ---

    static int64_t nowMs() {
        return chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();
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

        // Add intermediate directories between leaf folders and rawStoragePath
        // so the tree hierarchy can be properly built
        vector<string> leafPaths;
        for (const auto& [path, _] : folders) {
            leafPaths.push_back(path);
        }
        for (const auto& leafPath : leafPaths) {
            fs::path p = fs::path(leafPath).parent_path();
            while (!p.empty() && p.string() != "/" && p != p.root_path()) {
                string pstr = p.string();
                // Stop above rawStoragePath
                if (!rawStoragePath_.empty() && pstr.size() < rawStoragePath_.size()) break;
                // Already exists — ancestors above also exist
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

            // Delete local RAW/image file + XMP sidecar
            if (!photo.localPath.empty() && fs::exists(photo.localPath)) {
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

    // Queue photos for background SP generation (from RAW)
    void queueSmartPreviewGeneration(const vector<string>& ids) {
        lock_guard<mutex> lock(spMutex_);
        for (const auto& id : ids) {
            auto it = photos_.find(id);
            if (it == photos_.end()) continue;
            auto& photo = it->second;
            // Only queue RAW files that don't already have SP
            if (!photo.isRaw) continue;
            if (!photo.localSmartPreviewPath.empty() && fs::exists(photo.localSmartPreviewPath)) continue;
            if (photo.localPath.empty() || !fs::exists(photo.localPath)) continue;
            pendingSPGenerations_.push_back(id);
        }
        startSPGenerationThread();
    }

    // Queue all photos without smart preview
    int queueAllMissingSP() {
        vector<string> ids;
        for (const auto& [id, photo] : photos_) {
            if (!photo.isRaw) continue;
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

    // --- CLIP Embedding ---

    // Initialize CLIP embedder in background (downloads model if needed)
    void initEmbedder(const string& modelsDir) {
        clipEmbedder_.loadAsync(modelsDir);
    }

    bool isEmbedderReady() const { return clipEmbedder_.isReady(); }
    bool isEmbedderInitializing() const { return clipEmbedder_.isInitializing(); }
    bool isEmbedderDownloading() const { return clipEmbedder_.isDownloading(); }
    float getEmbedderDownloadProgress() { return clipEmbedder_.getDownloadProgress(); }
    const string& getEmbedderStatus() const { return clipEmbedder_.getStatusText(); }

    // Queue all photos that don't have embeddings yet
    int queueAllMissingEmbeddings() {
        if (!clipEmbedder_.isReady()) return 0;
        auto ids = db_.getPhotosWithoutEmbedding(ClipEmbedder::MODEL_NAME);
        if (ids.empty()) return 0;

        lock_guard<mutex> lock(embMutex_);
        for (const auto& id : ids) {
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
            if (!db_.hasEmbedding(id, ClipEmbedder::MODEL_NAME)) {
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
            db_.insertEmbedding(result.photoId, ClipEmbedder::MODEL_NAME,
                                "image", result.embedding);
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

private:
    HttpClient client_;
    PhotoDatabase db_;
    unordered_map<string, PhotoEntry> photos_;
    string thumbnailCacheDir_;
    string databasePath_;
    string jsonMigrationPath_;
    string rawStoragePath_;
    string smartPreviewDir_;
    string pendingDir_;
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

    // Background SP generation
    struct SPResult {
        string photoId;
        string spPath;
    };

    mutable mutex spMutex_;
    vector<string> pendingSPGenerations_;
    vector<SPResult> completedSPGenerations_;
    atomic<bool> spThreadRunning_{false};
    thread spThread_;

    // CLIP embedding
    ClipEmbedder clipEmbedder_;

    struct EmbeddingResult {
        string photoId;
        vector<float> embedding;
    };

    mutable mutex embMutex_;
    vector<string> pendingEmbeddings_;
    vector<EmbeddingResult> completedEmbeddings_;
    atomic<bool> embThreadRunning_{false};
    atomic<int> embCompletedCount_{0};
    atomic<int> embTotalCount_{0};
    thread embThread_;

    void startEmbeddingThread() {
        // Called with embMutex_ already held
        if (pendingEmbeddings_.empty() || embThreadRunning_) return;

        embThreadRunning_ = true;
        embCompletedCount_ = 0;
        if (embThread_.joinable()) embThread_.join();

        vector<string> ids = std::move(pendingEmbeddings_);
        pendingEmbeddings_.clear();
        embTotalCount_ = (int)ids.size();

        embThread_ = thread([this, ids = std::move(ids)]() {
            logNotice() << "[CLIP] Generating embeddings for " << ids.size() << " photos";
            int done = 0;
            for (const auto& id : ids) {
                // Get thumbnail path for this photo
                auto it = photos_.find(id);
                if (it == photos_.end()) continue;
                auto& photo = it->second;

                // Load thumbnail pixels (U8, small — ideal for CLIP)
                Pixels thumbPixels;
                string thumbPath = photo.localThumbnailPath;
                bool loaded = false;

                if (!thumbPath.empty() && fs::exists(thumbPath)) {
                    loaded = thumbPixels.load(thumbPath);
                }

                // Fallback: try to generate thumbnail on the fly
                if (!loaded) {
                    loaded = getThumbnail(id, thumbPixels);
                }

                if (!loaded) {
                    logWarning() << "[CLIP] No thumbnail for: " << id;
                    continue;
                }

                auto embedding = clipEmbedder_.embed(thumbPixels);
                if (embedding.empty()) continue;

                {
                    lock_guard<mutex> lock(embMutex_);
                    completedEmbeddings_.push_back({id, std::move(embedding)});
                }
                embCompletedCount_ = ++done;
            }
            logNotice() << "[CLIP] Embedding done: " << done << "/" << ids.size();
            embThreadRunning_ = false;
        });
        embThread_.detach();
    }

    void startSPGenerationThread() {
        // Called with spMutex_ already held
        if (pendingSPGenerations_.empty() || spThreadRunning_) return;

        spThreadRunning_ = true;
        if (spThread_.joinable()) spThread_.join();

        vector<string> ids = std::move(pendingSPGenerations_);
        pendingSPGenerations_.clear();

        spThread_ = thread([this, ids = std::move(ids)]() {
            logNotice() << "[SmartPreview] Starting generation for " << ids.size() << " photos";
            int done = 0;
            for (const auto& id : ids) {
                // Check photo still exists and has a local path
                PhotoEntry* photo = nullptr;
                string localPath;
                string spPath;
                {
                    auto it = photos_.find(id);
                    if (it == photos_.end()) continue;
                    photo = &it->second;
                    localPath = photo->localPath;
                    spPath = smartPreviewPath(*photo);
                }
                if (localPath.empty() || !fs::exists(localPath) || spPath.empty()) continue;

                // Already exists?
                if (fs::exists(spPath)) {
                    lock_guard<mutex> lock(spMutex_);
                    completedSPGenerations_.push_back({id, spPath});
                    done++;
                    continue;
                }

                // Load RAW to F32
                Pixels rawF32;
                bool loaded = RawLoader::loadFloat(localPath, rawF32);
                if (!loaded) {
                    logWarning() << "[SmartPreview] Failed to load RAW: " << localPath;
                    continue;
                }

                // Encode
                if (SmartPreview::encode(rawF32, spPath)) {
                    lock_guard<mutex> lock(spMutex_);
                    completedSPGenerations_.push_back({id, spPath});
                    done++;
                }
            }
            logNotice() << "[SmartPreview] Generation done: " << done << "/" << ids.size();
            spThreadRunning_ = false;
        });
        spThread_.detach();
    }

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

    // Write XMP only if photo has a local path
    static void writeXmpSidecarIfLocal(const PhotoEntry& photo) {
        if (!photo.localPath.empty() && fs::exists(photo.localPath)) {
            writeXmpSidecar(photo.localPath, photo);
        }
    }
};
