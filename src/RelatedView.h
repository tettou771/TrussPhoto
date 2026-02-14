#pragma once

// =============================================================================
// RelatedView.h - Related photos view with timeline strip and similarity graph
// =============================================================================

#include <TrussC.h>
#include "PhotoProvider.h"
#include "AsyncImageLoader.h"
#include "FolderTree.h"  // for loadJapaneseFont
#include <ctime>

using namespace std;
using namespace tc;

class RelatedView : public RectNode {
public:
    using Ptr = shared_ptr<RelatedView>;

    // Callbacks
    function<void(const string& photoId)> onPhotoClick;
    function<void(const string& photoId)> onCenterDoubleClick;  // center photo → single view
    function<void()> onRedraw;

    void setup() override {
        enableEvents();
        loadJapaneseFont(font_, 12);
        loadJapaneseFont(fontSmall_, 10);
    }

    // Set center photo and compute layout
    void setCenter(const string& centerId, PhotoProvider& provider,
                   bool pushToHistory = true) {
        // --- Snapshot old layout before clearing ---
        unordered_map<string, AnimSnapshot> oldSnapshots;
        bool hasOldLayout = !centerId_.empty();
        if (hasOldLayout) {
            // Center
            oldSnapshots[centerItem_.photoId] = {centerItem_.position, centerItem_.displaySize};
            // Timeline
            for (const auto& item : timelineItems_) {
                oldSnapshots[item.photoId] = {item.position, item.displaySize};
            }
            // Related
            for (const auto& item : relatedItems_) {
                oldSnapshots[item.photoId] = {item.position, item.displaySize};
            }
            // History items
            for (int i = 0; i < (int)history_.size(); i++) {
                Vec2 hPos = {0, HISTORY_START_Y + i * HISTORY_SPACING};
                // Reverse order: newest at top (index = history_.size()-1-i from bottom)
                int drawIdx = (int)history_.size() - 1 - i;
                hPos.y = HISTORY_START_Y + drawIdx * HISTORY_SPACING;
                oldSnapshots[history_[i]] = {hPos, HISTORY_SIZE};
            }

            // Push old center to history
            if (pushToHistory && !centerId_.empty()) {
                history_.push_back(centerId_);
                if ((int)history_.size() > MAX_HISTORY) {
                    history_.erase(history_.begin());
                }
            }
        }

        // --- Clear and recompute layout ---
        centerId_ = centerId;
        provider_ = &provider;
        timelineItems_.clear();
        relatedItems_.clear();
        centerItem_ = {};
        pendingLoads_.clear();

        auto* centerEntry = provider.getPhoto(centerId);
        if (!centerEntry) return;

        // Center item
        centerItem_.photoId = centerId;
        centerItem_.score = 1.0f;
        centerItem_.position = {0, 0};
        centerItem_.displaySize = CENTER_SIZE;
        centerItem_.width = centerEntry->width;
        centerItem_.height = centerEntry->height;
        centerItem_.isTimeline = false;

        // Build timeline
        buildTimeline(provider);

        // Compute related photos
        computeRelated(provider);

        // Layout related photos
        layoutRelated();

        // --- Smart texture management: keep textures still in use ---
        unordered_set<string> neededIds;
        neededIds.insert(centerItem_.photoId);
        for (const auto& item : timelineItems_) neededIds.insert(item.photoId);
        for (const auto& item : relatedItems_) neededIds.insert(item.photoId);
        for (const auto& hid : history_) neededIds.insert(hid);

        // Remove textures no longer needed
        for (auto it = textures_.begin(); it != textures_.end(); ) {
            if (neededIds.count(it->first) == 0) {
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }

        // Queue loads only for items without textures
        queueMissingThumbnails(neededIds);

        // --- Set up morphing animation ---
        if (hasOldLayout) {
            animOldSnapshots_.clear();
            // For each item in the new layout, check if it existed in old layout
            auto checkAndAdd = [&](const string& id, Vec2 newPos, float newSize) {
                auto it = oldSnapshots.find(id);
                if (it != oldSnapshots.end()) {
                    animOldSnapshots_[id] = it->second;
                }
            };
            checkAndAdd(centerItem_.photoId, centerItem_.position, centerItem_.displaySize);
            for (const auto& item : timelineItems_) {
                checkAndAdd(item.photoId, item.position, item.displaySize);
            }
            for (const auto& item : relatedItems_) {
                checkAndAdd(item.photoId, item.position, item.displaySize);
            }
            // History items
            for (int i = 0; i < (int)history_.size(); i++) {
                int drawIdx = (int)history_.size() - 1 - i;
                Vec2 hPos = {0, HISTORY_START_Y + drawIdx * HISTORY_SPACING};
                checkAndAdd(history_[i], hPos, HISTORY_SIZE);
            }

            // Start animation
            animProgress_.from(0).to(1).duration(0.4f).ease(EaseType::Cubic, EaseMode::Out).start();
            lastAnimTime_ = chrono::steady_clock::now();
            animating_ = true;
        }

        logNotice() << "[RelatedView] center=" << centerId
                    << " timeline=" << timelineItems_.size()
                    << " related=" << relatedItems_.size()
                    << " history=" << history_.size();
    }

    void shutdown() {
        textures_.clear();
        pendingLoads_.clear();
        timelineItems_.clear();
        relatedItems_.clear();
        history_.clear();
        animOldSnapshots_.clear();
        animating_ = false;
        if (loadThread_.joinable()) loadThread_.join();
    }

    void update() override {
        // Process completed thumbnail loads
        bool anyNew = false;
        {
            lock_guard<mutex> lock(loadMutex_);
            for (auto& result : loadResults_) {
                if (result.pixels.isAllocated()) {
                    Texture tex;
                    tex.allocate(result.pixels, TextureUsage::Immutable, false);
                    textures_[result.photoId] = std::move(tex);
                    anyNew = true;
                }
            }
            loadResults_.clear();
        }
        if (anyNew && onRedraw) onRedraw();

        // Start load thread if needed
        if (!pendingLoads_.empty() && !loadThreadRunning_) {
            startLoadThread();
        }

        // Morphing animation update
        if (animating_) {
            auto now = chrono::steady_clock::now();
            float dt = chrono::duration<float>(now - lastAnimTime_).count();
            lastAnimTime_ = now;
            animProgress_.update(dt);
            if (onRedraw) onRedraw();
            if (animProgress_.isComplete()) {
                animating_ = false;
                animOldSnapshots_.clear();
            }
        }
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.06f, 0.06f, 0.08f);
        fill();
        drawRect(0, 0, w, h);

        setClipping(true);

        // Animated center position
        Vec2 centerAnimPos = getAnimatedPosition(centerItem_.photoId, centerItem_.position);
        Vec2 centerScreen = worldToScreen(centerAnimPos);

        // Draw connection lines: center → related items
        for (const auto& item : relatedItems_) {
            Vec2 animPos = getAnimatedPosition(item.photoId, item.position);
            Vec2 to = worldToScreen(animPos);
            float alpha = 0.15f + item.score * 0.2f;
            setColor(0.4f, 0.4f, 0.5f, alpha);
            noFill();
            drawLine(centerScreen.x, centerScreen.y, to.x, to.y);
        }

        // Draw related items (far/low score first, near/high score on top)
        for (int i = (int)relatedItems_.size() - 1; i >= 0; i--) {
            auto& item = relatedItems_[i];
            Vec2 animPos = getAnimatedPosition(item.photoId, item.position);
            float animSize = getAnimatedSize(item.photoId, item.displaySize);
            drawItemAt(item.photoId, worldToScreen(animPos), animSize * zoom_,
                       Color(0.25f, 0.25f, 0.3f));
        }

        // Draw timeline items
        for (auto& item : timelineItems_) {
            Vec2 animPos = getAnimatedPosition(item.photoId, item.position);
            float animSize = getAnimatedSize(item.photoId, item.displaySize);
            drawItemAt(item.photoId, worldToScreen(animPos), animSize * zoom_,
                       Color(0.35f, 0.35f, 0.4f));
        }

        // Draw history chain (below center)
        drawHistoryChain();

        // Draw center → history connection line
        if (!history_.empty()) {
            int topIdx = (int)history_.size() - 1;
            Vec2 hWorldPos = {0, HISTORY_START_Y};
            Vec2 hAnimPos = getAnimatedPosition(history_[topIdx], hWorldPos);
            Vec2 hScreen = worldToScreen(hAnimPos);
            setColor(0.3f, 0.4f, 0.55f, 0.4f);
            noFill();
            drawLine(centerScreen.x, centerScreen.y, hScreen.x, hScreen.y);
        }

        // Draw center item (topmost)
        {
            float animSize = getAnimatedSize(centerItem_.photoId, centerItem_.displaySize);
            drawItemAt(centerItem_.photoId, centerScreen, animSize * zoom_,
                       Color(0.3f, 0.5f, 0.7f));
        }

        // Score labels for related items
        for (const auto& item : relatedItems_) {
            Vec2 animPos = getAnimatedPosition(item.photoId, item.position);
            float animSize = getAnimatedSize(item.photoId, item.displaySize);
            Vec2 sp = worldToScreen(animPos);
            float sz = animSize * zoom_;
            setColor(0.5f, 0.5f, 0.55f);
            string label = format("{:.0f}%", item.score * 100);
            fontSmall_.drawString(label, sp.x, sp.y + sz / 2 + 12,
                Direction::Center, Direction::Center);
        }

        setClipping(false);

        // View mode label
        setColor(0.0f, 0.0f, 0.0f, 0.5f);
        fill();
        drawRect(8, h - 28, 160, 20);
        setColor(0.7f, 0.7f, 0.75f);
        string modeLabel = format("Related  Zoom: {:.1f}", zoom_);
        if (!history_.empty()) modeLabel += format("  History: {}", history_.size());
        fontSmall_.drawString(modeLabel, 14, h - 18,
            Direction::Left, Direction::Center);
    }

    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;

        // Ignore clicks during animation
        if (animating_) return true;

        // Check for click on any item
        string hitId = hitTest(pos);
        if (!hitId.empty()) {
            auto now = chrono::steady_clock::now();
            bool isDouble = (hitId == lastClickId_ &&
                chrono::duration_cast<chrono::milliseconds>(now - lastClickTime_).count() < 400);
            lastClickTime_ = now;
            lastClickId_ = hitId;

            // Handle history item clicks
            if (hitId.starts_with("history:")) {
                int histIdx = stoi(hitId.substr(8));
                if (histIdx >= 0 && histIdx < (int)history_.size()) {
                    string targetId = history_[histIdx];
                    if (isDouble) {
                        // Double-click history → undo to that point
                        history_.resize(histIdx);  // truncate: remove this and newer
                        setCenter(targetId, *provider_, false);  // don't push to history
                        if (onRedraw) onRedraw();
                    } else if (onPhotoClick) {
                        onPhotoClick(targetId);
                    }
                }
                return true;
            }

            if (isDouble) {
                if (hitId == centerId_) {
                    // Double-click center → open in single view
                    if (onCenterDoubleClick) onCenterDoubleClick(hitId);
                } else {
                    // Double-click other → re-center on that photo
                    if (onPhotoClick) onPhotoClick(hitId);
                    setCenter(hitId, *provider_);
                    if (onRedraw) onRedraw();
                }
            } else if (onPhotoClick) {
                onPhotoClick(hitId);
            }
            return true;
        }

        // Start dragging
        dragging_ = true;
        dragStart_ = pos;
        dragPanStart_ = panOffset_;
        return true;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (!dragging_ || button != 0) return false;
        Vec2 delta = pos - dragStart_;
        panOffset_ = dragPanStart_ + delta / zoom_;
        if (onRedraw) onRedraw();
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 0) dragging_ = false;
        return true;
    }

    bool onMouseScroll(Vec2 pos, Vec2 scroll) override {
        float oldZoom = zoom_;
        zoom_ *= (1.0f + scroll.y * 0.1f);
        zoom_ = clamp(zoom_, 0.3f, 3.0f);

        // Zoom toward mouse position
        Vec2 center = {getWidth() / 2, getHeight() / 2};
        Vec2 toMouse = pos - center;
        float ratio = zoom_ / oldZoom;
        panOffset_ = panOffset_ - toMouse * (ratio - 1.0f) / zoom_;

        if (onRedraw) onRedraw();
        return true;
    }

private:
    static constexpr float CENTER_SIZE = 260.0f;
    static constexpr float TIMELINE_SIZE = 100.0f;
    static constexpr float RELATED_SIZE_MIN = 60.0f;
    static constexpr float RELATED_SIZE_MAX = 120.0f;
    static constexpr float TIMELINE_SPACING = 12.0f;
    static constexpr int TIMELINE_COUNT = 15;
    static constexpr int MAX_RELATED = 20;
    static constexpr int COLLISION_ITERATIONS = 8;

    // History chain constants
    static constexpr float HISTORY_SIZE = 80.0f;
    static constexpr float HISTORY_SPACING = 120.0f;
    static constexpr float HISTORY_START_Y = 200.0f;
    static constexpr int MAX_HISTORY = 10;

    // Animation snapshot for morphing transitions
    struct AnimSnapshot {
        Vec2 position;
        float displaySize;
    };

    struct RelatedItem {
        string photoId;
        float score;
        Vec2 position;        // world coordinates (center = origin)
        float displaySize;
        int width = 0, height = 0;
        bool isTimeline = false;
        int timelineIndex = 0;  // -5..+5
    };

    string centerId_;
    PhotoProvider* provider_ = nullptr;
    RelatedItem centerItem_;
    vector<RelatedItem> timelineItems_;
    vector<RelatedItem> relatedItems_;

    // Textures keyed by photoId
    unordered_map<string, Texture> textures_;

    // Background thumbnail loading (simple thread, no AsyncImageLoader dependency)
    struct LoadResult {
        string photoId;
        Pixels pixels;
    };
    mutex loadMutex_;
    vector<LoadResult> loadResults_;
    vector<string> pendingLoads_;
    atomic<bool> loadThreadRunning_{false};
    thread loadThread_;

    // Interaction
    Vec2 panOffset_ = {0, 0};
    float zoom_ = 1.0f;
    bool dragging_ = false;
    Vec2 dragStart_;
    Vec2 dragPanStart_;

    // Double-click
    chrono::steady_clock::time_point lastClickTime_;
    string lastClickId_;

    // Morphing animation
    Tween<float> animProgress_;
    unordered_map<string, AnimSnapshot> animOldSnapshots_;
    bool animating_ = false;
    chrono::steady_clock::time_point lastAnimTime_;

    // History chain (oldest first)
    vector<string> history_;

    // Fonts
    Font font_;
    Font fontSmall_;

    // --- Animation interpolation ---

    Vec2 getAnimatedPosition(const string& photoId, Vec2 targetPos) const {
        if (!animating_) return targetPos;
        auto it = animOldSnapshots_.find(photoId);
        if (it == animOldSnapshots_.end()) return targetPos;
        float t = animProgress_.getValue();
        return it->second.position.lerp(targetPos, t);
    }

    float getAnimatedSize(const string& photoId, float targetSize) const {
        if (!animating_) return targetSize;
        auto it = animOldSnapshots_.find(photoId);
        if (it == animOldSnapshots_.end()) return targetSize;
        float t = animProgress_.getValue();
        return it->second.displaySize + (targetSize - it->second.displaySize) * t;
    }

    // --- Coordinate transforms ---

    Vec2 worldToScreen(Vec2 w) const {
        return {getWidth() / 2 + (w.x + panOffset_.x) * zoom_,
                getHeight() / 2 + (w.y + panOffset_.y) * zoom_};
    }

    Vec2 screenToWorld(Vec2 s) const {
        return {(s.x - getWidth() / 2) / zoom_ - panOffset_.x,
                (s.y - getHeight() / 2) / zoom_ - panOffset_.y};
    }

    // --- Hit test ---

    string hitTest(Vec2 screenPos) const {
        // Check center first (topmost)
        {
            Vec2 animPos = getAnimatedPosition(centerItem_.photoId, centerItem_.position);
            Vec2 sp = worldToScreen(animPos);
            float sz = getAnimatedSize(centerItem_.photoId, centerItem_.displaySize) * zoom_;
            if (abs(screenPos.x - sp.x) < sz / 2 && abs(screenPos.y - sp.y) < sz / 2) {
                return centerItem_.photoId;
            }
        }

        // Timeline
        for (const auto& item : timelineItems_) {
            Vec2 animPos = getAnimatedPosition(item.photoId, item.position);
            Vec2 sp = worldToScreen(animPos);
            float sz = getAnimatedSize(item.photoId, item.displaySize) * zoom_;
            if (abs(screenPos.x - sp.x) < sz / 2 && abs(screenPos.y - sp.y) < sz / 2) {
                return item.photoId;
            }
        }

        // Related (front to back = high score first)
        for (const auto& item : relatedItems_) {
            Vec2 animPos = getAnimatedPosition(item.photoId, item.position);
            Vec2 sp = worldToScreen(animPos);
            float sz = getAnimatedSize(item.photoId, item.displaySize) * zoom_;
            if (abs(screenPos.x - sp.x) < sz / 2 && abs(screenPos.y - sp.y) < sz / 2) {
                return item.photoId;
            }
        }

        // History items
        for (int i = 0; i < (int)history_.size(); i++) {
            int drawIdx = (int)history_.size() - 1 - i;
            Vec2 worldPos = {0, HISTORY_START_Y + drawIdx * HISTORY_SPACING};
            Vec2 animPos = getAnimatedPosition(history_[i], worldPos);
            Vec2 sp = worldToScreen(animPos);
            float sz = getAnimatedSize(history_[i], HISTORY_SIZE) * zoom_;
            if (abs(screenPos.x - sp.x) < sz / 2 && abs(screenPos.y - sp.y) < sz / 2) {
                return "history:" + to_string(i);
            }
        }

        return "";
    }

    // --- Drawing ---

    // Draw a photo item at a specific screen position and size
    void drawItemAt(const string& photoId, Vec2 screenPos, float screenSize,
                    Color borderColor) {
        float halfSz = screenSize / 2;

        // Look up texture
        auto texIt = textures_.find(photoId);
        Texture* tex = (texIt != textures_.end() && texIt->second.isAllocated())
            ? &texIt->second : nullptr;

        // Border
        setColor(borderColor);
        fill();
        drawRect(screenPos.x - halfSz - 2, screenPos.y - halfSz - 2,
                 screenSize + 4, screenSize + 4);

        if (tex) {
            // Fit image into square area preserving aspect ratio
            float imgW = tex->getWidth();
            float imgH = tex->getHeight();
            float fitScale = min(screenSize / imgW, screenSize / imgH);
            float drawW = imgW * fitScale;
            float drawH = imgH * fitScale;

            setColor(1.0f, 1.0f, 1.0f);
            tex->draw(screenPos.x - drawW / 2, screenPos.y - drawH / 2, drawW, drawH);
        } else {
            // Placeholder
            setColor(0.15f, 0.15f, 0.18f);
            fill();
            drawRect(screenPos.x - halfSz, screenPos.y - halfSz, screenSize, screenSize);
        }
    }

    // Draw history chain below center
    void drawHistoryChain() {
        if (history_.empty()) return;

        for (int i = 0; i < (int)history_.size(); i++) {
            // Draw newest at top (closest to center), oldest at bottom
            int drawIdx = (int)history_.size() - 1 - i;
            Vec2 worldPos = {0, HISTORY_START_Y + drawIdx * HISTORY_SPACING};
            Vec2 animPos = getAnimatedPosition(history_[i], worldPos);
            Vec2 screenPos = worldToScreen(animPos);
            float animSize = getAnimatedSize(history_[i], HISTORY_SIZE);
            float sz = animSize * zoom_;

            // Connection line to next history item
            if (drawIdx > 0) {
                int nextI = i - 1;  // The item drawn below this one
                if (nextI >= 0) {
                    int nextDrawIdx = (int)history_.size() - 1 - nextI;
                    Vec2 nextWorldPos = {0, HISTORY_START_Y + nextDrawIdx * HISTORY_SPACING};
                    Vec2 nextAnimPos = getAnimatedPosition(history_[nextI], nextWorldPos);
                    Vec2 nextScreen = worldToScreen(nextAnimPos);
                    setColor(0.25f, 0.3f, 0.4f, 0.3f);
                    noFill();
                    drawLine(screenPos.x, screenPos.y, nextScreen.x, nextScreen.y);
                }
            }

            // Draw the history item
            drawItemAt(history_[i], screenPos, sz, Color(0.2f, 0.25f, 0.35f));

            // Label: #N (1-based, 1 = oldest)
            setColor(0.45f, 0.45f, 0.55f);
            string label = format("#{}", i + 1);
            fontSmall_.drawString(label, screenPos.x, screenPos.y + sz / 2 + 10,
                Direction::Center, Direction::Center);
        }
    }

    // --- Timeline construction ---

    // Parse "YYYY:MM:DD HH:MM:SS" to time_t
    static int64_t parseDateTimeOriginal(const string& dt) {
        if (dt.size() < 19) return 0;
        try {
            tm t = {};
            t.tm_year = stoi(dt.substr(0, 4)) - 1900;
            t.tm_mon = stoi(dt.substr(5, 2)) - 1;
            t.tm_mday = stoi(dt.substr(8, 2));
            t.tm_hour = stoi(dt.substr(11, 2));
            t.tm_min = stoi(dt.substr(14, 2));
            t.tm_sec = stoi(dt.substr(17, 2));
            return (int64_t)mktime(&t);
        } catch (...) {
            return 0;
        }
    }

    void buildTimeline(PhotoProvider& provider) {
        // Sort all photos by dateTimeOriginal
        struct TimeEntry {
            string id;
            int64_t time;
        };
        vector<TimeEntry> sorted;
        for (const auto& [id, entry] : provider.photos()) {
            int64_t t = parseDateTimeOriginal(entry.dateTimeOriginal);
            sorted.push_back({id, t});
        }
        sort(sorted.begin(), sorted.end(),
             [](const TimeEntry& a, const TimeEntry& b) { return a.time < b.time; });

        // Find center index
        int centerIdx = -1;
        for (int i = 0; i < (int)sorted.size(); i++) {
            if (sorted[i].id == centerId_) {
                centerIdx = i;
                break;
            }
        }
        if (centerIdx < 0) return;

        // Collect before/after
        for (int offset = -TIMELINE_COUNT; offset <= TIMELINE_COUNT; offset++) {
            if (offset == 0) continue;
            int idx = centerIdx + offset;
            if (idx < 0 || idx >= (int)sorted.size()) continue;

            auto* entry = provider.getPhoto(sorted[idx].id);
            if (!entry) continue;

            RelatedItem item;
            item.photoId = sorted[idx].id;
            item.score = 0;
            item.isTimeline = true;
            item.timelineIndex = offset;
            item.displaySize = TIMELINE_SIZE;
            item.width = entry->width;
            item.height = entry->height;

            // Position: start from center edge, no overlap
            float sign = offset > 0 ? 1.0f : -1.0f;
            int slot = abs(offset) - 1;  // 0-based slot index
            float startX = CENTER_SIZE / 2 + TIMELINE_SPACING + TIMELINE_SIZE / 2;
            float x = sign * (startX + slot * (TIMELINE_SIZE + TIMELINE_SPACING));
            item.position = {x, 0};

            timelineItems_.push_back(item);
        }
    }

    // --- Related photos computation ---

    // Haversine distance in km
    static double haversine(double lat1, double lon1, double lat2, double lon2) {
        double R = 6371.0;
        double dLat = (lat2 - lat1) * M_PI / 180.0;
        double dLon = (lon2 - lon1) * M_PI / 180.0;
        double a = sin(dLat / 2) * sin(dLat / 2) +
                   cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
                   sin(dLon / 2) * sin(dLon / 2);
        return R * 2 * atan2(sqrt(a), sqrt(1 - a));
    }

    void computeRelated(PhotoProvider& provider) {
        auto similar = provider.findSimilar(centerId_, MAX_RELATED * 2);
        if (similar.empty()) return;

        auto* centerEntry = provider.getPhoto(centerId_);

        // Build set of timeline IDs for exclusion
        unordered_set<string> timelineIds;
        timelineIds.insert(centerId_);
        for (const auto& tl : timelineItems_) {
            timelineIds.insert(tl.photoId);
        }

        vector<pair<string, float>> candidates;
        for (const auto& sr : similar) {
            if (timelineIds.count(sr.photoId)) continue;

            float combined = sr.score * 0.85f;

            // GPS bonus
            if (centerEntry && centerEntry->hasGps()) {
                auto* other = provider.getPhoto(sr.photoId);
                if (other && other->hasGps()) {
                    double dist = haversine(
                        centerEntry->latitude, centerEntry->longitude,
                        other->latitude, other->longitude);
                    float gpsBonus = 0.15f / (1.0f + (float)dist / 2.0f);
                    combined += gpsBonus;
                }
            }

            candidates.push_back({sr.photoId, combined});
        }

        // Sort by combined score descending
        sort(candidates.begin(), candidates.end(),
             [](const auto& a, const auto& b) { return a.second > b.second; });

        // Take top MAX_RELATED
        int count = min((int)candidates.size(), MAX_RELATED);
        for (int i = 0; i < count; i++) {
            auto* entry = provider.getPhoto(candidates[i].first);
            if (!entry) continue;

            RelatedItem item;
            item.photoId = candidates[i].first;
            item.score = candidates[i].second;
            item.displaySize = RELATED_SIZE_MIN +
                (RELATED_SIZE_MAX - RELATED_SIZE_MIN) * candidates[i].second;
            item.width = entry->width;
            item.height = entry->height;
            item.isTimeline = false;

            relatedItems_.push_back(item);
        }
    }

    // --- Layout ---

    void layoutRelated() {
        if (relatedItems_.empty()) return;

        // Normalize scores for distance mapping
        float maxScore = relatedItems_.front().score;
        float minScore = relatedItems_.back().score;
        float scoreRange = max(maxScore - minScore, 0.01f);

        // Radii (compact layout)
        float innerRadius = CENTER_SIZE / 2 + 60;
        float outerRadius = innerRadius + 160;

        // Golden angle spiral placement
        float goldenAngle = TAU * (1.0f - 1.0f / 1.618033988749895f);
        float angleOffset = TAU * 0.125f;  // 45° offset to avoid timeline axis

        for (int i = 0; i < (int)relatedItems_.size(); i++) {
            float normalized = (relatedItems_[i].score - minScore) / scoreRange;
            // Higher score = closer to center
            float dist = innerRadius + (1.0f - normalized * normalized) * (outerRadius - innerRadius);
            float angle = angleOffset + i * goldenAngle;
            relatedItems_[i].position = {cos(angle) * dist, sin(angle) * dist};
        }

        // Collision resolution
        resolveCollisions();
    }

    void resolveCollisions() {
        struct ColItem {
            Vec2* pos;
            float radius;
            bool fixed;
        };
        vector<ColItem> items;

        // Center is fixed
        items.push_back({&centerItem_.position, CENTER_SIZE / 2, true});

        // Timeline items are fixed
        for (auto& tl : timelineItems_) {
            items.push_back({&tl.position, TIMELINE_SIZE / 2, true});
        }

        // Related items are movable
        for (auto& rel : relatedItems_) {
            items.push_back({&rel.position, rel.displaySize / 2, false});
        }

        for (int iter = 0; iter < COLLISION_ITERATIONS; iter++) {
            for (int i = 0; i < (int)items.size(); i++) {
                for (int j = i + 1; j < (int)items.size(); j++) {
                    Vec2 diff = *items[j].pos - *items[i].pos;
                    float dist = diff.length();
                    float minDist = items[i].radius + items[j].radius + 8;

                    if (dist < minDist && dist > 0.01f) {
                        Vec2 push = diff / dist * (minDist - dist);
                        if (items[i].fixed && !items[j].fixed) {
                            *items[j].pos = *items[j].pos + push;
                        } else if (!items[i].fixed && items[j].fixed) {
                            *items[i].pos = *items[i].pos - push;
                        } else if (!items[i].fixed && !items[j].fixed) {
                            *items[i].pos = *items[i].pos - push * 0.5f;
                            *items[j].pos = *items[j].pos + push * 0.5f;
                        }
                    }
                }
            }
        }
    }

    // --- Thumbnail loading (own thread, load from cached file paths) ---

    void queueAllThumbnails() {
        pendingLoads_.clear();
        pendingLoads_.push_back(centerItem_.photoId);
        for (auto& item : timelineItems_) pendingLoads_.push_back(item.photoId);
        for (auto& item : relatedItems_) pendingLoads_.push_back(item.photoId);
    }

    // Queue only IDs that don't have textures yet
    void queueMissingThumbnails(const unordered_set<string>& neededIds) {
        pendingLoads_.clear();
        for (const auto& id : neededIds) {
            if (textures_.count(id) == 0) {
                pendingLoads_.push_back(id);
            }
        }
    }

    void startLoadThread() {
        if (loadThreadRunning_) return;
        loadThreadRunning_ = true;

        if (loadThread_.joinable()) loadThread_.join();

        vector<string> ids = std::move(pendingLoads_);
        pendingLoads_.clear();

        // Snapshot thumbnail paths from provider (main thread, safe)
        struct LoadTask {
            string photoId;
            string thumbPath;
        };
        vector<LoadTask> tasks;
        for (const auto& id : ids) {
            auto* entry = provider_->getPhoto(id);
            if (!entry) continue;
            tasks.push_back({id, entry->localThumbnailPath});
        }

        loadThread_ = thread([this, tasks = std::move(tasks)]() {
            for (const auto& task : tasks) {
                Pixels px;
                bool loaded = false;

                // Load directly from cached thumbnail file
                if (!task.thumbPath.empty() && fs::exists(task.thumbPath)) {
                    loaded = px.load(task.thumbPath);
                }

                if (loaded) {
                    lock_guard<mutex> lock(loadMutex_);
                    loadResults_.push_back({task.photoId, std::move(px)});
                }
            }
            loadThreadRunning_ = false;
        });
        loadThread_.detach();
    }
};
