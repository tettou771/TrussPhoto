#pragma once

// =============================================================================
// SidebarTabs.h - Tab switcher for sidebar (Folders / Collections)
// =============================================================================

#include <TrussC.h>
#include "FolderTree.h"  // for loadJapaneseFont
using namespace std;
using namespace tc;

class SidebarTabs : public RectNode {
public:
    using Ptr = shared_ptr<SidebarTabs>;

    Event<int> tabChanged;

    SidebarTabs() {
        enableEvents();
        loadJapaneseFont(font_, 12);
    }

    int activeTab() const { return activeTab_; }

    void setActiveTab(int tab) {
        if (activeTab_ == tab) return;
        activeTab_ = tab;
        tabChanged.notify(activeTab_);
        redraw();
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.09f, 0.09f, 0.11f);
        fill();
        drawRect(0, 0, w, h);

        // Bottom border
        setColor(0.2f, 0.2f, 0.22f);
        fill();
        drawRect(0, h - 1, w, 1);

        // Two tabs: Folders | Collections
        float halfW = w * 0.5f;

        for (int i = 0; i < 2; i++) {
            float tx = i * halfW;
            bool active = (activeTab_ == i);

            // Active tab underline
            if (active) {
                setColor(0.35f, 0.5f, 0.75f);
                fill();
                drawRect(tx + 4, h - 3, halfW - 8, 2);
            }

            // Tab label
            if (active) {
                setColor(0.9f, 0.9f, 0.95f);
            } else {
                setColor(0.5f, 0.5f, 0.55f);
            }

            const char* label = (i == 0) ? "Folders" : "Collections";
            font_.drawString(label, tx + halfW * 0.5f, h * 0.45f,
                Direction::Center, Direction::Center);
        }
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        if (button != 0) return false;
        float halfW = getWidth() * 0.5f;
        int tab = (local.x < halfW) ? 0 : 1;
        setActiveTab(tab);
        return true;
    }

private:
    int activeTab_ = 0;
    Font font_;
};
