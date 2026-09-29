#pragma once

// =============================================================================
// PhotoGrid.h - Virtualized scrollable grid (RecyclerView pattern)
// =============================================================================

#include <TrussC.h>
#include "PhotoProvider.h"
#include "PhotoItem.h"
#include "pipeline/AsyncImageLoader.h"
#include "RecyclerGrid.h"
#include "ContextMenu.h"
#include "FolderTree.h"  // for loadJapaneseFont
using namespace std;
using namespace tc;

// PhotoGrid - displays photos in a virtualized scrollable grid
class PhotoGrid : public RecyclerGrid<PhotoItem> {
public:
    using Ptr = shared_ptr<PhotoGrid>;

    // Events
    Event<int> itemClicked;
    Event<vector<string>> deleteRequested;
    Event<ContextMenu::Ptr> contextMenuRequested;
    Event<void> repairRequested;
    Event<void> consolidateRequested;
    Event<string> updateThumbnailRequested;
    Event<void> populated;   // fired after populate(): indices may have shifted

    PhotoGrid() {
        itemWidth_ = 140;
        itemHeight_ = 140 + 24;  // thumbnail + label
        spacing_ = 10;
        padding_ = 10;

        // Font for labels
        loadJapaneseFont(labelFont_, 12);

        // Font for memo-card bodies (wrapped, Japanese line-break rules)
        loadJapaneseFont(cardFont_, 11);
        cardFont_.enableWrap(true);
        cardFont_.setKinsoku(KinsokuLevel::Standard);

        // Hover bubble text, wrapped to the bubble's inner width
        loadJapaneseFont(bubbleFont_, 12);
        bubbleFont_.enableWrap(true);
        bubbleFont_.setKinsoku(KinsokuLevel::Standard);
        bubbleFont_.setMaxLineLength(PhotoItem::BUBBLE_W - PhotoItem::BUBBLE_PAD * 2);

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

    // Reload a specific photo's thumbnail (after developed thumbnail generation)
    void reloadItemThumbnail(const string& photoId) {
        for (int i = 0; i < (int)photoIds_.size(); i++) {
            if (photoIds_[i] != photoId) continue;
            auto it = poolMap_.find(i);
            if (it == poolMap_.end()) break;  // not currently visible
            auto& item = pool_[it->second];
            item->unloadImage();
            loader_.cancelRequest(i);
            item->setLoadState(LoadState::Loading);
            requestLoad(i);
            break;
        }
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

    // Memo (text) cards interleaved with photos. Takes effect on next populate().
    void setShowText(bool show) { showText_ = show; }
    bool isShowText() const { return showText_; }

    void setFilterPhotoIds(const unordered_set<string>& ids) { filterPhotoIds_ = ids; }
    void clearFilterPhotoIds() { filterPhotoIds_.clear(); }
    bool hasFilterPhotoIds() const { return !filterPhotoIds_.empty(); }

    struct GeoBBox {
        double south = 0, north = 0, west = 0, east = 0;
        bool valid = false;
    };

    void setGeoBBox(double south, double north, double west, double east) {
        geoBBox_ = {south, north, west, east, true};
    }
    void clearGeoBBox() { geoBBox_ = {}; }

    // --- Populate ---

    // keepView: keep the scroll position and the selection (by id) — for
    // refreshes the user did not ask for (sync, background EXIF completion).
    void populate(PhotoProvider& provider, bool keepView = false) {
        provider_ = &provider;
        float keptScrollY = keepView ? scrollContainer_->getScrollY() : 0;
        vector<string> keptSelection = keepView ? getSelectedIds() : vector<string>{};

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
            ids = provider.getSortedIds(showText_);
        }

        for (size_t i = 0; i < ids.size(); i++) {
            auto* photo = provider.getPhoto(ids[i]);
            if (!photo) continue;
            if (photo->deletedAt > 0) continue;
            if (photo->isText() && !showText_) continue;

            // Filter by explicit photo ID set (e.g. from People view)
            if (!filterPhotoIds_.empty() && filterPhotoIds_.count(ids[i]) == 0) continue;

            // Filter by GPS bounding box
            if (geoBBox_.valid) {
                if (!photo->hasGps()) continue;
                if (photo->latitude < geoBBox_.south || photo->latitude > geoBBox_.north) continue;
                if (photo->longitude < geoBBox_.west || photo->longitude > geoBBox_.east) continue;
            }

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

        memosInGrid_.clear();
        for (const auto& id : photoIds_) {
            if (auto* e = provider.getPhoto(id); e && e->isText()) memosInGrid_.insert(id);
        }

        if (keepView) {
            // Restore selection before binding so the items draw it
            if (!keptSelection.empty()) {
                unordered_set<string> want(keptSelection.begin(), keptSelection.end());
                for (int i = 0; i < (int)photoIds_.size(); i++) {
                    if (want.count(photoIds_[i])) selectionSet_.insert(i);
                }
            }
            rebuild();
            scrollContainer_->setScrollY(keptScrollY);
            lastScrollY_ = -99999;   // force the visible range to refresh
        } else {
            resetScroll();
            rebuild();
        }
        populated.notify();
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
        if (poolIdx == 0) poolListeners_.clear();
        poolListeners_.resize(poolIdx + 1);
        auto& L = poolListeners_[poolIdx];

        auto item = make_shared<PhotoItem>(-1, itemSize_);
        item->setBubbleFont(&bubbleFont_);

        L.click = item->clicked.listen([this, poolIdx]() {
            int dataIdx = reverseMap_[poolIdx];
            if (dataIdx >= 0) {
                itemClicked.notify(dataIdx);
            }
        });

        L.rightClick = item->rightClicked.listen([this, poolIdx]() {
            int dataIdx = reverseMap_[poolIdx];
            if (dataIdx < 0 || !provider_) return;

            // Auto-select right-clicked photo if not already selected
            if (!isSelected(dataIdx)) {
                clearSelection();
                toggleSelection(dataIdx);
            }

            string photoId = photoIds_[dataIdx];
            auto* photo = provider_->getPhoto(photoId);
            if (!photo) return;

            auto menu = make_shared<ContextMenu>();

            if (!photo->localPath.empty() && fs::exists(photo->localPath)) {
                menu->addChild(make_shared<MenuItem>("Show in Finder",
                    [path = photo->localPath]() {
                        revealInFinder(path);
                    }));
            }

            // "Update Thumbnail" — only for photos with dev edits
            if (photo->hasDevEdits()) {
                menu->addChild(make_shared<MenuItem>("Update Thumbnail",
                    [this, pid = photoId]() {
                        string id = pid;
                        updateThumbnailRequested.notify(id);
                    }));
            }

            menu->addChild(make_shared<MenuSeparator>());

            menu->addChild(make_shared<MenuItem>(
                photo->isText() ? "Remove from Catalog" : "Delete",
                [this]() {
                    auto ids = getSelectedIds();
                    if (!ids.empty()) deleteRequested.notify(ids);
                }));

            menu->addChild(make_shared<MenuSeparator>());

            menu->addChild(make_shared<MenuItem>("Repair Library",
                [this]() {
                    repairRequested.notify();
                }));
            menu->addChild(make_shared<MenuItem>("Consolidate Library",
                [this]() {
                    consolidateRequested.notify();
                }));

            contextMenuRequested.notify(menu);
        });

        L.stackClick = item->stackClicked.listen([this, poolIdx]() {
            int dataIdx = reverseMap_[poolIdx];
            if (dataIdx >= 0) {
                toggleCompanionPreview(dataIdx);
            }
        });

        L.load = item->loadRequested.listen([this](int& idx) {
            requestLoad(idx);
        });
        L.unload = item->unloadRequested.listen([this](int& idx) {
            loader_.cancelRequest(idx);
        });

        return item;
    }

    void onBind(int dataIdx, ItemPtr& item) override {
        if (!provider_) return;
        auto* photo = provider_->getPhoto(photoIds_[dataIdx]);
        if (photo && photo->isText()) {
            bindTextCard(dataIdx, item, *photo);
            return;
        }
        string stem = photo ? fs::path(photo->filename).stem().string() : "???";
        SyncState sync = photo ? photo->syncState : SyncState::LocalOnly;
        bool selected = selectionSet_.count(dataIdx) > 0;

        bool clipMatch = !clipResults_.empty() && !textMatchIds_.count(photoIds_[dataIdx]);
        item->setClipMatch(clipMatch);

        bool video = photo ? photo->isVideo : false;
        int stackSize = provider_->getStackSize(photoIds_[dataIdx]);
        item->rebindAndLoad(dataIdx, stem, sync, selected, video, &labelFont_, stackSize);

        // Linked-text bubble: shown when a linked memo has no card in the current
        // grid (cards hidden, or filtered out by folder / collection / search).
        if (auto* texts = provider_->getLinkedTexts(photoIds_[dataIdx])) {
            const PhotoEntry* first = nullptr;
            int missing = 0;
            for (const auto& tid : *texts) {
                if (memosInGrid_.count(tid)) continue;
                if (!first) first = provider_->getPhoto(tid);
                missing++;
            }
            if (missing > 0) item->setMemoBubble(first ? first->memo : string(), missing);
        }
    }

    void onUnbind(int dataIdx, ItemPtr& item) override {
        loader_.cancelRequest(dataIdx);
        item->unloadImage();

        // Clean up companion preview if its parent card is being recycled
        if (companionDataIdx_ == dataIdx && companionPreview_) {
            companionPreview_->hide();
            companionDataIdx_ = -1;
        }
    }

    void onSetup() override {
        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);
    }

    void onUpdate() override {
        scrollBar_->updateFromContainer();
        processLoadResults();
    }

    bool onMousePress(Vec2 local, int button) override {
        // Dismiss companion preview on click outside (click on grid background or another card)
        if (companionPreview_ && companionPreview_->isShowing() && button == 0) {
            companionPreview_->hide();
            redraw();
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
    GeoBBox geoBBox_;

    // --- Selection ---
    unordered_set<int> selectionSet_;
    int selectionAnchor_ = -1;

    // --- Loader ---
    AsyncImageLoader loader_;
    Font labelFont_;
    Font cardFont_;
    Font bubbleFont_;                     // hover bubble (wrapped to its width)
    bool showText_ = true;
    unordered_set<string> memosInGrid_;   // memo cards present after populate()
    ScrollBar::Ptr scrollBar_;

    // --- Pool item listeners ---
    struct PoolListeners {
        EventListener click, rightClick, stackClick, load, unload;
    };
    vector<PoolListeners> poolListeners_;

    // --- Grid params ---
    float itemSize_ = 140;

    // --- Stack companion preview ---
    CompanionPreview::Ptr companionPreview_;
    EventListener companionClickListener_;
    int companionDataIdx_ = -1;
    string companionId_;
    bool companionLoading_ = false;

    void ensureCompanionPreview() {
        if (companionPreview_) return;
        companionPreview_ = make_shared<CompanionPreview>();
        companionClickListener_ = companionPreview_->clicked.listen([this]() {
            // Open the primary photo in SingleView (companion's parent)
            if (companionDataIdx_ >= 0) {
                companionPreview_->hide();
                itemClicked.notify(companionDataIdx_);
            }
        });
    }

    // =========================================================================
    // Stack companion preview (toggle on badge click)
    // =========================================================================

    void toggleCompanionPreview(int dataIdx) {
        if (!provider_ || dataIdx < 0 || dataIdx >= (int)photoIds_.size()) return;
        ensureCompanionPreview();

        // Toggle off if same item clicked again
        if (companionPreview_->isShowing() && companionDataIdx_ == dataIdx) {
            companionPreview_->hide();
            redraw();
            return;
        }

        auto companions = provider_->getStackCompanions(photoIds_[dataIdx]);
        if (companions.empty()) return;

        companionDataIdx_ = dataIdx;
        companionId_ = companions[0];

        auto it = poolMap_.find(dataIdx);
        if (it == poolMap_.end()) return;
        auto& item = pool_[it->second];

        // Attach preview as child of the card — follows scroll automatically
        item->addChild(companionPreview_);

        float previewSize = itemSize_ * 0.7f;
        // Position relative to card: offset to bottom-right, overlapping
        float ox = item->getWidth() * 0.5f;
        float oy = item->getHeight() * 0.5f;

        // Get extension label
        string ext;
        auto* photo = provider_->getPhoto(companionId_);
        if (photo) {
            ext = fs::path(photo->filename).extension().string();
            transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        }

        companionPreview_->show(ox, oy, previewSize, ext);

        // Load companion thumbnail in background
        companionLoading_ = true;
        loader_.requestLoad(-1000 - dataIdx, companionId_);
        redraw();
    }

    // =========================================================================
    // Load management
    // =========================================================================

    void requestLoad(int index) {
        if (!provider_ || index < 0 || index >= (int)photoIds_.size()) return;
        auto* photo = provider_->getPhoto(photoIds_[index]);
        if (!photo || photo->isText()) return;   // memo cards have no thumbnail
        loader_.requestLoad(index, photoIds_[index]);
    }

    // Memo card: time header, body without leading #tags, camera glyph when
    // linked to a photo. The hover bubble carries longer memos in full.
    void bindTextCard(int dataIdx, ItemPtr& item, const PhotoEntry& memo) {
        const string& dt = memo.dateTimeOriginal;            // local wall clock
        bool diary = memo.hasTag("diary");
        string label = string(diary ? "Diary" : "Memo") +
            (dt.size() >= 10 ? " " + dt.substr(5, 2) + "/" + dt.substr(8, 2) : "");
        string header = dt.size() >= 16 ? dt.substr(11, 5) : "";
        string body = stripLeadingTags(memo.memo);
        replace(body.begin(), body.end(), '\n', ' ');   // one paragraph on the card
        string hover = codepoints(body) > CARD_BODY_CP ? body : string();
        const auto* linked = provider_->getLinkedPhotos(memo.id);

        item->setClipMatch(!clipResults_.empty() && !textMatchIds_.count(memo.id));
        item->rebindAsText(dataIdx, label, header, truncateCodepoints(body, CARD_BODY_CP), hover,
                           memo.syncState, selectionSet_.count(dataIdx) > 0,
                           &labelFont_, &cardFont_, linked && !linked->empty(), diary);
    }

    static constexpr int CARD_BODY_CP = 56;   // fits a 140 px card at 11 px (≈6 lines of CJK)

    // "#photo-memo 川の音" -> "川の音" (tags leading the note only)
    static string stripLeadingTags(const string& s) {
        size_t i = 0;
        while (true) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (i + 1 < s.size() && s[i] == '#' && s[i + 1] != ' ' && s[i + 1] != '#') {
                while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') i++;
            } else {
                break;
            }
        }
        return s.substr(i);
    }

    static int codepoints(const string& s) {
        int n = 0;
        for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
        return n;
    }

    static string truncateCodepoints(const string& s, int maxCp) {
        int cp = 0;
        size_t i = 0;
        while (i < s.size() && cp < maxCp) {
            unsigned char c = (unsigned char)s[i];
            i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            cp++;
        }
        return i < s.size() ? s.substr(0, i) + "…" : s;
    }

    void processLoadResults() {
        AsyncLoadResult result;
        bool anyLoaded = false;
        while (loader_.tryGetResult(result)) {
            if (!result.success) continue;

            // Companion preview load result (negative IDs)
            if (result.id < -999) {
                if (companionLoading_ && companionPreview_ && companionPreview_->isShowing() &&
                    result.photoId == companionId_) {
                    companionPreview_->setPixels(std::move(result.pixels));
                    companionLoading_ = false;
                    anyLoaded = true;
                }
                continue;
            }

            // A populate() may have rebound this index to another photo while the
            // request was queued: only apply pixels that belong to the current id.
            if (result.id < 0 || result.id >= (int)photoIds_.size() ||
                photoIds_[result.id] != result.photoId) continue;

            auto it = poolMap_.find(result.id);
            if (it == poolMap_.end()) continue;

            auto& item = pool_[it->second];
            if (item->getActive() && item->getLoadState() == LoadState::Loading) {
                item->setPixels(std::move(result.pixels));
                anyLoaded = true;
            }
        }
        if (anyLoaded) redraw();
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
