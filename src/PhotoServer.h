#pragma once

// =============================================================================
// PhotoServer.h - Crow HTTP server exposing PhotoProvider via REST API
// =============================================================================
// Runs in a background thread. Compatible with TrussPhotoServer API.
// Used only in --server mode.

#include <tcxCrow.h>
#include <tcxLibRaw.h>
#include <fstream>
#include <thread>
#include <atomic>
#include "PhotoProvider.h"

using namespace std;
using namespace tc;
using namespace tcx;

class PhotoServer {
public:
    void setup(PhotoProvider& provider, const string& thumbnailDir) {
        provider_ = &provider;
        thumbnailDir_ = thumbnailDir;
    }

    void start(int port, const string& apiKey) {
        apiKey_ = apiKey;

        // --- Health check (no auth) ---
        TCX_CROW_ROUTE(app_, "/api/health")([]() {
            return tcx::crow::jsonResponse({{"status", "ok"}});
        });

        // --- Photo list ---
        TCX_CROW_ROUTE(app_, "/api/photos")
        ([this](const ::crow::request& req) {
            if (!authorize(req)) return tcx::crow::errorResponse("Unauthorized", 401);

            // Presence list = rows for which the server holds an original payload.
            // Excludes tombstones and metadata-only catalog rows (pushed but not yet
            // uploaded) so clients only flip LocalOnly->Synced when the original exists.
            json photosJson = json::array();
            for (const auto& [id, photo] : provider_->photos()) {
                if (photo.deletedAt > 0) continue;
                if (photo.localPath.empty()) continue;
                photosJson.push_back({
                    {"id", photo.id},
                    {"filename", photo.filename},
                    {"fileSize", photo.fileSize},
                    {"camera", photo.camera},
                    {"width", photo.width},
                    {"height", photo.height}
                });
            }
            return tcx::crow::jsonResponse({{"photos", photosJson}, {"count", photosJson.size()}});
        });

        // --- Change feed (cursor sync) ---
        TCX_CROW_ROUTE(app_, "/api/changes")
        ([this](const ::crow::request& req) {
            if (!authorize(req)) return tcx::crow::errorResponse("Unauthorized", 401);

            int64_t since = 0;
            int limit = 500;
            if (const char* s = req.url_params.get("since")) {
                try { since = stoll(s); } catch (...) { since = 0; }
            }
            if (const char* l = req.url_params.get("limit")) {
                try { limit = stoi(l); } catch (...) { limit = 500; }
            }
            if (limit < 1) limit = 1;
            if (limit > 5000) limit = 5000;

            return tcx::crow::jsonResponse(provider_->buildChanges(since, limit));
        });

        // --- Sync push (client dirty rows -> field-level LWW merge) ---
        TCX_CROW_ROUTE(app_, "/api/sync/push").methods("POST"_method)
        ([this](const ::crow::request& req) {
            if (!authorize(req)) return tcx::crow::errorResponse("Unauthorized", 401);

            auto rows = tcx::crow::fromJson(req.body);
            if (!rows.is_array()) {
                return tcx::crow::errorResponse("Expected a JSON array of rows", 400);
            }

            provider_->applyServerPush(rows);
            return tcx::crow::jsonResponse({
                {"status", "ok"},
                {"applied", rows.size()}
            });
        });

        // --- Single photo info ---
        TCX_CROW_ROUTE(app_, "/api/photos/<string>")
        ([this](const ::crow::request& req, const string& id) {
            if (!authorize(req)) return tcx::crow::errorResponse("Unauthorized", 401);

            auto* photo = provider_->getPhoto(id);
            if (!photo) return tcx::crow::errorResponse("Photo not found", 404);

            nlohmann::json j;
            to_json(j, *photo);
            return tcx::crow::jsonResponse(j);
        });

        // --- Thumbnail ---
        TCX_CROW_ROUTE(app_, "/api/photos/<string>/thumbnail")
        ([this](const ::crow::request& req, const string& id) {
            if (!authorize(req)) return ::crow::response(401, "Unauthorized");

            auto* photo = provider_->getPhoto(id);
            if (!photo) return ::crow::response(404, "Photo not found");

            // Try cached thumbnail
            string thumbPath = photo->localThumbnailPath;
            if (thumbPath.empty() || !fs::exists(thumbPath)) {
                // Try generating thumbnail on the fly
                Pixels thumbPixels;
                if (provider_->getThumbnail(id, thumbPixels)) {
                    thumbPath = photo->localThumbnailPath;
                }
            }

            if (thumbPath.empty() || !fs::exists(thumbPath)) {
                return ::crow::response(404, "Thumbnail not available");
            }

            ifstream file(thumbPath, ios::binary | ios::ate);
            if (!file) return ::crow::response(500, "Failed to read thumbnail");

            auto size = file.tellg();
            file.seekg(0, ios::beg);
            string buffer(size, '\0');
            file.read(&buffer[0], size);

            ::crow::response res(200, buffer);
            res.set_header("Content-Type", "image/jpeg");
            return res;
        });

        // --- Import file ---
        TCX_CROW_ROUTE(app_, "/api/import").methods("POST"_method)
        ([this](const ::crow::request& req) {
            if (!authorize(req)) return tcx::crow::errorResponse("Unauthorized", 401);

            auto data = tcx::crow::fromJson(req.body);
            string rawPath = data.value("path", string(""));

            if (rawPath.empty()) {
                return tcx::crow::errorResponse("Missing 'path' in request body", 400);
            }
            if (!fs::exists(rawPath)) {
                return tcx::crow::errorResponse("File not found: " + rawPath, 404);
            }

            // Scan the file's parent folder (reuses PhotoProvider logic)
            fs::path folder = fs::path(rawPath).parent_path();
            provider_->scanFolder(folder.string());

            // Find the photo by path
            string fname = fs::path(rawPath).filename().string();
            auto fsize = fs::file_size(rawPath);
            string id = fname + "_" + to_string(fsize);

            auto* photo = provider_->getPhoto(id);
            if (!photo) {
                return tcx::crow::errorResponse("Failed to import file", 500);
            }

            // Assign a fresh server_seq so the new row appears in the change feed.
            provider_->bumpServerSeq(id);

            return tcx::crow::jsonResponse({
                {"id", id},
                {"filename", photo->filename},
                {"width", photo->width},
                {"height", photo->height},
                {"message", "Photo imported successfully"}
            }, 201);
        });

        // --- Update metadata ---
        TCX_CROW_ROUTE(app_, "/api/photos/<string>/metadata").methods("PATCH"_method)
        ([this](const ::crow::request& req, const string& id) {
            if (!authorize(req)) return tcx::crow::errorResponse("Unauthorized", 401);

            auto* photo = provider_->getPhoto(id);
            if (!photo) return tcx::crow::errorResponse("Photo not found", 404);

            auto data = tcx::crow::fromJson(req.body);

            if (data.contains("rating")) {
                provider_->setRating(id, data["rating"].get<int>());
            }
            if (data.contains("colorLabel")) {
                provider_->setColorLabel(id, data["colorLabel"].get<string>());
            }
            if (data.contains("flag")) {
                provider_->setFlag(id, data["flag"].get<int>());
            }
            if (data.contains("memo")) {
                provider_->setMemo(id, data["memo"].get<string>());
            }
            if (data.contains("tags")) {
                provider_->setTags(id, data["tags"].get<string>());
            }

            // Mutation must appear in the change feed for other nodes.
            provider_->bumpServerSeq(id);

            // Return updated entry
            photo = provider_->getPhoto(id);
            nlohmann::json j;
            to_json(j, *photo);
            return tcx::crow::jsonResponse(j);
        });

        // --- Delete photo ---
        TCX_CROW_ROUTE(app_, "/api/photos/<string>").methods("DELETE"_method)
        ([this](const ::crow::request& req, const string& id) {
            if (!authorize(req)) return tcx::crow::errorResponse("Unauthorized", 401);

            auto* photo = provider_->getPhoto(id);
            if (!photo) return tcx::crow::errorResponse("Photo not found", 404);

            // Tombstone (keep the row so the deletion propagates via the change feed),
            // then assign a fresh server_seq. deletePhotos() removes cache payloads.
            provider_->deletePhotos({id});
            provider_->bumpServerSeq(id);
            return tcx::crow::jsonResponse({{"message", "Photo deleted"}});
        });

        // Start server thread
        running_ = true;
        threadDone_ = false;
        serverThread_ = thread([this, port]() {
            app_.port(port).multithreaded().run();
            threadDone_ = true;
        });

        logNotice() << "[PhotoServer] Started on port " << port;
    }

    void stop() {
        if (running_) {
            logNotice() << "[PhotoServer] Stopping...";
            app_.stop();
            running_ = false;
            // Wait briefly for Crow to finish, then detach
            // Crow's ASIO can take time to drain; don't block exit
            auto start = chrono::steady_clock::now();
            while (!threadDone_ && chrono::steady_clock::now() - start < chrono::seconds(2)) {
                this_thread::sleep_for(chrono::milliseconds(50));
            }
            if (serverThread_.joinable()) {
                if (threadDone_) {
                    serverThread_.join();
                } else {
                    serverThread_.detach();
                    logWarning() << "[PhotoServer] Force detached (Crow slow to stop)";
                }
            }
            logNotice() << "[PhotoServer] Stopped";
        }
    }

    bool isRunning() const { return running_; }

private:
    ::crow::SimpleApp app_;
    thread serverThread_;
    atomic<bool> running_{false};
    atomic<bool> threadDone_{false};
    PhotoProvider* provider_ = nullptr;
    string thumbnailDir_;
    string apiKey_;

    // Bearer token auth check
    bool authorize(const ::crow::request& req) const {
        if (apiKey_.empty()) return true;
        string auth = req.get_header_value("Authorization");
        return auth == "Bearer " + apiKey_;
    }
};
