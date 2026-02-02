#pragma once

// =============================================================================
// PhotoItem.h - Single photo item (thumbnail + label)
// =============================================================================

#include <TrussC.h>
#include "PhotoLibrary.h"
using namespace std;
using namespace tc;

// Thumbnail node - displays the image
class ThumbnailNode : public RectNode {
public:
    using Ptr = shared_ptr<ThumbnailNode>;

    ThumbnailNode() {
        setSize(100, 100);
    }

    // Set image to display
    void setImage(Image&& img) {
        image_ = std::move(img);
        hasImage_ = true;
    }

    bool hasImage() const { return hasImage_; }

    void draw() override {
        if (hasImage_ && image_.isAllocated()) {
            // Draw image fitted to node size
            float imgW = image_.getWidth();
            float imgH = image_.getHeight();

            // Calculate fit scale (contain)
            float scaleX = getWidth() / imgW;
            float scaleY = getHeight() / imgH;
            float scale = min(scaleX, scaleY);

            float drawW = imgW * scale;
            float drawH = imgH * scale;
            float offsetX = (getWidth() - drawW) / 2;
            float offsetY = (getHeight() - drawH) / 2;

            setColor(1.0f, 1.0f, 1.0f);
            image_.draw(offsetX, offsetY, drawW, drawH);
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
    bool hasImage_ = false;
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
        pushStyle();

        // Debug: draw label rect
        setColor(0.3f, 0.2f, 0.2f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());

        popStyle();

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

    PhotoItem(int entryIndex, float thumbnailSize = 120) : entryIndex_(entryIndex) {
        enableEvents();

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

    void loadImage(const fs::path& path) {
        Image img;
        if (img.load(path)) {
            thumbnail_->setImage(std::move(img));
        }
    }

    void draw() override {
        // Hover highlight
        if (isMouseOver()) {
            setColor(0.3f, 0.35f, 0.45f, 0.5f);
            fill();
            drawRect(0, 0, getWidth(), getHeight());
        }

        // Selection highlight (optional, for future)
        if (isSelected_) {
            setColor(0.4f, 0.5f, 0.7f, 0.6f);
            fill();
            drawRect(0, 0, getWidth(), getHeight());
        }
    }

    void setSelected(bool selected) { isSelected_ = selected; }
    bool isSelected() const { return isSelected_; }

protected:
    bool onMousePress(Vec2 local, int button) override {
        (void)local;
        if (button == 0 && onClick) {
            onClick();
        }
        return RectNode::onMousePress(local, button);
    }

private:
    int entryIndex_ = -1;
    ThumbnailNode::Ptr thumbnail_;
    LabelNode::Ptr label_;
    bool isSelected_ = false;
};
