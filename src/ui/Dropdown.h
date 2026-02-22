#pragma once

// =============================================================================
// Dropdown.h - Reusable dropdown selector (Node-based popup)
// Follows ContextMenu.h pattern: Overlay + Popup + Items
// Uses setPopupParent() to escape ScrollContainer clipping.
// =============================================================================

#include <TrussC.h>
#include "FolderTree.h"  // for loadJapaneseFont
using namespace std;
using namespace tc;

class DropdownPopup;

struct DropdownOption {
    int id;
    string label;
};

// =============================================================================
// DropdownOverlay - Full-screen transparent backdrop that catches outside clicks
// =============================================================================
class DropdownOverlay : public RectNode {
public:
    using Ptr = shared_ptr<DropdownOverlay>;

    function<void()> onClick;

    DropdownOverlay() { enableEvents(); }

    void draw() override {} // invisible

protected:
    bool onMousePress(Vec2 local, int button) override {
        (void)local;
        if (onClick) onClick();
        return true;  // consume all clicks
    }
};

// =============================================================================
// DropdownItem - Single selectable row in popup
// =============================================================================
class DropdownItem : public RectNode {
public:
    using Ptr = shared_ptr<DropdownItem>;

    Event<int> clicked;
    bool selected = false;

    DropdownItem(int id, const string& label, Font* font)
        : id_(id), label_(label), font_(font) {
        setHeight(itemHeight_);
        enableEvents();
    }

    void update() override {
        if (prevHover_ != isMouseOver()) {
            prevHover_ = isMouseOver();
            redraw();
        }
    }

    void draw() override {
        float w = getWidth(), h = getHeight();

        // Hover highlight
        if (isMouseOver()) {
            setColor(0.28f, 0.45f, 0.72f);
            fill();
            drawRect(0, 0, w, h);
        }

        // Check mark for selected item (draw two lines: ✓)
        float cy = h / 2;
        if (selected) {
            float brightness = isMouseOver() ? 1.0f : 0.85f;
            setColor(brightness, brightness, brightness);
            noFill();
            drawLine(5, cy, 8, cy + 3);
            drawLine(8, cy + 3, 14, cy - 4);
        }

        // Label
        float brightness = isMouseOver() ? 1.0f : 0.85f;
        setColor(brightness, brightness, brightness);
        if (font_) font_->drawString(label_, checkWidth_, cy + 2, Left, Center);
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        (void)local;
        if (button != 0) return false;
        clicked.notify(id_);
        return true;
    }

private:
    int id_;
    string label_;
    Font* font_;
    bool prevHover_ = false;
    static constexpr float itemHeight_ = 24.0f;
    static constexpr float checkWidth_ = 20.0f;
};

// =============================================================================
// DropdownPopup - Popup container with vertical layout (shadow + border)
// =============================================================================
class DropdownPopup : public RectNode {
public:
    using Ptr = shared_ptr<DropdownPopup>;

    DropdownPopup() {
        enableEvents();
        auto* layout = addMod<LayoutMod>(LayoutDirection::Vertical, 1.0f);
        layout->setCrossAxis(AxisMode::Fill);
        layout->setMainAxis(AxisMode::Content);
        layout->setPadding(padding_);
    }

    // Set items, build children, wire click events
    void setItems(const vector<DropdownOption>& options, int selectedId,
                  Font* font, function<void(int)> onSelect) {
        for (auto& opt : options) {
            auto item = make_shared<DropdownItem>(opt.id, opt.label, font);
            item->selected = (opt.id == selectedId);
            listeners_.push_back(item->clicked.listen([onSelect](int& id) {
                if (onSelect) onSelect(id);
            }));
            addChild(item);
        }
    }

    // Call after addChild to parent — finalizes size and clamps to screen
    void finalizeLayout(float triggerY, float triggerH) {
        if (auto* lm = getMod<LayoutMod>()) {
            lm->updateLayout();
        }

        float screenH = getWindowHeight();
        float popupH = getHeight();

        // If popup goes below screen, flip above trigger
        if (triggerY + triggerH + popupH > screenH) {
            setY(getY() - triggerH - popupH);
        }

        // Clamp horizontal
        float screenW = getWindowWidth();
        if (getX() + getWidth() > screenW) setX(screenW - getWidth());
        if (getX() < 0) setX(0);

        // Clamp vertical
        if (getY() + popupH > screenH) setY(screenH - popupH);
        if (getY() < 0) setY(0);
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

    // Consume all clicks inside popup
    bool onMousePress(Vec2 local, int button) override {
        (void)local; (void)button;
        return true;
    }

private:
    float padding_ = 4.0f;
    vector<EventListener> listeners_;
};

// =============================================================================
// Dropdown - Trigger widget + popup manager
// =============================================================================
class Dropdown : public RectNode {
public:
    using Ptr = shared_ptr<Dropdown>;

    Event<int> selectionChanged;

    Dropdown(Font* font) : font_(font) {
        enableEvents();
    }

    void setOptions(const vector<DropdownOption>& options) {
        options_ = options;
    }

    void setSelectedId(int id) {
        selectedId_ = id;
        // Update label
        for (auto& opt : options_) {
            if (opt.id == id) { selectedLabel_ = opt.label; break; }
        }
    }

    int selectedId() const { return selectedId_; }

    // Popup is added to this parent (should be outside ScrollContainer)
    void setPopupParent(RectNode* p) { popupParent_ = p; }

    void draw() override {
        float w = getWidth(), h = getHeight();

        // Background
        setColor(0.15f, 0.15f, 0.17f);
        fill();
        drawRect(0, 0, w, h);

        // Border
        setColor(0.3f, 0.3f, 0.32f);
        noFill();
        drawRect(0, 0, w, h);

        // Label
        setColor(0.8f, 0.8f, 0.85f);
        if (font_) font_->drawString(selectedLabel_, 12, h / 2 + 2, Left, Center);

        // Down arrow triangle (▾)
        setColor(0.5f, 0.5f, 0.55f);
        fill();
        float ax = w - 16, ay = h / 2;
        drawTriangle(ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3);
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        (void)local;
        if (button != 0) return false;
        if (isOpen_) closePopup();
        else openPopup();
        return true;
    }

private:
    Font* font_;
    vector<DropdownOption> options_;
    int selectedId_ = 0;
    string selectedLabel_;
    RectNode* popupParent_ = nullptr;
    bool isOpen_ = false;

    DropdownOverlay::Ptr overlay_;
    DropdownPopup::Ptr popup_;

    void openPopup() {
        if (!popupParent_ || options_.empty()) return;
        isOpen_ = true;

        // Overlay (full screen, catches outside clicks)
        overlay_ = make_shared<DropdownOverlay>();
        overlay_->setRect(0, 0, getWindowWidth(), getWindowHeight());
        overlay_->onClick = [this]() { closePopup(); };

        // Popup position: below trigger, in popupParent_ coordinates
        auto globalPos = localToGlobal(Vec3(0, getHeight(), 0));
        auto localPos = popupParent_->globalToLocal(globalPos);

        popup_ = make_shared<DropdownPopup>();
        popup_->setX(localPos.x);
        popup_->setY(localPos.y);
        popup_->setWidth(getWidth());

        popup_->setItems(options_, selectedId_, font_, [this](int id) {
            selectedId_ = id;
            for (auto& opt : options_) {
                if (opt.id == id) { selectedLabel_ = opt.label; break; }
            }
            selectionChanged.notify(id);
            closePopup();
        });

        popupParent_->addChild(overlay_);
        popupParent_->addChild(popup_);

        // Finalize after added to tree (needs window size)
        popup_->finalizeLayout(localPos.y - getHeight(), getHeight());

        redraw();
    }

    void closePopup() {
        if (!isOpen_) return;
        isOpen_ = false;

        if (popup_) { popup_->destroy(); popup_ = nullptr; }
        if (overlay_) { overlay_->destroy(); overlay_ = nullptr; }

        redraw();
    }
};
