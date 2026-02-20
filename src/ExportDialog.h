#pragma once

// =============================================================================
// ExportDialog.h - Modal dialog for JPEG export settings
// =============================================================================
// Overlay dialog with size presets, quality slider, and export/cancel buttons.
// Follows NameEditOverlay pattern (full-screen RectNode overlay + centered dialog).
// =============================================================================

#include <TrussC.h>
#include "PhotoExporter.h"

using namespace std;
using namespace tc;

class ExportDialog : public RectNode {
public:
    using Ptr = shared_ptr<ExportDialog>;

    function<void(const ExportSettings&)> onExport;
    function<void()> onCancel;

    void setup() override {
        enableEvents();
    }

    void show(const ExportSettings& initial, int sourceW, int sourceH) {
        selectedMaxEdge_ = initial.maxEdge;
        quality_ = initial.quality;
        sourceW_ = sourceW;
        sourceH_ = sourceH;
        draggingSlider_ = false;
        setActive(true);
    }

    void hide() {
        setActive(false);
    }

    ExportSettings currentSettings() const {
        return { selectedMaxEdge_, quality_ };
    }

    void draw() override {
        float w = getWidth(), h = getHeight();

        // Semi-transparent backdrop
        setColor(0, 0, 0, 0.6f);
        fill();
        drawRect(0, 0, w, h);

        // Dialog box
        calcLayout(w, h);

        // Background
        setColor(0.15f, 0.15f, 0.18f);
        fill();
        drawRect(dlgX_, dlgY_, dlgW_, dlgH_);

        // Border
        setColor(0.3f, 0.3f, 0.35f);
        noFill();
        drawRect(dlgX_, dlgY_, dlgW_, dlgH_);

        // Title
        setColor(0.85f, 0.85f, 0.9f);
        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString("Export JPEG", dlgX_ + dlgW_ / 2, dlgY_ + 20);
        popStyle();

        // --- Size presets ---
        float rowY = dlgY_ + 46;
        setColor(0.6f, 0.6f, 0.65f);
        drawBitmapString("Size:", dlgX_ + pad_, rowY);

        float btnX = dlgX_ + 60;
        float btnW = 56, btnH = 24, gap = 6;
        const int presets[] = {0, 2560, 1920, 1280};
        const char* labels[] = {"Full", "2560", "1920", "1280"};

        for (int i = 0; i < 4; i++) {
            float bx = btnX + i * (btnW + gap);
            bool selected = (selectedMaxEdge_ == presets[i]);

            if (selected) {
                setColor(0.3f, 0.5f, 0.85f);
            } else {
                setColor(0.22f, 0.22f, 0.26f);
            }
            fill();
            drawRect(bx, rowY - 8, btnW, btnH);

            // Button border
            if (selected) {
                setColor(0.4f, 0.6f, 0.95f);
            } else {
                setColor(0.3f, 0.3f, 0.35f);
            }
            noFill();
            drawRect(bx, rowY - 8, btnW, btnH);

            setColor(selected ? 1.0f : 0.7f, selected ? 1.0f : 0.7f,
                     selected ? 1.0f : 0.75f);
            pushStyle();
            setTextAlign(Direction::Center, Direction::Center);
            drawBitmapString(labels[i], bx + btnW / 2, rowY + btnH / 2 - 8);
            popStyle();

            sizeBtnRects_[i] = {bx, rowY - 8, btnW, btnH};
        }

        // --- Quality slider ---
        float sliderY = rowY + 40;
        setColor(0.6f, 0.6f, 0.65f);
        drawBitmapString("Quality:", dlgX_ + pad_, sliderY);

        // Value text
        char qBuf[8];
        snprintf(qBuf, sizeof(qBuf), "%d", quality_);
        setColor(0.75f, 0.75f, 0.8f);
        float qw = getBitmapStringWidth(qBuf);
        drawBitmapString(qBuf, dlgX_ + dlgW_ - pad_ - qw, sliderY);

        // Track
        float trackLeft = dlgX_ + 80;
        float trackRight = dlgX_ + dlgW_ - pad_ - 36;
        float trackW = trackRight - trackLeft;
        float trackY = sliderY + 4;
        float trackH = 4.0f;
        float knobR = 6.0f;

        setColor(0.2f, 0.2f, 0.24f);
        fill();
        drawRect(trackLeft, trackY, trackW, trackH);

        float t = (float)(quality_ - 1) / 99.0f;
        t = clamp(t, 0.0f, 1.0f);
        setColor(0.4f, 0.6f, 0.9f);
        fill();
        drawRect(trackLeft, trackY, trackW * t, trackH);

        float knobX = trackLeft + trackW * t;
        float knobY = trackY + trackH * 0.5f;
        setColor(0.8f, 0.85f, 0.9f);
        drawCircle(knobX, knobY, knobR);

        // Store slider track geometry for hit testing
        sliderTrackLeft_ = trackLeft;
        sliderTrackRight_ = trackRight;
        sliderTrackY_ = trackY - 10;
        sliderTrackH_ = 24;

        // --- Output size ---
        float outY = sliderY + 32;
        auto [outW, outH] = calcOutputSize();
        char outBuf[64];
        snprintf(outBuf, sizeof(outBuf), "Output: %d x %d", outW, outH);
        setColor(0.5f, 0.5f, 0.55f);
        drawBitmapString(outBuf, dlgX_ + pad_, outY);

        // --- Buttons ---
        float btnRowY = dlgY_ + dlgH_ - 40;
        float cbtnW = 80, cbtnH = 28;
        float totalBtnW = cbtnW * 2 + 16;
        float bStartX = dlgX_ + (dlgW_ - totalBtnW) / 2;

        // Cancel button
        cancelRect_ = {bStartX, btnRowY, cbtnW, cbtnH};
        setColor(0.22f, 0.22f, 0.26f);
        fill();
        drawRect(cancelRect_.x, cancelRect_.y, cancelRect_.w, cancelRect_.h);
        setColor(0.3f, 0.3f, 0.35f);
        noFill();
        drawRect(cancelRect_.x, cancelRect_.y, cancelRect_.w, cancelRect_.h);
        setColor(0.7f, 0.7f, 0.75f);
        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString("Cancel", cancelRect_.x + cbtnW / 2,
                         cancelRect_.y + cbtnH / 2);
        popStyle();

        // Export button (accent)
        exportRect_ = {bStartX + cbtnW + 16, btnRowY, cbtnW, cbtnH};
        setColor(0.25f, 0.45f, 0.8f);
        fill();
        drawRect(exportRect_.x, exportRect_.y, exportRect_.w, exportRect_.h);
        setColor(0.35f, 0.55f, 0.9f);
        noFill();
        drawRect(exportRect_.x, exportRect_.y, exportRect_.w, exportRect_.h);
        setColor(1.0f, 1.0f, 1.0f);
        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString("Export", exportRect_.x + cbtnW / 2,
                         exportRect_.y + cbtnH / 2);
        popStyle();
    }

    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return true;

        // Size preset buttons
        const int presets[] = {0, 2560, 1920, 1280};
        for (int i = 0; i < 4; i++) {
            if (hitTest(pos, sizeBtnRects_[i])) {
                selectedMaxEdge_ = presets[i];
                redraw();
                return true;
            }
        }

        // Quality slider
        if (pos.x >= sliderTrackLeft_ - 8 && pos.x <= sliderTrackRight_ + 8 &&
            pos.y >= sliderTrackY_ && pos.y <= sliderTrackY_ + sliderTrackH_) {
            draggingSlider_ = true;
            updateQualityFromMouse(pos.x);
            return true;
        }

        // Cancel button
        if (hitTest(pos, cancelRect_)) {
            if (onCancel) onCancel();
            return true;
        }

        // Export button
        if (hitTest(pos, exportRect_)) {
            doExport();
            return true;
        }

        // Consume all clicks (modal)
        return true;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (draggingSlider_ && button == 0) {
            updateQualityFromMouse(pos.x);
        }
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos; (void)button;
        draggingSlider_ = false;
        return true;
    }

    bool onKeyPress(int key) override {
        if (key == 256 /* ESCAPE */) {
            if (onCancel) onCancel();
            return true;
        }
        if (key == 257 /* ENTER */ || key == 335 /* KP_ENTER */) {
            doExport();
            return true;
        }
        return true; // consume all keys (modal)
    }

private:
    int selectedMaxEdge_ = 0;
    int quality_ = 92;
    int sourceW_ = 0, sourceH_ = 0;
    bool draggingSlider_ = false;

    // Layout
    static constexpr float dlgW_ = 320, dlgH_ = 200, pad_ = 14;
    float dlgX_ = 0, dlgY_ = 0;

    struct Rect { float x, y, w, h; };
    Rect sizeBtnRects_[4] = {};
    float sliderTrackLeft_ = 0, sliderTrackRight_ = 0;
    float sliderTrackY_ = 0, sliderTrackH_ = 0;
    Rect cancelRect_ = {}, exportRect_ = {};

    void calcLayout(float parentW, float parentH) {
        dlgX_ = (parentW - dlgW_) / 2;
        dlgY_ = (parentH - dlgH_) / 2;
    }

    bool hitTest(Vec2 pos, const Rect& r) const {
        return pos.x >= r.x && pos.x <= r.x + r.w &&
               pos.y >= r.y && pos.y <= r.y + r.h;
    }

    pair<int, int> calcOutputSize() const {
        int outW = sourceW_, outH = sourceH_;
        if (selectedMaxEdge_ > 0 && max(sourceW_, sourceH_) > selectedMaxEdge_) {
            float scale = (float)selectedMaxEdge_ / max(sourceW_, sourceH_);
            outW = max(1, (int)round(sourceW_ * scale));
            outH = max(1, (int)round(sourceH_ * scale));
        }
        return {outW, outH};
    }

    void updateQualityFromMouse(float mx) {
        float trackW = sliderTrackRight_ - sliderTrackLeft_;
        float t = (mx - sliderTrackLeft_) / trackW;
        t = clamp(t, 0.0f, 1.0f);
        quality_ = 1 + (int)round(t * 99.0f);
        quality_ = clamp(quality_, 1, 100);
        redraw();
    }

    void doExport() {
        if (onExport) {
            onExport(currentSettings());
        }
    }
};
