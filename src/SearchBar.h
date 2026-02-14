#pragma once

// =============================================================================
// SearchBar.h - Search bar with IME text input for filtering photos
// =============================================================================

#include <TrussC.h>
#include <tcxIME.h>
#include "FolderTree.h"  // for loadJapaneseFont
using namespace std;
using namespace tc;

class SearchBar : public RectNode {
public:
    using Ptr = shared_ptr<SearchBar>;

    // Callbacks
    function<void(const string&)> onSearch;
    function<void()> onDeactivate;

    void setup() override {
        enableEvents();

        // Load font for IME
        loadJapaneseFont(labelFont_, 14);
        ime_.setFont(&labelFont_);
    }

    void activate() {
        if (active_) return;
        active_ = true;
        lastInputTime_ = getElapsedTimef();
        ime_.enable();
        redraw();
    }

    void deactivate() {
        if (!active_) return;
        active_ = false;
        ime_.disable();
        if (onDeactivate) onDeactivate();
        redraw();
    }

    bool isActive() const { return active_; }

    void clear() {
        ime_.clear();
        lastQuery_.clear();
        if (onSearch) onSearch("");
        redraw();
    }

    string getQuery() const {
        // const_cast needed because tcxIME::getString() isn't const
        return const_cast<tcxIME&>(ime_).getString();
    }

    void update() override {
        if (!active_) return;

        // Detect text changes (confirmed text)
        string current = getQuery();
        if (current != lastQuery_) {
            lastQuery_ = current;
            lastInputTime_ = getElapsedTimef();
            if (onSearch) onSearch(current);
        }

        // Detect composition changes (IME preedit) for redraw
        string marked = ime_.getMarkedText();
        if (marked != lastMarked_) {
            lastMarked_ = marked;
            lastInputTime_ = getElapsedTimef();
            redraw();
        }

        // Auto-deactivate after idle timeout (save battery from cursor blink)
        if (getElapsedTimef() - lastInputTime_ > idleTimeout_) {
            deactivate();
            return;
        }

        // Cursor blink: redraw on phase change (~2 redraws/sec)
        bool cursorOn = fmod(getElapsedTimef(), 1.0f) < 0.5f;
        if (cursorOn != lastCursorOn_) {
            lastCursorOn_ = cursorOn;
            redraw();
        }
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.1f, 0.1f, 0.12f);
        fill();
        drawRect(0, 0, w, h);

        // Bottom border
        setColor(0.2f, 0.2f, 0.22f);
        fill();
        drawRect(0, h - 1, w, 1);

        // Search icon / label
        float textY = h / 2.0f;
        float labelX = 12;

        setColor(0.45f, 0.45f, 0.5f);
        labelFont_.drawString("Search:", labelX, textY,
            Direction::Left, Direction::Center);

        float inputX = labelX + labelFont_.stringWidth("Search:") + 8;

        if (active_) {
            // Draw IME input
            setColor(1.0f, 1.0f, 1.0f);
            ime_.draw(inputX, textY - labelFont_.getAscent() / 2.0f);
        } else {
            // Draw current query (or placeholder)
            string q = getQuery();
            if (q.empty()) {
                setColor(0.35f, 0.35f, 0.4f);
                labelFont_.drawString("Type to filter...", inputX, textY,
                    Direction::Left, Direction::Center);
            } else {
                setColor(0.8f, 0.8f, 0.85f);
                labelFont_.drawString(q, inputX, textY,
                    Direction::Left, Direction::Center);
            }
        }
    }

    bool onMousePress(Vec2 localPos, int button) override {
        (void)localPos; (void)button;
        if (!active_) {
            activate();
        }
        return true;
    }

private:
    tcxIME ime_;
    Font labelFont_;
    bool active_ = false;
    bool lastCursorOn_ = false;
    float lastInputTime_ = 0;
    static constexpr float idleTimeout_ = 600.0f;  // 10 minutes
    string lastQuery_;
    string lastMarked_;
};
