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

    // Events
    Event<string> searched;
    Event<void> deactivated;

    // Parsed query: text part + location (@xxx) part
    struct ParsedQuery {
        string text;      // text part (@ tokens removed)
        string location;  // location part (after @, empty if no @ token)
    };

    static ParsedQuery parseQuery(const string& raw) {
        ParsedQuery result;
        string text;
        size_t i = 0;
        while (i < raw.size()) {
            // Skip leading spaces
            if (raw[i] == ' ') {
                if (!text.empty()) text += ' ';
                i++;
                continue;
            }
            if (raw[i] == '@') {
                // Extract location token: @ to next space or EOL
                i++; // skip @
                string loc;
                while (i < raw.size() && raw[i] != ' ') {
                    loc += raw[i++];
                }
                if (!loc.empty()) result.location = loc;
            } else {
                // Regular text character
                while (i < raw.size() && raw[i] != ' ') {
                    text += raw[i++];
                }
            }
        }
        // Trim trailing spaces
        while (!text.empty() && text.back() == ' ') text.pop_back();
        result.text = text;
        return result;
    }

    SearchBar() {
        textField_ = make_shared<tcxIME::TextField>();
        textField_->setEnableNewLine(false);
    }

    void setup() override {
        enableEvents();

        // Load font for label and IME
        loadJapaneseFont(labelFont_, 14);
        textField_->setFont(&labelFont_);

        // Compute input X offset and vertical centering
        inputX_ = 12 + labelFont_.stringWidth("Search:") + 8;
        updateTextFieldLayout();
        addChild(textField_);

        // Enter key triggers search
        textField_->getIME().onEnter = [this]() {
            string q = getQuery();
            searched.notify(q);
        };
    }

    void activate() {
        if (active_) return;
        active_ = true;
        lastInputTime_ = getElapsedTimef();
        textField_->enable();
        redraw();
    }

    void deactivate() {
        if (!active_) return;
        active_ = false;
        textField_->disable();
        deactivated.notify();
        redraw();
    }

    bool isActive() const { return active_; }

    void clear() {
        textField_->clear();
        lastQuery_.clear();
        string empty;
        searched.notify(empty);
        redraw();
    }

    string getQuery() const {
        return const_cast<tcxIME::TextField*>(textField_.get())->getString();
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        updateTextFieldLayout();
    }

    void update() override {
        if (!active_) return;

        // Detect text changes for idle timeout reset + redraw
        string current = getQuery();
        if (current != lastQuery_) {
            lastQuery_ = current;
            lastInputTime_ = getElapsedTimef();
            redraw();
        }

        // Detect composition changes (IME preedit) for redraw
        string marked = textField_->getMarkedText();
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

        // Search label (baseline-aligned with TextField)
        float labelX = 12;

        setColor(0.45f, 0.45f, 0.5f);
        labelFont_.drawString("Search:", labelX, baselineY_,
            Direction::Left, Direction::Baseline);

        if (active_) {
            // TextField draws itself as child node — just set color
            setColor(1.0f, 1.0f, 1.0f);
            textField_->setVisible(true);
        } else {
            textField_->setVisible(false);

            // Draw current query (or placeholder)
            string q = getQuery();
            if (q.empty()) {
                setColor(0.35f, 0.35f, 0.4f);
                labelFont_.drawString("Enter to search / @place", inputX_, baselineY_,
                    Direction::Left, Direction::Baseline);
            } else {
                setColor(0.8f, 0.8f, 0.85f);
                labelFont_.drawString(q, inputX_, baselineY_,
                    Direction::Left, Direction::Baseline);
            }
        }
    }

    bool onMousePress(Vec2 localPos, int button) override {
        (void)button;
        if (!active_) {
            activate();
        }
        lastInputTime_ = getElapsedTimef();
        return true;
    }

private:
    shared_ptr<tcxIME::TextField> textField_;
    Font labelFont_;
    float inputX_ = 0;
    float baselineY_ = 0;

    void updateTextFieldLayout() {
        if (!textField_) return;
        float h = getHeight();
        // Baseline at vertical center of bar (visually centered)
        baselineY_ = h / 2.0f + labelFont_.getAscent() / 2.0f;
        float textFieldY = baselineY_ - labelFont_.getAscent();
        textField_->setPos(inputX_, textFieldY);
        textField_->setSize(getWidth() - inputX_, h - textFieldY);
    }
    bool active_ = false;
    bool lastCursorOn_ = false;
    float lastInputTime_ = 0;
    static constexpr float idleTimeout_ = 600.0f;  // 10 minutes
    string lastQuery_;
    string lastMarked_;
};
