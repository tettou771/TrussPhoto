#pragma once

// =============================================================================
// VideoSeekBar.h - Video playback controls (play/pause + seek track + timecode)
// =============================================================================
// Extracted from SingleView's direct drawing code into RectNode children
// with automatic hit testing via the Node event system.
// =============================================================================

#include <TrussC.h>

using namespace std;
using namespace tc;

// --- PlayPauseButton: toggles between play (triangle) and pause (2 bars) ---

class PlayPauseButton : public RectNode {
public:
    Event<void> clicked;

    void setPlaying(bool p) { playing_ = p; }

    void setup() override { enableEvents(); }

    void draw() override {
        float cx = getWidth() / 2;
        float cy = getHeight() / 2;
        setColor(1, 1, 1, 0.9f);
        fill();
        if (playing_) {
            // Pause icon: two vertical bars
            drawRect(cx - 5, cy - 8, 4, 16);
            drawRect(cx + 1, cy - 8, 4, 16);
        } else {
            // Play icon: right-pointing triangle
            drawTriangle(cx - 4, cy - 8,
                         cx - 4, cy + 8,
                         cx + 8, cy);
        }
    }

    bool onMousePress(Vec2, int button) override {
        if (button == 0) clicked.notify();
        return true;
    }

private:
    bool playing_ = false;
};

// --- SeekTrack: draggable progress bar with knob + timecode display ---

class SeekTrack : public RectNode {
public:
    Event<float> seeked; // notifies new position (0.0-1.0)

    void setPosition(float pos) { position_ = clamp(pos, 0.0f, 1.0f); }
    void setDuration(float dur) { duration_ = dur; }

    void setup() override { enableEvents(); }

    void draw() override {
        float w = getWidth();
        float midY = getHeight() / 2;
        float trackW = w - rightMargin_;

        // Track background
        setColor(0.3f, 0.3f, 0.35f);
        fill();
        drawRect(0, midY - 2, trackW, 4);

        // Progress fill
        setColor(0.5f, 0.7f, 1.0f);
        drawRect(0, midY - 2, trackW * position_, 4);

        // Knob
        drawCircle(trackW * position_, midY, knobR_);

        // Timecode: "M:SS / M:SS" right-aligned
        float cur = position_ * duration_;
        string timeStr = formatTime(cur) + " / " + formatTime(duration_);
        setColor(0.8f, 0.8f, 0.85f);
        pushStyle();
        setTextAlign(Direction::Right, Direction::Center);
        drawBitmapString(timeStr, w - 10, midY);
        popStyle();
    }

    bool onMousePress(Vec2 pos, int button) override {
        if (button == 0) {
            dragging_ = true;
            seekToMouse(pos.x);
        }
        return true;
    }

    bool onMouseDrag(Vec2 pos, int) override {
        if (dragging_) seekToMouse(pos.x);
        return true;
    }

    bool onMouseRelease(Vec2, int) override {
        dragging_ = false;
        return true;
    }

private:
    float position_ = 0;
    float duration_ = 0;
    bool dragging_ = false;

    static constexpr float rightMargin_ = 100; // space for timecode
    static constexpr float knobR_ = 6.0f;

    void seekToMouse(float mx) {
        float trackW = getWidth() - rightMargin_;
        if (trackW <= 0) return;
        float pct = clamp(mx / trackW, 0.0f, 1.0f);
        position_ = pct;
        seeked.notify(pct);
        redraw();
    }

    static string formatTime(float seconds) {
        int s = (int)seconds;
        int m = s / 60;
        s = s % 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", m, s);
        return buf;
    }
};

// --- VideoSeekBar: container with play/pause button + seek track ---

class VideoSeekBar : public RectNode {
public:
    using Ptr = shared_ptr<VideoSeekBar>;

    Event<void> playPauseToggled;
    Event<float> seeked;

    void setPlaying(bool p) { playing_ = p; }
    void setPosition(float pos) { position_ = pos; }
    void setDuration(float dur) { duration_ = dur; }

    void setup() override {
        playPauseBtn_ = make_shared<PlayPauseButton>();
        playPauseBtn_->setRect(4, 4, 32, 32);
        addChild(playPauseBtn_);

        seekTrack_ = make_shared<SeekTrack>();
        addChild(seekTrack_);

        // Wire child events to our public events
        playPauseListener_ = playPauseBtn_->clicked.listen([this]() {
            playPauseToggled.notify();
        });
        seekListener_ = seekTrack_->seeked.listen([this](float& pct) {
            seeked.notify(pct);
        });

        layoutChildren();
    }

    void update() override {
        // Sync cached state to children
        playPauseBtn_->setPlaying(playing_);
        seekTrack_->setPosition(position_);
        seekTrack_->setDuration(duration_);

        layoutChildren();
    }

    void draw() override {
        // Semi-transparent black background
        setColor(0, 0, 0, 0.6f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());
    }

private:
    shared_ptr<PlayPauseButton> playPauseBtn_;
    shared_ptr<SeekTrack> seekTrack_;
    EventListener playPauseListener_;
    EventListener seekListener_;

    float playing_ = false;
    float position_ = 0;
    float duration_ = 0;

    void layoutChildren() {
        float w = getWidth();
        seekTrack_->setRect(44, 0, w - 44, getHeight());
    }
};
