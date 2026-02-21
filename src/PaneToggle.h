#pragma once

// =============================================================================
// PaneToggle.h - Collapsible pane toggle button (triangle on divider)
// =============================================================================

#include <TrussC.h>
using namespace std;
using namespace tc;

class PaneToggle : public RectNode {
public:
    using Ptr = shared_ptr<PaneToggle>;

    enum Direction { Left, Right };

    Direction direction = Right;
    Event<void> clicked;

    PaneToggle() {
        enableEvents();
        setSize(12, 30);
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background pill
        setColor(0.18f, 0.18f, 0.2f);
        fill();
        drawRect(0, 0, w, h);

        // Triangle arrow
        setColor(0.55f, 0.55f, 0.6f);
        fill();
        float cx = w * 0.5f;
        float cy = h * 0.5f;
        float sz = 4.0f;

        if (direction == Left) {
            // Left-pointing triangle
            drawTriangle(cx + sz * 0.5f, cy - sz, cx - sz * 0.5f, cy, cx + sz * 0.5f, cy + sz);
        } else {
            // Right-pointing triangle
            drawTriangle(cx - sz * 0.5f, cy - sz, cx + sz * 0.5f, cy, cx - sz * 0.5f, cy + sz);
        }
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        (void)local;
        if (button != 0) return false;
        clicked.notify();
        return true;
    }
};
