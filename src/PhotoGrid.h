#pragma once

// =============================================================================
// PhotoGrid.h - Virtualized scrollable grid (RecyclerView pattern)
// =============================================================================

#include <TrussC.h>
#include "PhotoProvider.h"
#include "PhotoItem.h"
#include "AsyncImageLoader.h"
#include "FolderTree.h"  // for loadJapaneseFont
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

// PhotoGrid - displays photos in a virtualized scrollable grid
// Only a small pool of PhotoItem nodes exists; items are recycled as the user scrolls.
class PhotoGrid : public RectNode {
public:
    using Ptr = shared_ptr<PhotoGrid>;

    // Callbacks
    function<void(int)> onItemClick;           // normal click → full view
    function<void(vector<string>)> onDeleteRequest;  // delete selected photos

    PhotoGrid() {
        scrollContainer_ = make_shared<PlainScrollContainer>();
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);

        // Font for labels
        loadJapaneseFont(labelFont_, 12);

        // Start async loader
        loader_.start();
    }

    void setup() override {
        addChild(scrollContainer_);
    }

    ~PhotoGrid() {
        loader_.stop();
    }

    // --- Grid parameters ---

    void setItemSize(float size) {
        itemSize_ = size;
        recalcLayout();
        rebuildPool();
    }

    void setSpacing(float spacing) {
        spacing_ = spacing;
        recalcLayout();
        rebuildPool();
    }

    void setPadding(float padding) {
        padding_ = padding;
        recalcLayout();
        rebuildPool();
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

            // Filter by folder path
            if (!filterPath_.empty()) {
                string dir = fs::path(photo->localPath).parent_path().string();
                if (dir.substr(0, filterPath_.size()) != filterPath_) continue;
            }

            // Filter by text query (only when no CLIP results)
            if (clipResults_.empty() && !textFilter_.empty()
                && !matchesTextFilter(*photo, textFilter_)) continue;

            photoIds_.push_back(ids[i]);
        }

        // Reset scroll
        scrollContainer_->setScrollY(0);

        recalcLayout();
        rebuildPool();
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

    // --- Selection management (externalized to selectionSet_) ---

    void toggleSelection(int index) {
        if (index < 0 || index >= (int)photoIds_.size()) return;
        if (selectionSet_.count(index)) {
            selectionSet_.erase(index);
        } else {
            selectionSet_.insert(index);
        }
        selectionAnchor_ = index;
        // Update visual if bound
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
        // Update all bound items
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
        // Update bound items in range
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
        // Update visuals for bound items
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

    // --- Size ---

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

    void draw() override {
        setColor(0.08f, 0.08f, 0.1f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());
    }

    void update() override {
        scrollContainer_->updateScrollBounds();
        scrollBar_->updateFromContainer();

        processLoadResults();
        updateVisibleRange();
    }

private:
    // --- UI nodes ---
    ScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;

    // --- Data ---
    PhotoProvider* provider_ = nullptr;
    vector<string> photoIds_;
    string filterPath_;
    string textFilter_;
    vector<PhotoProvider::SearchResult> clipResults_;

    // --- Pool ---
    vector<PhotoItem::Ptr> pool_;
    unordered_map<int, int> poolMap_;   // dataIndex → poolIndex
    vector<int> reverseMap_;             // poolIndex → dataIndex (-1 = free)
    vector<int> freeList_;

    // --- Layout cache ---
    int columns_ = 0;
    float rowHeight_ = 0;     // itemSize_ + label + spacing
    float itemHeight_ = 0;    // itemSize_ + label (without spacing)
    int totalRows_ = 0;
    int visibleRowFirst_ = -1;
    int visibleRowLast_ = -1;

    // --- Selection (externalized) ---
    unordered_set<int> selectionSet_;
    int selectionAnchor_ = -1;

    // --- Loader ---
    AsyncImageLoader loader_;
    Font labelFont_;

    // --- Grid params ---
    float itemSize_ = 140;
    float spacing_ = 10;
    float padding_ = 10;
    float lastScrollY_ = -99999;

    // =========================================================================
    // Layout
    // =========================================================================

    void recalcLayout() {
        float contentWidth = getWidth() - 20; // scrollbar space
        if (contentWidth <= 0) { columns_ = 1; return; }

        float totalItemWidth = itemSize_ + spacing_;
        columns_ = max(1, (int)((contentWidth - padding_ * 2 + spacing_) / totalItemWidth));

        itemHeight_ = itemSize_ + 24; // thumbnail + label
        rowHeight_ = itemHeight_ + spacing_;
        totalRows_ = photoIds_.empty() ? 0
            : ((int)photoIds_.size() + columns_ - 1) / columns_;

        // Set content height for scroll
        float contentHeight = totalRows_ > 0
            ? padding_ * 2 + totalRows_ * rowHeight_ - spacing_
            : 0;
        content_->setSize(contentWidth, contentHeight);
        scrollContainer_->updateScrollBounds();

        // Force visibility re-evaluation
        lastScrollY_ = -99999;
    }

    // Returns {firstRow, lastRow} inclusive, with buffer rows
    pair<int, int> calcVisibleRows(float scrollY) {
        if (totalRows_ == 0) return {0, -1};

        float viewTop = scrollY;
        float viewBottom = scrollY + getHeight();

        int firstRow = max(0, (int)((viewTop - padding_) / rowHeight_) - 2);
        int lastRow = min(totalRows_ - 1, (int)((viewBottom - padding_) / rowHeight_) + 2);

        return {firstRow, lastRow};
    }

    int calcPoolSize() {
        if (totalRows_ == 0 || columns_ == 0) return 0;
        int visibleRows = (int)(getHeight() / rowHeight_) + 1;
        int bufferedRows = visibleRows + 4; // 2 rows buffer each side
        return min(bufferedRows * columns_, (int)photoIds_.size());
    }

    // =========================================================================
    // Pool management
    // =========================================================================

    void rebuildPool() {
        // Unbind all existing
        unbindAll();

        // Remove old pool from content
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
            auto item = make_shared<PhotoItem>(-1, itemSize_);
            item->setActive(false);

            // onClick resolves data index dynamically via reverseMap_
            int poolIdx = i;
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

            pool_.push_back(item);
            freeList_.push_back(i);
            content_->addChild(item);
        }

        // Initial bind
        visibleRowFirst_ = -1;
        visibleRowLast_ = -1;
        lastScrollY_ = -99999;
        updateVisibleRange();
    }

    void unbindAll() {
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            pool_[poolIdx]->setActive(false);
            pool_[poolIdx]->unloadImage();
            reverseMap_[poolIdx] = -1;
            freeList_.push_back(poolIdx);
        }
        poolMap_.clear();
        visibleRowFirst_ = -1;
        visibleRowLast_ = -1;
    }

    // =========================================================================
    // Recycle logic
    // =========================================================================

    void updateVisibleRange() {
        if (pool_.empty() || photoIds_.empty()) return;

        float scrollY = scrollContainer_->getScrollY();

        // Only update if scroll changed
        if (abs(scrollY - lastScrollY_) < 0.5f) return;
        lastScrollY_ = scrollY;

        auto [newFirst, newLast] = calcVisibleRows(scrollY);
        if (newFirst == visibleRowFirst_ && newLast == visibleRowLast_) return;

        // Unbind items that are no longer in visible range
        vector<int> toUnbind;
        for (auto& [dataIdx, poolIdx] : poolMap_) {
            int row = dataIdx / columns_;
            if (row < newFirst || row > newLast) {
                toUnbind.push_back(dataIdx);
            }
        }
        for (int dataIdx : toUnbind) {
            unbindDataIndex(dataIdx);
        }

        // Bind new items that entered visible range
        int startIdx = newFirst * columns_;
        int endIdx = min((newLast + 1) * columns_, (int)photoIds_.size());
        for (int dataIdx = startIdx; dataIdx < endIdx; dataIdx++) {
            if (poolMap_.count(dataIdx) == 0) {
                bindDataIndex(dataIdx);
            }
        }

        visibleRowFirst_ = newFirst;
        visibleRowLast_ = newLast;
    }

    void bindDataIndex(int dataIdx) {
        if (freeList_.empty() || !provider_) return;
        if (dataIdx < 0 || dataIdx >= (int)photoIds_.size()) return;

        int poolIdx = freeList_.back();
        freeList_.pop_back();

        poolMap_[dataIdx] = poolIdx;
        reverseMap_[poolIdx] = dataIdx;

        auto& item = pool_[poolIdx];

        // Calculate position
        int col = dataIdx % columns_;
        int row = dataIdx / columns_;
        float x = padding_ + col * (itemSize_ + spacing_);
        float y = padding_ + row * rowHeight_;
        item->setPos(x, y);

        // Get photo data
        auto* photo = provider_->getPhoto(photoIds_[dataIdx]);
        string stem = photo ? fs::path(photo->filename).stem().string() : "???";
        SyncState sync = photo ? photo->syncState : SyncState::LocalOnly;
        bool selected = selectionSet_.count(dataIdx) > 0;

        item->setActive(true);
        item->rebindAndLoad(dataIdx, stem, sync, selected, &labelFont_);
    }

    void unbindDataIndex(int dataIdx) {
        auto it = poolMap_.find(dataIdx);
        if (it == poolMap_.end()) return;

        int poolIdx = it->second;
        auto& item = pool_[poolIdx];

        // Cancel any pending load
        loader_.cancelRequest(dataIdx);

        item->setActive(false);
        item->unloadImage();

        reverseMap_[poolIdx] = -1;
        freeList_.push_back(poolIdx);
        poolMap_.erase(it);
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

            // Find pool item for this data index
            auto it = poolMap_.find(result.id);
            if (it == poolMap_.end()) continue;

            auto& item = pool_[it->second];
            if (item->getActive() && item->getLoadState() == LoadState::Loading) {
                item->setPixels(std::move(result.pixels));
            }
        }
    }

    // =========================================================================
    // Text filter (unchanged)
    // =========================================================================

    static bool matchesTextFilter(const PhotoEntry& photo, const string& query) {
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

        return false;
    }
};
