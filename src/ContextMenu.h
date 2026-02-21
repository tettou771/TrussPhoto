#pragma once

// =============================================================================
// ContextMenu.h - Right-click context menu (Node-based popup)
// =============================================================================

#include <TrussC.h>
#include "FolderTree.h"  // for loadJapaneseFont
using namespace std;
using namespace tc;

class ContextMenu;

// =============================================================================
// MenuOverlay - Full-screen transparent backdrop that catches outside clicks
// =============================================================================
class MenuOverlay : public RectNode {
public:
    using Ptr = shared_ptr<MenuOverlay>;

    function<void()> onClick;

    MenuOverlay() { enableEvents(); }

    void draw() override {} // invisible

protected:
    bool onMousePress(Vec2 local, int button) override {
        if (onClick) onClick();
        // Consume left-clicks (prevent click-through to grid)
        // Pass right-clicks through (allow new context menu)
        return button == 0;
    }
};

// =============================================================================
// MenuItem - Clickable row in a context menu
// =============================================================================
class MenuItem : public RectNode {
public:
    using Ptr = shared_ptr<MenuItem>;

    MenuItem(const string& label, function<void()> action = nullptr)
        : label_(label), action_(action) {
        setHeight(itemHeight_);
        loadJapaneseFont(font_, (int)(itemHeight_ - textPadY_ * 2));
        enableEvents();
    }

    void setup() override {
        menu_ = dynamic_pointer_cast<ContextMenu>(getParent());
    }

    void update() override {
        if (prevHover_ != isMouseOver()) {
            prevHover_ = isMouseOver();
            redraw();
        }
    }

    void draw() override {
        float w = getWidth(), h = getHeight();

        // Hover background
        if (isMouseOver()) {
            setColor(0.28f, 0.45f, 0.72f);
            fill();
            drawRect(0, 0, w, h);
        }

        // Label text
        float brightness = isMouseOver() ? 1.0f : 0.85f;
        setColor(brightness, brightness, brightness);
        font_.drawString(label_, textPadX_, textPadY_,
            Direction::Left, Direction::Top);
    }

    bool onMousePress(Vec2 local, int button) override;  // defined after ContextMenu

private:
    string label_;
    function<void()> action_;
    shared_ptr<ContextMenu> menu_;
    Font font_;
    bool prevHover_ = false;
    static constexpr float itemHeight_ = 20.0f;
    static constexpr float textPadX_ = 6.0f;
    static constexpr float textPadY_ = 3.0f;
};

// =============================================================================
// MenuSeparator - Horizontal line between menu sections
// =============================================================================
class MenuSeparator : public RectNode {
public:
    using Ptr = shared_ptr<MenuSeparator>;

    MenuSeparator() {
        setHeight(sepHeight_);
    }

    void draw() override {
        float w = getWidth();
        float margin = 5.0f;
        setColor(0.35f, 0.35f, 0.38f);
        fill();
        drawRect(margin, sepHeight_ / 2.0f, w - margin * 2, 1);
    }

private:
    static constexpr float sepHeight_ = 9.0f;
};

// =============================================================================
// ContextMenu - Popup container with vertical layout
// =============================================================================
class ContextMenu : public RectNode {
public:
    using Ptr = shared_ptr<ContextMenu>;

    // Callback for menu dismissal (tcApp clears its pointer)
    function<void()> onClose;

    ContextMenu() {
        setWidth(defaultWidth_);
        enableEvents();

        // LayoutMod in constructor (safe: doesn't use weak_from_this)
        auto* layout = addMod<LayoutMod>(LayoutDirection::Vertical, 2.0f);
        layout->setCrossAxis(AxisMode::Fill);     // children width = menu width
        layout->setMainAxis(AxisMode::Content);   // menu height = sum of children
        layout->setPadding(padding_);
    }

    // Call after all items are added and position is set
    void finalizeLayout() {
        if (auto* lm = getMod<LayoutMod>()) {
            lm->updateLayout();
        }

        // Clamp to screen bounds
        float screenW = getWindowWidth();
        float screenH = getWindowHeight();
        if (getX() + getWidth() > screenW) setX(screenW - getWidth());
        if (getY() + getHeight() > screenH) setY(screenH - getHeight());
        if (getX() < 0) setX(0);
        if (getY() < 0) setY(0);
    }

    void close() {
        if (onClose) onClose();
    }

    void draw() override {
        float w = getWidth(), h = getHeight();

        // Shadow
        setColor(0.0f, 0.0f, 0.0f, 0.25f);
        fill();
        drawRect(3, 3, w, h);

        // Background
        setColor(0.18f, 0.18f, 0.20f);
        fill();
        drawRect(0, 0, w, h);

        // Border
        setColor(0.3f, 0.3f, 0.32f);
        noFill();
        drawRect(0, 0, w, h);
    }

    // Consume all clicks inside menu (prevent pass-through)
    bool onMousePress(Vec2 local, int button) override {
        return true;
    }

private:
    static constexpr float defaultWidth_ = 200.0f;
    static constexpr float padding_ = 4.0f;
};

// =============================================================================
// MenuItem::onMousePress (needs ContextMenu to be fully defined)
// =============================================================================
inline bool MenuItem::onMousePress(Vec2 local, int button) {
    if (button == 0) {
        if (action_) action_();
        if (menu_) menu_->close();
        return true;
    }
    return false;
}

// =============================================================================
// Helper: reveal file in Finder (macOS)
// =============================================================================
inline void revealInFinder(const string& path) {
    // Shell-safe: single-quote the path, escape embedded single quotes
    string escaped;
    for (char c : path) {
        if (c == '\'') escaped += "'\\''";
        else escaped += c;
    }
    system(("open -R '" + escaped + "'").c_str());
}
