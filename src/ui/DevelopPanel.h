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
        tempSlider_ = make_shared<DevelopSlider>("Temperature", 5500.0f, 2000.0f, 12000.0f);
        tintSlider_ = make_shared<DevelopSlider>("Tint", 0.0f, -150.0f, 150.0f);
        tempSlider_->formatValue = [](float v) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%dK", (int)v);
            return string(buf);
        };
        tintSlider_->centerZero = true;

        exposureSlider_->onChange = [this](float) {
            settingsChanged.notify();
        };
        tempSlider_->onChange = [this](float) {
            settingsChanged.notify();
        };
        tintSlider_->onChange = [this](float) {
            settingsChanged.notify();
        };

        // Tone section (GPU, no debounce)
        contrastSlider_ = make_shared<DevelopSlider>("Contrast", 0.0f, -100.0f, 100.0f);
        highlightsSlider_ = make_shared<DevelopSlider>("Highlights", 0.0f, -100.0f, 100.0f);
        shadowsSlider_ = make_shared<DevelopSlider>("Shadows", 0.0f, -100.0f, 100.0f);
        whitesSlider_ = make_shared<DevelopSlider>("Whites", 0.0f, -100.0f, 100.0f);
        blacksSlider_ = make_shared<DevelopSlider>("Blacks", 0.0f, -100.0f, 100.0f);

        contrastSlider_->centerZero = true;
        highlightsSlider_->centerZero = true;
        shadowsSlider_->centerZero = true;
        whitesSlider_->centerZero = true;
        blacksSlider_->centerZero = true;

        contrastSlider_->onChange = [this](float) { settingsChanged.notify(); };
        highlightsSlider_->onChange = [this](float) { settingsChanged.notify(); };
        shadowsSlider_->onChange = [this](float) { settingsChanged.notify(); };
        whitesSlider_->onChange = [this](float) { settingsChanged.notify(); };
        blacksSlider_->onChange = [this](float) { settingsChanged.notify(); };

        // Color section (GPU, no debounce)
        vibranceSlider_ = make_shared<DevelopSlider>("Vibrance", 0.0f, -100.0f, 100.0f);
        saturationSlider_ = make_shared<DevelopSlider>("Saturation", 0.0f, -100.0f, 100.0f);
        vibranceSlider_->centerZero = true;
        saturationSlider_->centerZero = true;

        vibranceSlider_->onChange = [this](float) { settingsChanged.notify(); };
        saturationSlider_->onChange = [this](float) { settingsChanged.notify(); };

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
        content_->addChild(contrastSlider_);
        content_->addChild(highlightsSlider_);
        content_->addChild(shadowsSlider_);
        content_->addChild(whitesSlider_);
        content_->addChild(blacksSlider_);
        content_->addChild(vibranceSlider_);
        content_->addChild(saturationSlider_);
        content_->addChild(chromaSlider_);
        content_->addChild(lumaSlider_);
        layoutSliders();
    }

    float getExposure() const { return exposureSlider_->value; }
    float getTemperature() const { return tempSlider_->value; }
    float getTint() const { return tintSlider_->value; }
    float getContrast() const { return contrastSlider_->value; }
    float getHighlights() const { return highlightsSlider_->value; }
    float getShadows() const { return shadowsSlider_->value; }
    float getWhites() const { return whitesSlider_->value; }
    float getBlacks() const { return blacksSlider_->value; }
    float getVibrance() const { return vibranceSlider_->value; }
    float getSaturation() const { return saturationSlider_->value; }
    float getChromaDenoise() const { return chromaSlider_->value; }
    float getLumaDenoise() const { return lumaSlider_->value; }

    void setValues(float exposure, float temperature, float tint,
                   float contrast, float highlights, float shadows,
                   float whites, float blacks,
                   float vibrance, float saturation,
                   float chroma, float luma) {
        exposureSlider_->value = exposure;
        tempSlider_->value = temperature;
        tintSlider_->value = tint;
        contrastSlider_->value = contrast;
        highlightsSlider_->value = highlights;
        shadowsSlider_->value = shadows;
        whitesSlider_->value = whites;
        blacksSlider_->value = blacks;
        vibranceSlider_->value = vibrance;
        saturationSlider_->value = saturation;
        chromaSlider_->value = chroma;
        lumaSlider_->value = luma;
        redraw();
    }

    // Set as-shot WB as double-click reset default
    void setAsShotDefaults(float asShotTemp, float asShotTint) {
        tempSlider_->defaultVal = (asShotTemp > 0) ? asShotTemp : 5500.0f;
        tintSlider_->defaultVal = asShotTint;
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

        // "Tone" section header
        setColor(0.45f, 0.45f, 0.5f);
        drawBitmapString("Tone", 12, toneSectionY_ + 1);
        setColor(0.25f, 0.25f, 0.28f);
        drawLine(12, toneSectionY_ + 22, w - 12, toneSectionY_ + 22);

        // "Color" section header
        setColor(0.45f, 0.45f, 0.5f);
        drawBitmapString("Color", 12, colorSectionY_ + 1);
        setColor(0.25f, 0.25f, 0.28f);
        drawLine(12, colorSectionY_ + 22, w - 12, colorSectionY_ + 22);

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

    // Tone section
    DevelopSlider::Ptr contrastSlider_;
    DevelopSlider::Ptr highlightsSlider_;
    DevelopSlider::Ptr shadowsSlider_;
    DevelopSlider::Ptr whitesSlider_;
    DevelopSlider::Ptr blacksSlider_;

    // Color section
    DevelopSlider::Ptr vibranceSlider_;
    DevelopSlider::Ptr saturationSlider_;

    // NR section
    DevelopSlider::Ptr chromaSlider_;
    DevelopSlider::Ptr lumaSlider_;

    float toneSectionY_ = 0;   // Y position of Tone section header
    float colorSectionY_ = 0;  // Y position of Color section header
    float nrSectionY_ = 0;     // Y position of NR section header

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

        // Tone section
        toneSectionY_ = y + 8;
        y = toneSectionY_ + sectionGap_;

        contrastSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        highlightsSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        shadowsSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        whitesSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        blacksSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        // Color section
        colorSectionY_ = y + 8;
        y = colorSectionY_ + sectionGap_;

        vibranceSlider_->setRect(0, y, w, sliderH_);
        y += sliderH_ + padding_;

        saturationSlider_->setRect(0, y, w, sliderH_);
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
