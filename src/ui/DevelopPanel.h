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

    // Event when any setting changes
    Event<void> settingsChanged;

    DevelopPanel() {
        scrollContainer_ = make_shared<PlainScrollContainer>();
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);

        // Create sliders - Basic section (GPU, no debounce)
        exposureSlider_ = make_shared<DevelopSlider>("Exposure", 0.0f, -3.0f, 3.0f);
        tempSlider_ = make_shared<DevelopSlider>("Temperature", 0.0f, -1.0f, 1.0f);
        tintSlider_ = make_shared<DevelopSlider>("Tint", 0.0f, -1.0f, 1.0f);

        exposureSlider_->onChange = [this](float) {
            settingsChanged.notify();
        };
        tempSlider_->onChange = [this](float) {
            settingsChanged.notify();
        };
        tintSlider_->onChange = [this](float) {
            settingsChanged.notify();
        };

        // Noise Reduction section (CPU, with debounce)
        chromaSlider_ = make_shared<DevelopSlider>("Chroma NR", 0.5f, 0.0f, 1.0f);
        lumaSlider_ = make_shared<DevelopSlider>("Luma NR", 0.0f, 0.0f, 1.0f);
        chromaSlider_->setDebounceTime(0.2);
        lumaSlider_->setDebounceTime(0.2);

        chromaSlider_->onChange = [this](float) {
            settingsChanged.notify();
        };
        lumaSlider_->onChange = [this](float) {
            settingsChanged.notify();
        };
    }

    void setup() override {
        addChild(scrollContainer_);
        content_->addChild(exposureSlider_);
        content_->addChild(tempSlider_);
        content_->addChild(tintSlider_);
        content_->addChild(chromaSlider_);
        content_->addChild(lumaSlider_);
        layoutSliders();
    }

    float getExposure() const { return exposureSlider_->value; }
    float getTemperature() const { return tempSlider_->value; }
    float getTint() const { return tintSlider_->value; }
    float getChromaDenoise() const { return chromaSlider_->value; }
    float getLumaDenoise() const { return lumaSlider_->value; }

    void setValues(float exposure, float temp, float tint, float chroma, float luma) {
        exposureSlider_->value = exposure;
        tempSlider_->value = temp;
        tintSlider_->value = tint;
        chromaSlider_->value = chroma;
        lumaSlider_->value = luma;
        redraw();
    }

    void setNrEnabled(bool en) {
        chromaSlider_->enabled = en;
        lumaSlider_->enabled = en;
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

        // "Basic" section header
        setColor(0.45f, 0.45f, 0.5f);
        drawBitmapString("Basic", 12, 7);
        setColor(0.25f, 0.25f, 0.28f);
        drawLine(12, 28, w - 12, 28);

        // "Noise Reduction" section header
        setColor(0.45f, 0.45f, 0.5f);
        drawBitmapString("Noise Reduction", 12, nrSectionY_ + 1);
        setColor(0.25f, 0.25f, 0.28f);
        drawLine(12, nrSectionY_ + 22, w - 12, nrSectionY_ + 22);
    }

private:
    PlainScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;

    // Basic section
    DevelopSlider::Ptr exposureSlider_;
    DevelopSlider::Ptr tempSlider_;
    DevelopSlider::Ptr tintSlider_;

    // NR section
    DevelopSlider::Ptr chromaSlider_;
    DevelopSlider::Ptr lumaSlider_;

    float nrSectionY_ = 0;  // Y position of NR section header

    static constexpr float sliderH_ = 44.0f;
    static constexpr float topOffset_ = 36.0f;
    static constexpr float sectionGap_ = 32.0f;
    static constexpr float padding_ = 4.0f;

    void layoutSliders() {
        float w = getWidth() - 12;  // scrollbar space
        float y = topOffset_;

        // Basic section sliders
        exposureSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        tempSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        tintSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        // NR section
        nrSectionY_ = y + 8;
        y = nrSectionY_ + sectionGap_;

        chromaSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        lumaSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        content_->setSize(w, y + 20);
        scrollContainer_->updateScrollBounds();
    }
};
