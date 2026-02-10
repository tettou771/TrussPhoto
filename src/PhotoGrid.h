#pragma once

// =============================================================================
// PhotoGrid.h - Scrollable grid of photo items with async loading
// =============================================================================

#include <TrussC.h>
#include "PhotoProvider.h"
#include "PhotoItem.h"
#include "AsyncImageLoader.h"
using namespace std;
using namespace tc;

// PhotoGrid - displays photos in a scrollable grid
class PhotoGrid : public RectNode {
public:
    using Ptr = shared_ptr<PhotoGrid>;

    // Callbacks
    function<void(int)> onItemClick;           // normal click → full view
    function<void(vector<string>)> onDeleteRequest;  // delete selected photos

    PhotoGrid() {
        // Create scroll container
        scrollContainer_ = make_shared<ScrollContainer>();
        addChild(scrollContainer_);

        // Create content node for grid items
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        // Create scroll bar
        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);

        // Start async loader
        loader_.start();
    }

    ~PhotoGrid() {
        loader_.stop();
    }

    // Set grid parameters
    void setItemSize(float size) {
        itemSize_ = size;
        updateGridLayout();
    }

    void setSpacing(float spacing) {
        spacing_ = spacing;
        updateGridLayout();
    }

    void setPadding(float padding) {
        padding_ = padding;
        updateGridLayout();
    }

    // Populate grid from PhotoProvider
    void populate(PhotoProvider& provider) {
        provider_ = &provider;

        // Set thumbnail loader to use provider
        loader_.setThumbnailLoader([&provider](const string& photoId, Pixels& outPixels) {
            return provider.getThumbnail(photoId, outPixels);
        });

        // Clear existing items
        content_->removeAllChildren();
        items_.clear();
        photoIds_.clear();

        // Get sorted photo IDs
        auto ids = provider.getSortedIds();

        for (size_t i = 0; i < ids.size(); i++) {
            auto* photo = provider.getPhoto(ids[i]);
            if (!photo) continue;

            photoIds_.push_back(ids[i]);

            auto item = make_shared<PhotoItem>(i, itemSize_);
            // Label: stem of filename
            string stem = fs::path(photo->filename).stem().string();
            item->setLabelText(stem);
            item->setSyncState(photo->syncState);

            // Connect click event
            item->onClick = [this, i]() {
                if (onItemClick) {
                    onItemClick((int)i);
                }
            };

            // Connect load/unload requests
            item->onRequestLoad = [this](int idx) {
                requestLoad(idx);
            };

            item->onRequestUnload = [this](int idx) {
                loader_.cancelRequest(idx);
            };

            items_.push_back(item);
            content_->addChild(item);
        }

        updateGridLayout();
        updateVisibility();
    }

    // Get photo ID at grid index
    const string& getPhotoId(int index) const {
        return photoIds_[index];
    }

    // Get number of photo IDs
    size_t getPhotoIdCount() const { return photoIds_.size(); }

    // Update sync state badges from provider, returns true if any changed
    bool updateSyncStates(PhotoProvider& provider) {
        bool changed = false;
        for (size_t i = 0; i < items_.size() && i < photoIds_.size(); i++) {
            auto* photo = provider.getPhoto(photoIds_[i]);
            if (photo && items_[i]->getSyncState() != photo->syncState) {
                items_[i]->setSyncState(photo->syncState);
                changed = true;
            }
        }
        return changed;
    }

    // --- Selection management ---

    void toggleSelection(int index) {
        if (index < 0 || index >= (int)items_.size()) return;
        bool sel = !items_[index]->isSelected();
        items_[index]->setSelected(sel);
        selectionAnchor_ = index;
        redraw();
    }

    void selectAll() {
        for (auto& item : items_) item->setSelected(true);
        redraw();
    }

    void selectRange(int from, int to, bool select = true) {
        int lo = min(from, to), hi = max(from, to);
        for (int i = lo; i <= hi && i < (int)items_.size(); i++) {
            items_[i]->setSelected(select);
        }
        redraw();
    }

    int getSelectionAnchor() const { return selectionAnchor_; }

    bool isSelected(int index) const {
        if (index < 0 || index >= (int)items_.size()) return false;
        return items_[index]->isSelected();
    }

    void clearSelection() {
        for (auto& item : items_) {
            item->setSelected(false);
        }
        redraw();
    }

    bool hasSelection() const {
        for (auto& item : items_) {
            if (item->isSelected()) return true;
        }
        return false;
    }

    // Get selected photo IDs
    vector<string> getSelectedIds() const {
        vector<string> ids;
        for (size_t i = 0; i < items_.size() && i < photoIds_.size(); i++) {
            if (items_[i]->isSelected()) {
                ids.push_back(photoIds_[i]);
            }
        }
        return ids;
    }

    int getSelectionCount() const {
        int count = 0;
        for (auto& item : items_) {
            if (item->isSelected()) count++;
        }
        return count;
    }

    // Get item count
    size_t getItemCount() const { return items_.size(); }

    // Get item at index
    PhotoItem::Ptr getItem(size_t index) {
        return index < items_.size() ? items_[index] : nullptr;
    }

    // Size changed
    void setSize(float w, float h) override {
        RectNode::setSize(w, h);

        // Update scroll container size
        scrollContainer_->setRect(0, 0, w, h);

        updateGridLayout();
    }

    void draw() override {
        // Background
        setColor(0.08f, 0.08f, 0.1f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());
    }

    void update() override {
        scrollContainer_->updateScrollBounds();
        scrollBar_->updateFromContainer();

        // Process async load results
        processLoadResults();

        // Update item visibility based on scroll position
        updateVisibility();
    }

private:
    ScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;
    vector<PhotoItem::Ptr> items_;
    PhotoProvider* provider_ = nullptr;
    vector<string> photoIds_;

    AsyncImageLoader loader_;

    float itemSize_ = 140;
    float spacing_ = 10;
    float padding_ = 10;
    float lastScrollY_ = -1;
    int selectionAnchor_ = -1;

    void requestLoad(int index) {
        if (!provider_ || index < 0 || index >= (int)photoIds_.size()) return;
        loader_.requestLoad(index, photoIds_[index]);
    }

    void processLoadResults() {
        LoadResult result;
        while (loader_.tryGetResult(result)) {
            if (result.success && result.id >= 0 && result.id < (int)items_.size()) {
                auto& item = items_[result.id];
                // Only set if item is still active (visible) and waiting for load
                if (item->getActive() && item->getLoadState() == LoadState::Loading) {
                    item->setPixels(std::move(result.pixels));
                }
            }
        }
    }

    void updateVisibility() {
        if (items_.empty()) return;

        float scrollY = scrollContainer_->getScrollY();

        // Only update if scroll changed significantly
        if (abs(scrollY - lastScrollY_) < 1.0f) return;
        lastScrollY_ = scrollY;

        float viewTop = scrollY;
        float viewBottom = scrollY + getHeight();

        // Add margin for preloading
        float margin = getHeight() * 0.5f;
        float loadTop = viewTop - margin;
        float loadBottom = viewBottom + margin;

        for (auto& item : items_) {
            float itemY = item->getY();
            float itemBottom = itemY + item->getHeight();

            bool shouldBeActive = (itemBottom >= loadTop && itemY <= loadBottom);

            if (shouldBeActive != item->getActive()) {
                item->setActive(shouldBeActive);
            }
        }
    }

    void updateGridLayout() {
        if (items_.empty()) return;

        float contentWidth = getWidth() - 20;  // Reserve space for scrollbar
        if (contentWidth <= 0) return;

        // Calculate columns based on available width
        float totalItemWidth = itemSize_ + spacing_;
        int columns = max(1, (int)((contentWidth - padding_ * 2 + spacing_) / totalItemWidth));

        // Item size with label
        float itemHeight = itemSize_ + 24;  // thumbnail + label

        // Position items in grid
        for (size_t i = 0; i < items_.size(); i++) {
            int col = i % columns;
            int row = i / columns;

            float x = padding_ + col * (itemSize_ + spacing_);
            float y = padding_ + row * (itemHeight + spacing_);

            items_[i]->setPos(x, y);
        }

        // Update content size
        int rows = (items_.size() + columns - 1) / columns;
        float contentHeight = padding_ * 2 + rows * (itemHeight + spacing_) - spacing_;
        content_->setSize(contentWidth, contentHeight);

        scrollContainer_->updateScrollBounds();

        // Reset scroll tracking to force visibility update
        lastScrollY_ = -1;
    }
};
