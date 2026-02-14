#pragma once

// =============================================================================
// RelatedView.h - Related photos view with timeline strip and similarity graph
// Node-tree based architecture: each photo is a PhotoItemNode for automatic
// hit testing, z-order, and transform propagation.
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
        ensureInitialized();
    }

    // Set center photo and compute layout
    void setCenter(const string& centerId, PhotoProvider& provider,
                   bool pushToHistory = true) {
        // Ensure node tree is initialized (setup() may not have been called yet
        // if the view was created with setActive(false))
        ensureInitialized();

        // --- Snapshot old layout before clearing ---
        unordered_map<string, AnimSnapshot> oldSnapshots;
        bool hasOldLayout = !centerId_.empty();
        if (hasOldLayout) {
            // Collect snapshots from existing nodes
            if (centerNode_) {
                oldSnapshots[centerNode_->photoId] = {
                    centerNode_->targetWorldPos, centerNode_->targetSize};
            }
            for (auto* node : timelineNodes_) {
                oldSnapshots[node->photoId] = {
                    node->targetWorldPos, node->targetSize};
            }
            for (auto* node : relatedNodes_) {
                oldSnapshots[node->photoId] = {
                    node->targetWorldPos, node->targetSize};
            }
            for (auto* node : historyNodes_) {
                oldSnapshots[node->photoId] = {
                    node->targetWorldPos, node->targetSize};
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
        pendingLoads_.clear();

        // Clear layout items (temporary data for computation)
        timelineItems_.clear();
        relatedItems_.clear();
        centerItem_ = {};

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

        // Layout related photos (pass old positions for continuity)
        {
            unordered_map<string, Vec2> oldPos;
            if (hasOldLayout) {
                for (const auto& [id, snap] : oldSnapshots) {
                    oldPos[id] = snap.position;
                }
            }
            layoutRelated(oldPos);
        }

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

        // --- Rebuild node tree ---
        rebuildNodes(oldSnapshots, hasOldLayout);

        logNotice() << "[RelatedView] center=" << centerId
                    << " timeline=" << timelineNodes_.size()
                    << " related=" << relatedNodes_.size()
                    << " history=" << historyNodes_.size();
    }

    void shutdown() {
        if (contentLayer_) contentLayer_->removeAllChildren();
        centerNode_ = nullptr;
        centerId_.clear();
        relatedNodes_.clear();
        timelineNodes_.clear();
        historyNodes_.clear();
        textures_.clear();
        pendingLoads_.clear();
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

        // Update texture references on all photo nodes
        if (anyNew) {
            for (auto* node : allPhotoNodes()) {
                auto it = textures_.find(node->photoId);
                node->textureRef = (it != textures_.end() && it->second.isAllocated())
                    ? &it->second : nullptr;
            }
            if (onRedraw) onRedraw();
        }

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

            float t = animProgress_.getValue();

            // Interpolate all photo node positions
            for (auto* node : allPhotoNodes()) {
                auto it = animOldSnapshots_.find(node->photoId);
                if (it != animOldSnapshots_.end()) {
                    Vec2 curPos = it->second.position.lerp(node->targetWorldPos, t);
                    float curSize = it->second.displaySize +
                        (node->targetSize - it->second.displaySize) * t;
                    node->setPos(curPos.x - curSize / 2, curPos.y - curSize / 2);
                    node->setSize(curSize, curSize);
                }
            }

            if (onRedraw) onRedraw();
            if (animProgress_.isComplete()) {
                // Snap to final positions
                for (auto* node : allPhotoNodes()) {
                    node->setPos(node->targetWorldPos.x - node->targetSize / 2,
                                 node->targetWorldPos.y - node->targetSize / 2);
                    node->setSize(node->targetSize, node->targetSize);
                }
                animating_ = false;
                animOldSnapshots_.clear();
            }
        }
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Keep contentLayer_ centered even when view size changes
        updateContentTransform();

        // Background
        setColor(0.06f, 0.06f, 0.08f);
        fill();
        drawRect(0, 0, w, h);

        // Connection lines (drawn BEFORE child nodes render)
        if (centerNode_) {
            Vec2 centerWorld = animating_
                ? getAnimatedWorldPos(centerNode_) : centerNode_->targetWorldPos;
            Vec2 centerScreen = worldToScreen(centerWorld);

            // Center → related
            for (auto* node : relatedNodes_) {
                Vec2 cur = animating_
                    ? getAnimatedWorldPos(node) : node->targetWorldPos;
                Vec2 to = worldToScreen(cur);
                float alpha = 0.25f + node->score * 0.35f;
                setColor(0.55f, 0.55f, 0.7f, alpha);
                noFill();
                drawLine(centerScreen.x, centerScreen.y, to.x, to.y);
            }

            // Center → topmost history
            if (!historyNodes_.empty()) {
                auto* topHist = historyNodes_.back();  // newest = last
                Vec2 hWorld = animating_
                    ? getAnimatedWorldPos(topHist) : topHist->targetWorldPos;
                Vec2 hScreen = worldToScreen(hWorld);
                setColor(0.45f, 0.55f, 0.75f, 0.6f);
                noFill();
                drawLine(centerScreen.x, centerScreen.y, hScreen.x, hScreen.y);
            }

            // History chain inter-connections
            for (int i = 1; i < (int)historyNodes_.size(); i++) {
                Vec2 a = animating_
                    ? getAnimatedWorldPos(historyNodes_[i]) : historyNodes_[i]->targetWorldPos;
                Vec2 b = animating_
                    ? getAnimatedWorldPos(historyNodes_[i-1]) : historyNodes_[i-1]->targetWorldPos;
                Vec2 sa = worldToScreen(a);
                Vec2 sb = worldToScreen(b);
                setColor(0.4f, 0.45f, 0.6f, 0.5f);
                noFill();
                drawLine(sa.x, sa.y, sb.x, sb.y);
            }
        }

        // Child nodes (contentLayer_ with photo nodes) are drawn automatically
        // by the node tree after this draw() returns

        // Update overlay
        if (overlayNode_) {
            overlayNode_->setSize(w, h);
            string modeLabel = format("Related  Zoom: {:.1f}", zoom_);
            if (!history_.empty()) modeLabel += format("  History: {}", history_.size());
            overlayNode_->text = modeLabel;
        }
    }

    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;
        // If we get here, no child node consumed the click → empty space drag
        dragging_ = true;
        dragStart_ = pos;
        dragPanStart_ = panOffset_;
        return true;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (!dragging_ || button != 0) return false;
        Vec2 delta = pos - dragStart_;
        panOffset_ = dragPanStart_ + delta / zoom_;
        updateContentTransform();
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

        updateContentTransform();
        if (onRedraw) onRedraw();
        return true;
    }

private:
    bool initialized_ = false;

    void ensureInitialized() {
        if (initialized_) return;
        initialized_ = true;

        enableEvents();  // pan/drag/zoom on empty space
        setClipping(true);
        loadJapaneseFont(font_, 12);
        loadJapaneseFont(fontSmall_, 10);

        contentLayer_ = make_shared<Node>();
        addChild(contentLayer_);

        overlayNode_ = make_shared<OverlayNode>();
        overlayNode_->fontRef = &fontSmall_;
        addChild(overlayNode_);
    }

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

    // =========================================================================
    // Inner node: PhotoItemNode
    // =========================================================================
    class PhotoItemNode : public RectNode {
    public:
        string photoId;
        float score = 0;
        Color borderColor;
        string label;  // "83%" or "#3"

        Vec2 targetWorldPos;   // layout target (world center coord)
        float targetSize = 0;  // layout target size

        // Borrowed pointers (owned by RelatedView)
        Texture* textureRef = nullptr;
        Font* fontRef = nullptr;

        // Callback: (photoId, isDoubleClick)
        function<void(const string&, bool)> onClicked;

        void setup() override {
            enableEvents();
        }

        void draw() override {
            float w = getWidth(), h = getHeight();

            // Border (drawn slightly outside)
            setColor(borderColor);
            fill();
            drawRect(-2, -2, w + 4, h + 4);

            // Image or placeholder
            if (textureRef && textureRef->isAllocated()) {
                float imgW = textureRef->getWidth();
                float imgH = textureRef->getHeight();
                float fitScale = min(w / imgW, h / imgH);
                float dw = imgW * fitScale, dh = imgH * fitScale;
                setColor(1, 1, 1);
                textureRef->draw((w - dw) / 2, (h - dh) / 2, dw, dh);
            } else {
                setColor(0.15f, 0.15f, 0.18f);
                fill();
                drawRect(0, 0, w, h);
            }

            // Score/history label below item
            if (!label.empty() && fontRef) {
                setColor(0.5f, 0.5f, 0.55f);
                fontRef->drawString(label, w / 2, h + 12,
                    Direction::Center, Direction::Center);
            }
        }

        bool onMousePress(Vec2 pos, int button) override {
            (void)pos;
            if (button != 0) return false;
            auto now = chrono::steady_clock::now();
            bool isDouble = (chrono::duration_cast<chrono::milliseconds>(
                now - lastPress_).count() < 400);
            lastPress_ = now;
            if (onClicked) onClicked(photoId, isDouble);
            return true;
        }

    private:
        chrono::steady_clock::time_point lastPress_;
    };

    // =========================================================================
    // Inner node: OverlayNode (label overlay, no events)
    // =========================================================================
    class OverlayNode : public RectNode {
    public:
        string text;
        Font* fontRef = nullptr;
        void draw() override {
            float w = getWidth(), h = getHeight();
            setColor(0, 0, 0, 0.5f);
            fill();
            drawRect(8, h - 28, 160, 20);
            setColor(0.7f, 0.7f, 0.75f);
            if (fontRef) fontRef->drawString(text, 14, h - 18,
                Direction::Left, Direction::Center);
        }
    };

    // Animation snapshot for morphing transitions
    struct AnimSnapshot {
        Vec2 position;
        float displaySize;
    };

    // Temporary layout data (used during setCenter computation only)
    struct RelatedItem {
        string photoId;
        float score;
        Vec2 position;        // world coordinates (center = origin)
        float displaySize;
        int width = 0, height = 0;
        bool isTimeline = false;
        int timelineIndex = 0;  // -5..+5
    };

    // --- State ---
    string centerId_;
    PhotoProvider* provider_ = nullptr;

    // Layout computation temporaries
    RelatedItem centerItem_;
    vector<RelatedItem> timelineItems_;
    vector<RelatedItem> relatedItems_;

    // Node tree
    shared_ptr<Node> contentLayer_;
    shared_ptr<OverlayNode> overlayNode_;
    PhotoItemNode* centerNode_ = nullptr;
    vector<PhotoItemNode*> relatedNodes_;   // high score first
    vector<PhotoItemNode*> timelineNodes_;
    vector<PhotoItemNode*> historyNodes_;   // oldest first, newest last

    // Textures keyed by photoId
    unordered_map<string, Texture> textures_;

    // Background thumbnail loading
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

    // =========================================================================
    // Node tree rebuild
    // =========================================================================

    void rebuildNodes(const unordered_map<string, AnimSnapshot>& oldSnapshots,
                      bool hasOldLayout) {
        // Clear old nodes
        contentLayer_->removeAllChildren();
        centerNode_ = nullptr;
        relatedNodes_.clear();
        timelineNodes_.clear();
        historyNodes_.clear();

        // Add in back-to-front order (addChild order = draw order)
        // 1. Related: low score (far) first → high score on top
        for (int i = (int)relatedItems_.size() - 1; i >= 0; i--) {
            auto& item = relatedItems_[i];
            auto* node = createPhotoNode(
                item.photoId, item.position, item.displaySize,
                Color(0.25f, 0.25f, 0.3f),
                format("{:.0f}%", item.score * 100));
            node->score = item.score;
            relatedNodes_.push_back(node);
        }

        // 2. Timeline items
        for (auto& item : timelineItems_) {
            auto* node = createPhotoNode(
                item.photoId, item.position, item.displaySize,
                Color(0.35f, 0.35f, 0.4f), "");
            timelineNodes_.push_back(node);
        }

        // 3. History: oldest first, newest on top
        for (int i = 0; i < (int)history_.size(); i++) {
            // Draw order: oldest at bottom, newest at top
            int drawIdx = (int)history_.size() - 1 - i;
            Vec2 worldPos = {0, HISTORY_START_Y + drawIdx * HISTORY_SPACING};
            auto* node = createPhotoNode(
                history_[i], worldPos, HISTORY_SIZE,
                Color(0.2f, 0.25f, 0.35f),
                format("#{}", i + 1));
            historyNodes_.push_back(node);
        }

        // 4. Center (topmost)
        centerNode_ = createPhotoNode(
            centerItem_.photoId, centerItem_.position, centerItem_.displaySize,
            Color(0.3f, 0.5f, 0.7f), "");

        // --- Set up morphing animation ---
        if (hasOldLayout) {
            animOldSnapshots_.clear();
            for (auto* node : allPhotoNodes()) {
                auto it = oldSnapshots.find(node->photoId);
                if (it != oldSnapshots.end()) {
                    animOldSnapshots_[node->photoId] = it->second;
                }
            }
            animProgress_.from(0).to(1).duration(0.4f)
                .ease(EaseType::Cubic, EaseMode::Out).start();
            lastAnimTime_ = chrono::steady_clock::now();
            animating_ = true;
        }

        updateContentTransform();
    }

    PhotoItemNode* createPhotoNode(const string& photoId, Vec2 worldPos,
                                   float size, Color border, const string& label) {
        auto node = make_shared<PhotoItemNode>();
        node->photoId = photoId;
        node->borderColor = border;
        node->label = label;
        node->fontRef = &fontSmall_;
        node->targetWorldPos = worldPos;
        node->targetSize = size;

        // Position: world center → top-left for Node
        node->setPos(worldPos.x - size / 2, worldPos.y - size / 2);
        node->setSize(size, size);

        // Texture reference
        auto texIt = textures_.find(photoId);
        node->textureRef = (texIt != textures_.end() && texIt->second.isAllocated())
            ? &texIt->second : nullptr;

        // Click handler
        node->onClicked = [this](const string& id, bool isDouble) {
            handlePhotoClick(id, isDouble);
        };

        contentLayer_->addChild(node);
        return node.get();
    }

    // =========================================================================
    // Content transform (pan + zoom via parent node)
    // =========================================================================

    void updateContentTransform() {
        if (!contentLayer_) return;
        contentLayer_->setPos(
            getWidth() / 2 + panOffset_.x * zoom_,
            getHeight() / 2 + panOffset_.y * zoom_);
        contentLayer_->setScale(zoom_);
    }

    // =========================================================================
    // Animation helpers
    // =========================================================================

    Vec2 getAnimatedWorldPos(PhotoItemNode* node) const {
        if (!animating_) return node->targetWorldPos;
        auto it = animOldSnapshots_.find(node->photoId);
        if (it == animOldSnapshots_.end()) return node->targetWorldPos;
        float t = animProgress_.getValue();
        return it->second.position.lerp(node->targetWorldPos, t);
    }

    // Collect all photo nodes for iteration
    vector<PhotoItemNode*> allPhotoNodes() const {
        vector<PhotoItemNode*> all;
        all.reserve(relatedNodes_.size() + timelineNodes_.size() +
                    historyNodes_.size() + 1);
        for (auto* n : relatedNodes_) all.push_back(n);
        for (auto* n : timelineNodes_) all.push_back(n);
        for (auto* n : historyNodes_) all.push_back(n);
        if (centerNode_) all.push_back(centerNode_);
        return all;
    }

    // =========================================================================
    // Click handling (called from PhotoItemNode callback)
    // =========================================================================

    void handlePhotoClick(const string& photoId, bool isDouble) {
        if (animating_) return;

        // Check if this is a history item
        int histIdx = -1;
        for (int i = 0; i < (int)history_.size(); i++) {
            if (history_[i] == photoId) { histIdx = i; break; }
        }

        if (isDouble) {
            if (photoId == centerId_) {
                // Double-click center → open in single view
                if (onCenterDoubleClick) onCenterDoubleClick(photoId);
            } else if (histIdx >= 0) {
                // Double-click history → undo to that point
                string targetId = history_[histIdx];
                history_.resize(histIdx);
                setCenter(targetId, *provider_, false);
                if (onRedraw) onRedraw();
            } else {
                // Double-click related/timeline → re-center
                if (onPhotoClick) onPhotoClick(photoId);
                setCenter(photoId, *provider_);
                if (onRedraw) onRedraw();
            }
        } else {
            if (onPhotoClick) onPhotoClick(photoId);
        }
    }

    // =========================================================================
    // Coordinate transforms (for connection line drawing)
    // =========================================================================

    Vec2 worldToScreen(Vec2 w) const {
        return {getWidth() / 2 + (w.x + panOffset_.x) * zoom_,
                getHeight() / 2 + (w.y + panOffset_.y) * zoom_};
    }

    Vec2 screenToWorld(Vec2 s) const {
        return {(s.x - getWidth() / 2) / zoom_ - panOffset_.x,
                (s.y - getHeight() / 2) / zoom_ - panOffset_.y};
    }

    // =========================================================================
    // Timeline construction
    // =========================================================================

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

        int centerIdx = -1;
        for (int i = 0; i < (int)sorted.size(); i++) {
            if (sorted[i].id == centerId_) {
                centerIdx = i;
                break;
            }
        }
        if (centerIdx < 0) return;

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

            float sign = offset > 0 ? 1.0f : -1.0f;
            int slot = abs(offset) - 1;
            float startX = CENTER_SIZE / 2 + TIMELINE_SPACING + TIMELINE_SIZE / 2;
            float x = sign * (startX + slot * (TIMELINE_SIZE + TIMELINE_SPACING));
            item.position = {x, 0};

            timelineItems_.push_back(item);
        }
    }

    // =========================================================================
    // Related photos computation
    // =========================================================================

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

        unordered_set<string> timelineIds;
        timelineIds.insert(centerId_);
        for (const auto& tl : timelineItems_) {
            timelineIds.insert(tl.photoId);
        }

        vector<pair<string, float>> candidates;
        for (const auto& sr : similar) {
            if (timelineIds.count(sr.photoId)) continue;

            float combined = sr.score * 0.85f;

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

        sort(candidates.begin(), candidates.end(),
             [](const auto& a, const auto& b) { return a.second > b.second; });

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

    // =========================================================================
    // Layout
    // =========================================================================

    void layoutRelated() {
        layoutRelated({});
    }

    // Layout with optional old positions for continuity
    void layoutRelated(const unordered_map<string, Vec2>& oldPositions) {
        if (relatedItems_.empty()) return;

        float maxScore = relatedItems_.front().score;
        float minScore = relatedItems_.back().score;
        float scoreRange = max(maxScore - minScore, 0.01f);

        float innerRadius = CENTER_SIZE / 2 + 60;
        float outerRadius = innerRadius + 160;

        // History exclusion zone: vertical strip below center
        float histExclX = HISTORY_SIZE;  // half-width of exclusion zone
        float histExclTop = HISTORY_START_Y - HISTORY_SIZE;
        float histExclBottom = HISTORY_START_Y + MAX_HISTORY * HISTORY_SPACING;

        float goldenAngle = TAU * (1.0f - 1.0f / 1.618033988749895f);
        float angleOffset = TAU * 0.125f;

        for (int i = 0; i < (int)relatedItems_.size(); i++) {
            auto& item = relatedItems_[i];
            float normalized = (item.score - minScore) / scoreRange;
            float dist = innerRadius + (1.0f - normalized * normalized) * (outerRadius - innerRadius);

            // Check if this item existed in the previous layout
            auto oldIt = oldPositions.find(item.photoId);
            if (oldIt != oldPositions.end()) {
                // Reuse old angle but adjust distance for new score
                Vec2 oldPos = oldIt->second;
                float oldAngle = atan2(oldPos.y, oldPos.x);
                item.position = {cos(oldAngle) * dist, sin(oldAngle) * dist};
            } else {
                float angle = angleOffset + i * goldenAngle;
                item.position = {cos(angle) * dist, sin(angle) * dist};
            }

            // Push away from history exclusion zone
            if (abs(item.position.x) < histExclX + item.displaySize / 2 &&
                item.position.y > histExclTop &&
                item.position.y < histExclBottom) {
                // Move horizontally away from center column
                float pushDir = (item.position.x >= 0) ? 1.0f : -1.0f;
                item.position.x = pushDir * (histExclX + item.displaySize / 2 + 10);
            }
        }

        resolveCollisions();
    }

    void resolveCollisions() {
        struct ColItem {
            Vec2* pos;
            float radius;
            bool fixed;
        };
        vector<ColItem> items;

        items.push_back({&centerItem_.position, CENTER_SIZE / 2, true});

        for (auto& tl : timelineItems_) {
            items.push_back({&tl.position, TIMELINE_SIZE / 2, true});
        }

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

    // =========================================================================
    // Thumbnail loading
    // =========================================================================

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
