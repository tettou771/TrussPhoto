#pragma once

// =============================================================================
// NameEditOverlay.h - Modal text input overlay (shared UI component)
// Extracted from PeopleView for reuse in CollectionTree rename/create.
// =============================================================================

#include <TrussC.h>
#include <tcxIME.h>
#include "FolderTree.h"  // for loadJapaneseFont
using namespace std;
using namespace tc;

class NameEditOverlay : public RectNode {
public:
    Font* fontRef = nullptr;
    function<void(const string&)> onConfirm;
    function<void()> onCancel;
    string placeholder;

    NameEditOverlay() {
        textField_ = make_shared<tcxIME::TextField>();
        textField_->setEnableNewLine(false);
    }

    void setup() override {
        enableEvents();
        textField_->setFont(fontRef);
        addChild(textField_);

        // Enter confirms
        textField_->getIME().onEnter = [this]() {
            string text = textField_->getString();
            // Trim whitespace
            auto s = text.find_first_not_of(" \t\n\r");
            auto e = text.find_last_not_of(" \t\n\r");
            string trimmed = (s != string::npos) ? text.substr(s, e - s + 1) : "";
            hide();
            if (!trimmed.empty() && onConfirm) onConfirm(trimmed);
            else if (onCancel) onCancel();
        };
    }

    void show(const string& initialText, const string& placeholderText) {
        placeholder = placeholderText;
        textField_->clear();
        if (!initialText.empty()) {
            textField_->getIME().setString(initialText);
        }
        textField_->enable();
        setActive(true);
        updateDialogLayout();
    }

    void hide() {
        textField_->disable();
        setActive(false);
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        updateDialogLayout();
    }

    void update() override {
        bool cursorOn = fmod(getElapsedTimef(), 1.0f) < 0.5f;
        if (cursorOn != lastCursorOn_) {
            lastCursorOn_ = cursorOn;
            redraw();
        }
    }

    void draw() override {
        float w = getWidth(), h = getHeight();

        setColor(0, 0, 0, 0.6f);
        fill();
        drawRect(0, 0, w, h);

        float dlgX = (w - DLG_W) / 2;
        float dlgY = (h - DLG_H) / 2;

        setColor(0.15f, 0.15f, 0.18f);
        fill();
        drawRect(dlgX, dlgY, DLG_W, DLG_H);

        setColor(0.3f, 0.3f, 0.35f);
        noFill();
        drawRect(dlgX, dlgY, DLG_W, DLG_H);

        setColor(0.7f, 0.7f, 0.75f);
        if (fontRef) fontRef->drawString("Name:", dlgX + 12, dlgY + 24,
            Direction::Left, Direction::Center);

        float inputX = dlgX + 12;
        float inputY = dlgY + 40;
        float inputW = DLG_W - 24;
        float inputH = 28;

        setColor(0.1f, 0.1f, 0.12f);
        fill();
        drawRect(inputX, inputY, inputW, inputH);

        setColor(0.25f, 0.25f, 0.28f);
        noFill();
        drawRect(inputX, inputY, inputW, inputH);

        string text = textField_->getString();
        if (text.empty() && !placeholder.empty()) {
            setColor(0.4f, 0.4f, 0.45f);
            if (fontRef) fontRef->drawString(placeholder,
                inputX + 6, inputY + inputH / 2,
                Direction::Left, Direction::Center);
        }

        // TextField draws itself as child at the correct position
        setColor(1, 1, 1);

        setColor(0.4f, 0.4f, 0.45f);
        if (fontRef) fontRef->drawString("Enter to confirm, ESC to cancel",
            dlgX + DLG_W / 2, dlgY + DLG_H - 12,
            Direction::Center, Direction::Center);
    }

    bool onMousePress(Vec2 pos, int button) override {
        (void)pos; (void)button;
        return true;
    }

    bool onKeyPress(int key) override {
        if (key == 256 /* ESCAPE */) {
            if (onCancel) onCancel();
            return true;
        }
        return false;
    }

private:
    shared_ptr<tcxIME::TextField> textField_;
    bool lastCursorOn_ = false;

    static constexpr float DLG_W = 320;
    static constexpr float DLG_H = 100;

    void updateDialogLayout() {
        if (!textField_) return;
        float w = getWidth(), h = getHeight();
        float dlgX = (w - DLG_W) / 2;
        float dlgY = (h - DLG_H) / 2;
        float inputX = dlgX + 12 + 6;  // dialog left + padding + text inset
        float inputY = dlgY + 40 + 4;  // dialog top + label area + padding
        float inputW = DLG_W - 24 - 12;
        float inputH = 20;
        textField_->setPos(inputX, inputY);
        textField_->setSize(inputW, inputH);
    }
};
