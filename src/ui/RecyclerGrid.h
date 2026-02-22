#pragma once

// =============================================================================
// RecyclerGrid.h - Virtual scroll grid base class (RecyclerView pattern)
// Only a small pool of item nodes exists; items are recycled as the user scrolls.
// =============================================================================

#include <TrussC.h>
using namespace std;
using namespace tc;

// ScrollContainer without default background/border (parent draws its own)
#ifndef PLAIN_SCROLL_CONTAINER_DEFINED
#define PLAIN_SCROLL_CONTAINER_DEFINED
class PlainScrollContainer : public ScrollContainer {
public:
    using Ptr = shared_ptr<PlainScrollContainer>;
    void draw() override {}
};
#endif

template <typename T>
class RecyclerGrid : public RectNode {
public:
    using ItemPtr = shared_ptr<T>;

    RecyclerGrid() {
        // Create scroll infrastructure in constructor (not setup!)
        // because setSize() may be called before setup() in TrussC.
        // setup() is called on first updateTree(), but setSize() can be
        // called immediately after addChild().
        scrollContainer_ = make_shared<PlainScrollContainer>();
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);
    }

    void setup() override {
        // addChild requires shared_ptr to be complete (weak_from_this),
        // so we do it here, not in the constructor.
        addChild(scrollContainer_);
        onSetup();
    }

    void update() override {
        scrollContainer_->updateScrollBounds();
        onUpdate();
        updateVisibleRange();
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        scrollContainer_->setRect(0, 0, w, h);

        int oldColumns = columns_;
        recalcLayout();
        if (columns_ != oldColumns) {
            rebuildPool();
        } else {
            updateVisibleRange();
        }
    }

    // Rebuild everything (call when data changes)
    void rebuild() {
        recalcLayout();
        rebuildPool();
    }

    // Reset scroll position to top
    void resetScroll() {
        scrollContainer_->setScrollY(0);
        lastScrollY_ = -99999;
    }

    // Unbind all items (release to free list)
    void unbindAll() {
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            onUnbind(dataIdx, pool_[poolIdx]);
            pool_[poolIdx]->setActive(false);
            reverseMap_[poolIdx] = -1;
            freeList_.push_back(poolIdx);
        }
        poolMap_.clear();
    }

    // Access pool mapping (for iterating bound items externally)
    const unordered_map<int, int>& getPoolMap() const { return poolMap_; }
    const vector<ItemPtr>& getPool() const { return pool_; }
    const vector<int>& getReverseMap() const { return reverseMap_; }

    PlainScrollContainer::Ptr getScrollContainer() { return scrollContainer_; }
    RectNode::Ptr getContent() { return content_; }

protected:
    // === Pure virtual (subclass must implement) ===
    virtual int getDataCount() const = 0;
    virtual ItemPtr createPoolItem(int poolIdx) = 0;
    virtual void onBind(int dataIdx, ItemPtr& item) = 0;
    virtual void onUnbind(int dataIdx, ItemPtr& item) = 0;

    // === Layout hooks (default: uniform grid, override for sections) ===
    virtual int calcColumns() {
        float contentWidth = getWidth() - scrollBarWidth_;
        if (contentWidth <= 0) return 1;
        float totalItemWidth = itemWidth_ + spacing_;
        return max(1, (int)((contentWidth - padding_ * 2 + spacing_) / totalItemWidth));
    }

    virtual float calcRowHeight() {
        return itemHeight_ + spacing_;
    }

    virtual float calcContentHeight() {
        if (totalRows_ == 0) return 0;
        return padding_ * 2 + totalRows_ * rowHeight_ - spacing_;
    }

    virtual int calcPoolSize() {
        if (totalRows_ == 0 || columns_ == 0) return 0;
        int visibleRows = (int)(getHeight() / rowHeight_) + 1;
        int bufferedRows = visibleRows + 4;
        return min(bufferedRows * columns_, getDataCount());
    }

    virtual Vec2 getItemPosition(int dataIdx) {
        int col = dataIdx % columns_;
        int row = dataIdx / columns_;
        float x = padding_ + col * (itemWidth_ + spacing_);
        float y = padding_ + row * rowHeight_;
        return {x, y};
    }

    // Returns {first, end} half-open range [first, end)
    virtual pair<int, int> calcVisibleDataRange(float scrollY) {
        if (totalRows_ == 0) return {0, 0};
        float viewTop = scrollY;
        float viewBottom = scrollY + getHeight();
        int firstRow = max(0, (int)((viewTop - padding_) / rowHeight_) - 2);
        int lastRow = min(totalRows_ - 1, (int)((viewBottom - padding_) / rowHeight_) + 2);
        int startIdx = firstRow * columns_;
        int endIdx = min((lastRow + 1) * columns_, getDataCount());
        return {startIdx, endIdx};
    }

    // === Subclass hooks ===
    virtual void onSetup() {}
    virtual void onUpdate() {}
    virtual void onPoolRebuilt() {}

    // === Core methods ===

    void recalcLayout() {
        columns_ = calcColumns();
        rowHeight_ = calcRowHeight();
        int dataCount = getDataCount();
        totalRows_ = (dataCount == 0) ? 0
            : (dataCount + columns_ - 1) / columns_;

        float contentWidth = getWidth() - scrollBarWidth_;
        float contentHeight = calcContentHeight();
        content_->setSize(contentWidth, contentHeight);
        scrollContainer_->updateScrollBounds();

        lastScrollY_ = -99999;
    }

    void rebuildPool() {
        unbindAll();

        // Remove old pool items from content (individually, not removeAllChildren)
        for (auto& item : pool_) {
            content_->removeChild(item);
        }
        pool_.clear();
        poolMap_.clear();
        reverseMap_.clear();
        freeList_.clear();

        int poolSize = calcPoolSize();
        if (poolSize == 0) return;

        pool_.reserve(poolSize);
        reverseMap_.resize(poolSize, -1);

        for (int i = 0; i < poolSize; i++) {
            auto item = createPoolItem(i);
            item->setActive(false);
            pool_.push_back(item);
            freeList_.push_back(i);
            content_->addChild(item);
        }

        lastScrollY_ = -99999;
        onPoolRebuilt();
        updateVisibleRange();
    }

    void updateVisibleRange() {
        if (pool_.empty() || getDataCount() == 0) return;

        float scrollY = scrollContainer_->getScrollY();
        if (abs(scrollY - lastScrollY_) < 0.5f) return;
        lastScrollY_ = scrollY;

        auto [newStart, newEnd] = calcVisibleDataRange(scrollY);

        // Unbind items outside visible range
        vector<int> toUnbind;
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            if (dataIdx < newStart || dataIdx >= newEnd)
                toUnbind.push_back(dataIdx);
        }
        for (int idx : toUnbind) unbindDataIndex(idx);

        // Bind items in visible range that aren't yet bound
        for (int idx = newStart; idx < newEnd; idx++) {
            if (poolMap_.count(idx) == 0) bindDataIndex(idx);
        }
    }

    void bindDataIndex(int dataIdx) {
        if (freeList_.empty()) return;
        if (dataIdx < 0 || dataIdx >= getDataCount()) return;

        int poolIdx = freeList_.back();
        freeList_.pop_back();

        poolMap_[dataIdx] = poolIdx;
        reverseMap_[poolIdx] = dataIdx;

        auto& item = pool_[poolIdx];
        Vec2 pos = getItemPosition(dataIdx);
        item->setPos(pos.x, pos.y);
        item->setActive(true);

        onBind(dataIdx, item);
    }

    void unbindDataIndex(int dataIdx) {
        auto it = poolMap_.find(dataIdx);
        if (it == poolMap_.end()) return;

        int poolIdx = it->second;
        auto& item = pool_[poolIdx];

        onUnbind(dataIdx, item);
        item->setActive(false);

        reverseMap_[poolIdx] = -1;
        freeList_.push_back(poolIdx);
        poolMap_.erase(it);
    }

    // === Member variables ===
    PlainScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;

    vector<ItemPtr> pool_;
    unordered_map<int, int> poolMap_;   // dataIdx -> poolIdx
    vector<int> reverseMap_;             // poolIdx -> dataIdx (-1 = free)
    vector<int> freeList_;

    float itemWidth_ = 140, itemHeight_ = 140;
    float spacing_ = 10, padding_ = 10;
    float scrollBarWidth_ = 20;
    int columns_ = 0;
    float rowHeight_ = 0;
    int totalRows_ = 0;
    float lastScrollY_ = -99999;
};
