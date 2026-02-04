#pragma once

// =============================================================================
// UploadQueue.h - Background upload thread with retry
// =============================================================================

#include <TrussC.h>
#include <tcxCurl.h>
#include <nlohmann/json.hpp>
#include <deque>
#include <mutex>

using namespace std;
using namespace tc;
using namespace tcx;

struct UploadTask {
    string photoId;
    string localPath;
    int retryCount = 0;
    static constexpr int MAX_RETRIES = 3;
};

struct UploadResult {
    string photoId;
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

    // Enqueue a photo for upload
    void enqueue(const string& photoId, const string& localPath) {
        lock_guard<mutex> lock(queueMutex_);
        // Skip if already queued
        for (const auto& t : pending_) {
            if (t.photoId == photoId) return;
        }
        pending_.push_back({photoId, localPath, 0});
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
                // Sleep outside of lock, use milliseconds
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // Attempt upload via path-based import
            auto res = client.post("/api/import",
                nlohmann::json{{"path", task.localPath}});

            UploadResult result;
            result.photoId = task.photoId;

            if (res.ok()) {
                result.success = true;
                logNotice() << "[UploadQueue] Uploaded: " << task.photoId;
                resultChannel_.send(std::move(result));
            } else {
                task.retryCount++;
                if (task.retryCount < UploadTask::MAX_RETRIES) {
                    logNotice() << "[UploadQueue] Retry " << task.retryCount
                                << "/" << UploadTask::MAX_RETRIES
                                << " for " << task.photoId;
                    // Re-enqueue at back
                    {
                        lock_guard<mutex> lock(queueMutex_);
                        pending_.push_back(task);
                    }
                    // Exponential backoff
                    std::this_thread::sleep_for(std::chrono::seconds(5 * task.retryCount));
                } else {
                    result.success = false;
                    result.error = res.error.empty()
                        ? "HTTP " + to_string(res.statusCode)
                        : res.error;
                    logWarning() << "[UploadQueue] Failed after retries: "
                                 << task.photoId << " - " << result.error;
                    resultChannel_.send(std::move(result));
                }
            }
        }
    }

private:
    string serverUrl_;
    string apiKey_;
    deque<UploadTask> pending_;
    mutex queueMutex_;
    ThreadChannel<UploadResult> resultChannel_;
};
