#pragma once

// =============================================================================
// PhotoGrid.h - Virtualized scrollable grid (RecyclerView pattern)
// =============================================================================

#include <TrussC.h>
#include "PhotoProvider.h"
#include "PhotoItem.h"
#include "AsyncImageLoader.h"
#include "RecyclerGrid.h"
#include "FolderTree.h"  // for loadJapaneseFont
using namespace std;
using namespace tc;

// PhotoGrid - displays photos in a virtualized scrollable grid
class PhotoGrid : public RecyclerGrid<PhotoItem> {
public:
    using Ptr = shared_ptr<PhotoGrid>;

    // Callbacks
    function<void(int)> onItemClick;           // normal click -> full view
    function<void(vector<string>)> onDeleteRequest;  // delete selected photos
    function<void(const string&)> onCompanionClick;  // companion photo id -> full view

    PhotoGrid() {
        itemWidth_ = 140;
        itemHeight_ = 140 + 24;  // thumbnail + label
        spacing_ = 10;
        padding_ = 10;

        // Font for labels
        loadJapaneseFont(labelFont_, 12);

        // Start async loader
        loader_.start();
    }

    ~PhotoGrid() {
        loader_.stop();
    }

    // --- Grid parameters ---

    void setItemSize(float size) {
        itemSize_ = size;
        itemWidth_ = size;
        itemHeight_ = size + 24;
        rebuild();
    }

    void setSpacing(float sp) {
        spacing_ = sp;
        rebuild();
    }

    void setPadding(float pad) {
        padding_ = pad;
        rebuild();
    }

    // --- Filters ---

    void setFilterPath(const string& path) { filterPath_ = path; }
    const string& getFilterPath() const { return filterPath_; }

    void setTextFilter(const string& query) { textFilter_ = query; }
    const string& getTextFilter() const { return textFilter_; }

    void setClipResults(const vector<PhotoProvider::SearchResult>& results) {
        clipResults_ = results;
    }
    void clearClipResults() { clipResults_.clear(); }

    void setTextMatchIds(const unordered_set<string>& ids) { textMatchIds_ = ids; }
    void clearTextMatchIds() { textMatchIds_.clear(); }

    void setFilterPhotoIds(const unordered_set<string>& ids) { filterPhotoIds_ = ids; }
    void clearFilterPhotoIds() { filterPhotoIds_.clear(); }
    bool hasFilterPhotoIds() const { return !filterPhotoIds_.empty(); }

    // --- Populate ---

    void populate(PhotoProvider& provider) {
        provider_ = &provider;

        loader_.setThumbnailLoader([&provider](const string& photoId, Pixels& outPixels) {
            return provider.getThumbnail(photoId, outPixels);
        });

        // Clear old state
        selectionSet_.clear();
        photoIds_.clear();
        vector<string> ids;
        if (!clipResults_.empty()) {
            for (const auto& r : clipResults_) ids.push_back(r.photoId);
        } else {
            ids = provider.getSortedIds();
        }

        for (size_t i = 0; i < ids.size(); i++) {
            auto* photo = provider.getPhoto(ids[i]);
            if (!photo) continue;

            // Filter by explicit photo ID set (e.g. from People view)
            if (!filterPhotoIds_.empty() && filterPhotoIds_.count(ids[i]) == 0) continue;

            // Filter by folder path
            if (!filterPath_.empty()) {
                string dir = fs::path(photo->localPath).parent_path().string();
                if (dir.substr(0, filterPath_.size()) != filterPath_) continue;
            }

            // Filter by text query (only when no CLIP results)
            if (clipResults_.empty() && !textFilter_.empty()
                && !matchesTextFilter(*photo, textFilter_, provider.getPersonNames(ids[i]))) continue;

            photoIds_.push_back(ids[i]);
        }

        // Reset scroll
        resetScroll();
        rebuild();
    }

    // --- Data access ---

    const string& getPhotoId(int index) const { return photoIds_[index]; }
    size_t getPhotoIdCount() const { return photoIds_.size(); }
    size_t getItemCount() const { return photoIds_.size(); }

    // Update sync state badges (only for currently bound pool items)
    bool updateSyncStates(PhotoProvider& provider) {
        bool changed = false;
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            if (dataIdx < 0 || dataIdx >= (int)photoIds_.size()) continue;
            auto* photo = provider.getPhoto(photoIds_[dataIdx]);
            if (photo && pool_[poolIdx]->getSyncState() != photo->syncState) {
                pool_[poolIdx]->setSyncState(photo->syncState);
                changed = true;
            }
        }
        return changed;
    }

    // --- Selection management ---

    void toggleSelection(int index) {
        if (index < 0 || index >= (int)photoIds_.size()) return;
        if (selectionSet_.count(index)) {
            selectionSet_.erase(index);
        } else {
            selectionSet_.insert(index);
        }
        selectionAnchor_ = index;
        auto it = poolMap_.find(index);
        if (it != poolMap_.end()) {
            pool_[it->second]->setSelected(selectionSet_.count(index));
        }
        redraw();
    }

    void selectAll() {
        for (int i = 0; i < (int)photoIds_.size(); i++) {
            selectionSet_.insert(i);
        }
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            pool_[poolIdx]->setSelected(true);
        }
        redraw();
    }

    void selectRange(int from, int to, bool select = true) {
        int lo = min(from, to), hi = max(from, to);
        for (int i = lo; i <= hi && i < (int)photoIds_.size(); i++) {
            if (select) selectionSet_.insert(i);
            else selectionSet_.erase(i);
        }
        for (int i = lo; i <= hi && i < (int)photoIds_.size(); i++) {
            auto it = poolMap_.find(i);
            if (it != poolMap_.end()) {
                pool_[it->second]->setSelected(select);
            }
        }
        redraw();
    }

    int getSelectionAnchor() const { return selectionAnchor_; }

    bool isSelected(int index) const {
        return selectionSet_.count(index) > 0;
    }

    void clearSelection() {
        for (int idx : selectionSet_) {
            auto it = poolMap_.find(idx);
            if (it != poolMap_.end()) {
                pool_[it->second]->setSelected(false);
            }
        }
        selectionSet_.clear();
        redraw();
    }

    bool hasSelection() const { return !selectionSet_.empty(); }

    vector<string> getSelectedIds() const {
        vector<string> ids;
        for (int idx : selectionSet_) {
            if (idx >= 0 && idx < (int)photoIds_.size()) {
                ids.push_back(photoIds_[idx]);
            }
        }
        return ids;
    }

    int getSelectionCount() const { return (int)selectionSet_.size(); }

    // --- Drawing ---

    void draw() override {
        setColor(0.08f, 0.08f, 0.1f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());
    }

protected:
    // === RecyclerGrid overrides ===

    int getDataCount() const override { return (int)photoIds_.size(); }

    ItemPtr createPoolItem(int poolIdx) override {
        auto item = make_shared<PhotoItem>(-1, itemSize_);

        item->onClick = [this, poolIdx]() {
            int dataIdx = reverseMap_[poolIdx];
            if (dataIdx >= 0 && onItemClick) {
                onItemClick(dataIdx);
            }
        };

        item->onRequestLoad = [this](int idx) {
            requestLoad(idx);
        };
        item->onRequestUnload = [this](int idx) {
            loader_.cancelRequest(idx);
        };

        return item;
    }

    void onBind(int dataIdx, ItemPtr& item) override {
        if (!provider_) return;
        auto* photo = provider_->getPhoto(photoIds_[dataIdx]);
        string stem = photo ? fs::path(photo->filename).stem().string() : "???";
        SyncState sync = photo ? photo->syncState : SyncState::LocalOnly;
        bool selected = selectionSet_.count(dataIdx) > 0;

        bool clipMatch = !clipResults_.empty() && !textMatchIds_.count(photoIds_[dataIdx]);
        item->setClipMatch(clipMatch);

        bool video = photo ? photo->isVideo : false;
        int stackSize = provider_->getStackSize(photoIds_[dataIdx]);
        item->rebindAndLoad(dataIdx, stem, sync, selected, video, &labelFont_, stackSize);
    }

    void onUnbind(int dataIdx, ItemPtr& item) override {
        loader_.cancelRequest(dataIdx);
        item->unloadImage();
    }

    void onSetup() override {
        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);
    }

    void onUpdate() override {
        scrollBar_->updateFromContainer();
        processLoadResults();
        updateStackHover();
    }

    void endDraw() override {
        drawCompanionPreview();
    }

    bool onMousePress(Vec2 local, int button) override {
        // Check companion preview click
        if (companionVisible_ && button == 0 && !companionId_.empty()) {
            if (companionRect_.contains(local.x, local.y)) {
                if (onCompanionClick) {
                    onCompanionClick(companionId_);
                }
                return true;
            }
        }
        return RecyclerGrid<PhotoItem>::onMousePress(local, button);
    }

    Vec2 getItemPosition(int dataIdx) override {
        int col = dataIdx % columns_;
        int row = dataIdx / columns_;
        float x = padding_ + col * (itemSize_ + spacing_);
        float y = padding_ + row * rowHeight_;
        return {x, y};
    }

private:
    // --- Data ---
    PhotoProvider* provider_ = nullptr;
    vector<string> photoIds_;
    string filterPath_;
    string textFilter_;
    vector<PhotoProvider::SearchResult> clipResults_;
    unordered_set<string> textMatchIds_;
    unordered_set<string> filterPhotoIds_;

    // --- Selection ---
    unordered_set<int> selectionSet_;
    int selectionAnchor_ = -1;

    // --- Loader ---
    AsyncImageLoader loader_;
    Font labelFont_;
    ScrollBar::Ptr scrollBar_;

    // --- Grid params ---
    float itemSize_ = 140;

    // --- Stack hover preview ---
    int hoverDataIdx_ = -1;          // data index currently hovered on badge
    uint64_t hoverTimerId_ = 0;      // callAfter timer id
    bool companionVisible_ = false;  // is companion preview showing
    int companionDataIdx_ = -1;      // data index of the hovered primary
    string companionId_;             // photo id of companion to show
    Texture companionTexture_;       // loaded companion thumbnail
    bool companionLoading_ = false;
    Rect companionRect_;             // screen rect for click detection

    // =========================================================================
    // Stack hover management
    // =========================================================================

    void updateStackHover() {
        if (!provider_) return;

        // Find which pool item the mouse is over
        int hoveredIdx = -1;
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            auto& item = pool_[poolIdx];
            if (!item->getActive()) continue;
            if (!item->isStacked()) continue;
            if (!item->isMouseOver()) continue;

            // Check if mouse is over the badge area
            Vec2 mouseLocal(item->getMouseX(), item->getMouseY());
            if (item->isOverStackBadge(mouseLocal)) {
                hoveredIdx = dataIdx;
            }
            break;
        }

        if (hoveredIdx != hoverDataIdx_) {
            // Hover target changed
            if (hoverTimerId_ != 0) {
                cancelTimer(hoverTimerId_);
                hoverTimerId_ = 0;
            }
            companionVisible_ = false;
            companionTexture_.clear();
            companionLoading_ = false;

            hoverDataIdx_ = hoveredIdx;

            if (hoveredIdx >= 0) {
                // Start delay timer
                int idx = hoveredIdx;
                hoverTimerId_ = callAfter(0.5, [this, idx]() {
                    hoverTimerId_ = 0;
                    showCompanionPreview(idx);
                });
            }
            redraw();
        }
    }

    void showCompanionPreview(int dataIdx) {
        if (!provider_ || dataIdx < 0 || dataIdx >= (int)photoIds_.size()) return;

        auto companions = provider_->getStackCompanions(photoIds_[dataIdx]);
        if (companions.empty()) return;

        companionDataIdx_ = dataIdx;
        companionId_ = companions[0]; // show first companion
        companionVisible_ = true;

        // Load companion thumbnail in background
        companionLoading_ = true;
        loader_.requestLoad(-1000 - dataIdx, companionId_);
        redraw();
    }

    void drawCompanionPreview() {
        if (!companionVisible_ || companionDataIdx_ < 0) {
            companionRect_ = {0, 0, 0, 0};
            return;
        }

        auto it = poolMap_.find(companionDataIdx_);
        if (it == poolMap_.end()) return;

        auto& item = pool_[it->second];
        if (!item->getActive()) return;

        // Position: offset from the primary item, bottom-right
        Vec2 itemContentPos = getItemPosition(companionDataIdx_);
        float scrollY = scrollContainer_->getScrollY();

        float previewSize = itemSize_ * 0.7f;
        float ox = itemContentPos.x + item->getWidth() * 0.5f;
        float oy = itemContentPos.y + item->getHeight() * 0.5f - scrollY;

        // Clamp to grid bounds
        if (ox + previewSize > getWidth()) ox = getWidth() - previewSize - 4;
        if (oy + previewSize > getHeight()) oy = getHeight() - previewSize - 4;

        // Record rect for click detection
        companionRect_ = {ox, oy, previewSize, previewSize};

        // Shadow
        setColor(0.0f, 0.0f, 0.0f, 0.4f);
        fill();
        drawRect(ox + 2, oy + 2, previewSize, previewSize);

        // Background
        setColor(0.15f, 0.15f, 0.18f);
        fill();
        drawRect(ox, oy, previewSize, previewSize);

        // Draw companion thumbnail
        if (companionTexture_.isAllocated()) {
            setColor(1.0f, 1.0f, 1.0f);
            float tw = companionTexture_.getWidth();
            float th = companionTexture_.getHeight();
            float scale = min(previewSize / tw, previewSize / th);
            float dw = tw * scale, dh = th * scale;
            float dx = ox + (previewSize - dw) / 2;
            float dy = oy + (previewSize - dh) / 2;
            companionTexture_.draw(dx, dy, dw, dh);
        } else {
            // Loading placeholder
            setColor(0.3f, 0.3f, 0.35f);
            fill();
            drawRect(ox + 4, oy + 4, previewSize - 8, previewSize - 8);
        }

        // Border
        setColor(0.5f, 0.5f, 0.55f);
        noFill();
        drawRect(ox, oy, previewSize, previewSize);

        // Label
        auto* photo = provider_->getPhoto(companionId_);
        if (photo) {
            string ext = fs::path(photo->filename).extension().string();
            transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            setColor(0.8f, 0.8f, 0.85f);
            fill();
            pushStyle();
            setTextAlign(Direction::Center, Direction::Top);
            drawBitmapString(ext, ox + previewSize / 2, oy + previewSize + 2);
            popStyle();
        }
    }

    // =========================================================================
    // Load management
    // =========================================================================

    void requestLoad(int index) {
        if (!provider_ || index < 0 || index >= (int)photoIds_.size()) return;
        loader_.requestLoad(index, photoIds_[index]);
    }

    void processLoadResults() {
        LoadResult result;
        while (loader_.tryGetResult(result)) {
            if (!result.success) continue;

            // Companion preview load result (negative IDs)
            if (result.id < -999) {
                if (companionLoading_ && companionVisible_) {
                    companionTexture_.allocate(result.pixels, TextureUsage::Immutable);
                    companionLoading_ = false;
                    redraw();
                }
                continue;
            }

            auto it = poolMap_.find(result.id);
            if (it == poolMap_.end()) continue;

            auto& item = pool_[it->second];
            if (item->getActive() && item->getLoadState() == LoadState::Loading) {
                item->setPixels(std::move(result.pixels));
            }
        }
    }

    // =========================================================================
    // Text filter
    // =========================================================================

    static bool matchesTextFilter(const PhotoEntry& photo, const string& query,
                                   const vector<string>* personNames = nullptr) {
        string lq = query;
        transform(lq.begin(), lq.end(), lq.begin(), ::tolower);

        auto contains = [&lq](const string& field) {
            if (field.empty()) return false;
            string lf = field;
            transform(lf.begin(), lf.end(), lf.begin(), ::tolower);
            return lf.find(lq) != string::npos;
        };

        if (contains(fs::path(photo.filename).stem().string())) return true;
        if (contains(photo.camera)) return true;
        if (contains(photo.cameraMake)) return true;
        if (contains(photo.lens)) return true;
        if (contains(photo.lensMake)) return true;
        if (contains(photo.memo)) return true;
        if (contains(photo.colorLabel)) return true;
        if (contains(photo.creativeStyle)) return true;
        if (contains(photo.dateTimeOriginal)) return true;

        if (!photo.tags.empty()) {
            if (contains(photo.tags)) return true;
        }

        if (personNames) {
            for (const auto& name : *personNames) {
                if (contains(name)) return true;
            }
        }

        return false;
    }
};
