#pragma once

// =============================================================================
// MapView.h - Slippy map view with OpenStreetMap tiles and GPS photo pins
// =============================================================================

#include <TrussC.h>
#include <tcxCurl.h>
#include "PhotoEntry.h"
#include "ViewContainer.h"
#include "PhotoStrip.h"
#include "Constants.h"   // for SEL_R/G/B
#include "FolderTree.h"  // for loadJapaneseFont
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

// =============================================================================
// ProvisionalPin - draggable temporary pin for unconfirmed geotag assignment
// =============================================================================

class MapCanvas;  // forward decl

class ProvisionalPin : public RectNode {
public:
    using Ptr = shared_ptr<ProvisionalPin>;

    string photoId;
    int photoIndex = -1;
    double lat = 0, lon = 0;

    static constexpr float SIZE = 20.0f;
    inline static const Color COLOR = Color(0.3f, 0.65f, 1.0f);

    ProvisionalPin(const string& id, int idx, double lat_, double lon_,
                   MapCanvas* canvas)
        : photoId(id), photoIndex(idx), lat(lat_), lon(lon_), canvas_(canvas) {
        enableEvents();
        setSize(SIZE, SIZE);
    }

    void setSelected(bool v) { selected_ = v; }
    bool isSelected() const { return selected_; }

    void draw() override {
        float cx = SIZE / 2;
        float cy = SIZE / 2;

        // Orange selection ring
        if (selected_) {
            setColor(SEL_R, SEL_G, SEL_B);
            fill();
            drawCircle(cx, cy, 10);
        } else {
            // Shadow (only when not selected)
            setColor(0.0f, 0.0f, 0.0f, 0.35f);
            fill();
            drawCircle(cx + 1, cy + 1, 7);
        }

        // Pin body (light blue)
        setColor(COLOR);
        fill();
        drawCircle(cx, cy, 7);

        // White inner dot
        setColor(1.0f, 1.0f, 1.0f);
        fill();
        drawCircle(cx, cy, 3);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override;
    bool onMouseDrag(Vec2 pos, int button) override;
    bool onMouseRelease(Vec2 pos, int button) override;

private:
    MapCanvas* canvas_;
    bool draggingPin_ = false;
    bool selected_ = false;
};

// =============================================================================
// MapCanvas - map tile rendering, pins, and interaction (child RectNode)
// =============================================================================

class MapCanvas : public RectNode {
public:
    using Ptr = shared_ptr<MapCanvas>;

    // Callbacks
    function<void(int index, const string& photoId)> onPinClick;
    function<void(int index, const string& photoId)> onPinDoubleClick;
    function<void(const string& photoId, double lat, double lon)> onGeotagConfirm;
    function<void()> onRedraw;

    void setPhotos(const vector<PhotoEntry>& photos, const vector<string>& ids) {
        pins_.clear();
        selectedPinIdx_ = -1;
        for (size_t i = 0; i < photos.size(); i++) {
            if (photos[i].hasGps()) {
                pins_.push_back({photos[i].latitude, photos[i].longitude, (int)i, ids[i]});
            }
        }
        clusterZoom_ = -999;
    }

    // Select a pin by photoId. Returns true if the photo has GPS (pin found).
    bool selectPin(const string& photoId) {
        selectedPinIdx_ = -1;
        for (size_t i = 0; i < pins_.size(); i++) {
            if (pins_[i].photoId == photoId) {
                selectedPinIdx_ = (int)i;
                return true;
            }
        }
        return false;
    }

    // Get selected pin's photoId (empty if none)
    string selectedPhotoId() const {
        if (selectedPinIdx_ >= 0 && selectedPinIdx_ < (int)pins_.size())
            return pins_[selectedPinIdx_].photoId;
        return "";
    }

    // Remove a pin by photoId and return true if found
    bool removePin(const string& photoId) {
        for (auto it = pins_.begin(); it != pins_.end(); ++it) {
            if (it->photoId == photoId) {
                bool wasSelected = (selectedPinIdx_ == (int)(it - pins_.begin()));
                pins_.erase(it);
                clusterZoom_ = -999;
                if (wasSelected) selectedPinIdx_ = -1;
                else if (selectedPinIdx_ >= (int)pins_.size()) selectedPinIdx_ = -1;
                if (onRedraw) onRedraw();
                return true;
            }
        }
        return false;
    }

    // Center map on pin if it's outside the viewport. Keep zoom level, animate.
    void centerOnSelectedPin() {
        if (selectedPinIdx_ < 0 || selectedPinIdx_ >= (int)pins_.size()) return;
        auto& pin = pins_[selectedPinIdx_];

        // Check if pin is within the visible viewport (with margin)
        auto pinPx = latLonToPixel(pin.lat, pin.lon, zoom_);
        auto centerPx = latLonToPixel(centerLat_, centerLon_, zoom_);
        float halfW = getWidth() / 2.0f;
        float halfH = getHeight() / 2.0f;
        float margin = 40.0f;

        float dx = pinPx.x - centerPx.x;
        float dy = pinPx.y - centerPx.y;
        if (abs(dx) < halfW - margin && abs(dy) < halfH - margin) return;

        // Animate to pin location
        panAnimating_ = true;
        panFromLat_ = centerLat_;
        panFromLon_ = centerLon_;
        panToLat_ = pin.lat;
        panToLon_ = pin.lon;
        panProgress_ = 0.0f;
    }

    void setTileCacheDir(const string& dir) {
        tileCacheDir_ = dir;
        if (!dir.empty()) fs::create_directories(dir);
    }

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

        float w = getWidth();
        float h = getHeight();
        if (w < 1 || h < 1) { zoom_ = 10; return; }

        for (int z = 18; z >= 1; z--) {
            auto pMin = latLonToPixel(maxLat, minLon, z);
            auto pMax = latLonToPixel(minLat, maxLon, z);
            float spanX = pMax.x - pMin.x;
            float spanY = pMax.y - pMin.y;
            if (spanX < w * 0.8f && spanY < h * 0.8f) {
                zoom_ = z;
                return;
            }
        }
        zoom_ = 1;
    }

    // --- Provisional pin management ---

    void addProvisionalPin(const string& photoId, int photoIndex, double lat, double lon) {
        // Don't add duplicate for same photo
        for (auto& pp : provisionalPins_) {
            if (pp->photoId == photoId) {
                pp->lat = lat;
                pp->lon = lon;
                updateProvisionalPinPositions();
                return;
            }
        }
        auto pp = make_shared<ProvisionalPin>(photoId, photoIndex, lat, lon, this);
        provisionalPins_.push_back(pp);
        addChild(pp);
        updateProvisionalPinPositions();
        if (onRedraw) onRedraw();
    }

    void clearProvisionalPins() {
        for (auto& pp : provisionalPins_) pp->destroy();
        provisionalPins_.clear();
        if (onRedraw) onRedraw();
    }

    void confirmPin(ProvisionalPin* pin) {
        if (!pin) return;
        if (onGeotagConfirm) onGeotagConfirm(pin->photoId, pin->lat, pin->lon);
        pins_.push_back({pin->lat, pin->lon, pin->photoIndex, pin->photoId});
        clusterZoom_ = -999;
        selectPin(pin->photoId);
        pin->destroy();
    }

    void confirmAllPins() {
        for (auto& pp : provisionalPins_) {
            if (onGeotagConfirm) onGeotagConfirm(pp->photoId, pp->lat, pp->lon);
            pins_.push_back({pp->lat, pp->lon, pp->photoIndex, pp->photoId});
            pp->destroy();
        }
        provisionalPins_.clear();
        clusterZoom_ = -999;
        if (onRedraw) onRedraw();
    }

    // Sweep destroyed provisional pins from our tracking vector (framework handles scene graph)
    void sweepDeadPins() {
        auto it = std::remove_if(provisionalPins_.begin(), provisionalPins_.end(),
            [](const ProvisionalPin::Ptr& pp) { return pp->isDead(); });
        if (it != provisionalPins_.end()) {
            provisionalPins_.erase(it, provisionalPins_.end());
            if (onRedraw) onRedraw();
        }
    }

    bool hasProvisionalPins() const { return !provisionalPins_.empty(); }

    const vector<ProvisionalPin::Ptr>& provisionalPins() const { return provisionalPins_; }

    // Update selection highlight on provisional pins based on strip selection
    void updateProvisionalPinSelection(const vector<string>& selectedIds) {
        unordered_set<string> sel(selectedIds.begin(), selectedIds.end());
        for (auto& pp : provisionalPins_) {
            pp->setSelected(sel.count(pp->photoId) > 0);
        }
    }

    // Convert local screen coords to lat/lon
    pair<double, double> screenToLatLon(float sx, float sy) {
        auto centerPx = latLonToPixel(centerLat_, centerLon_, zoom_);
        float halfW = getWidth() / 2.0f;
        float halfH = getHeight() / 2.0f;
        double px = centerPx.x + (sx - halfW);
        double py = centerPx.y + (sy - halfH);
        return pixelToLatLon(px, py, zoom_);
    }

    void updateProvisionalPinPositions() {
        auto centerPx = latLonToPixel(centerLat_, centerLon_, zoom_);
        float halfW = getWidth() / 2.0f;
        float halfH = getHeight() / 2.0f;
        float left = centerPx.x - halfW;
        float top = centerPx.y - halfH;

        for (auto& pp : provisionalPins_) {
            auto px = latLonToPixel(pp->lat, pp->lon, zoom_);
            pp->setPos(px.x - left - ProvisionalPin::SIZE / 2,
                       px.y - top - ProvisionalPin::SIZE / 2);
        }
    }

    void setup() override {
        enableEvents();
        setClipping(true);
        loadJapaneseFont(font_, 12);
        loadJapaneseFont(fontSmall_, 10);
        tileClient_.addHeader("User-Agent", "TrussPhoto/1.0");
    }

    void update() override {
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

        if (!tileThreadRunning_ && !tileThreadStop_) {
            bool hasQueue;
            {
                lock_guard<mutex> lock(tileMutex_);
                hasQueue = !tileQueue_.empty();
            }
            if (hasQueue) startTileThread();
        }

        // Pan animation
        if (panAnimating_) {
            panProgress_ += 0.04f;  // ~15 frames at 60fps
            if (panProgress_ >= 1.0f) {
                panProgress_ = 1.0f;
                panAnimating_ = false;
            }
            // Ease out cubic
            float t = 1.0f - (1.0f - panProgress_) * (1.0f - panProgress_) * (1.0f - panProgress_);
            centerLat_ = panFromLat_ + (panToLat_ - panFromLat_) * t;
            centerLon_ = panFromLon_ + (panToLon_ - panFromLon_) * t;
            if (onRedraw) onRedraw();
        }

        // Sweep confirmed (dead) provisional pins
        sweepDeadPins();

        // Keep provisional pins in sync with map pan/zoom
        if (!provisionalPins_.empty()) {
            updateProvisionalPinPositions();
        }
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.12f, 0.12f, 0.14f);
        fill();
        drawRect(0, 0, w, h);

        // Fractional zoom: tiles are fetched at integer zoom, scaled for display
        int tileZoom = clamp((int)floor(zoom_), 1, 19);
        double tileScale = pow(2.0, zoom_ - tileZoom);
        double tileSize = 256.0 * tileScale;

        auto centerPx = latLonToPixel(centerLat_, centerLon_, zoom_);

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
                    requestTile(tileZoom, tx, ty);
                }
            }
        }

        // Prefetch adjacent zoom levels
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
        drawPins(left, top, w, h);
    }

    void endDraw() override {
        float w = getWidth();
        float h = getHeight();

        // Zoom level indicator (bottom-left, flush to edges)
        float overlayH = 20;
        float overlayY = h - overlayH + 3;
        setColor(0.0f, 0.0f, 0.0f, 0.3f);
        fill();
        drawRect(0, overlayY, 88, overlayH);
        setColor(1.0f, 1.0f, 1.0f);
        fontSmall_.drawString(format("Zoom: {:.1f}", zoom_), 6, overlayY + overlayH / 2,
            Direction::Left, Direction::Center);

        // OSM attribution (bottom-right, flush to edges)
        float attrW = 184;
        setColor(0.0f, 0.0f, 0.0f, 0.3f);
        fill();
        drawRect(w - attrW, overlayY, attrW, overlayH);
        setColor(1.0f, 1.0f, 1.0f);
        fontSmall_.drawString("(C) OpenStreetMap contributors", w - attrW + 4, overlayY + overlayH / 2,
            Direction::Left, Direction::Center);

        // "No geotagged photos" message
        if (pins_.empty() && provisionalPins_.empty()) {
            setColor(0.5f, 0.5f, 0.55f);
            font_.drawString("No geotagged photos", w / 2, h / 2,
                Direction::Center, Direction::Center);
        }

        // Separator line at bottom
        setColor(0.25f, 0.25f, 0.28f);
        fill();
        drawRect(0, h - 1, w, 1);

        RectNode::endDraw();
    }

    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;

        // Hit test against cached clusters
        double qZoom = quantizeZoom(zoom_);
        double sf = pow(2.0, zoom_ - qZoom);
        auto centerPxQ = latLonToPixel(centerLat_, centerLon_, qZoom);
        float qLeft = centerPxQ.x * (float)sf - getWidth() / 2.0f;
        float qTop  = centerPxQ.y * (float)sf - getHeight() / 2.0f;

        for (const auto& cluster : cachedClusters_) {
            float sx = cluster.wx * (float)sf - qLeft;
            float sy = cluster.wy * (float)sf - qTop;
            float r = PIN_RADIUS;
            if (cluster.count >= 1000) r = PIN_RADIUS + 6;
            else if (cluster.count >= 100) r = PIN_RADIUS + 4;
            float dx = pos.x - sx;
            float dy = pos.y - sy;
            if (dx * dx + dy * dy > r * r) continue;

            if (cluster.count == 1) {
                auto& pin = pins_[cluster.firstPinIdx];
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
            } else {
                auto& pin = pins_[cluster.firstPinIdx];
                if (onPinClick) onPinClick(pin.photoIndex, pin.photoId);
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

        while (centerLon_ > 180.0) centerLon_ -= 360.0;
        while (centerLon_ < -180.0) centerLon_ += 360.0;

        if (!provisionalPins_.empty()) updateProvisionalPinPositions();
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
            {
                lock_guard<mutex> lock(tileMutex_);
                for (const auto& req : tileQueue_) {
                    tileLoading_.erase(TileKey{req.z, req.x, req.y});
                }
                tileQueue_.clear();
            }

            float w = getWidth();
            float h = getHeight();
            float dx = pos.x - w / 2.0f;
            float dy = pos.y - h / 2.0f;

            auto oldCenterPx = latLonToPixel(centerLat_, centerLon_, oldZoom);
            auto [mouseLat, mouseLon] = pixelToLatLon(
                oldCenterPx.x + dx, oldCenterPx.y + dy, oldZoom);

            auto newMousePx = latLonToPixel(mouseLat, mouseLon, zoom_);
            auto [newLat, newLon] = pixelToLatLon(
                newMousePx.x - dx, newMousePx.y - dy, zoom_);

            centerLat_ = clamp(newLat, -85.0, 85.0);
            centerLon_ = newLon;
            while (centerLon_ > 180.0) centerLon_ -= 360.0;
            while (centerLon_ < -180.0) centerLon_ += 360.0;

            evictOldTiles();
            if (!provisionalPins_.empty()) updateProvisionalPinPositions();
            if (onRedraw) onRedraw();
        }
        return true;
    }

    void shutdown() {
        tileThreadStop_ = true;
        if (tileThread_.joinable()) tileThread_.join();
    }

private:
    static constexpr float PIN_RADIUS = 7.0f;
    inline static const Color PIN_COLOR = Color(0.9f, 0.2f, 0.2f);

    // Map state
    double centerLat_ = 35.68;
    double centerLon_ = 139.77;
    double zoom_ = 10.0;

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

    std::map<TileKey, Texture> tileCache_;
    std::map<TileKey, bool> tileLoading_;
    std::map<TileKey, int> tileFailed_;
    string tileCacheDir_;

    struct Pin {
        double lat, lon;
        int photoIndex;
        string photoId;
    };
    vector<Pin> pins_;

    // Provisional (unconfirmed) pins
    vector<ProvisionalPin::Ptr> provisionalPins_;

    struct Cluster {
        float wx, wy;
        int count;
        int firstPinIdx;
    };
    vector<Cluster> cachedClusters_;
    double clusterZoom_ = -999;

    bool dragging_ = false;
    Vec2 dragStart_;
    double dragStartLat_ = 0, dragStartLon_ = 0;

    // Selected pin
    int selectedPinIdx_ = -1;

    // Pan animation
    bool panAnimating_ = false;
    double panFromLat_ = 0, panFromLon_ = 0;
    double panToLat_ = 0, panToLon_ = 0;
    float panProgress_ = 0;

    chrono::steady_clock::time_point lastPinClickTime_;
    int lastPinClickIndex_ = -1;

    Font font_;
    Font fontSmall_;

    HttpClient tileClient_;
    mutex tileMutex_;

    struct TileRequest { int z, x, y; };
    deque<TileRequest> tileQueue_;

    struct TileResult { int z, x, y; Pixels pixels; };
    deque<TileResult> tileResults_;

    thread tileThread_;
    atomic<bool> tileThreadRunning_{false};
    atomic<bool> tileThreadStop_{false};

    // --- Pin clustering ---

    static constexpr float CLUSTER_CELL = 24.0f;
    static constexpr double CLUSTER_ZOOM_STEP = 0.2;

    static double quantizeZoom(double z) {
        return floor(z / CLUSTER_ZOOM_STEP) * CLUSTER_ZOOM_STEP;
    }

    void rebuildClusters(double qZoom) {
        cachedClusters_.clear();
        clusterZoom_ = qZoom;

        std::map<pair<int,int>, Cluster> grid;

        for (size_t i = 0; i < pins_.size(); i++) {
            auto px = latLonToPixel(pins_[i].lat, pins_[i].lon, qZoom);
            float wx = px.x;
            float wy = px.y;

            int gx = (int)floor(wx / CLUSTER_CELL);
            int gy = (int)floor(wy / CLUSTER_CELL);
            auto key = make_pair(gx, gy);
            auto it = grid.find(key);
            if (it == grid.end()) {
                grid[key] = {wx, wy, 1, (int)i};
            } else {
                auto& c = it->second;
                c.wx = (c.wx * c.count + wx) / (c.count + 1);
                c.wy = (c.wy * c.count + wy) / (c.count + 1);
                c.count++;
            }
        }

        cachedClusters_.reserve(grid.size());
        for (const auto& [key, cluster] : grid) {
            cachedClusters_.push_back(cluster);
        }
    }

    void drawPins(float left, float top, float w, float h) {
        double qZoom = quantizeZoom(zoom_);
        if (qZoom != clusterZoom_) {
            rebuildClusters(qZoom);
        }

        double scaleFactor = pow(2.0, zoom_ - qZoom);

        auto centerPxQ = latLonToPixel(centerLat_, centerLon_, qZoom);
        float qLeft = centerPxQ.x * (float)scaleFactor - w / 2.0f;
        float qTop  = centerPxQ.y * (float)scaleFactor - h / 2.0f;

        // Draw all clusters (normal pins)
        for (const auto& cluster : cachedClusters_) {
            drawCluster(cluster, scaleFactor, qLeft, qTop, w, h);
        }

        // Draw selected pin on top (independently from clusters)
        if (selectedPinIdx_ >= 0 && selectedPinIdx_ < (int)pins_.size()) {
            auto& pin = pins_[selectedPinIdx_];
            auto pinPx = latLonToPixel(pin.lat, pin.lon, zoom_);
            float sx = pinPx.x - left;
            float sy = pinPx.y - top;

            if (sx >= -30 && sx <= w + 30 && sy >= -30 && sy <= h + 30) {
                // Shadow
                setColor(0.0f, 0.0f, 0.0f, 0.4f);
                fill();
                drawCircle(sx + 1, sy + 1, 10);

                // Orange selection pin
                setColor(SEL_R, SEL_G, SEL_B);
                fill();
                drawCircle(sx, sy, 10);

                // White inner dot
                setColor(1.0f, 1.0f, 1.0f);
                fill();
                drawCircle(sx, sy, 3);
            }
        }
    }

    void drawCluster(const Cluster& cluster, double scaleFactor,
                     float qLeft, float qTop, float w, float h) {
        float sx = cluster.wx * (float)scaleFactor - qLeft;
        float sy = cluster.wy * (float)scaleFactor - qTop;

        if (sx < -30 || sx > w + 30 || sy < -30 || sy > h + 30) return;

        float r = PIN_RADIUS;
        string label;
        if (cluster.count >= 1000) {
            r = PIN_RADIUS + 6;
            label = format("{:.0f}k", cluster.count / 1000.0f);
        } else if (cluster.count >= 100) {
            r = PIN_RADIUS + 4;
            label = to_string(cluster.count);
        } else if (cluster.count > 1) {
            label = to_string(cluster.count);
        }

        // Shadow
        setColor(0.0f, 0.0f, 0.0f, 0.3f);
        fill();
        drawCircle(sx + 1, sy + 1, r);

        // Pin body
        setColor(PIN_COLOR);
        fill();
        drawCircle(sx, sy, r);

        // Inner dot or label
        if (cluster.count == 1) {
            setColor(1.0f, 1.0f, 1.0f);
            fill();
            drawCircle(sx, sy, 3);
        } else {
            setColor(1.0f, 1.0f, 1.0f);
            fontSmall_.drawString(label, sx, sy + 2,
                Direction::Center, Direction::Center);
        }
    }

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

        if (tileCache_.count(key) || tileLoading_.count(key)) return;
        auto fit = tileFailed_.find(key);
        if (fit != tileFailed_.end() && fit->second >= 3) return;

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
                tileQueue_.push_front({z, x, y});
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

                this_thread::sleep_for(chrono::milliseconds(100));
            }
            tileThreadRunning_ = false;
        });
        tileThread_.detach();
    }

    void evictOldTiles() {
        const int MAX_TILES = 256;
        if ((int)tileCache_.size() <= MAX_TILES) return;

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

// --- ProvisionalPin mouse handlers (defined after MapCanvas) ---

inline bool ProvisionalPin::onMousePress(Vec2 pos, int button) {
    (void)pos;
    if (button == 0) {
        draggingPin_ = true;
        return true;
    }
    if (button == 1) {  // right-click → confirm
        canvas_->confirmPin(this);
        return true;
    }
    return false;
}

inline bool ProvisionalPin::onMouseDrag(Vec2 pos, int button) {
    if (!draggingPin_ || button != 0) return false;
    // Convert local pos to canvas-local coords
    float gx, gy;
    localToGlobal(pos.x, pos.y, gx, gy);
    float cx, cy;
    canvas_->globalToLocal(gx, gy, cx, cy);
    auto [newLat, newLon] = canvas_->screenToLatLon(cx, cy);
    lat = newLat;
    lon = newLon;
    canvas_->updateProvisionalPinPositions();
    if (canvas_->onRedraw) canvas_->onRedraw();
    return true;
}

inline bool ProvisionalPin::onMouseRelease(Vec2 pos, int button) {
    (void)pos;
    if (button == 0) draggingPin_ = false;
    return true;
}

// =============================================================================
// MapView - container for MapCanvas + PhotoStrip
// =============================================================================

class MapView : public ViewContainer {
public:
    using Ptr = shared_ptr<MapView>;

    // Callbacks (forwarded to canvas)
    function<void(int index, const string& photoId)> onPinClick;
    function<void(int index, const string& photoId)> onPinDoubleClick;
    function<void(const string& photoId, double lat, double lon)> onGeotagConfirm;
    function<void()> onRedraw;

    // Modifier key refs (set by tcApp)
    bool* cmdDownRef = nullptr;
    bool* shiftDownRef = nullptr;

    MapView() {
        canvas_ = make_shared<MapCanvas>();
        strip_ = make_shared<PhotoStrip>();
    }

    void setPhotos(const vector<PhotoEntry>& photos, const vector<string>& ids,
                   PhotoProvider& provider) {
        photos_ = photos;
        photoIds_ = ids;
        canvas_->setPhotos(photos, ids);

        vector<bool> hasGps(ids.size());
        for (size_t i = 0; i < photos.size(); i++)
            hasGps[i] = photos[i].hasGps();
        strip_->setPhotos(ids, hasGps, provider);
    }

    void setStripSelection(const string& photoId) {
        strip_->selectPhoto(photoId);
        canvas_->selectPin(photoId);
    }

    void setTileCacheDir(const string& dir) {
        canvas_->setTileCacheDir(dir);
    }

    void fitBounds() {
        canvas_->fitBounds();
    }

    // --- Provisional pin API ---

    bool hasProvisionalPins() const { return canvas_->hasProvisionalPins(); }

    // Get the selected pin's photoId (empty if none)
    string selectedPhotoId() const { return canvas_->selectedPhotoId(); }

    // Get all selected photo IDs from strip
    vector<string> selectedPhotoIds() const { return strip_->selectedPhotoIds(); }

    // Remove geotag from a photo: clear GPS in provider, remove pin from map, update strip
    void removeGeotag(const string& photoId, PhotoProvider& provider) {
        provider.setGps(photoId, 0, 0);
        canvas_->removePin(photoId);
        strip_->setHasGps(photoId, false);
        // Update local photos_ copy so auto-geotag sees the change
        updatePhotoCopy(photoId, 0, 0);
        logNotice() << "[MapView] Geotag removed: " << photoId;
    }

    void clearProvisionalPins() {
        // Reset strip indicators
        for (auto& pp : canvas_->provisionalPins()) {
            strip_->setProvisionalGeotag(pp->photoId, false);
        }
        canvas_->clearProvisionalPins();
    }

    void confirmAllPins() {
        for (auto& pp : canvas_->provisionalPins()) {
            strip_->setProvisionalGeotag(pp->photoId, false);
            strip_->setHasGps(pp->photoId, true);
        }
        canvas_->confirmAllPins();
    }

    // Auto-geotag: interpolate GPS from timestamps for selected photos
    void runAutoGeotag() {
        constexpr double MAX_GAP_SEC = 2.0 * 3600;
        constexpr double EXTRAP_MAX_SEC = MAX_GAP_SEC * 0.25;
        constexpr int MAX_PINS = 1000;

        // Only operate on selected photos
        auto selectedIds = strip_->selectedPhotoIds();
        if (selectedIds.empty()) return;

        unordered_set<string> targetIds(selectedIds.begin(), selectedIds.end());

        // Build sorted timeline (all photos for interpolation, but only selected as targets)
        struct TimeEntry {
            int idx;
            string id;
            int64_t time;
            bool hasGps;
            bool isTarget;
            double lat, lon;
        };
        vector<TimeEntry> sorted;
        for (size_t i = 0; i < photos_.size(); i++) {
            int64_t t = PhotoEntry::parseDateTimeOriginal(photos_[i].dateTimeOriginal);
            if (t == 0) continue;
            bool target = !photos_[i].hasGps() && targetIds.count(photoIds_[i]);
            sorted.push_back({(int)i, photoIds_[i], t,
                              photos_[i].hasGps(), target,
                              photos_[i].latitude, photos_[i].longitude});
        }
        sort(sorted.begin(), sorted.end(),
             [](const TimeEntry& a, const TimeEntry& b) { return a.time < b.time; });

        int count = 0;
        for (size_t ci = 0; ci < sorted.size(); ci++) {
            auto& c = sorted[ci];
            if (c.hasGps || !c.isTarget) continue;

            if (count >= MAX_PINS) {
                logWarning() << "[MapView] Auto-geotag capped at " << MAX_PINS << " pins";
                break;
            }

            // Already has provisional pin?
            bool alreadyProvisional = false;
            for (auto& pp : canvas_->provisionalPins()) {
                if (pp->photoId == c.id) { alreadyProvisional = true; break; }
            }
            if (alreadyProvisional) continue;

            // Find nearest GPS photo before and after
            int beforeIdx = -1, afterIdx = -1;
            for (int j = (int)ci - 1; j >= 0; j--) {
                if (sorted[j].hasGps) { beforeIdx = j; break; }
            }
            for (int j = (int)ci + 1; j < (int)sorted.size(); j++) {
                if (sorted[j].hasGps) { afterIdx = j; break; }
            }

            double lat = 0, lon = 0;
            bool found = false;

            if (beforeIdx >= 0 && afterIdx >= 0) {
                // Interpolation between A and B
                double gapA = abs((double)(c.time - sorted[beforeIdx].time));
                double gapB = abs((double)(sorted[afterIdx].time - c.time));
                if (gapA <= MAX_GAP_SEC && gapB <= MAX_GAP_SEC) {
                    double total = (double)(sorted[afterIdx].time - sorted[beforeIdx].time);
                    double t = (total > 0) ? (double)(c.time - sorted[beforeIdx].time) / total : 0.5;
                    lat = sorted[beforeIdx].lat + (sorted[afterIdx].lat - sorted[beforeIdx].lat) * t;
                    lon = sorted[beforeIdx].lon + (sorted[afterIdx].lon - sorted[beforeIdx].lon) * t;
                    found = true;
                }
            }

            if (!found && beforeIdx >= 0) {
                // Extrapolation forward from A
                double gap = abs((double)(c.time - sorted[beforeIdx].time));
                if (gap <= EXTRAP_MAX_SEC) {
                    // Find A2 (second GPS before)
                    int a2Idx = -1;
                    for (int j = beforeIdx - 1; j >= 0; j--) {
                        if (sorted[j].hasGps) { a2Idx = j; break; }
                    }
                    if (a2Idx >= 0) {
                        double segTime = (double)(sorted[beforeIdx].time - sorted[a2Idx].time);
                        if (segTime > 0) {
                            double t = (double)(c.time - sorted[beforeIdx].time) / segTime;
                            lat = sorted[beforeIdx].lat + (sorted[beforeIdx].lat - sorted[a2Idx].lat) * t;
                            lon = sorted[beforeIdx].lon + (sorted[beforeIdx].lon - sorted[a2Idx].lon) * t;
                            found = true;
                        }
                    }
                    if (!found) {
                        // Snap to nearest GPS
                        lat = sorted[beforeIdx].lat;
                        lon = sorted[beforeIdx].lon;
                        found = true;
                    }
                }
            }

            if (!found && afterIdx >= 0) {
                // Extrapolation backward from B
                double gap = abs((double)(sorted[afterIdx].time - c.time));
                if (gap <= EXTRAP_MAX_SEC) {
                    int b2Idx = -1;
                    for (int j = afterIdx + 1; j < (int)sorted.size(); j++) {
                        if (sorted[j].hasGps) { b2Idx = j; break; }
                    }
                    if (b2Idx >= 0) {
                        double segTime = (double)(sorted[b2Idx].time - sorted[afterIdx].time);
                        if (segTime > 0) {
                            double t = (double)(sorted[afterIdx].time - c.time) / segTime;
                            lat = sorted[afterIdx].lat + (sorted[afterIdx].lat - sorted[b2Idx].lat) * t;
                            lon = sorted[afterIdx].lon + (sorted[afterIdx].lon - sorted[b2Idx].lon) * t;
                            found = true;
                        }
                    }
                    if (!found) {
                        lat = sorted[afterIdx].lat;
                        lon = sorted[afterIdx].lon;
                        found = true;
                    }
                }
            }

            if (found) {
                canvas_->addProvisionalPin(c.id, c.idx, lat, lon);
                strip_->setProvisionalGeotag(c.id, true);
                count++;
            }
        }

        logNotice() << "[MapView] Auto-geotag: " << count << " provisional pins created";
        if (onRedraw) onRedraw();
    }

    // Layout helpers
    float stripHeight() const { return clamp(getHeight() * 0.15f, 80.0f, 160.0f); }
    float mapHeight() const { return getHeight() - stripHeight(); }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        layoutChildren();
    }

    void setup() override {
        addChild(canvas_);
        addChild(strip_);

        // Forward callbacks: map pin click → select strip + metadata
        canvas_->onPinClick = [this](int idx, const string& id) {
            canvas_->selectPin(id);
            strip_->selectPhoto(id);
            if (onPinClick) onPinClick(idx, id);
            if (onRedraw) onRedraw();
        };
        canvas_->onPinDoubleClick = [this](int idx, const string& id) {
            if (onPinDoubleClick) onPinDoubleClick(idx, id);
        };
        canvas_->onGeotagConfirm = [this](const string& id, double lat, double lon) {
            strip_->setProvisionalGeotag(id, false);
            strip_->setHasGps(id, true);
            updatePhotoCopy(id, lat, lon);
            if (onGeotagConfirm) onGeotagConfirm(id, lat, lon);
        };
        canvas_->onRedraw = [this]() {
            if (onRedraw) onRedraw();
        };

        // Strip click → select pin + center map if GPS
        strip_->onPhotoClick = [this](int idx, const string& id) {
            bool hasGps = canvas_->selectPin(id);
            if (hasGps) {
                canvas_->centerOnSelectedPin();
            }
            // Update provisional pin selection highlights
            canvas_->updateProvisionalPinSelection(strip_->selectedPhotoIds());
            if (onPinClick) onPinClick(idx, id);
            if (onRedraw) onRedraw();
        };

        // Strip drag move → ghost indicator
        strip_->onDragMove = [this](int idx, const string& id, float gx, float gy) {
            dragActive_ = true;
            dragGlobalX_ = gx;
            dragGlobalY_ = gy;
            if (onRedraw) onRedraw();
        };

        // Strip drag end → create provisional pin on map
        strip_->onDragEnd = [this](int idx, const string& id, float gx, float gy) {
            dragActive_ = false;

            float cx, cy;
            canvas_->globalToLocal(gx, gy, cx, cy);

            if (cx >= 0 && cx <= canvas_->getWidth() && cy >= 0 && cy <= canvas_->getHeight()) {
                auto [lat, lon] = canvas_->screenToLatLon(cx, cy);
                canvas_->addProvisionalPin(id, idx, lat, lon);
                strip_->setProvisionalGeotag(id, true);
            }
            if (onRedraw) onRedraw();
        };

        layoutChildren();
    }

    // ViewContainer lifecycle
    void beginView(ViewContext& ctx) override {
        // Pass modifier key refs to strip (must be deferred from setup()
        // because tcApp sets cmdDownRef/shiftDownRef after addChild triggers setup)
        strip_->cmdDownRef = cmdDownRef;
        strip_->shiftDownRef = shiftDownRef;
    }
    void endView() override {
        clearProvisionalPins();
    }
    bool wantsSearchBar() const override { return false; }
    bool wantsLeftSidebar() const override { return false; }

    void endDraw() override {
        // Ghost indicator during drag
        if (dragActive_) {
            float lx, ly;
            globalToLocal(dragGlobalX_, dragGlobalY_, lx, ly);
            setColor(0.3f, 0.65f, 1.0f, 0.5f);
            fill();
            drawCircle(lx, ly, 10);
            setColor(1.0f, 1.0f, 1.0f, 0.7f);
            fill();
            drawCircle(lx, ly, 3);
        }
        RectNode::endDraw();
    }

    void shutdown() {
        if (canvas_) canvas_->shutdown();
        if (strip_) strip_->shutdown();
    }

private:
    MapCanvas::Ptr canvas_;
    PhotoStrip::Ptr strip_;

    // Keep a copy of photo data for auto-geotag
    vector<PhotoEntry> photos_;
    vector<string> photoIds_;

    // Drag ghost state
    bool dragActive_ = false;
    float dragGlobalX_ = 0, dragGlobalY_ = 0;

    void layoutChildren() {
        if (canvas_) canvas_->setRect(0, 0, getWidth(), mapHeight());
        if (strip_) strip_->setRect(0, mapHeight(), getWidth(), stripHeight());
    }

    // Sync photos_ copy with actual GPS state
    void updatePhotoCopy(const string& photoId, double lat, double lon) {
        for (size_t i = 0; i < photoIds_.size(); i++) {
            if (photoIds_[i] == photoId) {
                photos_[i].latitude = lat;
                photos_[i].longitude = lon;
                break;
            }
        }
    }
};
