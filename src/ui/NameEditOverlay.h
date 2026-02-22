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

    void setup() override {
        enableEvents();
        ime_.setFont(fontRef);

        // Intercept Enter at IME level (prevents newline insertion)
        ime_.onEnter = [this]() {
            string text = ime_.getString();
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
        ime_.clear();
        if (!initialText.empty()) {
            ime_.setString(initialText);
        }
        ime_.enable();
        setActive(true);
    }

    void hide() {
        ime_.disable();
        setActive(false);
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

        float dlgW = 320, dlgH = 100;
        float dlgX = (w - dlgW) / 2;
        float dlgY = (h - dlgH) / 2;

        setColor(0.15f, 0.15f, 0.18f);
        fill();
        drawRect(dlgX, dlgY, dlgW, dlgH);

        setColor(0.3f, 0.3f, 0.35f);
        noFill();
        drawRect(dlgX, dlgY, dlgW, dlgH);

        setColor(0.7f, 0.7f, 0.75f);
        if (fontRef) fontRef->drawString("Name:", dlgX + 12, dlgY + 24,
            Direction::Left, Direction::Center);

        float inputX = dlgX + 12;
        float inputY = dlgY + 40;
        float inputW = dlgW - 24;
        float inputH = 28;

        setColor(0.1f, 0.1f, 0.12f);
        fill();
        drawRect(inputX, inputY, inputW, inputH);

        setColor(0.25f, 0.25f, 0.28f);
        noFill();
        drawRect(inputX, inputY, inputW, inputH);

        string text = const_cast<tcxIME&>(ime_).getString();
        if (text.empty() && !placeholder.empty()) {
            setColor(0.4f, 0.4f, 0.45f);
            if (fontRef) fontRef->drawString(placeholder,
                inputX + 6, inputY + inputH / 2,
                Direction::Left, Direction::Center);
        }

        setColor(1, 1, 1);
        ime_.draw(inputX + 6, inputY + 4);

        setColor(0.4f, 0.4f, 0.45f);
        if (fontRef) fontRef->drawString("Enter to confirm, ESC to cancel",
            dlgX + dlgW / 2, dlgY + dlgH - 12,
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
        // Enter is handled by ime_.onEnter (no newline insertion)
        return false;
    }

private:
    tcxIME ime_;
    bool lastCursorOn_ = false;
};
