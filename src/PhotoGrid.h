#pragma once

// =============================================================================
// PhotoGrid.h - Scrollable grid of photo items
// =============================================================================

#include <TrussC.h>
#include "PhotoLibrary.h"
#include "PhotoItem.h"
using namespace std;
using namespace tc;

// PhotoGrid - displays photos in a scrollable grid
class PhotoGrid : public RectNode {
public:
    using Ptr = shared_ptr<PhotoGrid>;

    // Item click callback (passes entry index)
    function<void(int)> onItemClick;

    PhotoGrid() {
        // Don't enableEvents() here - let children handle events

        // Create scroll container
        scrollContainer_ = make_shared<ScrollContainer>();
        addChild(scrollContainer_);

        // Create content node for grid items
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        // Create scroll bar
        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);
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

    // Populate grid from library
    void populate(PhotoLibrary& library) {
        // Clear existing items
        content_->removeAllChildren();
        items_.clear();

        // Create items for each entry
        for (size_t i = 0; i < library.getCount(); i++) {
            auto& entry = library.getEntry(i);

            auto item = make_shared<PhotoItem>(i, itemSize_);
            item->setLabelText(entry.getStem());

            // Connect click event
            item->onClick = [this, i]() {
                if (onItemClick) {
                    onItemClick((int)i);
                }
            };

            items_.push_back(item);
            content_->addChild(item);
        }

        updateGridLayout();
    }

    // Load thumbnails (for now, synchronous)
    void loadThumbnails(PhotoLibrary& library, size_t startIndex = 0, size_t count = 0) {
        if (count == 0) count = library.getCount();

        size_t endIndex = min(startIndex + count, library.getCount());
        for (size_t i = startIndex; i < endIndex; i++) {
            if (i < items_.size()) {
                auto& entry = library.getEntry(i);
                if (!entry.loaded) {
                    items_[i]->loadImage(entry.path);
                    entry.loaded = true;
                }
            }
        }
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
    }

private:
    ScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;
    vector<PhotoItem::Ptr> items_;

    float itemSize_ = 140;
    float spacing_ = 10;
    float padding_ = 10;

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
    }
};
