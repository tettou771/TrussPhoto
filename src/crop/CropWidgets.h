#pragma once

// =============================================================================
// CropWidgets.h - RectNode-based widgets for CropPanel
// All text uses Font rendering. All elements are proper child nodes.
// =============================================================================

#include <TrussC.h>
#include "CropTypes.h"

using namespace std;
using namespace tc;

// =============================================================================
// TextLabel - Simple text label with Font rendering
// =============================================================================
class TextLabel : public RectNode {
public:
    using Ptr = shared_ptr<TextLabel>;

    string text;
    Color color{0.45f, 0.45f, 0.5f};
    float xPad = 12;

    TextLabel(const string& t, Font* font) : text(t), font_(font) {}

    void draw() override {
        if (!font_ || text.empty()) return;
        setColor(color);
        font_->drawString(text, xPad, getHeight() / 2, Left, Center);
    }

private:
    Font* font_;
};

// =============================================================================
// Separator - Horizontal divider line
// =============================================================================
class Separator : public RectNode {
public:
    using Ptr = shared_ptr<Separator>;

    void draw() override {
        float w = getWidth();
        float y = getHeight() / 2;
        setColor(0.25f, 0.25f, 0.28f);
        noFill();
        drawLine(12, y, w - 12, y);
    }
};

// =============================================================================
// AspectButton - Single aspect ratio button (e.g. "16:9")
// =============================================================================
class AspectButton : public RectNode {
public:
    using Ptr = shared_ptr<AspectButton>;

    Event<CropAspect> clicked;
    bool selected = false;

    AspectButton(CropAspect aspect, Font* font)
        : aspect_(aspect), font_(font) {
        enableEvents();
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        if (selected) {
            setColor(0.2f, 0.35f, 0.55f);
            fill();
            drawRect(0, 0, w, h);
        }

        setColor(selected ? Color(0.9f, 0.9f, 0.95f) : Color(0.6f, 0.6f, 0.65f));
        if (font_) font_->drawString(cropAspectLabel(aspect_), 12, h / 2, Left, Center);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;
        clicked.notify(aspect_);
        return true;
    }

private:
    CropAspect aspect_;
    Font* font_;
};

// =============================================================================
// OrientationToggle - Landscape / Portrait toggle with icon rectangles
// =============================================================================
class OrientationToggle : public RectNode {
public:
    using Ptr = shared_ptr<OrientationToggle>;

    Event<bool> orientationChanged;
    bool isLandscape = true;
    bool grayed = false;

    OrientationToggle() {
        enableEvents();
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();
        float btnW = 36;
        float btnH = h;
        float gap = 8;
        float totalW = btnW * 2 + gap;
        float startX = (w - totalW) / 2;

        // Landscape button
        {
            bool sel = isLandscape && !grayed;
            setColor(sel ? Color(0.2f, 0.35f, 0.55f) : Color(0.15f, 0.15f, 0.17f));
            fill();
            drawRect(startX, 0, btnW, btnH);
            setColor(grayed ? Color(0.25f, 0.25f, 0.28f) :
                     (sel ? Color(0.9f, 0.9f, 0.95f) : Color(0.5f, 0.5f, 0.55f)));
            noFill();
            float iw = 20, ih = 14;
            drawRect(startX + (btnW - iw) / 2, (btnH - ih) / 2, iw, ih);
        }

        // Portrait button
        {
            float px = startX + btnW + gap;
            bool sel = !isLandscape && !grayed;
            setColor(sel ? Color(0.2f, 0.35f, 0.55f) : Color(0.15f, 0.15f, 0.17f));
            fill();
            drawRect(px, 0, btnW, btnH);
            setColor(grayed ? Color(0.25f, 0.25f, 0.28f) :
                     (sel ? Color(0.9f, 0.9f, 0.95f) : Color(0.5f, 0.5f, 0.55f)));
            noFill();
            float iw = 14, ih = 20;
            drawRect(px + (btnW - iw) / 2, (btnH - ih) / 2, iw, ih);
        }
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0 || grayed) return false;

        float w = getWidth();
        float btnW = 36;
        float gap = 8;
        float totalW = btnW * 2 + gap;
        float startX = (w - totalW) / 2;

        if (pos.x >= startX && pos.x <= startX + btnW) {
            if (!isLandscape) {
                isLandscape = true;
                orientationChanged.notify(isLandscape);
            }
            return true;
        }
        float px = startX + btnW + gap;
        if (pos.x >= px && pos.x <= px + btnW) {
            if (isLandscape) {
                isLandscape = false;
                orientationChanged.notify(isLandscape);
            }
            return true;
        }
        return false;
    }
};

// =============================================================================
// PanelButton - Simple labeled button (Reset, Cancel, Done)
// =============================================================================
class PanelButton : public RectNode {
public:
    using Ptr = shared_ptr<PanelButton>;

    Event<void> clicked;

    PanelButton(const string& label, bool isAccent, Font* font)
        : label_(label), isAccent_(isAccent), font_(font) {
        enableEvents();
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        setColor(isAccent_ ? Color(0.2f, 0.4f, 0.65f) : Color(0.18f, 0.18f, 0.2f));
        fill();
        drawRect(0, 0, w, h);

        setColor(isAccent_ ? Color(0.95f, 0.95f, 1.0f) : Color(0.7f, 0.7f, 0.75f));
        if (font_) font_->drawString(label_, w / 2, h / 2, Center, Center);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;
        clicked.notify();
        return true;
    }

private:
    string label_;
    bool isAccent_;
    Font* font_;
};

// =============================================================================
// AngleSlider - Center-zero slider for rotation angle (±45°)
// =============================================================================
class AngleSlider : public RectNode {
public:
    using Ptr = shared_ptr<AngleSlider>;

    Event<float> angleChanged;  // radians
    float angle = 0;            // radians

    AngleSlider(Font* font) : font_(font) {
        enableEvents();
    }

    void setAngle(float a) {
        angle = a;
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();
        float pad = 12.0f;
        float trackY = 24.0f;
        float trackH = 4.0f;
        float knobR = 6.0f;

        // Label + value
        float degrees = angle * (180.0f / 3.14159265f);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", degrees);
        if (font_) {
            setColor(0.45f, 0.45f, 0.5f);
            font_->drawString("Angle", pad, 12, Left, Center);
            setColor(0.65f, 0.65f, 0.7f);
            string valStr = string(buf) + "\xC2\xB0";  // UTF-8 degree sign
            font_->drawString(valStr, w - pad, 12, Right, Center);
        }

        // Track background
        float trackLeft = pad;
        float trackRight = w - pad;
        float trackW = trackRight - trackLeft;
        float centerX = trackLeft + trackW / 2;

        setColor(0.2f, 0.2f, 0.24f);
        fill();
        drawRect(trackLeft, trackY, trackW, trackH);

        // Center mark
        setColor(0.35f, 0.35f, 0.4f);
        drawRect(centerX - 0.5f, trackY - 2, 1, trackH + 4);

        // Fill from center
        constexpr float maxAngle = 3.14159265f / 4.0f;  // TAU/8 = PI/4
        float t = clamp(angle / maxAngle, -1.0f, 1.0f);
        float fillStart = centerX;
        float fillEnd = centerX + (trackW / 2) * t;
        if (fillEnd < fillStart) swap(fillStart, fillEnd);
        setColor(0.4f, 0.6f, 0.9f);
        drawRect(fillStart, trackY, fillEnd - fillStart, trackH);

        // Knob
        float knobX = centerX + (trackW / 2) * t;
        float knobY = trackY + trackH / 2;
        setColor(0.8f, 0.85f, 0.9f);
        drawCircle(knobX, knobY, knobR);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;
        // Double-click: reset to 0
        auto now = chrono::steady_clock::now();
        auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - lastClick_).count();
        lastClick_ = now;
        if (elapsed < 350) {
            angle = 0;
            angleChanged.notify(angle);
            return true;
        }
        dragging_ = true;
        updateFromMouse(pos.x);
        return true;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (!dragging_ || button != 0) return false;
        updateFromMouse(pos.x);
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 0) dragging_ = false;
        return true;
    }

private:
    Font* font_;
    bool dragging_ = false;
    chrono::steady_clock::time_point lastClick_;

    void updateFromMouse(float mx) {
        float pad = 12.0f;
        float trackLeft = pad;
        float trackRight = getWidth() - pad;
        float trackW = trackRight - trackLeft;
        float centerX = trackLeft + trackW / 2;

        // t: -1 to +1
        float t = (mx - centerX) / (trackW / 2);
        t = clamp(t, -1.0f, 1.0f);

        constexpr float maxAngle = 3.14159265f / 4.0f;  // PI/4 = 45°
        angle = t * maxAngle;
        angleChanged.notify(angle);
    }
};

// =============================================================================
// PerspSlider - Center-zero slider for perspective/shear values (±1)
// =============================================================================
class PerspSlider : public RectNode {
public:
    using Ptr = shared_ptr<PerspSlider>;

    Event<float> valueChanged;
    float value = 0;         // -1 to +1
    string label = "V";

    PerspSlider(const string& lbl, Font* font) : label(lbl), font_(font) {
        enableEvents();
    }

    void setValue(float v) { value = v; }

    void draw() override {
        float w = getWidth();
        float h = getHeight();
        float pad = 12.0f;
        float trackY = 24.0f;
        float trackH = 4.0f;
        float knobR = 6.0f;

        // Label + value
        char buf[16];
        snprintf(buf, sizeof(buf), "%+.0f%%", value * 100.0f);
        if (font_) {
            setColor(0.45f, 0.45f, 0.5f);
            font_->drawString(label, pad, 12, Left, Center);
            setColor(0.65f, 0.65f, 0.7f);
            font_->drawString(buf, w - pad, 12, Right, Center);
        }

        // Track background
        float trackLeft = pad;
        float trackRight = w - pad;
        float trackW = trackRight - trackLeft;
        float centerX = trackLeft + trackW / 2;

        setColor(0.2f, 0.2f, 0.24f);
        fill();
        drawRect(trackLeft, trackY, trackW, trackH);

        // Center mark
        setColor(0.35f, 0.35f, 0.4f);
        drawRect(centerX - 0.5f, trackY - 2, 1, trackH + 4);

        // Fill from center
        float t = clamp(value, -1.0f, 1.0f);
        float fillStart = centerX;
        float fillEnd = centerX + (trackW / 2) * t;
        if (fillEnd < fillStart) swap(fillStart, fillEnd);
        setColor(0.4f, 0.6f, 0.9f);
        drawRect(fillStart, trackY, fillEnd - fillStart, trackH);

        // Knob
        float knobX = centerX + (trackW / 2) * t;
        float knobY = trackY + trackH / 2;
        setColor(0.8f, 0.85f, 0.9f);
        drawCircle(knobX, knobY, knobR);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;
        // Double-click: reset to 0
        auto now = chrono::steady_clock::now();
        auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - lastClick_).count();
        lastClick_ = now;
        if (elapsed < 350) {
            value = 0;
            valueChanged.notify(value);
            return true;
        }
        dragging_ = true;
        updateFromMouse(pos.x);
        return true;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (!dragging_ || button != 0) return false;
        updateFromMouse(pos.x);
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 0) dragging_ = false;
        return true;
    }

private:
    Font* font_;
    bool dragging_ = false;
    chrono::steady_clock::time_point lastClick_;

    void updateFromMouse(float mx) {
        float pad = 12.0f;
        float trackLeft = pad;
        float trackRight = getWidth() - pad;
        float trackW = trackRight - trackLeft;
        float centerX = trackLeft + trackW / 2;

        float t = (mx - centerX) / (trackW / 2);
        t = clamp(t, -1.0f, 1.0f);

        value = t;
        valueChanged.notify(value);
    }
};

// =============================================================================
// Rotate90Row - Two 90° rotation buttons side by side (↺ | ↻)
// =============================================================================
class Rotate90Row : public RectNode {
public:
    using Ptr = shared_ptr<Rotate90Row>;

    Event<int> rotated;  // -1 = CCW, +1 = CW

    Rotate90Row(Font* font) : font_(font) {
        enableEvents();
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();
        float btnW = (w - 6) / 2;  // 6px gap

        // Left button (CCW)
        setColor(0.18f, 0.18f, 0.2f);
        fill();
        drawRect(0, 0, btnW, h);
        setColor(0.7f, 0.7f, 0.75f);
        if (font_) font_->drawString("\xE2\x86\xBA 90\xC2\xB0", btnW / 2, h / 2, Center, Center);

        // Right button (CW)
        setColor(0.18f, 0.18f, 0.2f);
        fill();
        drawRect(btnW + 6, 0, btnW, h);
        setColor(0.7f, 0.7f, 0.75f);
        if (font_) font_->drawString("\xE2\x86\xBB 90\xC2\xB0", btnW + 6 + btnW / 2, h / 2, Center, Center);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;
        float w = getWidth();
        float btnW = (w - 6) / 2;
        if (pos.x < btnW) {
            int dir = -1;
            rotated.notify(dir);
        } else if (pos.x > btnW + 6) {
            int dir = 1;
            rotated.notify(dir);
        }
        return true;
    }

private:
    Font* font_;
};

// =============================================================================
// FocalLengthRow - Focal length display / slider for perspective drag
// Two modes: EXIF read-only text, or log-scale slider (12-600mm)
// =============================================================================
class FocalLengthRow : public RectNode {
public:
    using Ptr = shared_ptr<FocalLengthRow>;

    Event<int> focalChanged;  // focal length in mm (0 = disabled)
    int value = 0;            // current focal length mm
    bool fromExif = false;    // true = read-only EXIF display

    FocalLengthRow(Font* font) : font_(font) {
        enableEvents();
    }

    void setFocalLength(int mm, bool exif) {
        value = mm;
        fromExif = exif;
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();
        float pad = 12.0f;

        if (fromExif) {
            // Read-only EXIF display (grayed out)
            if (font_) {
                setColor(0.35f, 0.35f, 0.4f);
                font_->drawString("Focal", pad, h / 2, Left, Center);
                char buf[32];
                snprintf(buf, sizeof(buf), "%dmm (35mm eq)", value);
                font_->drawString(buf, w - pad, h / 2, Right, Center);
            }
            return;
        }

        // Slider mode
        float trackY = 24.0f;
        float trackH = 4.0f;
        float knobR = 6.0f;

        // Label + value
        if (font_) {
            setColor(0.45f, 0.45f, 0.5f);
            font_->drawString("Focal", pad, 12, Left, Center);
            setColor(0.65f, 0.65f, 0.7f);
            if (value > 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%dmm", value);
                font_->drawString(buf, w - pad, 12, Right, Center);
            } else {
                font_->drawString("---", w - pad, 12, Right, Center);
            }
        }

        // Track
        float trackLeft = pad;
        float trackRight = w - pad;
        float trackW = trackRight - trackLeft;

        setColor(0.2f, 0.2f, 0.24f);
        fill();
        drawRect(trackLeft, trackY, trackW, trackH);

        // Position: log scale 12-600mm
        float pos = mmToPos(value);

        // Fill from left
        if (value > 0) {
            setColor(0.4f, 0.6f, 0.9f);
            drawRect(trackLeft, trackY, trackW * pos, trackH);
        }

        // Knob
        float knobX = trackLeft + trackW * pos;
        float knobY = trackY + trackH / 2;
        setColor(0.8f, 0.85f, 0.9f);
        drawCircle(knobX, knobY, knobR);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0 || fromExif) return false;
        // Double-click: reset to 0
        auto now = chrono::steady_clock::now();
        auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - lastClick_).count();
        lastClick_ = now;
        if (elapsed < 350) {
            value = 0;
            focalChanged.notify(value);
            return true;
        }
        dragging_ = true;
        updateFromMouse(pos.x);
        return true;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (!dragging_ || button != 0) return false;
        updateFromMouse(pos.x);
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 0) dragging_ = false;
        return true;
    }

private:
    Font* font_;
    bool dragging_ = false;
    chrono::steady_clock::time_point lastClick_;

    static constexpr float kMinMM = 12.0f;
    static constexpr float kMaxMM = 600.0f;

    // Log-scale: pos 0-1 -> 12-600mm
    static float posToMM(float pos) {
        if (pos <= 0.001f) return 0;  // left edge = disabled
        return kMinMM * exp(pos * log(kMaxMM / kMinMM));
    }

    static float mmToPos(int mm) {
        if (mm <= 0) return 0;
        float f = clamp((float)mm, kMinMM, kMaxMM);
        return log(f / kMinMM) / log(kMaxMM / kMinMM);
    }

    void updateFromMouse(float mx) {
        float pad = 12.0f;
        float trackLeft = pad;
        float trackRight = getWidth() - pad;
        float trackW = trackRight - trackLeft;

        float pos = clamp((mx - trackLeft) / trackW, 0.0f, 1.0f);
        float mm = posToMM(pos);
        value = (int)round(mm);
        focalChanged.notify(value);
    }
};

// =============================================================================
// ButtonRow - Horizontal container for action buttons (Reset | Cancel | Done)
// =============================================================================
class ButtonRow : public RectNode {
public:
    using Ptr = shared_ptr<ButtonRow>;

    PanelButton::Ptr resetBtn, cancelBtn, doneBtn;

    ButtonRow(Font* font) {
        resetBtn = make_shared<PanelButton>("Reset", false, font);
        cancelBtn = make_shared<PanelButton>("Cancel", false, font);
        doneBtn = make_shared<PanelButton>("Done", true, font);
    }

    void setup() override {
        addChild(resetBtn);
        addChild(cancelBtn);
        addChild(doneBtn);
    }

    void update() override {
        float w = getWidth();
        float h = getHeight();
        float gap = 6;
        float btnW = (w - gap * 2) / 3;
        resetBtn->setRect(0, 0, btnW, h);
        cancelBtn->setRect(btnW + gap, 0, btnW, h);
        doneBtn->setRect((btnW + gap) * 2, 0, btnW, h);
    }
};
