#pragma once

// =============================================================================
// PeopleView.h - People view with face clusters (Lightroom-style)
// Displays named persons and unnamed face clusters with thumbnail cards.
// Click a card to show face gallery; click name label to edit name.
// =============================================================================

#include <TrussC.h>
#include <tcxIME.h>
#include <optional>
#include "PhotoProvider.h"
#include "FolderTree.h"  // for loadJapaneseFont, PlainScrollContainer

using namespace std;
using namespace tc;

class PeopleView : public RectNode {
public:
    using Ptr = shared_ptr<PeopleView>;

    // Callbacks
    function<void()> onRedraw;

    void setup() override {
        enableEvents();
        setClipping(true);
        loadJapaneseFont(font_, 14);
        loadJapaneseFont(fontSmall_, 11);
        loadJapaneseFont(fontLarge_, 16);

        // Left: card list scroll container
        cardScroll_ = make_shared<PlainScrollContainer>();
        addChild(cardScroll_);
        cardContent_ = make_shared<RectNode>();
        cardScroll_->setContent(cardContent_);

        // Right: face gallery scroll container (hidden initially)
        galleryScroll_ = make_shared<PlainScrollContainer>();
        addChild(galleryScroll_);
        galleryContent_ = make_shared<RectNode>();
        galleryScroll_->setContent(galleryContent_);
        // Keep always active but move offscreen when not visible
        galleryScroll_->setPos(-9999, -9999);
        galleryScroll_->setSize(0, 0);

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

    bool hasGallery() const { return galleryVisible_; }
    bool isNameEditing() const { return nameOverlay_ && nameOverlay_->getActive(); }

    void populate(PhotoProvider& provider) {
        provider_ = &provider;
        editingCard_ = nullptr;
        selectedCard_ = nullptr;
        pendingClickCard_ = nullptr;
        clusteringDone_ = false;
        galleryVisible_ = false;

        // Clear previous state
        if (cardContent_) cardContent_->removeAllChildren();
        if (galleryContent_) galleryContent_->removeAllChildren();
        allCards_.clear();
        galleryCrops_.clear();
        clusters_.clear();
        if (galleryScroll_) { galleryScroll_->setPos(-9999, -9999);
        galleryScroll_->setSize(0, 0); }

        // Run DB load + clustering in background thread
        // (SQLite WAL + serialized mode allows reads from any thread)
        if (clusterThread_.joinable()) clusterThread_.join();
        clusterThread_ = thread([this]() {
            logNotice() << "[PeopleView] Loading face data from DB...";
            auto input = provider_->loadFaceClusterData();
            logNotice() << "[PeopleView] Loaded " << input.allFaces.size()
                        << " faces, clustering...";
            auto result = PhotoProvider::clusterFaces(
                input.allFaces, input.personNames);
            {
                lock_guard<mutex> lock(clusterMutex_);
                pendingClusters_ = std::move(result);
            }
            clusteringDone_ = true;
        });
    }

    void update() override {
        // Process deferred card click → show gallery
        if (pendingClickCard_) {
            auto* card = pendingClickCard_;
            pendingClickCard_ = nullptr;
            showGallery(card);
            // Don't return — let layout sync and rebuild run in the same frame
        }

        // Process clustering completion from background thread
        if (clusteringDone_) {
            clusteringDone_ = false;
            {
                lock_guard<mutex> lock(clusterMutex_);
                clusters_ = std::move(pendingClusters_);
            }
            needsRebuild_ = true;
            if (onRedraw) onRedraw();
        }

        // Sync scroll container layout (do in update, not draw)
        float w = getWidth(), h = getHeight();
        if (w > 0 && h > 0) {
            float cardListWidth = galleryVisible_ ? w * 0.35f : w;
            float galleryWidth = galleryVisible_ ? w - cardListWidth : 0;

            if (cardScroll_) {
                cardScroll_->setPos(0, 0);
                cardScroll_->setSize(cardListWidth, h);
            }
            if (galleryScroll_) {
                if (galleryVisible_) {
                    galleryScroll_->setPos(cardListWidth, 0);
                    galleryScroll_->setSize(galleryWidth, h);
                } else {
                    galleryScroll_->setPos(-9999, -9999);
                    galleryScroll_->setSize(0, 0);
                }
            }
        }

        // Deferred rebuild (do in update, not draw — addChild during draw is unsafe)
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
            // Update texture references on cards
            for (auto* card : allCards_) {
                auto it = textures_.find(card->cluster.repPhotoId);
                card->textureRef = (it != textures_.end() && it->second.isAllocated())
                    ? &it->second : nullptr;
            }
            // Update gallery crop textures
            updateGalleryTextures();
            if (onRedraw) onRedraw();
        }

        // Start load thread if needed
        if (!pendingLoads_.empty() && !loadThreadRunning_) {
            startLoadThread();
        }

        // Visibility culling: deactivate offscreen cards to avoid vertex buffer overflow
        // setActive(false) skips entire subtree (including NameLabel text draws)
        if (cardScroll_ && !allCards_.empty()) {
            float scrollY = cardScroll_->getScrollY();
            float viewH = cardScroll_->getHeight();
            float margin = CARD_HEIGHT;
            for (auto* card : allCards_) {
                float cy = card->getPos().y;
                bool vis = (cy + CARD_HEIGHT > scrollY - margin)
                        && (cy < scrollY + viewH + margin);
                card->setActive(vis);
            }
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

        // Gallery separator line
        if (galleryVisible_) {
            float cardListWidth = w * 0.35f;
            setColor(0.25f, 0.25f, 0.28f);
            fill();
            drawRect(cardListWidth, 0, 1, h);
        }

        // Status bar at bottom
        setColor(0, 0, 0, 0.5f);
        fill();
        drawRect(8, h - 28, 200, 20);
        setColor(0.7f, 0.7f, 0.75f);
        int namedCount = 0, unnamedCount = 0;
        for (auto& c : clusters_) {
            if (c.personId > 0) namedCount++; else unnamedCount++;
        }
        fontSmall_.drawString(
            format("People  {} named, {} clusters", namedCount, unnamedCount),
            14, h - 18, Direction::Left, Direction::Center);
    }

    void shutdown() {
        if (clusterThread_.joinable()) clusterThread_.join();
        if (loadThread_.joinable()) loadThread_.join();
        cardContent_->removeAllChildren();
        galleryContent_->removeAllChildren();
        allCards_.clear();
        selectedCard_ = nullptr;
        textures_.clear();
        pendingLoads_.clear();
        clusters_.clear();
        editingCard_ = nullptr;
        pendingClickCard_ = nullptr;
        galleryVisible_ = false;
        galleryScroll_->setPos(-9999, -9999);
        galleryScroll_->setSize(0, 0);
        galleryCrops_.clear();
    }

    bool onKeyPress(int key) override {
        // ESC: cancel name edit, or close gallery, or fall through to tcApp
        if (key == 256 /* ESCAPE */) {
            if (nameOverlay_ && nameOverlay_->getActive()) {
                hideNameOverlay();
                return true;
            }
            if (galleryVisible_) {
                hideGallery();
                return true;
            }
        }

        // N: edit name of selected card
        if (key == 'N' && selectedCard_ && !isNameEditing()) {
            editingCard_ = selectedCard_;
            string initial = selectedCard_->cluster.name;
            string placeholder = selectedCard_->cluster.suggestedName;
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

    // Background clustering
    thread clusterThread_;
    mutex clusterMutex_;
    vector<PhotoProvider::FaceCluster> pendingClusters_;
    atomic<bool> clusteringDone_{false};

    // UI components
    PlainScrollContainer::Ptr cardScroll_;
    RectNode::Ptr cardContent_;
    PlainScrollContainer::Ptr galleryScroll_;
    RectNode::Ptr galleryContent_;
    Font font_, fontSmall_, fontLarge_;

    // Gallery state
    bool galleryVisible_ = false;

    // Layout constants
    static constexpr float CARD_WIDTH = 180.0f;
    static constexpr float CARD_HEIGHT = 72.0f;
    static constexpr float CARD_SPACING = 8.0f;
    static constexpr float SECTION_HEADER_HEIGHT = 32.0f;
    static constexpr float PADDING = 16.0f;
    static constexpr float CROP_SIZE = 80.0f;
    static constexpr float CROP_SPACING = 6.0f;
    static constexpr int MAX_GALLERY_FACES = 200;
    static constexpr int MAX_CARD_TEXTURES = 300;  // limit textures for card list

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

        // Display-only node — no events (clicks pass through to PersonCard)

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

            // Counts
            string countStr = format("{} photos, {} faces", photoCount, faceCount);
            setColor(0.45f, 0.45f, 0.5f);
            if (fontSmallRef) fontSmallRef->drawString(countStr, textX, textY + 18,
                Direction::Left, Direction::Top);
        }
    };

    // =========================================================================
    // Inner node: PersonCard
    // =========================================================================
    class PersonCard : public RectNode {
    public:
        PhotoProvider::FaceCluster cluster;
        bool selected = false;

        // Borrowed pointers
        Texture* textureRef = nullptr;
        Font* fontRef = nullptr;
        Font* fontSmallRef = nullptr;

        // Callbacks
        function<void(PersonCard*)> onCardClick;   // card click → gallery

        shared_ptr<NameLabel> nameLabel;

        void setup() override {
            enableEvents();

            // Create name label as child node
            nameLabel = make_shared<NameLabel>();
            nameLabel->fontRef = fontRef;
            nameLabel->fontSmallRef = fontSmallRef;
            nameLabel->name = cluster.name;
            nameLabel->suggestedName = cluster.suggestedName;
            nameLabel->faceCount = (int)cluster.faceIds.size();
            nameLabel->photoCount = cluster.photoCount;

            // Position name label correctly from the start
            // (default RectNode size is 100x100 which overlaps the face thumbnail area)
            float thumbSize = getHeight() - 12;
            float labelX = 6 + thumbSize + 4;
            float labelW = getWidth() - labelX - 4;
            nameLabel->setPos(labelX, 8);
            nameLabel->setSize(max(1.0f, labelW), max(1.0f, getHeight() - 16));

            addChild(nameLabel);
        }

        void draw() override {
            float w = getWidth(), h = getHeight();

            // Background
            if (selected) {
                setColor(0.2f, 0.35f, 0.55f);
            } else {
                setColor(0.12f, 0.12f, 0.14f);
            }
            fill();
            drawRect(0, 0, w, h);

            // Border
            setColor(selected ? Color(0.4f, 0.6f, 0.9f) : Color(0.2f, 0.2f, 0.22f));
            noFill();
            drawRect(0, 0, w, h);

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

                // Expand crop for some margin
                float margin = max(fw, fh) * 0.3f;
                float sx = fx - margin;
                float sy = fy - margin;
                float sw = fw + margin * 2;
                float sh = fh + margin * 2;

                // Clamp to image bounds
                sx = max(0.0f, sx);
                sy = max(0.0f, sy);
                if (sx + sw > imgW) sw = imgW - sx;
                if (sy + sh > imgH) sh = imgH - sy;

                // Fit into square
                float fitScale = min(thumbSize / sw, thumbSize / sh);
                float dw = sw * fitScale;
                float dh = sh * fitScale;
                float dx = thumbX + (thumbSize - dw) / 2;
                float dy = thumbY + (thumbSize - dh) / 2;

                setColor(1, 1, 1);
                textureRef->drawSubsection(dx, dy, dw, dh, sx, sy, sw, sh);
            } else {
                // Placeholder
                setColor(0.2f, 0.2f, 0.22f);
                fill();
                drawRect(thumbX, thumbY, thumbSize, thumbSize);
            }

            // Position name label to the right of thumbnail
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
            if (onCardClick) onCardClick(this);
            return true;
        }
    };

    // =========================================================================
    // Inner node: SectionHeader
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

            // Bottom line
            setColor(0.25f, 0.25f, 0.28f);
            fill();
            drawRect(0, h - 1, w, 1);

            setColor(0.7f, 0.7f, 0.75f);
            if (fontRef) fontRef->drawString(text, 8, h / 2,
                Direction::Left, Direction::Center);
        }
    };

    // =========================================================================
    // Inner node: FaceCropNode (face crop in gallery)
    // =========================================================================
    class FaceCropNode : public RectNode {
    public:
        Texture* textureRef = nullptr;
        float faceX = 0, faceY = 0, faceW = 0, faceH = 0;
        string photoId;

        void draw() override {
            float w = getWidth(), h = getHeight();

            if (textureRef && textureRef->isAllocated()) {
                float imgW = textureRef->getWidth();
                float imgH = textureRef->getHeight();
                float fx = faceX * imgW;
                float fy = faceY * imgH;
                float fw = faceW * imgW;
                float fh = faceH * imgH;

                // Expand crop for margin
                float margin = max(fw, fh) * 0.3f;
                float sx = fx - margin;
                float sy = fy - margin;
                float sw = fw + margin * 2;
                float sh = fh + margin * 2;

                // Clamp
                sx = max(0.0f, sx);
                sy = max(0.0f, sy);
                if (sx + sw > imgW) sw = imgW - sx;
                if (sy + sh > imgH) sh = imgH - sy;

                // Fit into square
                float fitScale = min(w / sw, h / sh);
                float dw = sw * fitScale;
                float dh = sh * fitScale;
                float dx = (w - dw) / 2;
                float dy = (h - dh) / 2;

                setColor(1, 1, 1);
                textureRef->drawSubsection(dx, dy, dw, dh, sx, sy, sw, sh);
            } else {
                // Placeholder
                setColor(0.15f, 0.15f, 0.18f);
                fill();
                drawRect(0, 0, w, h);
            }

            // Border
            setColor(0.2f, 0.2f, 0.22f);
            noFill();
            drawRect(0, 0, w, h);
        }
    };

    // =========================================================================
    // Inner node: GalleryHeader
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
        }

        void show(const string& initialText, const string& placeholderText) {
            placeholder = placeholderText;
            ime_.clear();
            if (!initialText.empty()) {
                // IME doesn't have a setText, so we just show placeholder
            }
            ime_.enable();
            setActive(true);
        }

        void hide() {
            ime_.disable();
            setActive(false);
        }

        void update() override {
            // Cursor blink
            bool cursorOn = fmod(getElapsedTimef(), 1.0f) < 0.5f;
            if (cursorOn != lastCursorOn_) {
                lastCursorOn_ = cursorOn;
                redraw();
            }
        }

        void draw() override {
            float w = getWidth(), h = getHeight();

            // Dimmed background
            setColor(0, 0, 0, 0.6f);
            fill();
            drawRect(0, 0, w, h);

            // Dialog box
            float dlgW = 320, dlgH = 100;
            float dlgX = (w - dlgW) / 2;
            float dlgY = (h - dlgH) / 2;

            setColor(0.15f, 0.15f, 0.18f);
            fill();
            drawRect(dlgX, dlgY, dlgW, dlgH);

            setColor(0.3f, 0.3f, 0.35f);
            noFill();
            drawRect(dlgX, dlgY, dlgW, dlgH);

            // Label
            setColor(0.7f, 0.7f, 0.75f);
            if (fontRef) fontRef->drawString("Name:", dlgX + 12, dlgY + 24,
                Direction::Left, Direction::Center);

            // Input field background
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

            // Draw IME text
            string text = const_cast<tcxIME&>(ime_).getString();
            if (text.empty() && !placeholder.empty()) {
                setColor(0.4f, 0.4f, 0.45f);
                if (fontRef) fontRef->drawString(placeholder,
                    inputX + 6, inputY + inputH / 2,
                    Direction::Left, Direction::Center);
            }

            setColor(1, 1, 1);
            ime_.draw(inputX + 6, inputY + 4);

            // Hint
            setColor(0.4f, 0.4f, 0.45f);
            if (fontRef) fontRef->drawString("Enter to confirm, ESC to cancel",
                dlgX + dlgW / 2, dlgY + dlgH - 12,
                Direction::Center, Direction::Center);
        }

        bool onMousePress(Vec2 pos, int button) override {
            (void)pos; (void)button;
            return true;  // Consume all clicks
        }

        bool onKeyPress(int key) override {
            if (key == 256 /* ESCAPE */) {
                if (onCancel) onCancel();
                return true;
            }
            if (key == 257 /* ENTER */ || key == 335 /* KP_ENTER */) {
                string text = const_cast<tcxIME&>(ime_).getString();
                if (!text.empty() && onConfirm) onConfirm(text);
                return true;
            }
            return false;
        }

    private:
        tcxIME ime_;
        bool lastCursorOn_ = false;
    };

    // =========================================================================
    // Card management
    // =========================================================================

    vector<PersonCard*> allCards_;
    PersonCard* selectedCard_ = nullptr;
    PersonCard* editingCard_ = nullptr;
    shared_ptr<NameEditOverlay> nameOverlay_;

    // Deferred actions (avoid destroying nodes during their own callbacks)
    PersonCard* pendingClickCard_ = nullptr;

    // Textures keyed by photoId
    unordered_map<string, Texture> textures_;

    // Gallery face crop nodes (kept alive as long as gallery is visible)
    vector<shared_ptr<FaceCropNode>> galleryCrops_;

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
        int prevSelectedClusterId = selectedCard_ ? selectedCard_->cluster.clusterId : INT_MIN;
        cardContent_->removeAllChildren();
        allCards_.clear();
        selectedCard_ = nullptr;

        float y = PADDING;
        float cardListWidth = galleryVisible_ ? getWidth() * 0.35f : getWidth();
        float contentWidth = cardListWidth - PADDING * 2;
        int cols = max(1, (int)((contentWidth + CARD_SPACING) / (CARD_WIDTH + CARD_SPACING)));

        // Separate named and unnamed clusters
        vector<const PhotoProvider::FaceCluster*> named, unnamed;
        for (auto& c : clusters_) {
            if (c.personId > 0) named.push_back(&c);
            else unnamed.push_back(&c);
        }

        // Known People section
        if (!named.empty()) {
            y = addSectionHeader(format("Known People ({})", named.size()), y, contentWidth);
            y = addCards(named, y, cols, prevSelectedClusterId);
            y += PADDING;
        }

        // Unknown Faces section
        if (!unnamed.empty()) {
            y = addSectionHeader(format("Unknown Faces ({})", unnamed.size()), y, contentWidth);
            y = addCards(unnamed, y, cols, prevSelectedClusterId);
            y += PADDING;
        }

        // Set content height for scrolling
        cardContent_->setSize(cardListWidth, y);
        if (cardScroll_) cardScroll_->updateScrollBounds();

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

    float addSectionHeader(const string& text, float y, float width) {
        auto header = make_shared<SectionHeader>();
        header->text = text;
        header->fontRef = &fontLarge_;
        header->setPos(PADDING, y);
        header->setSize(width, SECTION_HEADER_HEIGHT);
        cardContent_->addChild(header);
        return y + SECTION_HEADER_HEIGHT + CARD_SPACING;
    }

    float addCards(const vector<const PhotoProvider::FaceCluster*>& items,
                   float startY, int cols, int selectedClusterId = INT_MIN) {
        float y = startY;
        int col = 0;

        for (auto* clusterPtr : items) {
            float x = PADDING + col * (CARD_WIDTH + CARD_SPACING);

            auto card = make_shared<PersonCard>();
            card->cluster = *clusterPtr;
            card->fontRef = &font_;
            card->fontSmallRef = &fontSmall_;

            // Texture reference
            auto texIt = textures_.find(clusterPtr->repPhotoId);
            card->textureRef = (texIt != textures_.end() && texIt->second.isAllocated())
                ? &texIt->second : nullptr;

            card->onCardClick = [this](PersonCard* c) {
                // Defer to update() to avoid node destruction during callback
                pendingClickCard_ = c;
            };

            card->setPos(x, y);
            card->setSize(CARD_WIDTH, CARD_HEIGHT);
            cardContent_->addChild(card);
            allCards_.push_back(card.get());

            // Restore selection by cluster ID
            if (selectedClusterId != INT_MIN && clusterPtr->clusterId == selectedClusterId) {
                card->selected = true;
                selectedCard_ = card.get();
            }

            col++;
            if (col >= cols) {
                col = 0;
                y += CARD_HEIGHT + CARD_SPACING;
            }
        }

        // Advance y if last row was partial
        if (col > 0) y += CARD_HEIGHT + CARD_SPACING;
        return y;
    }

    // =========================================================================
    // Gallery
    // =========================================================================

    void showGallery(PersonCard* card) {
        // Select this card
        if (selectedCard_) selectedCard_->selected = false;
        card->selected = true;
        selectedCard_ = card;

        galleryVisible_ = true;

        // Rebuild card list with new widths
        needsRebuild_ = true;

        // Build gallery content
        rebuildGallery(card->cluster);

        if (onRedraw) onRedraw();
    }

    void hideGallery() {
        if (selectedCard_) selectedCard_->selected = false;
        selectedCard_ = nullptr;
        galleryVisible_ = false;
        galleryScroll_->setPos(-9999, -9999);
        galleryScroll_->setSize(0, 0);
        galleryContent_->removeAllChildren();
        galleryCrops_.clear();

        // Rebuild card list at full width
        needsRebuild_ = true;
        if (onRedraw) onRedraw();
    }

    void rebuildGallery(const PhotoProvider::FaceCluster& cluster) {
        galleryContent_->removeAllChildren();
        galleryCrops_.clear();

        if (!provider_) {
            logWarning() << "[PeopleView] rebuildGallery: no provider!";
            return;
        }

        // Clean up textures not needed by card list (free pool slots)
        cleanupUnusedTextures();

        // Get face details (limited to MAX_GALLERY_FACES)
        int totalFaces = (int)cluster.faceIds.size();
        vector<int> limitedIds;
        if (totalFaces > MAX_GALLERY_FACES) {
            limitedIds.assign(cluster.faceIds.begin(),
                              cluster.faceIds.begin() + MAX_GALLERY_FACES);
        } else {
            limitedIds = cluster.faceIds;
        }
        auto briefs = provider_->getFaceBriefs(limitedIds);

        float galleryWidth = getWidth() * 0.65f;
        float y = PADDING;

        // Header
        string headerText;
        if (cluster.name.empty()) {
            headerText = format("Cluster ({} faces)", totalFaces);
        } else {
            headerText = format("{} ({} faces)", cluster.name, totalFaces);
        }
        if (totalFaces > MAX_GALLERY_FACES) {
            headerText += format("  showing first {}", MAX_GALLERY_FACES);
        }

        auto header = make_shared<GalleryHeader>();
        header->text = headerText;
        header->fontRef = &fontLarge_;
        header->setPos(PADDING, y);
        header->setSize(galleryWidth - PADDING * 2, SECTION_HEADER_HEIGHT);
        galleryContent_->addChild(header);
        y += SECTION_HEADER_HEIGHT + CROP_SPACING;

        // Face crop grid
        float contentWidth = galleryWidth - PADDING * 2;
        int cols = max(1, (int)((contentWidth + CROP_SPACING) / (CROP_SIZE + CROP_SPACING)));
        int col = 0;

        // Queue missing thumbnails for gallery
        unordered_set<string> neededIds;
        for (auto& fb : briefs) {
            neededIds.insert(fb.photoId);
        }
        queueMissingThumbnails(neededIds);

        for (auto& fb : briefs) {
            float x = PADDING + col * (CROP_SIZE + CROP_SPACING);

            auto crop = make_shared<FaceCropNode>();
            crop->photoId = fb.photoId;
            crop->faceX = fb.x;
            crop->faceY = fb.y;
            crop->faceW = fb.w;
            crop->faceH = fb.h;

            // Texture reference
            auto texIt = textures_.find(fb.photoId);
            crop->textureRef = (texIt != textures_.end() && texIt->second.isAllocated())
                ? &texIt->second : nullptr;

            crop->setPos(x, y);
            crop->setSize(CROP_SIZE, CROP_SIZE);
            galleryContent_->addChild(crop);
            galleryCrops_.push_back(crop);

            col++;
            if (col >= cols) {
                col = 0;
                y += CROP_SIZE + CROP_SPACING;
            }
        }

        if (col > 0) y += CROP_SIZE + CROP_SPACING;
        y += PADDING;

        galleryContent_->setSize(galleryWidth, y);
        if (galleryScroll_) galleryScroll_->updateScrollBounds();

    }

    void updateGalleryTextures() {
        for (auto& crop : galleryCrops_) {
            auto it = textures_.find(crop->photoId);
            crop->textureRef = (it != textures_.end() && it->second.isAllocated())
                ? &it->second : nullptr;
        }
    }

    // =========================================================================
    // Name edit handling
    // =========================================================================

    void handleNameConfirm(const string& name) {
        if (!editingCard_ || !provider_) return;

        if (editingCard_->cluster.personId > 0) {
            // Rename existing person
            provider_->renamePerson(editingCard_->cluster.personId, name);
        } else {
            // Assign name to unnamed cluster
            provider_->assignNameToCluster(editingCard_->cluster, name);
        }

        hideNameOverlay();

        // Rebuild clusters
        clusters_ = provider_->buildFaceClusters();
        galleryVisible_ = false;
        galleryScroll_->setPos(-9999, -9999);
        galleryScroll_->setSize(0, 0);
        galleryContent_->removeAllChildren();
        galleryCrops_.clear();
        selectedCard_ = nullptr;
        rebuildUI();
    }

    void hideNameOverlay() {
        if (nameOverlay_) nameOverlay_->hide();
        editingCard_ = nullptr;
        if (onRedraw) onRedraw();
    }

    // =========================================================================
    // Thumbnail loading
    // =========================================================================

    // Remove textures not used by current card list or gallery
    void cleanupUnusedTextures() {
        unordered_set<string> needed;
        for (auto& c : clusters_) {
            needed.insert(c.repPhotoId);
        }
        for (auto& crop : galleryCrops_) {
            needed.insert(crop->photoId);
        }
        for (auto it = textures_.begin(); it != textures_.end(); ) {
            if (needed.count(it->first) == 0) {
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void queueMissingThumbnails(const unordered_set<string>& neededIds) {
        for (const auto& id : neededIds) {
            if (textures_.count(id) == 0) {
                // Avoid duplicate entries in pendingLoads_
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
