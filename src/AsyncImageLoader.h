#pragma once

// =============================================================================
// AsyncImageLoader.h - Background thread image loader
// =============================================================================

#include <TrussC.h>
#include <filesystem>
#include <tcxLibRaw.h>
using namespace std;
using namespace tc;

namespace fs = std::filesystem;

// Load request
struct LoadRequest {
    int id;             // Photo item ID (grid index)
    fs::path path;      // File path (used for direct file loading)
    string photoId;     // Photo ID (used with custom loader)
    int maxSize;        // Max dimension for thumbnail (0 = full size)
};

// Load result (Pixels, not Image - texture must be created on main thread)
struct LoadResult {
    int id;
    Pixels pixels;
    bool success = false;
};

// Custom thumbnail loader callback
using ThumbnailLoader = function<bool(const string& photoId, Pixels& outPixels)>;

// Async image loader using Thread + ThreadChannel
class AsyncImageLoader : public Thread {
public:
    AsyncImageLoader() = default;

    ~AsyncImageLoader() {
        stop();
    }

    // Set custom thumbnail loader (called instead of file-based loading)
    void setThumbnailLoader(ThumbnailLoader loader) {
        customLoader_ = std::move(loader);
    }

    // Start loader thread
    void start() {
        if (!isThreadRunning()) {
            requestChannel_.clear();
            startThread();
        }
    }

    // Stop loader thread
    void stop() {
        requestChannel_.close();
        waitForThread();
    }

    // Request load via custom loader (using photo ID)
    void requestLoad(int id, const string& photoId) {
        {
            lock_guard<mutex> lock(cancelMutex_);
            cancelledIds_.erase(id);
        }

        LoadRequest req;
        req.id = id;
        req.photoId = photoId;
        req.maxSize = 256;
        requestChannel_.send(std::move(req));
    }

    // Request load from file path (legacy / fallback)
    void requestLoad(int id, const fs::path& path, int maxSize = 0) {
        {
            lock_guard<mutex> lock(cancelMutex_);
            cancelledIds_.erase(id);
        }

        LoadRequest req;
        req.id = id;
        req.path = path;
        req.maxSize = maxSize;
        requestChannel_.send(std::move(req));
    }

    // Cancel pending request
    void cancelRequest(int id) {
        lock_guard<mutex> lock(cancelMutex_);
        cancelledIds_.insert(id);
    }

    // Check for completed loads (call from main thread in update)
    bool tryGetResult(LoadResult& result) {
        return resultChannel_.tryReceive(result);
    }

    // Get number of pending requests (approximate)
    size_t getPendingCount() const {
        return requestChannel_.size();
    }

protected:
    void threadedFunction() override {
        LoadRequest req;

        while (isThreadRunning()) {
            if (!requestChannel_.tryReceive(req, 100)) {
                continue;
            }

            // Check if cancelled
            {
                lock_guard<mutex> lock(cancelMutex_);
                if (cancelledIds_.count(req.id)) {
                    cancelledIds_.erase(req.id);
                    continue;
                }
            }

            // Load image
            LoadResult result;
            result.id = req.id;

            bool loaded = false;

            // Use custom loader if available and photoId is set
            if (customLoader_ && !req.photoId.empty()) {
                loaded = customLoader_(req.photoId, result.pixels);
            }
            // Fallback: file-based loading
            else if (!req.path.empty()) {
                if (RawLoader::isRawFile(req.path)) {
                    if (req.maxSize > 0) {
                        loaded = RawLoader::loadWithMaxSize(req.path, result.pixels, req.maxSize);
                    } else {
                        loaded = RawLoader::load(req.path, result.pixels);
                    }
                } else {
                    loaded = result.pixels.load(req.path);
                    if (loaded && req.maxSize > 0) {
                        int w = result.pixels.getWidth();
                        int h = result.pixels.getHeight();
                        if (w > req.maxSize || h > req.maxSize) {
                            float scale = (float)req.maxSize / max(w, h);
                            int newW = (int)(w * scale);
                            int newH = (int)(h * scale);
                            resizePixels(result.pixels, newW, newH);
                        }
                    }
                }
            }

            result.success = loaded;
            resultChannel_.send(std::move(result));
        }
    }

private:
    ThreadChannel<LoadRequest> requestChannel_;
    ThreadChannel<LoadResult> resultChannel_;
    ThumbnailLoader customLoader_;

    mutex cancelMutex_;
    unordered_set<int> cancelledIds_;

    // Simple resize (nearest neighbor)
    void resizePixels(Pixels& src, int newW, int newH) {
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
