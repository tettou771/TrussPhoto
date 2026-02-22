#pragma once

// =============================================================================
// ExportDialog.h - Modal dialog for JPEG export settings
// =============================================================================
// Overlay dialog with size presets, quality slider, and export/cancel buttons.
// All interactive elements are RectNode children with event-driven hit testing.
// =============================================================================

#include <TrussC.h>
#include "pipeline/PhotoExporter.h"

using namespace std;
using namespace tc;

// --- Widget classes (used by ExportDialog) ---

class SizeButton : public RectNode {
public:
    Event<int> clicked;

    SizeButton(int value, const string& label)
        : value_(value), label_(label) {}

    void setSelected(bool s) {
        if (selected_ != s) {
            selected_ = s;
            redraw();
        }
    }

    void setDisabled(bool d) {
        if (disabled_ != d) {
            disabled_ = d;
            redraw();
        }
    }

    bool isDisabled() const { return disabled_; }

    void setup() override { enableEvents(); }

    void draw() override {
        float w = getWidth(), h = getHeight();

        if (disabled_) {
            // Greyed out
            setColor(0.16f, 0.16f, 0.18f);
            fill();
            drawRect(0, 0, w, h);
            setColor(0.25f, 0.25f, 0.28f);
            noFill();
            drawRect(0, 0, w, h);
            setColor(0.35f, 0.35f, 0.38f);
        } else if (selected_) {
            setColor(0.3f, 0.5f, 0.85f);
            fill();
            drawRect(0, 0, w, h);
            setColor(0.4f, 0.6f, 0.95f);
            noFill();
            drawRect(0, 0, w, h);
            setColor(1.0f, 1.0f, 1.0f);
        } else {
            setColor(0.22f, 0.22f, 0.26f);
            fill();
            drawRect(0, 0, w, h);
            setColor(0.3f, 0.3f, 0.35f);
            noFill();
            drawRect(0, 0, w, h);
            setColor(0.7f, 0.7f, 0.75f);
        }

        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString(label_, w / 2, h / 2);
        popStyle();
    }

    bool onMousePress(Vec2, int button) override {
        if (button == 0 && !disabled_) clicked.notify(value_);
        return true;
    }

private:
    int value_;
    string label_;
    bool selected_ = false;
    bool disabled_ = false;
};

class QualitySlider : public RectNode {
public:
    int value = 92;

    void setup() override { enableEvents(); }

    void draw() override {
        float w = getWidth();

        // "Quality:" label
        setColor(0.6f, 0.6f, 0.65f);
        drawBitmapString("Quality:", pad_, labelY_);

        // Value text
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", value);
        setColor(0.75f, 0.75f, 0.8f);
        float tw = getBitmapStringWidth(buf);
        drawBitmapString(buf, w - pad_ - tw, labelY_);

        // Track
        float trackL = trackLeftX_;
        float trackR = w - pad_ - 36;
        float trackW = trackR - trackL;

        setColor(0.2f, 0.2f, 0.24f);
        fill();
        drawRect(trackL, trackY_, trackW, trackH_);

        float t = (float)(value - 1) / 99.0f;
        t = clamp(t, 0.0f, 1.0f);
        setColor(0.4f, 0.6f, 0.9f);
        fill();
        drawRect(trackL, trackY_, trackW * t, trackH_);

        // Knob
        float knobX = trackL + trackW * t;
        float knobY = trackY_ + trackH_ * 0.5f;
        setColor(0.8f, 0.85f, 0.9f);
        drawCircle(knobX, knobY, knobR_);

        // Store right edge for mouse handling
        trackRight_ = trackR;
    }

    bool onMousePress(Vec2 pos, int button) override {
        if (button == 0) {
            dragging_ = true;
            updateFromMouse(pos.x);
        }
        return true;
    }

    bool onMouseDrag(Vec2 pos, int) override {
        if (dragging_) updateFromMouse(pos.x);
        return true;
    }

    bool onMouseRelease(Vec2, int) override {
        dragging_ = false;
        return true;
    }

private:
    bool dragging_ = false;
    float trackRight_ = 0;

    static constexpr float pad_ = 14;
    static constexpr float labelY_ = 10;
    static constexpr float trackLeftX_ = 80;
    static constexpr float trackY_ = 14;
    static constexpr float trackH_ = 4.0f;
    static constexpr float knobR_ = 6.0f;

    void updateFromMouse(float mx) {
        float trackW = trackRight_ - trackLeftX_;
        if (trackW <= 0) return;
        float t = (mx - trackLeftX_) / trackW;
        t = clamp(t, 0.0f, 1.0f);
        value = 1 + (int)round(t * 99.0f);
        value = clamp(value, 1, 100);
        redraw();
    }
};

class DialogButton : public RectNode {
public:
    Event<void> clicked;

    DialogButton(const string& label, bool accent = false)
        : label_(label), accent_(accent) {}

    void setup() override { enableEvents(); }

    void draw() override {
        float w = getWidth(), h = getHeight();

        // Background
        if (accent_) setColor(0.25f, 0.45f, 0.8f);
        else setColor(0.22f, 0.22f, 0.26f);
        fill();
        drawRect(0, 0, w, h);

        // Border
        if (accent_) setColor(0.35f, 0.55f, 0.9f);
        else setColor(0.3f, 0.3f, 0.35f);
        noFill();
        drawRect(0, 0, w, h);

        // Label
        setColor(accent_ ? 1.0f : 0.7f, accent_ ? 1.0f : 0.7f,
                 accent_ ? 1.0f : 0.75f);
        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString(label_, w / 2, h / 2);
        popStyle();
    }

    bool onMousePress(Vec2, int button) override {
        if (button == 0) clicked.notify();
        return true;
    }

private:
    string label_;
    bool accent_;
};

// --- DialogBox (background, border, title, static labels) ---

class ExportDialogBox : public RectNode {
public:
    string outputText;

    void setup() override { enableEvents(); }

    void draw() override {
        float w = getWidth();

        // Background
        setColor(0.15f, 0.15f, 0.18f);
        fill();
        drawRect(0, 0, w, getHeight());

        // Border
        setColor(0.3f, 0.3f, 0.35f);
        noFill();
        drawRect(0, 0, w, getHeight());

        // Title
        setColor(0.85f, 0.85f, 0.9f);
        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString("Export JPEG", w / 2, 20);
        popStyle();

        // "Size:" label
        setColor(0.6f, 0.6f, 0.65f);
        drawBitmapString("Size:", 14, 46);

        // Output size
        setColor(0.5f, 0.5f, 0.55f);
        drawBitmapString(outputText, 14, 118);
    }

    // Consume clicks on dialog background (modal)
    bool onMousePress(Vec2, int) override { return true; }
};

// --- ExportDialog (full-screen modal overlay) ---

class ExportDialog : public RectNode {
public:
    using Ptr = shared_ptr<ExportDialog>;

    Event<ExportSettings> exportRequested;
    Event<void> cancelled;

    void setup() override {
        enableEvents();

        // Dialog box
        dialogBox_ = make_shared<ExportDialogBox>();
        dialogBox_->setSize(dlgW_, dlgH_);
        addChild(dialogBox_);

        // Size preset buttons
        const int presets[] = {0, 2560, 1920, 1280};
        const char* labels[] = {"Full", "2560", "1920", "1280"};
        for (int i = 0; i < 4; i++) {
            sizeButtons_[i] = make_shared<SizeButton>(presets[i], labels[i]);
            sizeButtons_[i]->setSize(btnW_, btnH_);
            sizeListeners_[i] = sizeButtons_[i]->clicked.listen([this](int& val) {
                selectedMaxEdge_ = val;
                updateSizeSelection();
                updateOutputText();
                redraw();
            });
            dialogBox_->addChild(sizeButtons_[i]);
        }
        sizeButtons_[0]->setSelected(true);

        // Quality slider
        slider_ = make_shared<QualitySlider>();
        slider_->setRect(0, 76, dlgW_, 36);
        dialogBox_->addChild(slider_);

        // Cancel / Export buttons
        cancelBtn_ = make_shared<DialogButton>("Cancel");
        exportBtn_ = make_shared<DialogButton>("Export", true);
        cancelListener_ = cancelBtn_->clicked.listen([this]() {
            cancelled.notify();
        });
        exportListener_ = exportBtn_->clicked.listen([this]() {
            doExport();
        });
        dialogBox_->addChild(cancelBtn_);
        dialogBox_->addChild(exportBtn_);

        layoutChildren();
    }

    void show(const ExportSettings& initial, int sourceW, int sourceH) {
        quality_ = initial.quality;
        sourceW_ = sourceW;
        sourceH_ = sourceH;
        // If previous selection is now too large, fall back to Full
        int maxEdge = max(sourceW, sourceH);
        if (initial.maxEdge > 0 && initial.maxEdge >= maxEdge) {
            selectedMaxEdge_ = 0;  // Full
        } else {
            selectedMaxEdge_ = initial.maxEdge;
        }
        needsSync_ = true;
        setActive(true);
    }

    void hide() {
        setActive(false);
    }

    ExportSettings currentSettings() const {
        return { selectedMaxEdge_, quality_ };
    }

    void update() override {
        // Sync state to children after setup() has created them
        if (needsSync_) {
            slider_->value = quality_;
            updateSizeSelection();
            updateOutputText();
            needsSync_ = false;
        }

        // Center dialog box
        float w = getWidth(), h = getHeight();
        dialogBox_->setPos((w - dlgW_) / 2, (h - dlgH_) / 2);
    }

    void draw() override {
        // Semi-transparent backdrop only
        setColor(0, 0, 0, 0.6f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());
    }

    // Consume all mouse events on backdrop (modal)
    bool onMousePress(Vec2, int) override { return true; }
    bool onMouseDrag(Vec2, int) override { return true; }
    bool onMouseRelease(Vec2, int) override { return true; }

    bool onKeyPress(int key) override {
        if (key == 256 /* ESCAPE */) {
            cancelled.notify();
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
    bool needsSync_ = false;

    // Layout constants
    static constexpr float dlgW_ = 320, dlgH_ = 200;
    static constexpr float btnW_ = 56, btnH_ = 24, btnGap_ = 6;
    static constexpr float actionW_ = 80, actionH_ = 28;

    // Children
    shared_ptr<ExportDialogBox> dialogBox_;
    shared_ptr<SizeButton> sizeButtons_[4];
    shared_ptr<QualitySlider> slider_;
    shared_ptr<DialogButton> cancelBtn_, exportBtn_;

    // Event listeners
    EventListener sizeListeners_[4];
    EventListener cancelListener_, exportListener_;

    void layoutChildren() {
        // Size buttons: y=38, starting at x=60
        for (int i = 0; i < 4; i++) {
            sizeButtons_[i]->setPos(60 + i * (btnW_ + btnGap_), 38);
        }

        // Quality slider is set in setup() via setRect

        // Action buttons: y=160, centered
        float totalW = actionW_ * 2 + 16;
        float startX = (dlgW_ - totalW) / 2;
        cancelBtn_->setRect(startX, 160, actionW_, actionH_);
        exportBtn_->setRect(startX + actionW_ + 16, 160, actionW_, actionH_);
    }

    void updateSizeSelection() {
        const int presets[] = {0, 2560, 1920, 1280};
        int maxEdge = max(sourceW_, sourceH_);
        for (int i = 0; i < 4; i++) {
            // Disable presets that are >= native size (no upscale)
            bool disabled = (presets[i] > 0 && presets[i] >= maxEdge);
            sizeButtons_[i]->setDisabled(disabled);
            sizeButtons_[i]->setSelected(presets[i] == selectedMaxEdge_);
        }
    }

    void updateOutputText() {
        auto [outW, outH] = calcOutputSize();
        char buf[64];
        snprintf(buf, sizeof(buf), "Output: %d x %d", outW, outH);
        dialogBox_->outputText = buf;
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

    void doExport() {
        // Read latest slider value before exporting
        if (slider_) quality_ = slider_->value;
        auto settings = currentSettings();
        exportRequested.notify(settings);
    }
};
