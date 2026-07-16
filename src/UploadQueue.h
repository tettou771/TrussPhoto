#pragma once

// =============================================================================
// UploadQueue.h - Background upload thread with retry
// =============================================================================
// Transfers real payload bytes to the server (Phase B):
//   - Original: PUT /api/photos/<id>/original  (raw bytes + X-Filename header)
//   - Preview:  PUT /api/photos/<id>/preview   (smart preview JXL bytes)
//   - Thumbnail:PUT /api/photos/<id>/thumbnail (JPEG bytes)
// No multipart — raw body via HttpClient::put(). Large RAWs go through memory
// (streaming is a future optimization).

#include <TrussC.h>
#include <tcxCurl.h>
#include <nlohmann/json.hpp>
#include <deque>
#include <mutex>
#include <fstream>

using namespace std;
using namespace tc;
using namespace tcx;

enum class UploadJobType {
    Original,   // RAW/JPEG original — drives LocalOnly -> Synced
    Preview,    // smart preview (JXL) distribution
    Thumbnail   // edit-baked thumbnail (JPEG) distribution
};

struct UploadTask {
    string photoId;
    string localPath;                 // file whose bytes are uploaded
    string filename;                  // basename, sent as X-Filename (Original)
    UploadJobType type = UploadJobType::Original;
    int retryCount = 0;
    static constexpr int MAX_RETRIES = 3;
};

struct UploadResult {
    string photoId;
    UploadJobType type = UploadJobType::Original;
    bool success = false;
    string error;
};

class UploadQueue : public Thread {
public:
    UploadQueue() = default;

    ~UploadQueue() {
        stop();
    }

    void setServerUrl(const string& url) {
        serverUrl_ = url;
    }

    void setApiKey(const string& key) {
        apiKey_ = key;
    }

    // Enqueue an original for upload (LocalOnly -> Synced on success)
    void enqueue(const string& photoId, const string& localPath) {
        enqueueJob({photoId, localPath, fs::path(localPath).filename().string(),
                    UploadJobType::Original, 0});
    }

    // Enqueue a smart preview (JXL) for distribution to the server
    void enqueuePreview(const string& photoId, const string& spPath) {
        enqueueJob({photoId, spPath, "", UploadJobType::Preview, 0});
    }

    // Enqueue a thumbnail (JPEG) for distribution to the server
    void enqueueThumbnail(const string& photoId, const string& thumbPath) {
        enqueueJob({photoId, thumbPath, "", UploadJobType::Thumbnail, 0});
    }

    // Start upload thread
    void start() {
        if (!isThreadRunning()) {
            startThread();
        }
    }

    // Stop upload thread
    void stop() {
        resultChannel_.close();
        waitForThread();
    }

    // Get upload result (call from main thread)
    bool tryGetResult(UploadResult& result) {
        return resultChannel_.tryReceive(result);
    }

    // Pending count
    size_t getPendingCount() {
        lock_guard<mutex> lock(queueMutex_);
        return pending_.size();
    }

protected:
    void threadedFunction() override {
        HttpClient client;
        client.setBaseUrl(serverUrl_);
        client.setBearerToken(apiKey_);
        client.setTimeout(600); // large RAW uploads over slow links

        while (isThreadRunning()) {
            UploadTask task;
            bool hasTask = false;

            // Get next task (lock scope is minimal)
            {
                lock_guard<mutex> lock(queueMutex_);
                if (!pending_.empty()) {
                    task = pending_.front();
                    pending_.pop_front();
                    hasTask = true;
                }
            }

            if (!hasTask) {
                // Sleep outside of lock
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            UploadResult result;
            result.photoId = task.photoId;
            result.type = task.type;

            // Read file bytes
            string body;
            if (!readFile(task.localPath, body)) {
                result.success = false;
                result.error = "read failed: " + task.localPath;
                logWarning() << "[UploadQueue] " << result.error;
                resultChannel_.send(std::move(result));
                continue;
            }

            // Build request per job type
            string endpoint;
            string contentType;
            switch (task.type) {
                case UploadJobType::Original:
                    endpoint = "/api/photos/" + task.photoId + "/original";
                    contentType = "application/octet-stream";
                    client.addHeader("X-Filename", task.filename);
                    break;
                case UploadJobType::Preview:
                    endpoint = "/api/photos/" + task.photoId + "/preview";
                    contentType = "image/jxl";
                    break;
                case UploadJobType::Thumbnail:
                    endpoint = "/api/photos/" + task.photoId + "/thumbnail";
                    contentType = "image/jpeg";
                    break;
            }

            auto res = client.put(endpoint, body, contentType);

            if (res.ok()) {
                result.success = true;
                logNotice() << "[UploadQueue] Uploaded "
                            << jobTypeName(task.type) << ": " << task.photoId;
                resultChannel_.send(std::move(result));
            } else {
                task.retryCount++;
                if (task.retryCount < UploadTask::MAX_RETRIES) {
                    logNotice() << "[UploadQueue] Retry " << task.retryCount
                                << "/" << UploadTask::MAX_RETRIES
                                << " for " << jobTypeName(task.type)
                                << " " << task.photoId;
                    {
                        lock_guard<mutex> lock(queueMutex_);
                        pending_.push_back(task);
                    }
                    // Exponential backoff (outside the lock)
                    std::this_thread::sleep_for(std::chrono::seconds(5 * task.retryCount));
                } else {
                    result.success = false;
                    result.error = res.error.empty()
                        ? "HTTP " + to_string(res.statusCode)
                        : res.error;
                    logWarning() << "[UploadQueue] Failed after retries: "
                                 << jobTypeName(task.type) << " "
                                 << task.photoId << " - " << result.error;
                    resultChannel_.send(std::move(result));
                }
            }
        }
    }

private:
    void enqueueJob(const UploadTask& task) {
        lock_guard<mutex> lock(queueMutex_);
        // Skip if the same (photo, type) is already queued
        for (const auto& t : pending_) {
            if (t.photoId == task.photoId && t.type == task.type) return;
        }
        pending_.push_back(task);
    }

    static bool readFile(const string& path, string& out) {
        ifstream f(path, ios::binary | ios::ate);
        if (!f) return false;
        auto size = f.tellg();
        if (size < 0) return false;
        f.seekg(0, ios::beg);
        out.resize((size_t)size);
        if (size > 0) f.read(&out[0], size);
        return (bool)f;
    }

    static const char* jobTypeName(UploadJobType t) {
        switch (t) {
            case UploadJobType::Original:  return "original";
            case UploadJobType::Preview:   return "preview";
            case UploadJobType::Thumbnail: return "thumbnail";
        }
        return "?";
    }

    string serverUrl_;
    string apiKey_;
    deque<UploadTask> pending_;
    mutex queueMutex_;
    ThreadChannel<UploadResult> resultChannel_;
};
