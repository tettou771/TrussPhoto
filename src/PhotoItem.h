#pragma once

// =============================================================================
// PhotoItem.h - Single photo item (thumbnail + label)
// =============================================================================

#include <TrussC.h>
#include "PhotoProvider.h"
using namespace std;
using namespace tc;

// Load state
enum class LoadState {
    Unloaded,
    Loading,
    Loaded
};

// Thumbnail node - displays the image
class ThumbnailNode : public RectNode {
public:
    using Ptr = shared_ptr<ThumbnailNode>;

    ThumbnailNode() {
        setSize(100, 100);
    }

    // Set image from pixels (creates texture on main thread)
    void setPixels(Pixels&& pix) {
        pixels_ = std::move(pix);
        if (pixels_.isAllocated()) {
            texture_.allocate(pixels_, TextureUsage::Immutable);
            hasImage_ = true;
        }
    }

    // Set image directly
    void setImage(Image&& img) {
        image_ = std::move(img);
        hasImage_ = true;
        useImage_ = true;
    }

    // Clear image to free memory
    void clearImage() {
        image_ = Image();
        pixels_.clear();
        texture_.clear();
        hasImage_ = false;
        useImage_ = false;
    }

    bool hasImage() const { return hasImage_; }

    void draw() override {
        if (hasImage_) {
            float imgW, imgH;
            if (useImage_) {
                imgW = image_.getWidth();
                imgH = image_.getHeight();
            } else {
                imgW = texture_.getWidth();
                imgH = texture_.getHeight();
            }

            // Calculate fit scale (contain)
            float scaleX = getWidth() / imgW;
            float scaleY = getHeight() / imgH;
            float scale = min(scaleX, scaleY);

            float drawW = imgW * scale;
            float drawH = imgH * scale;
            float offsetX = (getWidth() - drawW) / 2;
            float offsetY = (getHeight() - drawH) / 2;

            setColor(1.0f, 1.0f, 1.0f);
            if (useImage_) {
                image_.draw(offsetX, offsetY, drawW, drawH);
            } else {
                texture_.draw(offsetX, offsetY, drawW, drawH);
            }
        } else {
            // Placeholder
            setColor(0.2f, 0.2f, 0.25f);
            fill();
            drawRect(0, 0, getWidth(), getHeight());

            setColor(0.4f, 0.4f, 0.45f);
            noFill();
            drawRect(0, 0, getWidth(), getHeight());
        }
    }

private:
    Image image_;
    Pixels pixels_;
    Texture texture_;
    bool hasImage_ = false;
    bool useImage_ = false;  // true = use Image, false = use Texture from Pixels
};

// Label node - displays filename
class LabelNode : public RectNode {
public:
    using Ptr = shared_ptr<LabelNode>;

    string text;
    Color textColor = Color(0.8f, 0.8f, 0.85f);
    Color bgColor = Color(0.12f, 0.12f, 0.14f);
    Font* font = nullptr;  // set by PhotoGrid

    LabelNode() {
        setSize(100, 20);
    }

    void draw() override {
        // Background
        setColor(bgColor);
        fill();
        drawRect(0, 0, getWidth(), getHeight());

        if (!text.empty()) {
            setColor(textColor);
            if (font) {
                font->drawString(text, getWidth() / 2, getHeight() / 2,
                    Direction::Center, Direction::Center);
            } else {
                pushStyle();
                setTextAlign(Direction::Center, Direction::Center);
                drawBitmapString(text, getWidth() / 2, getHeight() / 2);
                popStyle();
            }
        }
    }
};

// StackBadge - clickable badge showing stack count
class StackBadge : public RectNode {
public:
    using Ptr = shared_ptr<StackBadge>;

    function<void()> onClick;

    StackBadge() {
        enableEvents();
        setSize(28, 20);
        setActive(false); // hidden by default
    }

    void setCount(int n) {
        count_ = n;
        setActive(n > 1);
    }
    int getCount() const { return count_; }

    void draw() override {
        if (count_ <= 1) return;

        float bx = 6;
        float by = 4;
        float bw = 10, bh = 8;

        // Back rectangle (offset)
        setColor(0.6f, 0.6f, 0.65f, 0.7f);
        noFill();
        drawRect(bx + 3, by, bw, bh);

        // Front rectangle
        setColor(0.75f, 0.75f, 0.8f, 0.9f);
        fill();
        drawRect(bx, by + 3, bw, bh);
        setColor(0.5f, 0.5f, 0.55f, 0.9f);
        noFill();
        drawRect(bx, by + 3, bw, bh);

        // Count text
        setColor(0.9f, 0.9f, 0.9f, 0.9f);
        fill();
        pushStyle();
        setTextAlign(Direction::Left, Direction::Center);
        drawBitmapString(to_string(count_), bx + bw + 3, by + bh / 2 + 2);
        popStyle();
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        (void)local;
        if (button == 0 && onClick) {
            onClick();
            return true; // consume event
        }
        return RectNode::onMousePress(local, button);
    }

private:
    int count_ = 0;
};

// CompanionPreview - clickable companion thumbnail overlay
class CompanionPreview : public RectNode {
public:
    using Ptr = shared_ptr<CompanionPreview>;

    function<void()> onClick;

    CompanionPreview() {
        enableEvents();
        setActive(false);
    }

    void show(float x, float y, float size, const string& ext) {
        setPos(x, y);
        previewSize_ = size;
        setSize(size, size + 14); // thumbnail + label
        extLabel_ = ext;
        setActive(true);
    }

    void hide() {
        setActive(false);
        texture_.clear();
    }

    bool isShowing() const { return getActive(); }

    void setPixels(Pixels&& pix) {
        if (pix.isAllocated()) {
            texture_.allocate(pix, TextureUsage::Immutable);
            redraw();
        }
    }

    void draw() override {
        // Shadow
        setColor(0.0f, 0.0f, 0.0f, 0.4f);
        fill();
        drawRect(2, 2, previewSize_, previewSize_);

        // Background
        setColor(0.15f, 0.15f, 0.18f);
        fill();
        drawRect(0, 0, previewSize_, previewSize_);

        // Thumbnail
        if (texture_.isAllocated()) {
            setColor(1.0f, 1.0f, 1.0f);
            float tw = texture_.getWidth();
            float th = texture_.getHeight();
            float scale = min(previewSize_ / tw, previewSize_ / th);
            float dw = tw * scale, dh = th * scale;
            float dx = (previewSize_ - dw) / 2;
            float dy = (previewSize_ - dh) / 2;
            texture_.draw(dx, dy, dw, dh);
        } else {
            // Loading placeholder
            setColor(0.3f, 0.3f, 0.35f);
            fill();
            drawRect(4, 4, previewSize_ - 8, previewSize_ - 8);
        }

        // Border
        setColor(0.5f, 0.5f, 0.55f);
        noFill();
        drawRect(0, 0, previewSize_, previewSize_);

        // Extension label
        if (!extLabel_.empty()) {
            setColor(0.8f, 0.8f, 0.85f);
            fill();
            pushStyle();
            setTextAlign(Direction::Center, Direction::Top);
            drawBitmapString(extLabel_, previewSize_ / 2, previewSize_ + 2);
            popStyle();
        }
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        (void)local;
        if (button == 0 && onClick) {
            onClick();
            return true;
        }
        return RectNode::onMousePress(local, button);
    }

private:
    Texture texture_;
    float previewSize_ = 100;
    string extLabel_;
};

// PhotoItem - combines thumbnail and label
class PhotoItem : public RectNode {
public:
    using Ptr = shared_ptr<PhotoItem>;

    // Callbacks (set by parent)
    function<void()> onClick;
    function<void()> onStackClick;
    function<void(int)> onRequestLoad;
    function<void(int)> onRequestUnload;

    PhotoItem(int entryIndex, float thumbnailSize = 120) : entryIndex_(entryIndex) {
        enableEvents();

        // Start inactive - will be activated by visibility check
        setActive(false);

        // Size: thumbnail + label area
        float labelHeight = 20;
        float padding = 4;
        setSize(thumbnailSize, thumbnailSize + labelHeight + padding);

        // Thumbnail node
        thumbnail_ = make_shared<ThumbnailNode>();
        thumbnail_->setSize(thumbnailSize, thumbnailSize);
        thumbnail_->setPos(0, 0);
        addChild(thumbnail_);

        // Stack badge (top-right of thumbnail)
        stackBadge_ = make_shared<StackBadge>();
        stackBadge_->setPos(thumbnailSize - 28, 0);
        stackBadge_->onClick = [this]() {
            if (onStackClick) onStackClick();
        };
        addChild(stackBadge_);

        // Label node
        label_ = make_shared<LabelNode>();
        label_->setSize(thumbnailSize, labelHeight);
        label_->setPos(0, thumbnailSize + padding);
        addChild(label_);
    }

    int getEntryIndex() const { return entryIndex_; }

    ThumbnailNode::Ptr getThumbnail() { return thumbnail_; }
    LabelNode::Ptr getLabel() { return label_; }

    void setLabelText(const string& text) {
        // Truncate if too long
        if (text.length() > 15) {
            label_->text = text.substr(0, 12) + "...";
        } else {
            label_->text = text;
        }
    }

    // Load state management
    LoadState getLoadState() const { return loadState_; }
    void setLoadState(LoadState state) {
        loadState_ = state;
        redraw();
    }

    // Set pixels from async loader (call from main thread)
    void setPixels(Pixels&& pix) {
        thumbnail_->setPixels(std::move(pix));
        setLoadState(LoadState::Loaded);
    }

    // Unload image to free memory
    void unloadImage() {
        thumbnail_->clearImage();
        setLoadState(LoadState::Unloaded);
    }

    // Rebind this item to a different data index and start loading
    void setClipMatch(bool v) { clipMatch_ = v; }
    bool isClipMatch() const { return clipMatch_; }

    void setIsVideo(bool v) { isVideo_ = v; }
    bool isVideo() const { return isVideo_; }

    void setStackSize(int n) { stackBadge_->setCount(n); }
    int getStackSize() const { return stackBadge_->getCount(); }
    bool isStacked() const { return stackBadge_->getCount() > 1; }

    void rebindAndLoad(int dataIndex, const string& label, SyncState syncState,
                       bool selected, bool isVideo, Font* font, int stackSize = 0) {
        // Cancel pending load for old data
        if (loadState_ == LoadState::Loading && onRequestUnload)
            onRequestUnload(entryIndex_);

        entryIndex_ = dataIndex;
        setLabelText(label);
        label_->font = font;
        setSyncState(syncState);
        setSelected(selected);
        setIsVideo(isVideo);
        setStackSize(stackSize);
        label_->bgColor = isVideo ? Color(0.08f, 0.14f, 0.18f) : Color(0.12f, 0.12f, 0.14f);
        thumbnail_->clearImage();

        // Set Loading before firing request to prevent onActiveChanged re-request
        loadState_ = LoadState::Loading;
        if (onRequestLoad) onRequestLoad(entryIndex_);
    }
    
    void update() override {
        if (pastMouseOver != isMouseOver()) {
            pastMouseOver = isMouseOver();
            redraw();
        }
    }
    
    void draw() override {
        // Hover highlight (background, behind thumbnail)
        if (isMouseOver()) {
            setColor(0.3f, 0.35f, 0.45f, 0.5f);
            fill();
            drawRect(0, 0, getWidth(), getHeight());
        }

        // Selection highlight (background, behind thumbnail)
        if (isSelected_) {
            setColor(0.4f, 0.5f, 0.7f, 0.6f);
            fill();
            drawRect(0, 0, getWidth(), getHeight());
        }
    }

    // Badges drawn AFTER children (on top of thumbnail)
    void endDraw() override {
        // Card border (same color as label background)
        setColor(label_->bgColor);
        noFill();
        drawRect(0, 0, getWidth(), getHeight());

        // Sync state badge (bottom-right corner of thumbnail)
        float badgeSize = 8;
        float badgeX = getWidth() - badgeSize - 4;
        float badgeY = getWidth() - badgeSize - 4;  // thumbnail is square, width = thumbnail height
        fill();
        switch (syncState_) {
            case SyncState::LocalOnly:
                setColor(0.9f, 0.65f, 0.2f);  // orange
                drawCircle(badgeX, badgeY, badgeSize);
                break;
            case SyncState::Syncing:
                setColor(0.3f, 0.6f, 0.95f);  // blue
                drawCircle(badgeX, badgeY, badgeSize);
                break;
            case SyncState::Synced:
                setColor(0.3f, 0.8f, 0.4f);   // green
                drawCircle(badgeX, badgeY, badgeSize);
                break;
            case SyncState::ServerOnly:
                setColor(0.7f, 0.5f, 0.9f);   // purple
                drawCircle(badgeX, badgeY, badgeSize);
                break;
            case SyncState::Missing:
                setColor(0.9f, 0.3f, 0.3f);   // red
                drawCircle(badgeX, badgeY, badgeSize);
                break;
        }

        // CLIP similarity match border
        if (clipMatch_) {
            setColor(0.4f, 0.7f, 1.0f, 0.7f);  // light blue
            noFill();
            drawRect(0, 0, getWidth(), getHeight());
        }
    }

    void setSelected(bool selected) { isSelected_ = selected; }
    bool isSelected() const { return isSelected_; }

    void setSyncState(SyncState state) { syncState_ = state; }
    SyncState getSyncState() const { return syncState_; }

protected:
    bool onMousePress(Vec2 local, int button) override {
        (void)local;
        if (button == 0 && onClick) {
            onClick();
        }
        return RectNode::onMousePress(local, button);
    }

    void onActiveChanged(bool active) override {
        if (active) {
            // Becoming visible - request load if needed
            if (loadState_ == LoadState::Unloaded && onRequestLoad) {
                loadState_ = LoadState::Loading;
                onRequestLoad(entryIndex_);
            }
        } else {
            // Becoming invisible - unload to save memory
            if (loadState_ == LoadState::Loaded) {
                unloadImage();
                if (onRequestUnload) {
                    onRequestUnload(entryIndex_);
                }
            } else if (loadState_ == LoadState::Loading) {
                // Cancel pending load
                loadState_ = LoadState::Unloaded;
                if (onRequestUnload) {
                    onRequestUnload(entryIndex_);
                }
            }
        }
    }

private:
    int entryIndex_ = -1;
    ThumbnailNode::Ptr thumbnail_;
    LabelNode::Ptr label_;
    StackBadge::Ptr stackBadge_;
    bool isSelected_ = false;
    bool isVideo_ = false;
    bool clipMatch_ = false;
    bool pastMouseOver = false;
    LoadState loadState_ = LoadState::Unloaded;
    SyncState syncState_ = SyncState::LocalOnly;
};
