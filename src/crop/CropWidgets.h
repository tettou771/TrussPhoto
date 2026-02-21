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
