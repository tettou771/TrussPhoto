#pragma once

// =============================================================================
// SyncEngine - cursor-based replication protocol (Phase A)
// =============================================================================
// Owns the "local catalog <-> server catalog" replication. Local UI always reads
// and writes its own library.db; the SyncEngine moves the diff in the background.
//
// Protocol (state-based, field-level last-write-wins):
//   push : POST /api/sync/push  - locally dirty rows (updatedAt/deletedAt advanced
//                                  past last_push_at). Server merges + assigns seq.
//   pull : GET  /api/changes    - rows with server_seq > pull_cursor, paged. Merged
//                                  locally with field-level LWW + tombstones.
//   sync = push then pull.
//
// Threading (mirrors PhotoProvider's produce/apply copy pattern):
//   - sync() runs on the background sync thread. push() only READS the photo map;
//     pull() only performs network I/O and buffers the raw changes.
//   - processResults() runs on the MAIN thread and is the only place that mutates
//     the in-memory photo map (via PhotoProvider::applyPulledChanges).

#include <tcxCurl.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <string>
#include "PhotoProvider.h"

using namespace std;
using namespace tcx;

class SyncEngine {
public:
    void setup(PhotoProvider* provider) { provider_ = provider; }

    // Configure the replication target. Empty url = local-only (sync is a no-op).
    void configure(const string& url, const string& apiKey) {
        client_.setBaseUrl(url);
        client_.setBearerToken(apiKey);
        hasServer_ = !url.empty();
    }

    bool hasServer() const { return hasServer_; }

    // --- Background thread ---

    // Full sync cycle: push local changes, then pull remote changes.
    // Buffers pulled changes for the main thread to apply (processResults).
    void sync() {
        if (!provider_ || !hasServer_) return;
        if (!client_.isReachable()) return;
        push();
        pull();
    }

    // --- Main thread ---

    // Apply buffered pull results to the photo map + DB. Returns number of entries
    // changed (caller decides whether to rebuild the grid / redraw).
    int processResults() {
        vector<nlohmann::json> changes;
        unordered_set<string> serverIds;
        bool haveServerIds = false;
        int64_t cursor = 0;
        {
            lock_guard<mutex> lock(mutex_);
            if (!hasPending_) return 0;
            changes.swap(pendingChanges_);
            serverIds.swap(pendingServerIds_);
            haveServerIds = pendingHaveServerIds_;
            cursor = pendingCursor_;
            hasPending_ = false;
        }
        if (!provider_) return 0;
        return provider_->applyPulledChanges(changes, serverIds, haveServerIds, cursor);
    }

private:
    static int64_t parseI64(const string& s, int64_t def = 0) {
        try { return s.empty() ? def : stoll(s); } catch (...) { return def; }
    }

    // Push locally-dirty rows to the server.
    void push() {
        int64_t lastPushAt = parseI64(provider_->db().getSyncState("last_push_at", "0"));
        int64_t pushStart = PhotoProvider::nowMs();

        auto rows = provider_->collectDirtyForPush(lastPushAt);
        if (rows.empty()) {
            // Nothing dirty; still advance the watermark.
            provider_->db().setSyncState("last_push_at", to_string(pushStart));
            return;
        }

        nlohmann::json body = rows; // JSON array of sync rows
        auto res = client_.request("POST", "/api/sync/push",
                                   body.dump(-1, ' ', false,
                                       nlohmann::json::error_handler_t::replace),
                                   "application/json");
        if (res.ok()) {
            provider_->db().setSyncState("last_push_at", to_string(pushStart));
            logNotice() << "[Sync] Pushed " << rows.size() << " dirty rows";
        } else {
            logWarning() << "[Sync] Push failed (HTTP " << res.statusCode << ")";
        }
    }

    // Pull the change feed since the persisted cursor, paging until drained.
    void pull() {
        int64_t cursor = parseI64(provider_->db().getSyncState("pull_cursor", "0"));
        int64_t maxSeq = cursor;
        vector<nlohmann::json> all;

        const int kMaxPages = 1000; // safety bound
        for (int page = 0; page < kMaxPages; page++) {
            auto res = client_.get("/api/changes?since=" + to_string(cursor) + "&limit=500");
            if (!res.ok()) {
                logWarning() << "[Sync] Pull failed (HTTP " << res.statusCode << ")";
                break;
            }
            auto data = res.json();
            if (!data.contains("changes") || !data["changes"].is_array()) break;

            for (const auto& c : data["changes"]) {
                all.push_back(c);
                int64_t s = c.value("serverSeq", (int64_t)0);
                if (s > maxSeq) maxSeq = s;
            }

            int64_t newCursor = data.value("maxSeq", cursor);
            bool hasMore = data.value("hasMore", false);
            if (newCursor <= cursor) break; // no progress guard
            cursor = newCursor;
            if (!hasMore) break;
        }

        // Presence list (originals the server holds) for badge reconciliation.
        unordered_set<string> serverIds;
        bool haveServerIds = false;
        auto pres = client_.get("/api/photos");
        if (pres.ok()) {
            auto data = pres.json();
            if (data.contains("photos") && data["photos"].is_array()) {
                haveServerIds = true;
                for (const auto& p : data["photos"]) {
                    string id = p.value("id", string(""));
                    if (!id.empty()) serverIds.insert(id);
                }
            }
        }

        {
            lock_guard<mutex> lock(mutex_);
            pendingChanges_ = std::move(all);
            pendingServerIds_ = std::move(serverIds);
            pendingHaveServerIds_ = haveServerIds;
            pendingCursor_ = maxSeq;
            hasPending_ = true;
        }
    }

    PhotoProvider* provider_ = nullptr;
    HttpClient client_;
    bool hasServer_ = false;

    // Produce/apply buffer (bg produces, main applies)
    mutex mutex_;
    bool hasPending_ = false;
    vector<nlohmann::json> pendingChanges_;
    unordered_set<string> pendingServerIds_;
    bool pendingHaveServerIds_ = false;
    int64_t pendingCursor_ = 0;
};
