#pragma once

// =============================================================================
// PhotoStrip.h - Horizontal scrolling photo strip with virtual recycling
// =============================================================================

#include <TrussC.h>
#include "PhotoItem.h"   // for ThumbnailNode
#include "PhotoProvider.h"
#include "AsyncImageLoader.h"
#include "FolderTree.h"  // for PlainScrollContainer, loadJapaneseFont
#include "Constants.h"   // for SELECTION_COLOR
#include <set>
using namespace std;
using namespace tc;

// StripItem - single thumbnail cell with GPS/selection border
class StripItem : public RectNode {
public:
    using Ptr = shared_ptr<StripItem>;

    function<void()> onClick;
    function<void(float globalX, float globalY)> onDragMove;
    function<void(float globalX, float globalY)> onDragEnd;

    StripItem(float size) : size_(size) {
        enableEvents();
        setSize(size, size);

        thumb_ = make_shared<ThumbnailNode>();
        thumb_->setSize(size - 4, size - 4);  // inset for border
        thumb_->setPos(2, 2);
        addChild(thumb_);
    }

    void setSelected(bool v) { selected_ = v; }
    bool isSelected() const { return selected_; }

    void setHasGps(bool v) { hasGps_ = v; }
    bool hasGps() const { return hasGps_; }

    void setHasProvisionalGeotag(bool v) { hasProvisionalGeotag_ = v; }
    bool hasProvisionalGeotag() const { return hasProvisionalGeotag_; }

    ThumbnailNode* getThumbnail() { return thumb_.get(); }

    void setPixels(Pixels&& pix) {
        thumb_->setPixels(std::move(pix));
        loaded_ = true;
    }

    void clearImage() {
        thumb_->clearImage();
        loaded_ = false;
    }

    bool isLoaded() const { return loaded_; }

    void draw() override {
        // Selection border (bright orange, thick)
        if (selected_) {
            setColor(SEL_R, SEL_G, SEL_B);
            fill();
            drawRect(0, 0, size_, size_);
        }
    }

    void endDraw() override {
        if (!hasGps_) {
            float iconSize = min(16.0f, size_ * 0.25f);
            float cx = 2 + iconSize * 0.5f + 2;
            float cy = 2 + iconSize * 0.5f + 2;
            float r = iconSize * 0.5f;

            if (hasProvisionalGeotag_) {
                // Provisional geotag: light blue dot
                setColor(0.0f, 0.0f, 0.0f, 0.5f);
                fill();
                drawRect(cx - r - 2, cy - r - 2, iconSize + 4, iconSize + 4);
                setColor(0.3f, 0.65f, 1.0f);
                fill();
                drawCircle(cx, cy, r * 0.7f);
            } else {
                // No-GPS indicator: circle with diagonal line
                setColor(0.0f, 0.0f, 0.0f, 0.5f);
                fill();
                drawRect(cx - r - 2, cy - r - 2, iconSize + 4, iconSize + 4);
                setColor(0.7f, 0.7f, 0.72f);
                noFill();
                setStrokeWeight(1.5f);
                drawCircle(cx, cy, r);
                drawLine(cx - r * 0.7f, cy - r * 0.7f,
                         cx + r * 0.7f, cy + r * 0.7f);
                fill();
            }
        }
        RectNode::endDraw();
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        if (button != 0) return RectNode::onMousePress(local, button);
        if (!hasGps_ && !hasProvisionalGeotag_) {
            // GPS-less photo: start drag detection, select immediately
            dragPending_ = true;
            isDragging_ = false;
            dragStartPos_ = local;
            if (onClick) onClick();  // select on press
        }
        return true;
    }

    bool onMouseDrag(Vec2 local, int button) override {
        if (button != 0 || !dragPending_) return false;
        float dx = local.x - dragStartPos_.x;
        float dy = local.y - dragStartPos_.y;
        if (!isDragging_ && (dx * dx + dy * dy) > 25.0f) {  // > 5px
            isDragging_ = true;
        }
        if (isDragging_ && onDragMove) {
            float gx, gy;
            localToGlobal(local.x, local.y, gx, gy);
            onDragMove(gx, gy);
        }
        return isDragging_;
    }

    bool onMouseRelease(Vec2 local, int button) override {
        if (button != 0) return false;
        if (isDragging_ && onDragEnd) {
            float gx, gy;
            localToGlobal(local.x, local.y, gx, gy);
            onDragEnd(gx, gy);
        } else if (!isDragging_ && dragPending_) {
            // Already selected on press, no extra action needed
        } else if (!isDragging_ && onClick) {
            onClick();  // normal click (GPS photo)
        }
        dragPending_ = false;
        isDragging_ = false;
        return true;
    }

private:
    ThumbnailNode::Ptr thumb_;
    float size_;
    bool selected_ = false;
    bool hasGps_ = true;
    bool hasProvisionalGeotag_ = false;
    bool loaded_ = false;

    // Drag state
    bool dragPending_ = false;
    bool isDragging_ = false;
    Vec2 dragStartPos_;
};

// PhotoStrip - horizontal scrolling strip with pool-based recycling
class PhotoStrip : public RectNode {
public:
    using Ptr = shared_ptr<PhotoStrip>;

    function<void(int stripIndex, const string& photoId)> onPhotoClick;
    function<void(const set<int>& selectedIndices)> onSelectionChanged;
    function<void(int stripIndex, const string& photoId, float globalX, float globalY)> onDragMove;
    function<void(int stripIndex, const string& photoId, float globalX, float globalY)> onDragEnd;

    // Modifier key refs (set by MapView from tcApp)
    bool* cmdDownRef = nullptr;
    bool* shiftDownRef = nullptr;

    PhotoStrip() {
        loadJapaneseFont(font_, 10);
        loader_.start();

        scrollContainer_ = make_shared<PlainScrollContainer>();
        scrollContainer_->setVerticalScrollEnabled(false);
        scrollContainer_->setHorizontalScrollEnabled(true);
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);
    }

    ~PhotoStrip() {
        loader_.stop();
    }

    void setup() override {
        setClipping(true);
        addChild(scrollContainer_);
    }

    // Set photo data and rebuild
    void setPhotos(const vector<string>& photoIds, const vector<bool>& hasGps,
                   PhotoProvider& provider) {
        photoIds_ = photoIds;
        hasGps_ = hasGps;
        provider_ = &provider;

        loader_.setThumbnailLoader([&provider](const string& photoId, Pixels& outPixels) {
            return provider.getThumbnail(photoId, outPixels);
        });

        selectedIndices_.clear();
        lastClickIdx_ = -1;
        rebuildPool();
    }

    // Update provisional geotag indicators
    void setProvisionalGeotag(const string& photoId, bool has) {
        for (size_t i = 0; i < photoIds_.size(); i++) {
            if (photoIds_[i] == photoId) {
                auto it = poolMap_.find((int)i);
                if (it != poolMap_.end()) {
                    pool_[it->second]->setHasProvisionalGeotag(has);
                }
                break;
            }
        }
    }

    // Mark a photo as now having GPS (update data + visible pool item)
    void setHasGps(const string& photoId, bool v) {
        for (size_t i = 0; i < photoIds_.size(); i++) {
            if (photoIds_[i] == photoId) {
                if (i < hasGps_.size()) hasGps_[i] = v;
                auto it = poolMap_.find((int)i);
                if (it != poolMap_.end()) pool_[it->second]->setHasGps(v);
                break;
            }
        }
    }

    void clearAllProvisionalGeotags() {
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            pool_[poolIdx]->setHasProvisionalGeotag(false);
        }
    }

    // Select a single photo and scroll to it (used for external selection, e.g. pin click)
    void selectPhoto(const string& photoId) {
        int idx = -1;
        for (size_t i = 0; i < photoIds_.size(); i++) {
            if (photoIds_[i] == photoId) { idx = (int)i; break; }
        }
        if (idx < 0) return;
        clearSelectionVisuals();
        selectedIndices_.clear();
        selectedIndices_.insert(idx);
        lastClickIdx_ = idx;
        applySelectionVisuals();
        scrollToIndex(idx);
        redraw();
    }

    // Multi-selection by click with modifiers
    void handleSelection(int idx, bool cmd, bool shift) {
        if (shift && lastClickIdx_ >= 0) {
            // Range select
            int lo = min(lastClickIdx_, idx);
            int hi = max(lastClickIdx_, idx);
            if (!cmd) {
                clearSelectionVisuals();
                selectedIndices_.clear();
            }
            for (int i = lo; i <= hi; i++) selectedIndices_.insert(i);
        } else if (cmd) {
            // Toggle
            if (selectedIndices_.count(idx)) selectedIndices_.erase(idx);
            else selectedIndices_.insert(idx);
            lastClickIdx_ = idx;
        } else {
            // Single
            clearSelectionVisuals();
            selectedIndices_.clear();
            selectedIndices_.insert(idx);
            lastClickIdx_ = idx;
        }
        applySelectionVisuals();
        if (onSelectionChanged) onSelectionChanged(selectedIndices_);
        redraw();
    }

    const set<int>& selectedIndices() const { return selectedIndices_; }

    int selectedIndex() const {
        if (selectedIndices_.empty()) return -1;
        return *selectedIndices_.begin();
    }

    string selectedPhotoId() const {
        int idx = selectedIndex();
        if (idx >= 0 && idx < (int)photoIds_.size())
            return photoIds_[idx];
        return "";
    }

    // Get all selected photo IDs
    vector<string> selectedPhotoIds() const {
        vector<string> result;
        for (int idx : selectedIndices_) {
            if (idx >= 0 && idx < (int)photoIds_.size())
                result.push_back(photoIds_[idx]);
        }
        return result;
    }

    void update() override {
        scrollContainer_->updateScrollBounds();
        processLoadResults();
        updateVisibleRange();
    }

    void draw() override {
        // Background
        setColor(0.1f, 0.1f, 0.12f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        scrollContainer_->setRect(0, 0, w, h);

        float newItemSize = calcItemSize();
        if (!pool_.empty() && abs(newItemSize - itemSize_) > 0.5f) {
            // Item size changed — rebuild pool with correct sizes
            rebuildPool();
        } else {
            recalcLayout();
            updateVisibleRange();
        }
    }

    void shutdown() {
        loader_.stop();
    }

private:
    static constexpr float PADDING = 4.0f;
    static constexpr float SPACING = 3.0f;

    // Data
    vector<string> photoIds_;
    vector<bool> hasGps_;
    PhotoProvider* provider_ = nullptr;
    set<int> selectedIndices_;
    int lastClickIdx_ = -1;  // anchor for shift-range selection

    // Scroll
    PlainScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;

    // Pool
    vector<StripItem::Ptr> pool_;
    unordered_map<int, int> poolMap_;   // dataIdx -> poolIdx
    vector<int> reverseMap_;            // poolIdx -> dataIdx
    vector<int> freeList_;

    // Layout
    float itemSize_ = 0;
    float lastScrollX_ = -99999;

    // Loader
    AsyncImageLoader loader_;
    Font font_;

    float calcItemSize() const {
        return getHeight() - PADDING * 2;
    }

    float calcContentWidth() const {
        int n = (int)photoIds_.size();
        if (n == 0) return 0;
        return PADDING * 2 + n * itemSize_ + (n - 1) * SPACING;
    }

    float itemX(int idx) const {
        return PADDING + idx * (itemSize_ + SPACING);
    }

    void recalcLayout() {
        itemSize_ = calcItemSize();
        if (itemSize_ <= 0) return;

        float cw = calcContentWidth();
        content_->setSize(cw, getHeight());
        scrollContainer_->updateScrollBounds();
        lastScrollX_ = -99999;
    }

    int calcPoolSize() const {
        if (itemSize_ <= 0 || photoIds_.empty()) return 0;
        int visible = (int)(getWidth() / (itemSize_ + SPACING)) + 1;
        int buffered = visible + 4;
        return min(buffered, (int)photoIds_.size());
    }

    pair<int, int> calcVisibleRange(float scrollX) const {
        if (photoIds_.empty() || itemSize_ <= 0) return {0, 0};
        float viewLeft = scrollX;
        float viewRight = scrollX + getWidth();
        int first = max(0, (int)((viewLeft - PADDING) / (itemSize_ + SPACING)) - 2);
        int last = min((int)photoIds_.size() - 1,
                       (int)((viewRight - PADDING) / (itemSize_ + SPACING)) + 2);
        return {first, last + 1};
    }

    void rebuildPool() {
        // Unbind all
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            loader_.cancelRequest(dataIdx);
            pool_[poolIdx]->clearImage();
            pool_[poolIdx]->setActive(false);
            reverseMap_[poolIdx] = -1;
            freeList_.push_back(poolIdx);
        }
        poolMap_.clear();

        // Remove old pool
        for (auto& item : pool_) {
            content_->removeChild(item);
        }
        pool_.clear();
        reverseMap_.clear();
        freeList_.clear();

        recalcLayout();

        int poolSize = calcPoolSize();
        if (poolSize == 0) return;

        pool_.reserve(poolSize);
        reverseMap_.resize(poolSize, -1);

        for (int i = 0; i < poolSize; i++) {
            float sz = max(1.0f, itemSize_);
            auto item = make_shared<StripItem>(sz);
            item->setActive(false);

            int poolIdx = i;
            item->onClick = [this, poolIdx]() {
                int dataIdx = reverseMap_[poolIdx];
                if (dataIdx < 0) return;

                bool cmd = cmdDownRef && *cmdDownRef;
                bool shift = shiftDownRef && *shiftDownRef;
                handleSelection(dataIdx, cmd, shift);

                if (onPhotoClick) onPhotoClick(dataIdx, photoIds_[dataIdx]);
            };

            item->onDragMove = [this, poolIdx](float gx, float gy) {
                int dataIdx = reverseMap_[poolIdx];
                if (dataIdx < 0) return;
                if (onDragMove) onDragMove(dataIdx, photoIds_[dataIdx], gx, gy);
            };

            item->onDragEnd = [this, poolIdx](float gx, float gy) {
                int dataIdx = reverseMap_[poolIdx];
                if (dataIdx < 0) return;
                if (onDragEnd) onDragEnd(dataIdx, photoIds_[dataIdx], gx, gy);
            };

            pool_.push_back(item);
            freeList_.push_back(i);
            content_->addChild(item);
        }

        lastScrollX_ = -99999;
        updateVisibleRange();
    }

    void updateVisibleRange() {
        if (pool_.empty() || photoIds_.empty()) return;

        float scrollX = scrollContainer_->getScrollX();
        if (abs(scrollX - lastScrollX_) < 0.5f) return;
        lastScrollX_ = scrollX;

        auto [newStart, newEnd] = calcVisibleRange(scrollX);

        // Unbind out-of-range items
        vector<int> toUnbind;
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            if (dataIdx < newStart || dataIdx >= newEnd)
                toUnbind.push_back(dataIdx);
        }
        for (int idx : toUnbind) unbindItem(idx);

        // Bind in-range items
        for (int idx = newStart; idx < newEnd; idx++) {
            if (poolMap_.count(idx) == 0) bindItem(idx);
        }
    }

    void bindItem(int dataIdx) {
        if (freeList_.empty()) return;
        if (dataIdx < 0 || dataIdx >= (int)photoIds_.size()) return;

        int poolIdx = freeList_.back();
        freeList_.pop_back();

        poolMap_[dataIdx] = poolIdx;
        reverseMap_[poolIdx] = dataIdx;

        auto& item = pool_[poolIdx];
        item->setPos(itemX(dataIdx), PADDING);
        item->setActive(true);

        // Set GPS and selection state
        item->setHasGps(dataIdx < (int)hasGps_.size() && hasGps_[dataIdx]);
        item->setSelected(selectedIndices_.count(dataIdx) > 0);

        // Request thumbnail load
        item->clearImage();
        loader_.requestLoad(dataIdx, photoIds_[dataIdx]);
    }

    void unbindItem(int dataIdx) {
        auto it = poolMap_.find(dataIdx);
        if (it == poolMap_.end()) return;

        int poolIdx = it->second;
        auto& item = pool_[poolIdx];

        loader_.cancelRequest(dataIdx);
        item->clearImage();
        item->setActive(false);

        reverseMap_[poolIdx] = -1;
        freeList_.push_back(poolIdx);
        poolMap_.erase(it);
    }

    void processLoadResults() {
        LoadResult result;
        bool any = false;
        while (loader_.tryGetResult(result)) {
            if (!result.success) continue;
            auto it = poolMap_.find(result.id);
            if (it == poolMap_.end()) continue;

            auto& item = pool_[it->second];
            if (item->getActive()) {
                item->setPixels(std::move(result.pixels));
                any = true;
            }
        }
        if (any) redraw();
    }

    void scrollToIndex(int idx) {
        if (itemSize_ <= 0) return;
        float targetX = itemX(idx) + itemSize_ / 2.0f - getWidth() / 2.0f;
        float maxScroll = scrollContainer_->getMaxScrollX();
        targetX = clamp(targetX, 0.0f, maxScroll);
        scrollContainer_->setScrollX(targetX);
        lastScrollX_ = -99999;
    }

    // Clear selected visual state for all visible pool items
    void clearSelectionVisuals() {
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            pool_[poolIdx]->setSelected(false);
        }
    }

    // Apply selected visual state based on selectedIndices_
    void applySelectionVisuals() {
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            pool_[poolIdx]->setSelected(selectedIndices_.count(dataIdx) > 0);
        }
    }
};
