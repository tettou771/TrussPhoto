#pragma once

// =============================================================================
// PeopleView.h - People view with face clusters (Lightroom-style)
// Displays named persons and unnamed face clusters with thumbnail cards.
// Click a card to show face gallery; click name label to edit name.
// Uses RecyclerGrid for virtualized scrolling of both card list and gallery.
// =============================================================================

#include <TrussC.h>
#include <tcxIME.h>
#include <optional>
#include "PhotoProvider.h"
#include "RecyclerGrid.h"
#include "ViewContainer.h"
#include "MetadataPanel.h"  // for OverlayRect
#include "FolderTree.h"  // for loadJapaneseFont

using namespace std;
using namespace tc;

class PeopleView : public ViewContainer {
public:
    using Ptr = shared_ptr<PeopleView>;

    // Callbacks
    function<void()> onRedraw;
    function<void(const string& photoId)> onFaceSelect;
    function<void(const string& photoId)> onFaceDoubleClick;
    function<void(const vector<OverlayRect>& overlays)> onOverlayUpdate;

    // Modifier key state (set by tcApp)
    bool* cmdDownRef = nullptr;
    bool* shiftDownRef = nullptr;

    void setup() override {
        enableEvents();
        setClipping(true);
        loadJapaneseFont(font_, 14);
        loadJapaneseFont(fontSmall_, 11);
        loadJapaneseFont(fontLarge_, 16);

        // Left: card list (virtualized)
        cardRecycler_ = make_shared<CardRecycler>();
        cardRecycler_->fontRef = &font_;
        cardRecycler_->fontSmallRef = &fontSmall_;
        cardRecycler_->fontLargeRef = &fontLarge_;
        cardRecycler_->texturesRef = &textures_;
        cardRecycler_->onCardClick = [this](int dataIdx) {
            pendingClickDataIdx_ = dataIdx;
        };
        addChild(cardRecycler_);

        // Right: face gallery (virtualized, hidden initially)
        galleryRecycler_ = make_shared<GalleryRecycler>();
        galleryRecycler_->texturesRef = &textures_;
        galleryRecycler_->fontLargeRef = &fontLarge_;
        galleryRecycler_->onFaceClick = [this](int dataIdx) {
            pendingFaceClickIdx_ = dataIdx;
        };
        galleryRecycler_->onDragStart = [this](Vec2 sp) { handleDragStart(sp); };
        galleryRecycler_->onDragMove = [this](Vec2 sp) { handleDragMove(sp); };
        galleryRecycler_->onDragEnd = [this](Vec2 sp) { handleDragEnd(sp); };
        addChild(galleryRecycler_);

        // Name edit overlay (hidden by default)
        nameOverlay_ = make_shared<NameEditOverlay>();
        nameOverlay_->fontRef = &font_;
        nameOverlay_->onConfirm = [this](const string& name) {
            handleNameConfirm(name);
        };
        nameOverlay_->onCancel = [this]() {
            hideNameOverlay();
        };
        addChild(nameOverlay_);
        nameOverlay_->setActive(false);
    }

    bool hasSelection() const { return selectedDataIdx_ >= 0; }
    bool isNameEditing() const { return nameOverlay_ && nameOverlay_->getActive(); }

    void populate(PhotoProvider& provider) {
        provider_ = &provider;
        editingClusterId_ = INT_MIN;
        selectedDataIdx_ = -1;
        pendingClickDataIdx_ = -1;
        clusteringDone_ = false;
        maxFaces_ = 1000;
        hasMoreFaces_ = false;
        totalUnnamedFaces_ = 0;

        // Clear previous state
        if (galleryRecycler_) {
            galleryRecycler_->selectedSet.clear();
            galleryRecycler_->unbindAll();
        }
        clusters_.clear();

        runClustering();
    }

    void loadMore() {
        maxFaces_ += 200;
        runClustering();
    }

    void update() override {
        // Process deferred card click -> show gallery or name edit
        if (pendingClickDataIdx_ >= 0) {
            int idx = pendingClickDataIdx_;
            pendingClickDataIdx_ = -1;
            if (idx == selectedDataIdx_ && !isNameEditing()) {
                startNameEdit(idx);  // Re-click selected card -> name edit
            } else {
                showGallery(idx);
            }
        }

        // Process deferred face click (from GalleryRecycler)
        if (pendingFaceClickIdx_ >= 0) {
            int idx = pendingFaceClickIdx_;
            pendingFaceClickIdx_ = -1;
            handleFaceClick(idx);
        }

        // Process clustering completion from background thread
        if (clusteringDone_) {
            clusteringDone_ = false;
            {
                lock_guard<mutex> lock(clusterMutex_);
                clusters_ = std::move(pendingClusterResult_.clusters);
                totalUnnamedFaces_ = pendingClusterResult_.totalUnnamed;
                hasMoreFaces_ = pendingClusterResult_.processedUnnamed < pendingClusterResult_.totalUnnamed;
            }
            needsRebuild_ = true;
            if (onRedraw) onRedraw();
        }

        // Sync scroll container layout (top/bottom split)
        float w = getWidth(), h = getHeight();
        if (w > 0 && h > 0) {
            float galleryH = h * 0.25f;
            float cardH = h - galleryH;

            if (cardRecycler_) {
                cardRecycler_->setPos(0, 0);
                cardRecycler_->setSize(w, cardH);
            }
            if (galleryRecycler_) {
                galleryRecycler_->setPos(0, cardH);
                galleryRecycler_->setSize(w, galleryH);
            }
        }

        // Deferred rebuild
        if (needsRebuild_ && w > 0 && h > 0) {
            needsRebuild_ = false;
            rebuildUI();
        }

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

        if (anyNew) {
            // Update texture references on bound card items
            if (cardRecycler_) {
                for (auto& [dataIdx, poolIdx] : cardRecycler_->getPoolMap()) {
                    auto& item = cardRecycler_->getPool()[poolIdx];
                    if (dataIdx < (int)cardItems_.size()) {
                        auto it = textures_.find(cardItems_[dataIdx]->repPhotoId);
                        item->textureRef = (it != textures_.end() && it->second.isAllocated())
                            ? &it->second : nullptr;
                    }
                }
            }
            // Update gallery crop textures
            if (galleryRecycler_ && selectedDataIdx_ >= 0) {
                for (auto& [dataIdx, poolIdx] : galleryRecycler_->getPoolMap()) {
                    auto& item = galleryRecycler_->getPool()[poolIdx];
                    auto it = textures_.find(item->photoId);
                    item->textureRef = (it != textures_.end() && it->second.isAllocated())
                        ? &it->second : nullptr;
                }
            }
            if (onRedraw) onRedraw();
        }

        // Start load thread if needed
        if (!pendingLoads_.empty() && !loadThreadRunning_) {
            startLoadThread();
        }
    }

    void draw() override {
        float w = getWidth(), h = getHeight();

        // Background
        setColor(0.06f, 0.06f, 0.08f);
        fill();
        drawRect(0, 0, w, h);

        // Loading indicator while clustering runs
        if (clusterThread_.joinable() && !clusteringDone_ && clusters_.empty()) {
            setColor(0.5f, 0.5f, 0.55f);
            font_.drawString("Building face clusters...", w / 2, h / 2,
                Direction::Center, Direction::Center);
        }

        // Gallery separator line (horizontal, between cards and gallery)
        float galleryH = h * 0.25f;
        float cardH = h - galleryH;
        setColor(0.25f, 0.25f, 0.28f);
        fill();
        drawRect(0, cardH, w, 1);

        // Placeholder when no card selected
        if (selectedDataIdx_ < 0) {
            setColor(0.4f, 0.4f, 0.45f);
            font_.drawString("Select a person to view faces",
                w / 2, cardH + galleryH / 2,
                Direction::Center, Direction::Center);
        }

        // Status bar at bottom
        setColor(0, 0, 0, 0.5f);
        fill();
        drawRect(8, h - 28, 300, 20);
        setColor(0.7f, 0.7f, 0.75f);
        int namedCount = 0, unnamedCount = 0;
        for (auto& c : clusters_) {
            if (c.personId > 0) namedCount++; else unnamedCount++;
        }
        string statusText = format("People  {} named, {} clusters", namedCount, unnamedCount);
        if (hasMoreFaces_) {
            statusText += format("  ({}/{} unnamed loaded)", maxFaces_, totalUnnamedFaces_);
        }
        fontSmall_.drawString(statusText,
            14, h - 18, Direction::Left, Direction::Center);
    }

    void drawChildren() override {
        Node::drawChildren();

        // Draw drag ghost on top of everything
        if (isDragging_ && dragFaceCount_ > 0) {
            float lx, ly;
            globalToLocal(dragGhostPos_.x, dragGhostPos_.y, lx, ly);
            float gx = lx + 14;
            float gy = ly - 6;
            float gw = 80, gh = 28;

            // Ghost card background
            setColor(0.2f, 0.4f, 0.8f, 0.8f);
            fill();
            drawRect(gx, gy, gw, gh);

            // Ghost card border
            setColor(0.4f, 0.6f, 1.0f);
            noFill();
            drawRect(gx, gy, gw, gh);

            // Face count text
            setColor(1, 1, 1);
            string text = format("{} face{}", dragFaceCount_, dragFaceCount_ > 1 ? "s" : "");
            fontSmall_.drawString(text, gx + gw / 2, gy + gh / 2,
                Direction::Center, Direction::Center);
        }
    }

    // ViewContainer lifecycle
    void beginView(ViewContext& ctx) override { /* populated via populate() before activation */ }
    void endView() override { shutdown(); }
    void suspendView() override { suspend(); }
    bool hasState() const override { return !clusters_.empty(); }
    bool wantsSearchBar() const override { return false; }
    bool wantsLeftSidebar() const override { return false; }

    // Suspend: stop threads but keep data (for temporary exit)
    void suspend() {
        if (clusterThread_.joinable()) clusterThread_.join();
        if (loadThread_.joinable()) loadThread_.join();
        loadThreadRunning_ = false;
    }

    void shutdown() {
        suspend();
        if (cardRecycler_) cardRecycler_->unbindAll();
        if (galleryRecycler_) galleryRecycler_->unbindAll();
        selectedDataIdx_ = -1;
        textures_.clear();
        pendingLoads_.clear();
        clusters_.clear();
        cardItems_.clear();
        editingClusterId_ = INT_MIN;
        pendingClickDataIdx_ = -1;
        pendingFaceClickIdx_ = -1;
        lastFaceClickIdx_ = -1;
        if (galleryRecycler_) {
            galleryRecycler_->selectedSet.clear();
            galleryRecycler_->unbindAll();
        }
    }

    bool onKeyPress(int key) override {
        // ESC: cancel name edit, or close gallery, or fall through to tcApp
        if (key == 256 /* ESCAPE */) {
            if (nameOverlay_ && nameOverlay_->getActive()) {
                hideNameOverlay();
                return true;
            }
            if (selectedDataIdx_ >= 0) {
                clearGallerySelection();
                return true;
            }
        }

        // Delete/Backspace: unassign selected faces from person
        if ((key == 259 /* BACKSPACE */ || key == 261 /* DELETE */) &&
            selectedDataIdx_ >= 0 && galleryRecycler_ && !galleryRecycler_->selectedSet.empty()) {
            handleDeleteSelectedFaces();
            return true;
        }

        // Cmd+A / Cmd+Shift+A: select all / deselect all gallery faces
        if ((key == 'A' || key == 'a') && cmdDownRef && *cmdDownRef &&
            selectedDataIdx_ >= 0 && galleryRecycler_) {
            bool shiftHeld = shiftDownRef ? *shiftDownRef : false;
            if (shiftHeld) {
                galleryRecycler_->selectedSet.clear();
            } else {
                int count = (int)galleryRecycler_->faces.size();
                for (int i = 0; i < count; i++) {
                    galleryRecycler_->selectedSet.insert(i);
                }
            }
            // Update visual selection on bound items
            for (auto& [di, pi] : galleryRecycler_->getPoolMap()) {
                galleryRecycler_->getPool()[pi]->selected =
                    galleryRecycler_->selectedSet.count(di) > 0;
            }
            if (onRedraw) onRedraw();
            return true;
        }

        // N: edit name of selected card
        if (key == 'N' && selectedDataIdx_ >= 0 && !isNameEditing()) {
            editingClusterId_ = cardItems_[selectedDataIdx_]->clusterId;
            string initial = cardItems_[selectedDataIdx_]->name;
            string placeholder = cardItems_[selectedDataIdx_]->suggestedName;
            if (nameOverlay_) {
                nameOverlay_->setPos(0, 0);
                nameOverlay_->setSize(getWidth(), getHeight());
                nameOverlay_->show(initial, placeholder);
            }
            if (onRedraw) onRedraw();
            return true;
        }

        return false;
    }

private:
    PhotoProvider* provider_ = nullptr;
    vector<PhotoProvider::FaceCluster> clusters_;
    bool needsRebuild_ = false;

    // Incremental clustering
    int maxFaces_ = 1000;
    bool hasMoreFaces_ = false;
    int totalUnnamedFaces_ = 0;

    // Background clustering
    thread clusterThread_;
    mutex clusterMutex_;
    PhotoProvider::ClusterResult pendingClusterResult_;
    atomic<bool> clusteringDone_{false};

    // UI fonts
    Font font_, fontSmall_, fontLarge_;

    // Selection
    int selectedDataIdx_ = -1;
    int pendingClickDataIdx_ = -1;
    int pendingFaceClickIdx_ = -1;
    int editingClusterId_ = INT_MIN;  // sentinel: INT_MIN = not editing

    // Face gallery double-click detection
    chrono::steady_clock::time_point lastFaceClickTime_;
    int lastFaceClickIdx_ = -1;

    // Drag & drop state
    bool isDragging_ = false;
    int dragFaceCount_ = 0;
    Vec2 dragGhostPos_;
    int dropTargetDataIdx_ = -1;

    // Layout constants
    static constexpr float CARD_WIDTH = 144.0f;
    static constexpr float CARD_HEIGHT = 58.0f;
    static constexpr float CARD_SPACING = 6.0f;
    static constexpr float SECTION_HEADER_HEIGHT = 32.0f;
    static constexpr float PADDING = 16.0f;
    static constexpr float CROP_SIZE = 80.0f;
    static constexpr float CROP_SPACING = 6.0f;
    static constexpr int MAX_CARD_TEXTURES = 300;

    // Flat list of cluster pointers for card recycler
    // (named first, then unnamed — matches display order)
    vector<const PhotoProvider::FaceCluster*> cardItems_;
    int namedCount_ = 0;

    // =========================================================================
    // Inner node: NameLabel (child of PersonCard)
    // =========================================================================
    class NameLabel : public RectNode {
    public:
        string name;
        string suggestedName;
        int faceCount = 0;
        int photoCount = 0;
        Font* fontRef = nullptr;
        Font* fontSmallRef = nullptr;

        void draw() override {
            float textX = 4;
            float textY = 4;

            if (!name.empty()) {
                setColor(0.9f, 0.9f, 0.95f);
                if (fontRef) fontRef->drawString(name, textX, textY,
                    Direction::Left, Direction::Top);
            } else if (!suggestedName.empty()) {
                setColor(0.6f, 0.6f, 0.7f);
                if (fontRef) fontRef->drawString(suggestedName + "?",
                    textX, textY, Direction::Left, Direction::Top);
            } else {
                setColor(0.5f, 0.5f, 0.55f);
                if (fontRef) fontRef->drawString("Unknown", textX, textY,
                    Direction::Left, Direction::Top);
            }

            // Counts (two lines)
            setColor(0.45f, 0.45f, 0.5f);
            if (fontSmallRef) {
                fontSmallRef->drawString(format("{} photos", photoCount),
                    textX, textY + 16, Direction::Left, Direction::Top);
                fontSmallRef->drawString(format("{} faces", faceCount),
                    textX, textY + 28, Direction::Left, Direction::Top);
            }
        }
    };

    // =========================================================================
    // Inner node: PersonCard (pool item for CardRecycler)
    // =========================================================================
    class PersonCard : public RectNode {
    public:
        using Ptr = shared_ptr<PersonCard>;

        PhotoProvider::FaceCluster cluster;
        bool selected = false;
        bool dropHighlight = false;
        bool isLoadMoreButton = false;
        string loadMoreText;

        // Borrowed pointers
        Texture* textureRef = nullptr;
        Font* fontRef = nullptr;
        Font* fontSmallRef = nullptr;

        // Callback (set by CardRecycler)
        function<void()> onClick;

        shared_ptr<NameLabel> nameLabel;

        void setup() override {
            enableEvents();

            nameLabel = make_shared<NameLabel>();
            nameLabel->fontRef = fontRef;
            nameLabel->fontSmallRef = fontSmallRef;

            float thumbSize = getHeight() - 12;
            float labelX = 6 + thumbSize + 4;
            float labelW = getWidth() - labelX - 4;
            nameLabel->setPos(labelX, 8);
            nameLabel->setSize(max(1.0f, labelW), max(1.0f, getHeight() - 16));

            addChild(nameLabel);

            // Apply already-bound cluster data (onBind runs before setup)
            if (cluster.personId > 0 || cluster.photoCount > 0) {
                nameLabel->name = cluster.name;
                nameLabel->suggestedName = cluster.suggestedName;
                nameLabel->faceCount = (int)cluster.faceIds.size();
                nameLabel->photoCount = cluster.photoCount;
            }
        }

        void bindCluster(const PhotoProvider::FaceCluster& c, bool sel) {
            cluster = c;
            selected = sel;
            if (nameLabel) {
                nameLabel->name = c.name;
                nameLabel->suggestedName = c.suggestedName;
                nameLabel->faceCount = (int)c.faceIds.size();
                nameLabel->photoCount = c.photoCount;
            }
        }

        void draw() override {
            float w = getWidth(), h = getHeight();

            // "Load more" button mode
            if (isLoadMoreButton) {
                setColor(0.15f, 0.18f, 0.22f);
                fill();
                drawRect(0, 0, w, h);
                setColor(0.35f, 0.5f, 0.7f);
                noFill();
                drawRect(0, 0, w, h);
                setColor(0.5f, 0.7f, 0.95f);
                if (fontRef) fontRef->drawString("Load more", w / 2, h / 2 - 8,
                    Direction::Center, Direction::Center);
                setColor(0.4f, 0.55f, 0.75f);
                if (fontSmallRef) fontSmallRef->drawString(loadMoreText, w / 2, h / 2 + 8,
                    Direction::Center, Direction::Center);
                return;
            }

            // Background
            if (selected) {
                setColor(0.2f, 0.35f, 0.55f);
            } else {
                setColor(0.12f, 0.12f, 0.14f);
            }
            fill();
            drawRect(0, 0, w, h);

            // Border
            if (dropHighlight) {
                setColor(0.3f, 0.6f, 1.0f);
                noFill();
                drawRect(0, 0, w, h);
                drawRect(1, 1, w - 2, h - 2);
            } else {
                setColor(selected ? Color(0.4f, 0.6f, 0.9f) : Color(0.2f, 0.2f, 0.22f));
                noFill();
                drawRect(0, 0, w, h);
            }

            // Face thumbnail (cropped from photo thumbnail)
            float thumbSize = h - 12;
            float thumbX = 6, thumbY = 6;

            if (textureRef && textureRef->isAllocated()) {
                float imgW = textureRef->getWidth();
                float imgH = textureRef->getHeight();
                float fx = cluster.repFaceX * imgW;
                float fy = cluster.repFaceY * imgH;
                float fw = cluster.repFaceW * imgW;
                float fh = cluster.repFaceH * imgH;

                float margin = max(fw, fh) * 0.3f;
                float sx = fx - margin;
                float sy = fy - margin;
                float sw = fw + margin * 2;
                float sh = fh + margin * 2;

                sx = max(0.0f, sx);
                sy = max(0.0f, sy);
                if (sx + sw > imgW) sw = imgW - sx;
                if (sy + sh > imgH) sh = imgH - sy;

                float fitScale = min(thumbSize / sw, thumbSize / sh);
                float dw = sw * fitScale;
                float dh = sh * fitScale;
                float dx = thumbX + (thumbSize - dw) / 2;
                float dy = thumbY + (thumbSize - dh) / 2;

                setColor(1, 1, 1);
                textureRef->drawSubsection(dx, dy, dw, dh, sx, sy, sw, sh);
            } else {
                setColor(0.2f, 0.2f, 0.22f);
                fill();
                drawRect(thumbX, thumbY, thumbSize, thumbSize);
            }

            // Position name label
            float labelX = thumbX + thumbSize + 4;
            float labelW = w - labelX - 4;
            if (nameLabel) {
                nameLabel->setPos(labelX, 8);
                nameLabel->setSize(labelW, h - 16);
            }
        }

        bool onMousePress(Vec2 pos, int button) override {
            (void)pos;
            if (button != 0) return false;
            if (onClick) onClick();
            return true;
        }
    };

    // =========================================================================
    // Inner node: SectionHeader (non-recycled, stays in content)
    // =========================================================================
    class SectionHeader : public RectNode {
    public:
        string text;
        Font* fontRef = nullptr;

        void draw() override {
            float w = getWidth(), h = getHeight();
            setColor(0.08f, 0.08f, 0.1f);
            fill();
            drawRect(0, 0, w, h);

            setColor(0.25f, 0.25f, 0.28f);
            fill();
            drawRect(0, h - 1, w, 1);

            setColor(0.7f, 0.7f, 0.75f);
            if (fontRef) fontRef->drawString(text, 8, h / 2,
                Direction::Left, Direction::Center);
        }
    };

    // =========================================================================
    // Inner node: FaceCropNode (pool item for GalleryRecycler)
    // =========================================================================
    class FaceCropNode : public RectNode {
    public:
        using Ptr = shared_ptr<FaceCropNode>;

        Texture* textureRef = nullptr;
        float faceX = 0, faceY = 0, faceW = 0, faceH = 0;
        string photoId;
        int faceId = 0;
        bool selected = false;

        // Callbacks (set by GalleryRecycler)
        function<void()> onClick;
        function<void(Vec2 screenPos)> onDragStart;
        function<void(Vec2 screenPos)> onDragMove;
        function<void(Vec2 screenPos)> onDragEnd;

        void setup() override {
            enableEvents();
        }

        void draw() override {
            float w = getWidth(), h = getHeight();

            if (textureRef && textureRef->isAllocated()) {
                float imgW = textureRef->getWidth();
                float imgH = textureRef->getHeight();
                float fx = faceX * imgW;
                float fy = faceY * imgH;
                float fw = faceW * imgW;
                float fh = faceH * imgH;

                float margin = max(fw, fh) * 0.3f;
                float sx = fx - margin;
                float sy = fy - margin;
                float sw = fw + margin * 2;
                float sh = fh + margin * 2;

                sx = max(0.0f, sx);
                sy = max(0.0f, sy);
                if (sx + sw > imgW) sw = imgW - sx;
                if (sy + sh > imgH) sh = imgH - sy;

                float fitScale = min(w / sw, h / sh);
                float dw = sw * fitScale;
                float dh = sh * fitScale;
                float dx = (w - dw) / 2;
                float dy = (h - dh) / 2;

                setColor(1, 1, 1);
                textureRef->drawSubsection(dx, dy, dw, dh, sx, sy, sw, sh);
            } else {
                setColor(0.15f, 0.15f, 0.18f);
                fill();
                drawRect(0, 0, w, h);
            }

            // Border: highlight when selected
            if (selected) {
                setColor(0.4f, 0.7f, 1.0f);
                noFill();
                drawRect(0, 0, w, h);
                drawRect(1, 1, w - 2, h - 2);
            } else {
                setColor(0.2f, 0.2f, 0.22f);
                noFill();
                drawRect(0, 0, w, h);
            }
        }

        bool onMousePress(Vec2 pos, int button) override {
            if (button != 0) return false;
            float gx, gy;
            localToGlobal(pos.x, pos.y, gx, gy);
            mouseDownPos_ = {gx, gy};
            isDragging_ = false;
            mouseDown_ = true;
            return true;
        }

        bool onMouseDrag(Vec2 pos, int button) override {
            if (button != 0 || !mouseDown_) return false;
            float gx, gy;
            localToGlobal(pos.x, pos.y, gx, gy);
            Vec2 screenPos{gx, gy};
            if (!isDragging_) {
                float dist = screenPos.distance(mouseDownPos_);
                if (dist > 5.0f) {
                    isDragging_ = true;
                    if (onDragStart) onDragStart(screenPos);
                }
            }
            if (isDragging_) {
                if (onDragMove) onDragMove(screenPos);
            }
            return isDragging_;
        }

        bool onMouseRelease(Vec2 pos, int button) override {
            if (button != 0) return false;
            float gx, gy;
            localToGlobal(pos.x, pos.y, gx, gy);
            Vec2 screenPos{gx, gy};
            if (isDragging_) {
                if (onDragEnd) onDragEnd(screenPos);
            } else {
                if (onClick) onClick();
            }
            isDragging_ = false;
            mouseDown_ = false;
            return true;
        }

    private:
        Vec2 mouseDownPos_;
        bool mouseDown_ = false;
        bool isDragging_ = false;
    };

    // =========================================================================
    // Inner node: GalleryHeader (non-recycled, stays in gallery content)
    // =========================================================================
    class GalleryHeader : public RectNode {
    public:
        string text;
        Font* fontRef = nullptr;

        void draw() override {
            float w = getWidth(), h = getHeight();

            setColor(0.08f, 0.08f, 0.1f);
            fill();
            drawRect(0, 0, w, h);

            setColor(0.25f, 0.25f, 0.28f);
            fill();
            drawRect(0, h - 1, w, 1);

            setColor(0.85f, 0.85f, 0.9f);
            if (fontRef) fontRef->drawString(text, 12, h / 2,
                Direction::Left, Direction::Center);
        }
    };

    // =========================================================================
    // Inner node: NameEditOverlay
    // =========================================================================
    class NameEditOverlay : public RectNode {
    public:
        Font* fontRef = nullptr;
        function<void(const string&)> onConfirm;
        function<void()> onCancel;
        string placeholder;

        void setup() override {
            enableEvents();
            ime_.setFont(fontRef);

            // Intercept Enter at IME level (prevents newline insertion)
            ime_.onEnter = [this]() {
                string text = ime_.getString();
                // Trim whitespace
                auto s = text.find_first_not_of(" \t\n\r");
                auto e = text.find_last_not_of(" \t\n\r");
                string trimmed = (s != string::npos) ? text.substr(s, e - s + 1) : "";
                hide();
                if (!trimmed.empty() && onConfirm) onConfirm(trimmed);
                else if (onCancel) onCancel();
            };
        }

        void show(const string& initialText, const string& placeholderText) {
            placeholder = placeholderText;
            ime_.clear();
            if (!initialText.empty()) {
                ime_.setString(initialText);
            }
            ime_.enable();
            setActive(true);
        }

        void hide() {
            ime_.disable();
            setActive(false);
        }

        void update() override {
            bool cursorOn = fmod(getElapsedTimef(), 1.0f) < 0.5f;
            if (cursorOn != lastCursorOn_) {
                lastCursorOn_ = cursorOn;
                redraw();
            }
        }

        void draw() override {
            float w = getWidth(), h = getHeight();

            setColor(0, 0, 0, 0.6f);
            fill();
            drawRect(0, 0, w, h);

            float dlgW = 320, dlgH = 100;
            float dlgX = (w - dlgW) / 2;
            float dlgY = (h - dlgH) / 2;

            setColor(0.15f, 0.15f, 0.18f);
            fill();
            drawRect(dlgX, dlgY, dlgW, dlgH);

            setColor(0.3f, 0.3f, 0.35f);
            noFill();
            drawRect(dlgX, dlgY, dlgW, dlgH);

            setColor(0.7f, 0.7f, 0.75f);
            if (fontRef) fontRef->drawString("Name:", dlgX + 12, dlgY + 24,
                Direction::Left, Direction::Center);

            float inputX = dlgX + 12;
            float inputY = dlgY + 40;
            float inputW = dlgW - 24;
            float inputH = 28;

            setColor(0.1f, 0.1f, 0.12f);
            fill();
            drawRect(inputX, inputY, inputW, inputH);

            setColor(0.25f, 0.25f, 0.28f);
            noFill();
            drawRect(inputX, inputY, inputW, inputH);

            string text = const_cast<tcxIME&>(ime_).getString();
            if (text.empty() && !placeholder.empty()) {
                setColor(0.4f, 0.4f, 0.45f);
                if (fontRef) fontRef->drawString(placeholder,
                    inputX + 6, inputY + inputH / 2,
                    Direction::Left, Direction::Center);
            }

            setColor(1, 1, 1);
            ime_.draw(inputX + 6, inputY + 4);

            setColor(0.4f, 0.4f, 0.45f);
            if (fontRef) fontRef->drawString("Enter to confirm, ESC to cancel",
                dlgX + dlgW / 2, dlgY + dlgH - 12,
                Direction::Center, Direction::Center);
        }

        bool onMousePress(Vec2 pos, int button) override {
            (void)pos; (void)button;
            return true;
        }

        bool onKeyPress(int key) override {
            if (key == 256 /* ESCAPE */) {
                if (onCancel) onCancel();
                return true;
            }
            // Enter is handled by ime_.onEnter (no newline insertion)
            return false;
        }

    private:
        tcxIME ime_;
        bool lastCursorOn_ = false;
    };

    // =========================================================================
    // CardRecycler - RecyclerGrid<PersonCard> with section layout
    // =========================================================================
    class CardRecycler : public RecyclerGrid<PersonCard> {
    public:
        using Ptr = shared_ptr<CardRecycler>;

        // External references (set by PeopleView before setup)
        Font* fontRef = nullptr;
        Font* fontSmallRef = nullptr;
        Font* fontLargeRef = nullptr;
        unordered_map<string, Texture>* texturesRef = nullptr;
        function<void(int dataIdx)> onCardClick;

        // Data
        vector<const PhotoProvider::FaceCluster*> items;
        int namedCount = 0;
        int selectedIdx = -1;
        bool hasMore = false;
        int remainingCount = 0;
        function<void()> onLoadMore;

        void setData(const vector<const PhotoProvider::FaceCluster*>& newItems, int named) {
            items = newItems;
            namedCount = named;
        }

        void draw() override {
            // Transparent - PeopleView draws the background
        }

    protected:
        int getDataCount() const override {
            return (int)items.size() + (hasMore ? 1 : 0);
        }

        ItemPtr createPoolItem(int poolIdx) override {
            auto card = make_shared<PersonCard>();
            card->fontRef = fontRef;
            card->fontSmallRef = fontSmallRef;
            card->setSize(CARD_WIDTH, CARD_HEIGHT);

            int pi = poolIdx;
            card->onClick = [this, pi]() {
                int dataIdx = getReverseMap()[pi];
                if (dataIdx >= 0) {
                    // "Load more" button is the last item
                    if (hasMore && dataIdx == (int)items.size()) {
                        if (onLoadMore) onLoadMore();
                    } else if (onCardClick) {
                        onCardClick(dataIdx);
                    }
                }
            };

            return card;
        }

        void onBind(int dataIdx, ItemPtr& item) override {
            // "Load more" button
            if (hasMore && dataIdx == (int)items.size()) {
                item->isLoadMoreButton = true;
                item->loadMoreText = format("{} remaining", remainingCount);
                item->textureRef = nullptr;
                item->selected = false;
                if (item->nameLabel) item->nameLabel->setActive(false);
                return;
            }

            item->isLoadMoreButton = false;
            if (item->nameLabel) item->nameLabel->setActive(true);

            if (dataIdx >= (int)items.size()) return;
            auto* cluster = items[dataIdx];
            bool sel = (dataIdx == selectedIdx);
            item->bindCluster(*cluster, sel);

            // Texture reference
            if (texturesRef) {
                auto it = texturesRef->find(cluster->repPhotoId);
                item->textureRef = (it != texturesRef->end() && it->second.isAllocated())
                    ? &it->second : nullptr;
            }
        }

        void onUnbind(int dataIdx, ItemPtr& item) override {
            (void)dataIdx;
            item->textureRef = nullptr;
            item->isLoadMoreButton = false;
        }

        // Section-aware layout
        int calcColumns() override {
            float contentWidth = getWidth() - scrollBarWidth_;
            if (contentWidth <= 0) return 1;
            return max(1, (int)((contentWidth - PADDING * 2 + CARD_SPACING)
                                / (CARD_WIDTH + CARD_SPACING)));
        }

        float calcRowHeight() override {
            return CARD_HEIGHT + CARD_SPACING;
        }

        float calcContentHeight() override {
            float h = PADDING;
            if (namedCount > 0) {
                h += SECTION_HEADER_HEIGHT + CARD_SPACING;
                int rows = (namedCount + columns_ - 1) / columns_;
                h += rows * (CARD_HEIGHT + CARD_SPACING);
                h += PADDING;
            }
            int unnamedCount = (int)items.size() - namedCount;
            if (unnamedCount > 0 || hasMore) {
                h += SECTION_HEADER_HEIGHT + CARD_SPACING;
                int rows = (unnamedCount + columns_ - 1) / columns_;
                h += rows * (CARD_HEIGHT + CARD_SPACING);
                // "Load more" button row
                if (hasMore) h += CARD_HEIGHT + CARD_SPACING;
                h += PADDING;
            }
            return h;
        }

        Vec2 getItemPosition(int dataIdx) override {
            // "Load more" button: full-width centered below unnamed cards
            if (hasMore && dataIdx == (int)items.size()) {
                int unnamedCount = (int)items.size() - namedCount;
                int unnamedRows = (unnamedCount > 0) ? (unnamedCount + columns_ - 1) / columns_ : 0;
                float y = unnamedStartY_ + unnamedRows * (CARD_HEIGHT + CARD_SPACING);
                return {PADDING, y};
            }

            int col, row;
            float baseY;
            if (dataIdx < namedCount) {
                col = dataIdx % columns_;
                row = dataIdx / columns_;
                baseY = namedStartY_;
            } else {
                int localIdx = dataIdx - namedCount;
                col = localIdx % columns_;
                row = localIdx / columns_;
                baseY = unnamedStartY_;
            }
            float x = PADDING + col * (CARD_WIDTH + CARD_SPACING);
            float y = baseY + row * (CARD_HEIGHT + CARD_SPACING);
            return {x, y};
        }

        pair<int, int> calcVisibleDataRange(float scrollY) override {
            float viewTop = scrollY - (CARD_HEIGHT + CARD_SPACING) * 2;
            float viewBottom = scrollY + getHeight() + (CARD_HEIGHT + CARD_SPACING) * 2;
            int startIdx = getDataCount(), endIdx = 0;

            // Named section
            if (namedCount > 0) {
                int rows = (namedCount + columns_ - 1) / columns_;
                float top = namedStartY_;
                float bot = top + rows * (CARD_HEIGHT + CARD_SPACING);
                if (bot > viewTop && top < viewBottom) {
                    int r0 = max(0, (int)((viewTop - top) / (CARD_HEIGHT + CARD_SPACING)));
                    int r1 = min(rows - 1, (int)((viewBottom - top) / (CARD_HEIGHT + CARD_SPACING)));
                    startIdx = min(startIdx, r0 * columns_);
                    endIdx = max(endIdx, min((r1 + 1) * columns_, namedCount));
                }
            }
            // Unnamed section (including Load more button)
            int unnamedCount = (int)items.size() - namedCount;
            int unnamedDataCount = unnamedCount + (hasMore ? 1 : 0);
            if (unnamedDataCount > 0) {
                int unnamedRows = (unnamedCount + columns_ - 1) / columns_;
                int totalRows = unnamedRows + (hasMore ? 1 : 0);
                float top = unnamedStartY_;
                float bot = top + totalRows * (CARD_HEIGHT + CARD_SPACING);
                if (bot > viewTop && top < viewBottom) {
                    int r0 = max(0, (int)((viewTop - top) / (CARD_HEIGHT + CARD_SPACING)));
                    int r1 = min(totalRows - 1, (int)((viewBottom - top) / (CARD_HEIGHT + CARD_SPACING)));
                    // Map rows to data indices
                    int s = namedCount + r0 * columns_;
                    int e = namedCount + min((r1 + 1) * columns_, unnamedCount);
                    // If Load more button row is visible, include it
                    if (hasMore && r1 >= unnamedRows) {
                        e = max(e, (int)items.size() + 1);
                    }
                    startIdx = min(startIdx, s);
                    endIdx = max(endIdx, e);
                }
            }
            if (endIdx <= startIdx) return {0, 0};
            return {startIdx, endIdx};
        }

        void onSetup() override {
            // Section headers (non-recycled, permanent children of content_)
            namedHeader_ = make_shared<SectionHeader>();
            namedHeader_->fontRef = fontLargeRef;
            content_->addChild(namedHeader_);
            namedHeader_->setActive(false);

            unnamedHeader_ = make_shared<SectionHeader>();
            unnamedHeader_->fontRef = fontLargeRef;
            content_->addChild(unnamedHeader_);
            unnamedHeader_->setActive(false);
        }

        void onPoolRebuilt() override {
            // Re-add section headers (rebuildPool removes pool items individually,
            // but headers were removed when content was cleared on first rebuild)
            // Headers are permanent children — just ensure they're in the tree
            if (namedHeader_ && !namedHeader_->getParent()) {
                content_->addChild(namedHeader_);
            }
            if (unnamedHeader_ && !unnamedHeader_->getParent()) {
                content_->addChild(unnamedHeader_);
            }
            updateSectionHeaders();
        }

    private:
        friend class PeopleView;

        // Section Y positions (computed in recalcLayout via updateSectionHeaders)
        float namedStartY_ = 0;
        float unnamedStartY_ = 0;

        shared_ptr<SectionHeader> namedHeader_;
        shared_ptr<SectionHeader> unnamedHeader_;

        static constexpr float CARD_WIDTH = 144.0f;
        static constexpr float CARD_HEIGHT = 58.0f;
        static constexpr float CARD_SPACING = 6.0f;
        static constexpr float SECTION_HEADER_HEIGHT = 32.0f;
        static constexpr float PADDING = 16.0f;

        void updateSectionHeaders() {
            float contentWidth = getWidth() - scrollBarWidth_;
            float y = PADDING;

            if (namedCount > 0) {
                namedHeader_->setActive(true);
                namedHeader_->text = format("Known People ({})", namedCount);
                namedHeader_->setPos(PADDING, y);
                namedHeader_->setSize(contentWidth - PADDING * 2, SECTION_HEADER_HEIGHT);
                y += SECTION_HEADER_HEIGHT + CARD_SPACING;
                namedStartY_ = y;

                int rows = (namedCount + columns_ - 1) / columns_;
                y += rows * (CARD_HEIGHT + CARD_SPACING);
                y += PADDING;
            } else {
                namedHeader_->setActive(false);
                namedStartY_ = y;
            }

            int uCount = (int)items.size() - namedCount;
            if (uCount > 0) {
                unnamedHeader_->setActive(true);
                unnamedHeader_->text = format("Unknown Faces ({})", uCount);
                unnamedHeader_->setPos(PADDING, y);
                unnamedHeader_->setSize(contentWidth - PADDING * 2, SECTION_HEADER_HEIGHT);
                y += SECTION_HEADER_HEIGHT + CARD_SPACING;
                unnamedStartY_ = y;
            } else {
                unnamedHeader_->setActive(false);
                unnamedStartY_ = y;
            }
        }
    };

    // =========================================================================
    // GalleryRecycler - RecyclerGrid<FaceCropNode> (uniform grid)
    // =========================================================================
    class GalleryRecycler : public RecyclerGrid<FaceCropNode> {
    public:
        using Ptr = shared_ptr<GalleryRecycler>;

        // External references
        unordered_map<string, Texture>* texturesRef = nullptr;
        Font* fontLargeRef = nullptr;

        // Callbacks
        function<void(int dataIdx)> onFaceClick;
        function<void(Vec2 screenPos)> onDragStart;
        function<void(Vec2 screenPos)> onDragMove;
        function<void(Vec2 screenPos)> onDragEnd;

        // Data
        vector<PhotoDatabase::FaceBrief> faces;
        string headerText;

        // Selection (dataIdx-based)
        unordered_set<int> selectedSet;

        void setData(const vector<PhotoDatabase::FaceBrief>& newFaces,
                     const string& header) {
            faces = newFaces;
            headerText = header;
            selectedSet.clear();
            itemWidth_ = CROP_SIZE;
            itemHeight_ = CROP_SIZE;
            spacing_ = CROP_SPACING;
            padding_ = PADDING;
        }

        void draw() override {
            // Transparent - PeopleView draws the background
        }

    protected:
        int getDataCount() const override { return (int)faces.size(); }

        ItemPtr createPoolItem(int poolIdx) override {
            auto crop = make_shared<FaceCropNode>();
            crop->setSize(CROP_SIZE, CROP_SIZE);
            int pi = poolIdx;
            crop->onClick = [this, pi]() {
                int dataIdx = getReverseMap()[pi];
                if (dataIdx >= 0 && onFaceClick) onFaceClick(dataIdx);
            };
            crop->onDragStart = [this](Vec2 sp) { if (onDragStart) onDragStart(sp); };
            crop->onDragMove = [this](Vec2 sp) { if (onDragMove) onDragMove(sp); };
            crop->onDragEnd = [this](Vec2 sp) { if (onDragEnd) onDragEnd(sp); };
            return crop;
        }

        void onBind(int dataIdx, ItemPtr& item) override {
            if (dataIdx >= (int)faces.size()) return;
            auto& fb = faces[dataIdx];
            item->photoId = fb.photoId;
            item->faceId = fb.faceId;
            item->faceX = fb.x;
            item->faceY = fb.y;
            item->faceW = fb.w;
            item->faceH = fb.h;
            item->selected = selectedSet.count(dataIdx) > 0;

            if (texturesRef) {
                auto it = texturesRef->find(fb.photoId);
                item->textureRef = (it != texturesRef->end() && it->second.isAllocated())
                    ? &it->second : nullptr;
            }
        }

        void onUnbind(int dataIdx, ItemPtr& item) override {
            (void)dataIdx;
            item->textureRef = nullptr;
            item->selected = false;
        }

        // Override content height to account for gallery header
        float calcContentHeight() override {
            float h = PADDING + SECTION_HEADER_HEIGHT + CROP_SPACING;
            if (totalRows_ > 0) {
                h += totalRows_ * rowHeight_ - spacing_;
            }
            h += PADDING;
            return h;
        }

        Vec2 getItemPosition(int dataIdx) override {
            int col = dataIdx % columns_;
            int row = dataIdx / columns_;
            float x = padding_ + col * (CROP_SIZE + CROP_SPACING);
            float y = headerBaseY_ + row * rowHeight_;
            return {x, y};
        }

        pair<int, int> calcVisibleDataRange(float scrollY) override {
            if (totalRows_ == 0) return {0, 0};
            float viewTop = scrollY;
            float viewBottom = scrollY + getHeight();
            int firstRow = max(0, (int)((viewTop - headerBaseY_) / rowHeight_) - 2);
            int lastRow = min(totalRows_ - 1, (int)((viewBottom - headerBaseY_) / rowHeight_) + 2);
            int startIdx = firstRow * columns_;
            int endIdx = min((lastRow + 1) * columns_, getDataCount());
            return {startIdx, endIdx};
        }

        void onSetup() override {
            galleryHeader_ = make_shared<GalleryHeader>();
            galleryHeader_->fontRef = fontLargeRef;
            content_->addChild(galleryHeader_);
        }

        void onPoolRebuilt() override {
            if (galleryHeader_ && !galleryHeader_->getParent()) {
                content_->addChild(galleryHeader_);
            }
            headerBaseY_ = PADDING + SECTION_HEADER_HEIGHT + CROP_SPACING;
            if (galleryHeader_) {
                galleryHeader_->setActive(true);
                galleryHeader_->text = headerText;
                float contentWidth = getWidth() - scrollBarWidth_;
                galleryHeader_->setPos(PADDING, PADDING);
                galleryHeader_->setSize(contentWidth - PADDING * 2, SECTION_HEADER_HEIGHT);
            }
        }

    private:
        static constexpr float CROP_SIZE = 80.0f;
        static constexpr float CROP_SPACING = 6.0f;
        static constexpr float PADDING = 16.0f;
        static constexpr float SECTION_HEADER_HEIGHT = 32.0f;

        float headerBaseY_ = 0;
        shared_ptr<GalleryHeader> galleryHeader_;
    };

    // =========================================================================
    // Recycler instances
    // =========================================================================
    shared_ptr<CardRecycler> cardRecycler_;
    shared_ptr<GalleryRecycler> galleryRecycler_;
    shared_ptr<NameEditOverlay> nameOverlay_;

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

    // =========================================================================
    // UI rebuild (card list)
    // =========================================================================

    void rebuildUI() {
        int prevSelectedClusterId = -1;
        if (selectedDataIdx_ >= 0 && selectedDataIdx_ < (int)cardItems_.size()) {
            prevSelectedClusterId = cardItems_[selectedDataIdx_]->clusterId;
        }

        // Build flat list: named first, then unnamed
        cardItems_.clear();
        namedCount_ = 0;
        vector<const PhotoProvider::FaceCluster*> named, unnamed;
        for (auto& c : clusters_) {
            if (c.personId > 0) named.push_back(&c);
            else unnamed.push_back(&c);
        }
        for (auto* p : named) cardItems_.push_back(p);
        namedCount_ = (int)named.size();
        for (auto* p : unnamed) cardItems_.push_back(p);

        // Restore selection by cluster ID
        selectedDataIdx_ = -1;
        if (prevSelectedClusterId >= 0) {
            for (int i = 0; i < (int)cardItems_.size(); i++) {
                if (cardItems_[i]->clusterId == prevSelectedClusterId) {
                    selectedDataIdx_ = i;
                    break;
                }
            }
        }

        // Update card recycler
        if (cardRecycler_) {
            cardRecycler_->setData(cardItems_, namedCount_);
            cardRecycler_->selectedIdx = selectedDataIdx_;
            cardRecycler_->hasMore = hasMoreFaces_;
            cardRecycler_->remainingCount = totalUnnamedFaces_ - maxFaces_;
            if (cardRecycler_->remainingCount < 0) cardRecycler_->remainingCount = 0;
            cardRecycler_->onLoadMore = [this]() { loadMore(); };
            cardRecycler_->resetScroll();
            cardRecycler_->rebuild();
        }

        // Queue thumbnail loads (limit to avoid exhausting texture pool)
        unordered_set<string> neededIds;
        int queued = 0;
        for (auto& c : clusters_) {
            if (textures_.count(c.repPhotoId) == 0) {
                neededIds.insert(c.repPhotoId);
                if (++queued >= MAX_CARD_TEXTURES) break;
            }
        }
        queueMissingThumbnails(neededIds);

        if (onRedraw) onRedraw();
    }

    // =========================================================================
    // Gallery
    // =========================================================================

    void showGallery(int dataIdx) {
        if (dataIdx < 0 || dataIdx >= (int)cardItems_.size()) return;

        // Update selection
        selectedDataIdx_ = dataIdx;

        // Update bound cards' selection state
        if (cardRecycler_) {
            cardRecycler_->selectedIdx = selectedDataIdx_;
            for (auto& [di, pi] : cardRecycler_->getPoolMap()) {
                cardRecycler_->getPool()[pi]->selected = (di == selectedDataIdx_);
            }
        }

        // Build gallery content (no need to rebuild card list — width unchanged)
        rebuildGallery(*cardItems_[dataIdx]);

        if (onRedraw) onRedraw();
    }

    void clearGallerySelection() {
        selectedDataIdx_ = -1;
        lastFaceClickIdx_ = -1;
        if (cardRecycler_) {
            cardRecycler_->selectedIdx = -1;
            for (auto& [di, pi] : cardRecycler_->getPoolMap()) {
                cardRecycler_->getPool()[pi]->selected = false;
            }
        }
        if (galleryRecycler_) {
            galleryRecycler_->selectedSet.clear();
            galleryRecycler_->unbindAll();
        }
        if (onRedraw) onRedraw();
    }

    void rebuildGallery(const PhotoProvider::FaceCluster& cluster) {
        if (!provider_) {
            logWarning() << "[PeopleView] rebuildGallery: no provider!";
            return;
        }

        // Clean up textures not needed by card list
        cleanupUnusedTextures();

        // Get face details
        int totalFaces = (int)cluster.faceIds.size();
        auto briefs = provider_->getFaceBriefs(cluster.faceIds);

        // Header text
        string headerText;
        if (cluster.name.empty()) {
            headerText = format("Cluster ({} faces)", totalFaces);
        } else {
            headerText = format("{} ({} faces)", cluster.name, totalFaces);
        }

        // Queue missing thumbnails
        unordered_set<string> neededIds;
        for (auto& fb : briefs) {
            neededIds.insert(fb.photoId);
        }
        queueMissingThumbnails(neededIds);

        // Set data and rebuild
        if (galleryRecycler_) {
            galleryRecycler_->setData(briefs, headerText);
            galleryRecycler_->resetScroll();
            galleryRecycler_->rebuild();
        }
    }

    // =========================================================================
    // Face click / selection / delete handling
    // =========================================================================

    void handleFaceClick(int dataIdx) {
        if (!galleryRecycler_ || dataIdx < 0 || dataIdx >= (int)galleryRecycler_->faces.size())
            return;

        auto now = chrono::steady_clock::now();
        bool isDoubleClick = (dataIdx == lastFaceClickIdx_ &&
            chrono::duration_cast<chrono::milliseconds>(now - lastFaceClickTime_).count() < 300);
        lastFaceClickTime_ = now;
        lastFaceClickIdx_ = dataIdx;

        if (isDoubleClick) {
            // Double-click: open in single view
            const string& photoId = galleryRecycler_->faces[dataIdx].photoId;
            if (onFaceDoubleClick) onFaceDoubleClick(photoId);
            return;
        }

        // Check modifier for multi-select
        bool cmdHeld = cmdDownRef ? *cmdDownRef : false;

        if (cmdHeld) {
            // Toggle selection
            if (galleryRecycler_->selectedSet.count(dataIdx)) {
                galleryRecycler_->selectedSet.erase(dataIdx);
            } else {
                galleryRecycler_->selectedSet.insert(dataIdx);
            }
        } else {
            // Single select
            galleryRecycler_->selectedSet.clear();
            galleryRecycler_->selectedSet.insert(dataIdx);
        }

        // Update visual selection on bound items
        for (auto& [di, pi] : galleryRecycler_->getPoolMap()) {
            galleryRecycler_->getPool()[pi]->selected =
                galleryRecycler_->selectedSet.count(di) > 0;
        }

        // Notify callback with selected photo
        const string& photoId = galleryRecycler_->faces[dataIdx].photoId;
        if (onFaceSelect) onFaceSelect(photoId);

        // Send face overlays for the selected photo
        sendFaceOverlays(photoId);

        if (onRedraw) onRedraw();
    }

    void handleDeleteSelectedFaces() {
        if (!galleryRecycler_ || !provider_) return;
        if (galleryRecycler_->selectedSet.empty()) return;
        if (selectedDataIdx_ < 0 || selectedDataIdx_ >= (int)cardItems_.size()) return;

        // Collect face IDs to unassign
        vector<int> faceIdsToRemove;
        vector<int> dataIndicesToRemove;
        for (int di : galleryRecycler_->selectedSet) {
            if (di >= 0 && di < (int)galleryRecycler_->faces.size()) {
                faceIdsToRemove.push_back(galleryRecycler_->faces[di].faceId);
                dataIndicesToRemove.push_back(di);
            }
        }

        if (faceIdsToRemove.empty()) return;

        // Unassign in DB
        provider_->unassignFaces(faceIdsToRemove);
        galleryRecycler_->selectedSet.clear();

        // Update the cluster's faceIds in-place
        int clusterIdx = -1;
        for (int i = 0; i < (int)clusters_.size(); i++) {
            if (cardItems_[selectedDataIdx_] == &clusters_[i]) {
                clusterIdx = i;
                break;
            }
        }

        if (clusterIdx >= 0) {
            auto& cFaceIds = clusters_[clusterIdx].faceIds;
            unordered_set<int> removeSet(faceIdsToRemove.begin(), faceIdsToRemove.end());
            cFaceIds.erase(
                remove_if(cFaceIds.begin(), cFaceIds.end(),
                    [&](int id) { return removeSet.count(id) > 0; }),
                cFaceIds.end());
            clusters_[clusterIdx].photoCount = (int)provider_->getPhotoIdsForFaceIds(cFaceIds).size();

            if (cFaceIds.empty()) {
                // Cluster is now empty — clear selection and rebuild
                clearGallerySelection();
                clusters_.erase(clusters_.begin() + clusterIdx);
                rebuildUI();
            } else {
                // Rebuild gallery with updated face list
                rebuildGallery(clusters_[clusterIdx]);
                // Update card display
                if (cardRecycler_) {
                    for (auto& [di, pi] : cardRecycler_->getPoolMap()) {
                        if (di == selectedDataIdx_) {
                            cardRecycler_->getPool()[pi]->bindCluster(
                                clusters_[clusterIdx], true);
                        }
                    }
                }
            }
        }

        if (onRedraw) onRedraw();
    }

    // =========================================================================
    // Name edit handling
    // =========================================================================

    void handleNameConfirm(const string& name) {
        if (editingClusterId_ == INT_MIN || !provider_) return;

        // Find the cluster by ID
        const PhotoProvider::FaceCluster* editCluster = nullptr;
        for (auto& c : clusters_) {
            if (c.clusterId == editingClusterId_) {
                editCluster = &c;
                break;
            }
        }
        if (!editCluster) { hideNameOverlay(); return; }
        if (editCluster->personId > 0) {
            provider_->renamePerson(editCluster->personId, name);
        } else {
            provider_->assignNameToCluster(*editCluster, name);
        }

        hideNameOverlay();

        // Rebuild clusters
        auto cr = provider_->buildFaceClusters(0.60f, maxFaces_);
        clusters_ = std::move(cr.clusters);
        totalUnnamedFaces_ = cr.totalUnnamed;
        hasMoreFaces_ = cr.processedUnnamed < cr.totalUnnamed;
        if (galleryRecycler_) {
            galleryRecycler_->selectedSet.clear();
            galleryRecycler_->unbindAll();
        }
        selectedDataIdx_ = -1;
        rebuildUI();
    }

    void startNameEdit(int dataIdx) {
        if (dataIdx < 0 || dataIdx >= (int)cardItems_.size()) return;
        auto* cluster = cardItems_[dataIdx];
        editingClusterId_ = cluster->clusterId;
        string initial = cluster->name;
        // For unnamed clusters, preset suggested name (without "?")
        if (initial.empty()) initial = cluster->suggestedName;
        string placeholder = cluster->suggestedName;
        if (nameOverlay_) {
            nameOverlay_->setPos(0, 0);
            nameOverlay_->setSize(getWidth(), getHeight());
            nameOverlay_->show(initial, placeholder);
        }
        if (onRedraw) onRedraw();
    }

    void hideNameOverlay() {
        if (nameOverlay_) nameOverlay_->hide();
        editingClusterId_ = INT_MIN;
        if (onRedraw) onRedraw();
    }

    // =========================================================================
    // Face overlay on metadata thumbnail
    // =========================================================================

    void sendFaceOverlays(const string& photoId) {
        if (!onOverlayUpdate || !galleryRecycler_) return;

        vector<OverlayRect> overlays;
        for (int di : galleryRecycler_->selectedSet) {
            if (di < 0 || di >= (int)galleryRecycler_->faces.size()) continue;
            auto& fb = galleryRecycler_->faces[di];
            if (fb.photoId != photoId) continue;

            overlays.push_back({
                fb.x, fb.y, fb.w, fb.h,
                Color(0.3f, 0.7f, 1.0f, 0.9f),
                2.0f
            });
        }

        // Also show all other faces in same photo as dim outlines
        for (int i = 0; i < (int)galleryRecycler_->faces.size(); i++) {
            auto& fb = galleryRecycler_->faces[i];
            if (fb.photoId != photoId) continue;
            if (galleryRecycler_->selectedSet.count(i)) continue;  // skip selected

            overlays.push_back({
                fb.x, fb.y, fb.w, fb.h,
                Color(0.5f, 0.5f, 0.55f, 0.5f),
                1.0f
            });
        }

        onOverlayUpdate(overlays);
    }

    // =========================================================================
    // Drag & drop
    // =========================================================================

    void handleDragStart(Vec2 screenPos) {
        if (!galleryRecycler_ || selectedDataIdx_ < 0) return;

        // Ensure the dragged face is in the selection
        // (if nothing selected yet, the click handler hasn't fired, so check)
        auto& sel = galleryRecycler_->selectedSet;
        if (sel.empty()) {
            // Select all faces in current gallery
            int count = (int)galleryRecycler_->faces.size();
            for (int i = 0; i < count; i++) sel.insert(i);
        }

        isDragging_ = true;
        dragFaceCount_ = (int)sel.size();
        dragGhostPos_ = screenPos;
        if (onRedraw) onRedraw();
    }

    void handleDragMove(Vec2 screenPos) {
        if (!isDragging_) return;
        dragGhostPos_ = screenPos;
        updateDropHighlight(screenPos);
        if (onRedraw) onRedraw();
    }

    void handleDragEnd(Vec2 screenPos) {
        if (!isDragging_) return;

        if (dropTargetDataIdx_ >= 0) {
            performDrop(dropTargetDataIdx_);
        }

        isDragging_ = false;
        dragFaceCount_ = 0;
        dropTargetDataIdx_ = -1;
        clearDropHighlight();
        if (onRedraw) onRedraw();
    }

    void updateDropHighlight(Vec2 screenPos) {
        if (!cardRecycler_) return;

        int newTarget = -1;

        for (auto& [dataIdx, poolIdx] : cardRecycler_->getPoolMap()) {
            auto& card = cardRecycler_->getPool()[poolIdx];
            if (card->isLoadMoreButton) {
                card->dropHighlight = false;
                continue;
            }

            // Only named cards (personId > 0) and not the source card
            if (dataIdx < (int)cardItems_.size() &&
                cardItems_[dataIdx]->personId > 0 &&
                dataIdx != selectedDataIdx_) {

                float lx, ly;
                card->globalToLocal(screenPos.x, screenPos.y, lx, ly);
                float cw = card->getWidth(), ch = card->getHeight();
                if (lx >= 0 && lx < cw && ly >= 0 && ly < ch) {
                    newTarget = dataIdx;
                    card->dropHighlight = true;
                } else {
                    card->dropHighlight = false;
                }
            } else {
                card->dropHighlight = false;
            }
        }
        dropTargetDataIdx_ = newTarget;
    }

    void clearDropHighlight() {
        if (!cardRecycler_) return;
        for (auto& [di, pi] : cardRecycler_->getPoolMap()) {
            cardRecycler_->getPool()[pi]->dropHighlight = false;
        }
    }

    void performDrop(int targetCardIdx) {
        if (!provider_ || !galleryRecycler_) return;
        if (targetCardIdx < 0 || targetCardIdx >= (int)cardItems_.size()) return;

        auto* targetCluster = cardItems_[targetCardIdx];
        if (targetCluster->personId <= 0) return;

        // Collect selected face IDs
        vector<int> faceIds;
        for (int di : galleryRecycler_->selectedSet) {
            if (di >= 0 && di < (int)galleryRecycler_->faces.size()) {
                faceIds.push_back(galleryRecycler_->faces[di].faceId);
            }
        }
        if (faceIds.empty()) return;

        // Assign faces to target person
        provider_->assignFacesToPerson(faceIds, targetCluster->personId);

        // Clear selection and re-cluster
        galleryRecycler_->selectedSet.clear();
        runClustering();
    }

    // =========================================================================
    // Thumbnail loading
    // =========================================================================

    void cleanupUnusedTextures() {
        unordered_set<string> needed;
        for (auto& c : clusters_) {
            needed.insert(c.repPhotoId);
        }
        // Also keep gallery face textures
        if (galleryRecycler_ && selectedDataIdx_ >= 0) {
            for (auto& fb : galleryRecycler_->faces) {
                needed.insert(fb.photoId);
            }
        }
        for (auto it = textures_.begin(); it != textures_.end(); ) {
            if (needed.count(it->first) == 0) {
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void runClustering() {
        // Run DB load + clustering in background thread
        if (clusterThread_.joinable()) clusterThread_.join();
        int mf = maxFaces_;
        clusterThread_ = thread([this, mf]() {
            logNotice() << "[PeopleView] Loading face data from DB...";
            auto input = provider_->loadFaceClusterData();
            logNotice() << "[PeopleView] Loaded " << input.allFaces.size()
                        << " faces, clustering (maxFaces=" << mf << ")...";
            auto result = PhotoProvider::clusterFaces(
                input.allFaces, input.personNames, 0.60f, mf);
            {
                lock_guard<mutex> lock(clusterMutex_);
                pendingClusterResult_ = std::move(result);
            }
            clusteringDone_ = true;
        });
    }

    void queueMissingThumbnails(const unordered_set<string>& neededIds) {
        for (const auto& id : neededIds) {
            if (textures_.count(id) == 0) {
                bool alreadyQueued = false;
                for (const auto& p : pendingLoads_) {
                    if (p == id) { alreadyQueued = true; break; }
                }
                if (!alreadyQueued) {
                    pendingLoads_.push_back(id);
                }
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
            if (!provider_) continue;
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
    }
};
