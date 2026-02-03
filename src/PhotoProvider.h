#pragma once

// =============================================================================
// PhotoProvider - Abstraction layer for photo access (local + server)
// =============================================================================

#include <TrussC.h>
#include <tcxLibRaw.h>
#include <tcxCurl.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <fstream>
#include <mutex>

using namespace std;
using namespace tc;
using namespace tcx;

namespace fs = std::filesystem;

// Sync state of a photo
enum class SyncState {
    LocalOnly = 0,   // Only on local disk (not yet uploaded)
    Syncing   = 1,   // Upload in progress
    Synced    = 2,   // Both local and server
    ServerOnly = 3   // Only on server (local RAW deleted)
};

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

    // Metadata
    string cameraMake;
    string camera;
    string lens;
    string lensMake;
    int width = 0;
    int height = 0;
    bool isRaw = false;
    string creativeStyle;
    float focalLength = 0;
    float aperture = 0;
    float iso = 0;

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
        {"cameraMake", e.cameraMake},
        {"camera", e.camera},
        {"lens", e.lens},
        {"lensMake", e.lensMake},
        {"width", e.width},
        {"height", e.height},
        {"isRaw", e.isRaw},
        {"creativeStyle", e.creativeStyle},
        {"focalLength", e.focalLength},
        {"aperture", e.aperture},
        {"iso", e.iso},
        {"syncState", static_cast<int>(e.syncState)}
    };
}

inline void from_json(const nlohmann::json& j, PhotoEntry& e) {
    e.id = j.value("id", string(""));
    e.filename = j.value("filename", string(""));
    e.fileSize = j.value("fileSize", (uintmax_t)0);
    e.dateTimeOriginal = j.value("dateTimeOriginal", string(""));
    e.localPath = j.value("localPath", string(""));
    e.localThumbnailPath = j.value("localThumbnailPath", string(""));
    e.cameraMake = j.value("cameraMake", string(""));
    e.camera = j.value("camera", string(""));
    e.lens = j.value("lens", string(""));
    e.lensMake = j.value("lensMake", string(""));
    e.width = j.value("width", 0);
    e.height = j.value("height", 0);
    e.isRaw = j.value("isRaw", false);
    e.creativeStyle = j.value("creativeStyle", string(""));
    e.focalLength = j.value("focalLength", 0.0f);
    e.aperture = j.value("aperture", 0.0f);
    e.iso = j.value("iso", 0.0f);

    int state = j.value("syncState", 0);
    e.syncState = static_cast<SyncState>(state);
    // Syncing state doesn't survive restart - revert to LocalOnly
    if (e.syncState == SyncState::Syncing) {
        e.syncState = SyncState::LocalOnly;
    }
}

// Photo provider - manages local + server photos
class PhotoProvider {
public:
    PhotoProvider() = default;

    // --- Configuration ---

    void setServerUrl(const string& url) {
        client_.setBaseUrl(url);
        serverChecked_ = false;
    }

    void setThumbnailCacheDir(const string& dir) {
        thumbnailCacheDir_ = dir;
        fs::create_directories(dir);
    }

    void setLibraryPath(const string& path) {
        libraryPath_ = path;
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

    // --- Library persistence ---

    void saveLibrary() {
        if (libraryPath_.empty()) return;

        nlohmann::json j;
        j["photos"] = nlohmann::json::array();
        for (const auto& [id, entry] : photos_) {
            j["photos"].push_back(entry);
        }

        ofstream file(libraryPath_);
        if (file) {
            file << j.dump(2);
            dirty_ = false;
            logNotice() << "[PhotoProvider] Library saved: " << photos_.size() << " photos";
        }
    }

    bool loadLibrary() {
        if (libraryPath_.empty()) return false;
        if (!fs::exists(libraryPath_)) return false;

        ifstream file(libraryPath_);
        if (!file) return false;

        try {
            nlohmann::json j;
            file >> j;

            for (const auto& photoJson : j["photos"]) {
                PhotoEntry entry = photoJson.get<PhotoEntry>();
                photos_[entry.id] = entry;
            }
            logNotice() << "[PhotoProvider] Library loaded: " << photos_.size() << " photos";
            return true;
        } catch (const exception& e) {
            logWarning() << "[PhotoProvider] Failed to load library: " << e.what();
            return false;
        }
    }

    void markDirty() { dirty_ = true; }

    void saveIfDirty() {
        if (dirty_) {
            saveLibrary();
        }
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

            // Extract metadata from RAW files
            if (photo.isRaw) {
                RawMetadata meta;
                if (RawLoader::extractMetadata(entry.path(), meta)) {
                    photo.cameraMake = meta.cameraMake;
                    photo.camera = meta.cameraModel;
                    photo.lens = meta.lens;
                    photo.lensMake = meta.lensMake;
                    photo.width = meta.width;
                    photo.height = meta.height;
                    photo.creativeStyle = meta.creativeStyle;
                    photo.focalLength = meta.focalLength;
                    photo.aperture = meta.aperture;
                    photo.iso = meta.iso;
                }
            }

            photos_[id] = photo;
            added++;

            // Queue file copy if library folder is configured and source is outside it
            if (!libraryFolder_.empty()) {
                fs::path srcParent = fs::path(path).parent_path();
                fs::path libPath = fs::path(libraryFolder_);
                if (srcParent != libPath) {
                    lock_guard<mutex> lock(copyMutex_);
                    pendingCopies_.push_back({id, path, (libPath / fname).string()});
                }
            }
        }

        if (added > 0) dirty_ = true;
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
        for (const auto& p : data["photos"]) {
            string id = p.value("id", string(""));
            if (id.empty()) continue;
            serverIds.insert(id);

            if (photos_.count(id)) {
                // Already known locally
                if (photos_[id].syncState == SyncState::LocalOnly) {
                    photos_[id].syncState = SyncState::Synced;
                    dirty_ = true;
                }
            } else {
                // Server-only photo
                PhotoEntry photo;
                photo.id = id;
                photo.filename = p.value("filename", string(""));
                photo.fileSize = p.value("fileSize", (uintmax_t)0);
                photo.width = p.value("width", 0);
                photo.height = p.value("height", 0);
                photo.syncState = SyncState::ServerOnly;
                photos_[id] = photo;
                dirty_ = true;
            }
        }

        // Revert Synced photos not found on server back to LocalOnly
        for (auto& [id, photo] : photos_) {
            if (photo.syncState == SyncState::Synced && !serverIds.count(id)) {
                photo.syncState = SyncState::LocalOnly;
                dirty_ = true;
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
                string cachePath = thumbnailCacheDir_ + "/" + id + ".jpg";
                ofstream file(cachePath, ios::binary);
                if (file) {
                    file.write(res.body.data(), res.body.size());
                    file.close();
                    photo.localThumbnailPath = cachePath;
                    dirty_ = true;

                    if (outPixels.load(cachePath)) {
                        return true;
                    }
                }
            }
        }

        // 3. Fallback: decode from local file
        if (!photo.localPath.empty() && fs::exists(photo.localPath)) {
            if (photo.isRaw) {
                if (RawLoader::loadWithMaxSize(photo.localPath, outPixels, 256)) {
                    saveThumbnailCache(id, photo, outPixels);
                    return true;
                }
            } else {
                Pixels full;
                if (full.load(photo.localPath)) {
                    int w = full.getWidth();
                    int h = full.getHeight();
                    if (w > 256 || h > 256) {
                        float scale = 256.0f / max(w, h);
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
            dirty_ = true;
            return true;
        }

        photo.syncState = SyncState::LocalOnly;
        return false;
    }

    // --- Accessors ---

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
                dirty_ = true;
            }
        }
        completedCopies_.clear();
    }

    bool hasPendingCopies() const {
        lock_guard<mutex> lock(copyMutex_);
        return !pendingCopies_.empty() || copyThreadRunning_;
    }

private:
    HttpClient client_;
    unordered_map<string, PhotoEntry> photos_;
    string thumbnailCacheDir_;
    string libraryPath_;
    string libraryFolder_;
    bool serverReachable_ = false;
    bool serverChecked_ = false;
    bool dirty_ = false;

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

    void saveThumbnailCache(const string& id, PhotoEntry& photo, const Pixels& pixels) {
        if (thumbnailCacheDir_.empty()) return;
        string cachePath = thumbnailCacheDir_ + "/" + id + ".jpg";
        const_cast<Pixels&>(pixels).save(cachePath);
        photo.localThumbnailPath = cachePath;
        dirty_ = true;
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
