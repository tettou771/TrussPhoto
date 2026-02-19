#pragma once

// =============================================================================
// DevelopPanel.h - Right sidebar for develop parameters (NR, etc.)
// =============================================================================

#include <TrussC.h>
#include "DevelopSlider.h"
#include "FolderTree.h"  // for PlainScrollContainer

using namespace std;
using namespace tc;

class DevelopPanel : public RectNode {
public:
    using Ptr = shared_ptr<DevelopPanel>;

    // Callback when any setting changes
    function<void()> onSettingsChanged;

    DevelopPanel() {
        scrollContainer_ = make_shared<PlainScrollContainer>();
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);

        // Create sliders
        chromaSlider_ = make_shared<DevelopSlider>("Chroma NR", 0.5f, 0.0f, 1.0f);
        lumaSlider_ = make_shared<DevelopSlider>("Luma NR", 0.0f, 0.0f, 1.0f);

        chromaSlider_->onChange = [this](float v) {
            if (onSettingsChanged) onSettingsChanged();
        };
        lumaSlider_->onChange = [this](float v) {
            if (onSettingsChanged) onSettingsChanged();
        };
    }

    void setup() override {
        addChild(scrollContainer_);
        content_->addChild(chromaSlider_);
        content_->addChild(lumaSlider_);
        layoutSliders();
    }

    float getChromaDenoise() const { return chromaSlider_->value; }
    float getLumaDenoise() const { return lumaSlider_->value; }

    void setValues(float chroma, float luma) {
        chromaSlider_->value = chroma;
        lumaSlider_->value = luma;
        redraw();
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        scrollContainer_->setRect(0, 0, w, h);
        layoutSliders();
    }

    void update() override {
        scrollContainer_->updateScrollBounds();
        scrollBar_->updateFromContainer();
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.09f, 0.09f, 0.11f);
        fill();
        drawRect(0, 0, w, h);

        // Left border
        setColor(0.2f, 0.2f, 0.22f);
        noFill();
        drawLine(0, 0, 0, h);

        // Section header
        setColor(0.45f, 0.45f, 0.5f);
        drawBitmapString("Noise Reduction", 12, 20);

        // Separator line
        setColor(0.25f, 0.25f, 0.28f);
        drawLine(12, 28, w - 12, 28);
    }

private:
    PlainScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;

    DevelopSlider::Ptr chromaSlider_;
    DevelopSlider::Ptr lumaSlider_;

    static constexpr float sliderH_ = 44.0f;
    static constexpr float topOffset_ = 36.0f;
    static constexpr float padding_ = 4.0f;

    void layoutSliders() {
        float w = getWidth() - 12;  // scrollbar space
        float y = topOffset_;

        chromaSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        lumaSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        content_->setSize(w, y + 20);
        scrollContainer_->updateScrollBounds();
    }
};
