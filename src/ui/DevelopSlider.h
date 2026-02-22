#pragma once

// =============================================================================
// DevelopSlider.h - Draggable slider widget for develop panel
// =============================================================================

#include <TrussC.h>
#include <functional>
#include <chrono>

using namespace std;
using namespace tc;

class DevelopSlider : public RectNode {
public:
    using Ptr = shared_ptr<DevelopSlider>;

    string label;
    float value = 0;
    float minVal = 0;
    float maxVal = 1;
    float defaultVal = 0;
    function<void(float)> onChange;

    DevelopSlider() = default;

    DevelopSlider(const string& lbl, float def, float lo = 0.0f, float hi = 1.0f)
        : label(lbl), value(def), minVal(lo), maxVal(hi), defaultVal(def) {}

    // Set debounce time in seconds. 0 = immediate (default).
    void setDebounceTime(double seconds) { debounceSec_ = seconds; }

    void setup() override {
        enableEvents();
    }

    void draw() override {
        float w = getWidth();
        float pad = 8.0f;
        float trackY = 28.0f;
        float trackH = 4.0f;
        float knobR = 6.0f;

        // Label
        setColor(0.6f, 0.6f, 0.65f);
        drawBitmapString(label, pad, 14);

        // Value text
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", value);
        setColor(0.75f, 0.75f, 0.8f);
        float tw = getBitmapStringWidth(buf);
        drawBitmapString(buf, w - pad - tw, 14);

        // Track background
        float trackLeft = pad;
        float trackRight = w - pad;
        float trackW = trackRight - trackLeft;

        setColor(0.2f, 0.2f, 0.24f);
        fill();
        drawRect(trackLeft, trackY, trackW, trackH);

        // Fill
        float t = (value - minVal) / (maxVal - minVal);
        t = clamp(t, 0.0f, 1.0f);
        setColor(0.4f, 0.6f, 0.9f);
        drawRect(trackLeft, trackY, trackW * t, trackH);

        // Knob
        float knobX = trackLeft + trackW * t;
        float knobY = trackY + trackH * 0.5f;
        setColor(0.8f, 0.85f, 0.9f);
        drawCircle(knobX, knobY, knobR);
    }

    bool onMousePress(Vec2 pos, int button) override {
        if (button == 0) {
            // Double-click detection: reset to default
            auto now = chrono::steady_clock::now();
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - lastClickTime_).count();
            lastClickTime_ = now;

            if (elapsed < 350) {
                value = defaultVal;
                fireImmediate();
                redraw();
                return true;
            }

            dragging_ = true;
            updateFromMouse(pos.x);
        }
        return true;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (dragging_ && button == 0) {
            updateFromMouse(pos.x);
        }
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        if (dragging_) {
            // On release: cancel pending debounce and fire immediately
            cancelPending();
            if (onChange) onChange(value);
        }
        dragging_ = false;
        return true;
    }

private:
    bool dragging_ = false;
    double debounceSec_ = 0;
    uint64_t pendingTimerId_ = 0;
    chrono::steady_clock::time_point lastClickTime_;

    void cancelPending() {
        if (pendingTimerId_ != 0) {
            cancelTimer(pendingTimerId_);
            pendingTimerId_ = 0;
        }
    }

    void fireImmediate() {
        cancelPending();
        if (onChange) onChange(value);
    }

    void fireDebounced() {
        if (debounceSec_ <= 0) {
            if (onChange) onChange(value);
            return;
        }
        // Cancel previous pending call, schedule new one
        cancelPending();
        pendingTimerId_ = callAfter(debounceSec_, [this]() {
            pendingTimerId_ = 0;
            if (onChange) onChange(value);
        });
    }

    void updateFromMouse(float mx) {
        float pad = 8.0f;
        float trackLeft = pad;
        float trackRight = getWidth() - pad;
        float trackW = trackRight - trackLeft;

        float t = (mx - trackLeft) / trackW;
        t = clamp(t, 0.0f, 1.0f);
        value = minVal + t * (maxVal - minVal);
        fireDebounced();
        redraw();
    }
};
