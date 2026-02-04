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

    LabelNode() {
        setSize(100, 20);
    }

    void draw() override {
        // Background
        setColor(0.12f, 0.12f, 0.14f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());

        if (!text.empty()) {
            pushStyle();
            setColor(textColor);
            setTextAlign(Direction::Center, Direction::Center);
            drawBitmapString(text, getWidth() / 2, getHeight() / 2);
            popStyle();
        }
    }
};

// PhotoItem - combines thumbnail and label
class PhotoItem : public RectNode {
public:
    using Ptr = shared_ptr<PhotoItem>;

    // Callback for click (set by parent)
    function<void()> onClick;

    // Callback for load request (set by parent)
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
    void setLoadState(LoadState state) { loadState_ = state; }

    // Set pixels from async loader (call from main thread)
    void setPixels(Pixels&& pix) {
        thumbnail_->setPixels(std::move(pix));
        loadState_ = LoadState::Loaded;
    }

    // Unload image to free memory
    void unloadImage() {
        thumbnail_->clearImage();
        loadState_ = LoadState::Unloaded;
    }

    void draw() override {
        // Hover highlight
        if (isMouseOver()) {
            setColor(0.3f, 0.35f, 0.45f, 0.5f);
            fill();
            drawRect(0, 0, getWidth(), getHeight());
        }

        // Selection highlight
        if (isSelected_) {
            setColor(0.4f, 0.5f, 0.7f, 0.6f);
            fill();
            drawRect(0, 0, getWidth(), getHeight());
        }

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
    }

    void setSelected(bool selected) { isSelected_ = selected; }
    bool isSelected() const { return isSelected_; }

    void setSyncState(SyncState state) { syncState_ = state; }

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
    bool isSelected_ = false;
    LoadState loadState_ = LoadState::Unloaded;
    SyncState syncState_ = SyncState::LocalOnly;
};
