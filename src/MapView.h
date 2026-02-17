#pragma once

// =============================================================================
// MapView.h - Slippy map view with OpenStreetMap tiles and GPS photo pins
// =============================================================================

#include <TrussC.h>
#include <tcxCurl.h>
#include "PhotoEntry.h"
#include "ViewContainer.h"
#include "FolderTree.h"  // for PlainScrollContainer, loadJapaneseFont
#include <deque>
#include <thread>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <cmath>

using namespace std;
using namespace tc;
using namespace tcx;

namespace fs = std::filesystem;

class MapView : public ViewContainer {
public:
    using Ptr = shared_ptr<MapView>;

    // Set photos to display as pins (filters to GPS-only)
    void setPhotos(const vector<PhotoEntry>& photos, const vector<string>& ids) {
        pins_.clear();
        for (size_t i = 0; i < photos.size(); i++) {
            if (photos[i].hasGps()) {
                pins_.push_back({photos[i].latitude, photos[i].longitude, (int)i, ids[i]});
            }
        }
    }

    // Set tile disk cache directory
    void setTileCacheDir(const string& dir) {
        tileCacheDir_ = dir;
        if (!dir.empty()) fs::create_directories(dir);
    }

    // Fit view to show all pins
    void fitBounds() {
        if (pins_.empty()) {
            centerLat_ = 35.68;
            centerLon_ = 139.77;
            zoom_ = 5;
            return;
        }

        double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
        for (const auto& pin : pins_) {
            minLat = min(minLat, pin.lat);
            maxLat = max(maxLat, pin.lat);
            minLon = min(minLon, pin.lon);
            maxLon = max(maxLon, pin.lon);
        }

        centerLat_ = (minLat + maxLat) / 2.0;
        centerLon_ = (minLon + maxLon) / 2.0;

        // Find zoom level that fits all pins
        float w = getWidth();
        float h = getHeight();
        if (w < 1 || h < 1) { zoom_ = 10; return; }

        for (int z = 18; z >= 1; z--) {
            auto pMin = latLonToPixel(maxLat, minLon, z);  // top-left
            auto pMax = latLonToPixel(minLat, maxLon, z);  // bottom-right
            float spanX = pMax.x - pMin.x;
            float spanY = pMax.y - pMin.y;
            if (spanX < w * 0.8f && spanY < h * 0.8f) {
                zoom_ = z;
                return;
            }
        }
        zoom_ = 1;
    }

    // Callbacks
    function<void(int index, const string& photoId)> onPinClick;        // single click on pin
    function<void(int index, const string& photoId)> onPinDoubleClick;  // double click on pin
    function<void()> onRedraw;

    void setup() override {
        enableEvents();
        loadJapaneseFont(font_, 12);
        loadJapaneseFont(fontSmall_, 10);

        tileClient_.addHeader("User-Agent", "TrussPhoto/1.0");
    }

    void update() override {
        // Process completed tile downloads on main thread
        bool hadResults = false;
        {
            lock_guard<mutex> lock(tileMutex_);
            for (auto& result : tileResults_) {
                TileKey key{result.z, result.x, result.y};
                if (result.pixels.isAllocated()) {
                    Texture tex;
                    tex.allocate(result.pixels, TextureUsage::Immutable, false);
                    tileCache_[key] = std::move(tex);
                    tileFailed_.erase(key);
                } else {
                    tileFailed_[key]++;
                }
                tileLoading_.erase(key);
            }
            hadResults = !tileResults_.empty();
            tileResults_.clear();
        }

        if (hadResults && onRedraw) onRedraw();

        // Restart tile thread if it stopped but queue still has items (race condition fix)
        if (!tileThreadRunning_ && !tileThreadStop_) {
            bool hasQueue;
            {
                lock_guard<mutex> lock(tileMutex_);
                hasQueue = !tileQueue_.empty();
            }
            if (hasQueue) startTileThread();
        }
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.12f, 0.12f, 0.14f);
        fill();
        drawRect(0, 0, w, h);

        setClipping(true);

        // Fractional zoom: tiles are fetched at integer zoom, scaled for display
        int tileZoom = clamp((int)floor(zoom_), 1, 19);
        double tileScale = pow(2.0, zoom_ - tileZoom);
        double tileSize = 256.0 * tileScale;

        // Center pixel coordinates (at fractional zoom level)
        auto centerPx = latLonToPixel(centerLat_, centerLon_, zoom_);

        // Visible pixel range
        float halfW = w / 2.0f;
        float halfH = h / 2.0f;
        float left = centerPx.x - halfW;
        float top = centerPx.y - halfH;
        float right = centerPx.x + halfW;
        float bottom = centerPx.y + halfH;

        int maxTile = (1 << tileZoom);
        int tileMinX = max(0, (int)floor(left / tileSize));
        int tileMaxX = min(maxTile - 1, (int)floor(right / tileSize));
        int tileMinY = max(0, (int)floor(top / tileSize));
        int tileMaxY = min(maxTile - 1, (int)floor(bottom / tileSize));

        // Draw tiles
        for (int ty = tileMinY; ty <= tileMaxY; ty++) {
            for (int tx = tileMinX; tx <= tileMaxX; tx++) {
                // Compute tile rect from edge positions to avoid float gaps
                float drawX = (float)(tx * tileSize) - left;
                float drawY = (float)(ty * tileSize) - top;
                float drawW = (float)((tx + 1) * tileSize) - left - drawX;
                float drawH = (float)((ty + 1) * tileSize) - top - drawY;

                TileKey key{tileZoom, tx, ty};
                auto it = tileCache_.find(key);
                if (it != tileCache_.end() && it->second.isAllocated()) {
                    setColor(1.0f, 1.0f, 1.0f);
                    it->second.draw(drawX, drawY, drawW, drawH);
                } else {
                    // Fallback: draw parent tile's sub-region (blurry but better than nothing)
                    bool fallbackDrawn = false;
                    if (tileZoom > 1) {
                        TileKey parentKey{tileZoom - 1, tx / 2, ty / 2};
                        auto pit = tileCache_.find(parentKey);
                        if (pit != tileCache_.end() && pit->second.isAllocated()) {
                            setColor(1.0f, 1.0f, 1.0f);
                            float sx = (tx % 2) * 128.0f;
                            float sy = (ty % 2) * 128.0f;
                            pit->second.drawSubsection(
                                drawX, drawY, drawW, drawH,
                                sx, sy, 128, 128);
                            fallbackDrawn = true;
                        }
                    }
                    if (!fallbackDrawn) {
                        setColor(0.15f, 0.15f, 0.18f);
                        fill();
                        drawRect(drawX, drawY, drawW, drawH);
                    }

                    // Request tile
                    requestTile(tileZoom, tx, ty);
                }
            }
        }

        // Prefetch adjacent zoom level tiles for smooth zooming
        for (int pz : {tileZoom + 1, tileZoom - 1}) {
            if (pz < 1 || pz > 19) continue;
            auto pCenter = latLonToPixel(centerLat_, centerLon_, (double)pz);
            float pLeft = pCenter.x - halfW;
            float pTop = pCenter.y - halfH;
            float pRight = pCenter.x + halfW;
            float pBottom = pCenter.y + halfH;
            int pMax = (1 << pz);
            int pMinX = max(0, (int)floor(pLeft / 256.0));
            int pMaxX = min(pMax - 1, (int)floor(pRight / 256.0));
            int pMinY = max(0, (int)floor(pTop / 256.0));
            int pMaxY = min(pMax - 1, (int)floor(pBottom / 256.0));
            for (int ty = pMinY; ty <= pMaxY; ty++) {
                for (int tx = pMinX; tx <= pMaxX; tx++) {
                    requestTile(pz, tx, ty, true);
                }
            }
        }

        // Draw pins
        for (const auto& pin : pins_) {
            auto px = latLonToPixel(pin.lat, pin.lon, zoom_);
            float sx = px.x - left;
            float sy = px.y - top;

            // Skip if off screen
            if (sx < -PIN_RADIUS || sx > w + PIN_RADIUS ||
                sy < -PIN_RADIUS || sy > h + PIN_RADIUS) continue;

            // Pin shadow
            setColor(0.0f, 0.0f, 0.0f, 0.3f);
            fill();
            drawCircle(sx + 1, sy + 1, PIN_RADIUS);

            // Pin body
            setColor(0.9f, 0.2f, 0.2f);
            fill();
            drawCircle(sx, sy, PIN_RADIUS);

            // Pin center dot
            setColor(1.0f, 1.0f, 1.0f);
            fill();
            drawCircle(sx, sy, 3);
        }

        setClipping(false);

        // Zoom level indicator
        setColor(0.0f, 0.0f, 0.0f, 0.5f);
        fill();
        drawRect(8, h - 28, 80, 20);
        setColor(0.8f, 0.8f, 0.85f);
        fontSmall_.drawString(format("Zoom: {:.1f}", zoom_), 14, h - 18,
            Direction::Left, Direction::Center);

        // OSM attribution
        setColor(0.0f, 0.0f, 0.0f, 0.5f);
        fill();
        float attrW = 180;
        drawRect(w - attrW - 4, h - 28, attrW, 20);
        setColor(0.6f, 0.6f, 0.65f);
        fontSmall_.drawString("(C) OpenStreetMap contributors", w - attrW, h - 18,
            Direction::Left, Direction::Center);

        // "No geotagged photos" message
        if (pins_.empty()) {
            setColor(0.5f, 0.5f, 0.55f);
            font_.drawString("No geotagged photos", w / 2, h / 2,
                Direction::Center, Direction::Center);
        }
    }

    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;

        // Check pin clicks
        float w = getWidth();
        float h = getHeight();
        auto centerPx = latLonToPixel(centerLat_, centerLon_, zoom_);
        float left = centerPx.x - w / 2.0f;
        float top = centerPx.y - h / 2.0f;

        float bestDist = PIN_RADIUS * 2;
        int bestPinIdx = -1;

        for (size_t i = 0; i < pins_.size(); i++) {
            auto px = latLonToPixel(pins_[i].lat, pins_[i].lon, zoom_);
            float sx = px.x - left;
            float sy = px.y - top;
            float dist = sqrt((pos.x - sx) * (pos.x - sx) + (pos.y - sy) * (pos.y - sy));
            if (dist < bestDist) {
                bestDist = dist;
                bestPinIdx = (int)i;
            }
        }

        if (bestPinIdx >= 0) {
            auto& pin = pins_[bestPinIdx];
            auto now = chrono::steady_clock::now();
            bool isDouble = (pin.photoIndex == lastPinClickIndex_ &&
                chrono::duration_cast<chrono::milliseconds>(now - lastPinClickTime_).count() < 400);
            lastPinClickTime_ = now;
            lastPinClickIndex_ = pin.photoIndex;

            if (isDouble && onPinDoubleClick) {
                onPinDoubleClick(pin.photoIndex, pin.photoId);
            } else if (onPinClick) {
                onPinClick(pin.photoIndex, pin.photoId);
            }
            return true;
        }

        // Start dragging
        dragging_ = true;
        dragStart_ = pos;
        dragStartLat_ = centerLat_;
        dragStartLon_ = centerLon_;
        return true;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (!dragging_ || button != 0) return false;

        auto startPx = latLonToPixel(dragStartLat_, dragStartLon_, zoom_);
        float dx = dragStart_.x - pos.x;
        float dy = dragStart_.y - pos.y;

        auto [newLat, newLon] = pixelToLatLon(startPx.x + dx, startPx.y + dy, zoom_);
        centerLat_ = clamp(newLat, -85.0, 85.0);
        centerLon_ = newLon;

        // Wrap longitude
        while (centerLon_ > 180.0) centerLon_ -= 360.0;
        while (centerLon_ < -180.0) centerLon_ += 360.0;

        if (onRedraw) onRedraw();
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 0) dragging_ = false;
        return true;
    }

    bool onMouseScroll(Vec2 pos, Vec2 scroll) override {
        double oldZoom = zoom_;
        zoom_ += scroll.y * 0.05;
        zoom_ = clamp(zoom_, 1.0, 19.0);

        if (zoom_ != oldZoom) {
            // Flush stale tile queue so visible tiles load first
            {
                lock_guard<mutex> lock(tileMutex_);
                for (const auto& req : tileQueue_) {
                    tileLoading_.erase(TileKey{req.z, req.x, req.y});
                }
                tileQueue_.clear();
            }

            // Keep the point under the mouse cursor fixed
            float w = getWidth();
            float h = getHeight();
            float dx = pos.x - w / 2.0f;
            float dy = pos.y - h / 2.0f;

            // lat/lon of the point under cursor (at old zoom)
            auto oldCenterPx = latLonToPixel(centerLat_, centerLon_, oldZoom);
            auto [mouseLat, mouseLon] = pixelToLatLon(
                oldCenterPx.x + dx, oldCenterPx.y + dy, oldZoom);

            // Adjust center so that point stays under cursor at new zoom
            auto newMousePx = latLonToPixel(mouseLat, mouseLon, zoom_);
            auto [newLat, newLon] = pixelToLatLon(
                newMousePx.x - dx, newMousePx.y - dy, zoom_);

            centerLat_ = clamp(newLat, -85.0, 85.0);
            centerLon_ = newLon;
            while (centerLon_ > 180.0) centerLon_ -= 360.0;
            while (centerLon_ < -180.0) centerLon_ += 360.0;

            evictOldTiles();
            if (onRedraw) onRedraw();
        }
        return true;
    }

    // ViewContainer lifecycle
    void beginView(ViewContext& ctx) override { /* pins set via setPhotos() before activation */ }
    void endView() override { /* keep tile cache, just deactivate */ }
    bool wantsSearchBar() const override { return false; }
    bool wantsLeftSidebar() const override { return false; }

    // Stop background tile thread
    void shutdown() {
        tileThreadStop_ = true;
        if (tileThread_.joinable()) tileThread_.join();
    }

private:
    static constexpr float PIN_RADIUS = 8.0f;

    // Map state
    double centerLat_ = 35.68;  // Tokyo default
    double centerLon_ = 139.77;
    double zoom_ = 10.0;

    // Tile key for map storage
    struct TileKey {
        int z, x, y;
        bool operator<(const TileKey& o) const {
            if (z != o.z) return z < o.z;
            if (x != o.x) return x < o.x;
            return y < o.y;
        }
        bool operator==(const TileKey& o) const {
            return z == o.z && x == o.x && y == o.y;
        }
    };

    // Tile cache
    std::map<TileKey, Texture> tileCache_;
    std::map<TileKey, bool> tileLoading_;
    std::map<TileKey, int> tileFailed_;  // retry count for failed tiles
    string tileCacheDir_;

    // Photo pins
    struct Pin {
        double lat, lon;
        int photoIndex;
        string photoId;
    };
    vector<Pin> pins_;

    // Drag state
    bool dragging_ = false;
    Vec2 dragStart_;
    double dragStartLat_ = 0, dragStartLon_ = 0;

    // Double-click detection for pins
    chrono::steady_clock::time_point lastPinClickTime_;
    int lastPinClickIndex_ = -1;

    // Fonts
    Font font_;
    Font fontSmall_;

    // Tile fetching
    HttpClient tileClient_;
    mutex tileMutex_;

    struct TileRequest { int z, x, y; };
    deque<TileRequest> tileQueue_;

    struct TileResult { int z, x, y; Pixels pixels; };
    deque<TileResult> tileResults_;

    thread tileThread_;
    atomic<bool> tileThreadRunning_{false};
    atomic<bool> tileThreadStop_{false};

    // --- Mercator projection ---

    static Vec2 latLonToPixel(double lat, double lon, double zoom) {
        double n = pow(2.0, zoom);
        double x = (lon + 180.0) / 360.0 * n * 256.0;
        double latRad = lat * M_PI / 180.0;
        double y = (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n * 256.0;
        return {(float)x, (float)y};
    }

    static pair<double, double> pixelToLatLon(double px, double py, double zoom) {
        double n = pow(2.0, zoom);
        double lon = px / (n * 256.0) * 360.0 - 180.0;
        double latRad = atan(sinh(M_PI * (1.0 - 2.0 * py / (n * 256.0))));
        return {latRad * 180.0 / M_PI, lon};
    }

    // --- Tile loading ---

    void requestTile(int z, int x, int y, bool prefetch = false) {
        TileKey key{z, x, y};

        // Already loaded, loading, or failed too many times?
        if (tileCache_.count(key) || tileLoading_.count(key)) return;
        auto fit = tileFailed_.find(key);
        if (fit != tileFailed_.end() && fit->second >= 3) return;

        // Check disk cache first
        if (!tileCacheDir_.empty()) {
            string cachePath = format("{}/{}/{}/{}.png", tileCacheDir_, z, x, y);
            if (fs::exists(cachePath)) {
                Pixels px;
                if (px.load(cachePath)) {
                    Texture tex;
                    tex.allocate(px, TextureUsage::Immutable, false);
                    tileCache_[key] = std::move(tex);
                    return;
                }
            }
        }

        tileLoading_[key] = true;

        {
            lock_guard<mutex> lock(tileMutex_);
            if (prefetch) {
                tileQueue_.push_back({z, x, y});
            } else {
                tileQueue_.push_front({z, x, y});  // visible tiles first
            }
        }

        startTileThread();
    }

    void startTileThread() {
        if (tileThreadRunning_) return;
        tileThreadRunning_ = true;
        tileThreadStop_ = false;

        if (tileThread_.joinable()) tileThread_.join();

        tileThread_ = thread([this]() {
            while (!tileThreadStop_) {
                TileRequest req;
                {
                    lock_guard<mutex> lock(tileMutex_);
                    if (tileQueue_.empty()) break;
                    req = tileQueue_.front();
                    tileQueue_.pop_front();
                }

                // Fetch from OSM
                string url = format("https://tile.openstreetmap.org/{}/{}/{}.png",
                    req.z, req.x, req.y);

                HttpClient client;
                client.addHeader("User-Agent", "TrussPhoto/1.0");
                client.setBaseUrl("");
                auto res = client.get(url);

                Pixels px;
                if (res.ok() && !res.body.empty()) {
                    px.loadFromMemory(
                        (const unsigned char*)res.body.data(),
                        (int)res.body.size());

                    // Save to disk cache
                    if (!tileCacheDir_.empty()) {
                        string dir = format("{}/{}/{}", tileCacheDir_, req.z, req.x);
                        fs::create_directories(dir);
                        string cachePath = format("{}/{}.png", dir, req.y);
                        ofstream file(cachePath, ios::binary);
                        if (file) {
                            file.write(res.body.data(), res.body.size());
                        }
                    }
                }

                {
                    lock_guard<mutex> lock(tileMutex_);
                    tileResults_.push_back({req.z, req.x, req.y, std::move(px)});
                }

                // Small delay to respect OSM rate limits
                this_thread::sleep_for(chrono::milliseconds(100));
            }
            tileThreadRunning_ = false;
        });
        tileThread_.detach();
    }

    // LRU eviction: remove tiles from cache when it gets too large
    void evictOldTiles() {
        const int MAX_TILES = 256;
        if ((int)tileCache_.size() <= MAX_TILES) return;

        // Keep current and adjacent zoom levels for smooth transitions
        int currentZoom = (int)floor(zoom_);
        auto it = tileCache_.begin();
        while (it != tileCache_.end() && (int)tileCache_.size() > MAX_TILES) {
            int tz = it->first.z;
            if (tz < currentZoom - 1 || tz > currentZoom + 1) {
                it = tileCache_.erase(it);
            } else {
                ++it;
            }
        }
    }
};
