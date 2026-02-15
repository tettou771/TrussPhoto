#pragma once

// =============================================================================
// PeopleView.h - People view with face clusters (Lightroom-style)
// Displays named persons and unnamed face clusters with thumbnail cards.
// =============================================================================

#include <TrussC.h>
#include <tcxIME.h>
#include "PhotoProvider.h"
#include "FolderTree.h"  // for loadJapaneseFont, PlainScrollContainer

using namespace std;
using namespace tc;

class PeopleView : public RectNode {
public:
    using Ptr = shared_ptr<PeopleView>;

    // Callbacks
    function<void(const PhotoProvider::FaceCluster& cluster)> onPersonClick;
    function<void()> onRedraw;

    void setup() override {
        enableEvents();
        setClipping(true);
        loadJapaneseFont(font_, 14);
        loadJapaneseFont(fontSmall_, 11);
        loadJapaneseFont(fontLarge_, 16);

        scrollContainer_ = make_shared<PlainScrollContainer>();
        addChild(scrollContainer_);
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

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

    void populate(PhotoProvider& provider) {
        provider_ = &provider;
        clusters_ = provider.buildFaceClusters();
        editingCard_ = nullptr;
        selectedCards_.clear();
        needsRebuild_ = true;  // defer until draw when we have correct size
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

        if (anyNew) {
            // Update texture references on cards
            for (auto* card : allCards_) {
                auto it = textures_.find(card->cluster.repPhotoId);
                card->textureRef = (it != textures_.end() && it->second.isAllocated())
                    ? &it->second : nullptr;
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

        // Sync scroll container size with view size
        if (scrollContainer_) {
            scrollContainer_->setSize(w, h);
        }

        // Deferred rebuild (need correct size from setRect)
        if (needsRebuild_ && w > 0 && h > 0) {
            needsRebuild_ = false;
            rebuildUI();
        }

        // Background
        setColor(0.06f, 0.06f, 0.08f);
        fill();
        drawRect(0, 0, w, h);

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
        if (loadThread_.joinable()) loadThread_.join();
        content_->removeAllChildren();
        allCards_.clear();
        selectedCards_.clear();
        textures_.clear();
        pendingLoads_.clear();
        clusters_.clear();
        editingCard_ = nullptr;
    }

    bool onKeyPress(int key) override {
        // ESC: cancel name edit or close
        if (key == 256 /* ESCAPE */) {
            if (nameOverlay_ && nameOverlay_->getActive()) {
                hideNameOverlay();
                return true;
            }
        }
        return false;
    }

private:
    PhotoProvider* provider_ = nullptr;
    vector<PhotoProvider::FaceCluster> clusters_;
    bool needsRebuild_ = false;

    // UI components
    PlainScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    Font font_, fontSmall_, fontLarge_;

    // Layout constants
    static constexpr float CARD_WIDTH = 180.0f;
    static constexpr float CARD_HEIGHT = 72.0f;
    static constexpr float CARD_SPACING = 8.0f;
    static constexpr float SECTION_HEADER_HEIGHT = 32.0f;
    static constexpr float PADDING = 16.0f;

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

        // Callback: (card, isDoubleClick)
        function<void(PersonCard*, bool)> onClicked;

        void setup() override {
            enableEvents();
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
                // Crop face region from thumbnail
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

            // Name and count
            float textX = thumbX + thumbSize + 8;
            float textY = 16;

            if (!cluster.name.empty()) {
                setColor(0.9f, 0.9f, 0.95f);
                if (fontRef) fontRef->drawString(cluster.name, textX, textY,
                    Direction::Left, Direction::Center);
            } else if (!cluster.suggestedName.empty()) {
                setColor(0.6f, 0.6f, 0.7f);
                if (fontRef) fontRef->drawString(cluster.suggestedName + "?",
                    textX, textY, Direction::Left, Direction::Center);
            } else {
                setColor(0.5f, 0.5f, 0.55f);
                if (fontRef) fontRef->drawString("Unknown", textX, textY,
                    Direction::Left, Direction::Center);
            }

            // Photo count
            string countStr = format("{} photos", cluster.photoCount);
            setColor(0.45f, 0.45f, 0.5f);
            if (fontSmallRef) fontSmallRef->drawString(countStr, textX, textY + 18,
                Direction::Left, Direction::Center);

            // Face count
            string faceStr = format("{} faces", (int)cluster.faceIds.size());
            if (fontSmallRef) fontSmallRef->drawString(faceStr, textX, textY + 32,
                Direction::Left, Direction::Center);
        }

        bool onMousePress(Vec2 pos, int button) override {
            (void)pos;
            if (button != 0) return false;
            auto now = chrono::steady_clock::now();
            bool isDouble = (chrono::duration_cast<chrono::milliseconds>(
                now - lastPress_).count() < 400);
            lastPress_ = now;
            if (onClicked) onClicked(this, isDouble);
            return true;
        }

    private:
        chrono::steady_clock::time_point lastPress_;
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
    unordered_set<PersonCard*> selectedCards_;
    PersonCard* editingCard_ = nullptr;
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
    // UI rebuild
    // =========================================================================

    void rebuildUI() {
        content_->removeAllChildren();
        allCards_.clear();

        float y = PADDING;
        float viewWidth = getWidth() > 0 ? getWidth() : 800;
        float contentWidth = viewWidth - PADDING * 2;
        int cols = max(1, (int)((contentWidth + CARD_SPACING) / (CARD_WIDTH + CARD_SPACING)));

        // Separate named and unnamed clusters
        vector<const PhotoProvider::FaceCluster*> named, unnamed;
        for (auto& c : clusters_) {
            if (c.personId > 0) named.push_back(&c);
            else unnamed.push_back(&c);
        }

        // Sort by photo count descending (already sorted from provider)

        // Known People section
        if (!named.empty()) {
            y = addSectionHeader(format("Known People ({})", named.size()), y, contentWidth);
            y = addCards(named, y, cols);
            y += PADDING;
        }

        // Unknown Faces section
        if (!unnamed.empty()) {
            y = addSectionHeader(format("Unknown Faces ({})", unnamed.size()), y, contentWidth);
            y = addCards(unnamed, y, cols);
            y += PADDING;
        }

        // Set content height for scrolling
        content_->setSize(viewWidth, y);

        // Queue thumbnail loads
        unordered_set<string> neededIds;
        for (auto& c : clusters_) {
            neededIds.insert(c.repPhotoId);
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
        content_->addChild(header);
        return y + SECTION_HEADER_HEIGHT + CARD_SPACING;
    }

    float addCards(const vector<const PhotoProvider::FaceCluster*>& items,
                   float startY, int cols) {
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

            card->onClicked = [this](PersonCard* c, bool isDouble) {
                handleCardClick(c, isDouble);
            };

            card->setPos(x, y);
            card->setSize(CARD_WIDTH, CARD_HEIGHT);
            content_->addChild(card);
            allCards_.push_back(card.get());

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
    // Click handling
    // =========================================================================

    void handleCardClick(PersonCard* card, bool isDouble) {
        if (isDouble) {
            // Double-click: edit name
            editingCard_ = card;
            string initial = card->cluster.name;
            string placeholder = card->cluster.suggestedName;
            if (nameOverlay_) {
                nameOverlay_->setPos(0, 0);
                nameOverlay_->setSize(getWidth(), getHeight());
                nameOverlay_->show(initial, placeholder);
            }
            if (onRedraw) onRedraw();
        } else {
            // Single click: navigate to person's photos
            if (onPersonClick) onPersonClick(card->cluster);
        }
    }

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
        loadThread_.detach();
    }
};
