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
        CROW_ROUTE(app_, "/api/health")([]() {
            return tcx::jsonResponse({{"status", "ok"}});
        });

        // --- Photo list ---
        CROW_ROUTE(app_, "/api/photos")
        ([this](const crow::request& req) {
            if (!authorize(req)) return tcx::errorResponse("Unauthorized", 401);

            json photosJson = json::array();
            for (const auto& [id, photo] : provider_->photos()) {
                photosJson.push_back({
                    {"id", photo.id},
                    {"filename", photo.filename},
                    {"fileSize", photo.fileSize},
                    {"camera", photo.camera},
                    {"width", photo.width},
                    {"height", photo.height}
                });
            }
            return tcx::jsonResponse({{"photos", photosJson}, {"count", photosJson.size()}});
        });

        // --- Single photo info ---
        CROW_ROUTE(app_, "/api/photos/<string>")
        ([this](const crow::request& req, const string& id) {
            if (!authorize(req)) return tcx::errorResponse("Unauthorized", 401);

            auto* photo = provider_->getPhoto(id);
            if (!photo) return tcx::errorResponse("Photo not found", 404);

            nlohmann::json j;
            to_json(j, *photo);
            return tcx::jsonResponse(j);
        });

        // --- Thumbnail ---
        CROW_ROUTE(app_, "/api/photos/<string>/thumbnail")
        ([this](const crow::request& req, const string& id) {
            if (!authorize(req)) return crow::response(401, "Unauthorized");

            auto* photo = provider_->getPhoto(id);
            if (!photo) return crow::response(404, "Photo not found");

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
                return crow::response(404, "Thumbnail not available");
            }

            ifstream file(thumbPath, ios::binary | ios::ate);
            if (!file) return crow::response(500, "Failed to read thumbnail");

            auto size = file.tellg();
            file.seekg(0, ios::beg);
            string buffer(size, '\0');
            file.read(&buffer[0], size);

            crow::response res(200, buffer);
            res.set_header("Content-Type", "image/jpeg");
            return res;
        });

        // --- Import file ---
        CROW_ROUTE(app_, "/api/import").methods("POST"_method)
        ([this](const crow::request& req) {
            if (!authorize(req)) return tcx::errorResponse("Unauthorized", 401);

            auto data = tcx::fromJson(req.body);
            string rawPath = data.value("path", string(""));

            if (rawPath.empty()) {
                return tcx::errorResponse("Missing 'path' in request body", 400);
            }
            if (!fs::exists(rawPath)) {
                return tcx::errorResponse("File not found: " + rawPath, 404);
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
                return tcx::errorResponse("Failed to import file", 500);
            }

            return tcx::jsonResponse({
                {"id", id},
                {"filename", photo->filename},
                {"width", photo->width},
                {"height", photo->height},
                {"message", "Photo imported successfully"}
            }, 201);
        });

        // --- Delete photo ---
        CROW_ROUTE(app_, "/api/photos/<string>").methods("DELETE"_method)
        ([this](const crow::request& req, const string& id) {
            if (!authorize(req)) return tcx::errorResponse("Unauthorized", 401);

            auto* photo = provider_->getPhoto(id);
            if (!photo) return tcx::errorResponse("Photo not found", 404);

            // Remove thumbnail cache
            if (!photo->localThumbnailPath.empty() && fs::exists(photo->localThumbnailPath)) {
                fs::remove(photo->localThumbnailPath);
            }

            provider_->removePhoto(id);
            return tcx::jsonResponse({{"message", "Photo deleted"}});
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
    crow::SimpleApp app_;
    thread serverThread_;
    atomic<bool> running_{false};
    atomic<bool> threadDone_{false};
    PhotoProvider* provider_ = nullptr;
    string thumbnailDir_;
    string apiKey_;

    // Bearer token auth check
    bool authorize(const crow::request& req) const {
        if (apiKey_.empty()) return true;
        string auth = req.get_header_value("Authorization");
        return auth == "Bearer " + apiKey_;
    }
};
