#pragma once

// =============================================================================
// CropPanel.h - Right sidebar for crop controls (aspect ratio, preview, etc.)
// =============================================================================

#include <TrussC.h>

using namespace std;
using namespace tc;

class CropPanel : public RectNode {
public:
    using Ptr = shared_ptr<CropPanel>;

    // Aspect ratio presets
    enum Aspect { Original, A16_9, A4_3, A3_2, A1_1, A5_4, Free };

    // Callbacks
    function<void(Aspect)> onAspectChanged;
    function<void(bool)> onOrientationChanged;  // true = landscape
    function<void()> onReset;
    function<void()> onDone;
    function<void()> onCancel;

    CropPanel() = default;

    void setup() override {
        enableEvents();
    }

    Aspect aspect() const { return currentAspect_; }
    bool isLandscape() const { return isLandscape_; }
    void setOrientation(bool landscape) { isLandscape_ = landscape; }

    // Set the preview texture (borrowed from CropView, updated each frame)
    void setPreviewInfo(sg_view view, sg_sampler sampler,
                        float u0, float v0, float u1, float v1,
                        int outputW, int outputH) {
        previewView_ = view;
        previewSampler_ = sampler;
        previewU0_ = u0; previewV0_ = v0;
        previewU1_ = u1; previewV1_ = v1;
        outputW_ = outputW;
        outputH_ = outputH;
        hasPreview_ = true;
    }

    void clearPreview() { hasPreview_ = false; }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.09f, 0.09f, 0.11f);
        fill();
        drawRect(0, 0, w, h);

        // Left border
        setColor(0.2f, 0.2f, 0.22f);
        noFill();
        drawLine(0, 0, 0, h);

        float y = 9;
        float pad = 12;

        // --- Preview ---
        setColor(0.45f, 0.45f, 0.5f);
        drawBitmapString("Preview", pad, y);
        y += 13;

        float previewAreaW = w - pad * 2;
        float previewAreaH = previewAreaW * 0.6f; // max aspect area

        if (hasPreview_) {
            float cropAspect = (float)outputW_ / max(1, outputH_);
            float fitW, fitH;
            if (cropAspect > previewAreaW / previewAreaH) {
                fitW = previewAreaW;
                fitH = previewAreaW / cropAspect;
            } else {
                fitH = previewAreaH;
                fitW = previewAreaH * cropAspect;
            }
            float px = pad + (previewAreaW - fitW) / 2;
            float py = y + (previewAreaH - fitH) / 2;

            // Draw preview texture with UV crop
            setColor(1, 1, 1);
            sgl_enable_texture();
            sgl_texture(previewView_, previewSampler_);
            Color col = getDefaultContext().getColor();
            sgl_begin_quads();
            sgl_c4f(col.r, col.g, col.b, col.a);
            sgl_v2f_t2f(px, py, previewU0_, previewV0_);
            sgl_v2f_t2f(px + fitW, py, previewU1_, previewV0_);
            sgl_v2f_t2f(px + fitW, py + fitH, previewU1_, previewV1_);
            sgl_v2f_t2f(px, py + fitH, previewU0_, previewV1_);
            sgl_end();
            sgl_disable_texture();
        } else {
            setColor(0.15f, 0.15f, 0.17f);
            fill();
            drawRect(pad, y, previewAreaW, previewAreaH);
        }
        y += previewAreaH + 12;

        // Separator
        setColor(0.25f, 0.25f, 0.28f);
        drawLine(pad, y, w - pad, y);
        y += 8;

        // --- Orientation toggle (Landscape / Portrait) ---
        bool grayed = (currentAspect_ == Free || currentAspect_ == A1_1);
        {
            float totalW = orientBtnW_ * 2 + 8;
            float startX = pad + (w - pad * 2 - totalW) / 2;
            orientLandBtnX_ = startX;
            orientBtnY_ = y;

            // Landscape button
            {
                bool sel = isLandscape_ && !grayed;
                setColor(sel ? Color(0.2f, 0.35f, 0.55f) : Color(0.15f, 0.15f, 0.17f));
                fill();
                drawRect(startX, y, orientBtnW_, orientBtnH_);
                setColor(grayed ? Color(0.25f, 0.25f, 0.28f) :
                         (sel ? Color(0.9f, 0.9f, 0.95f) : Color(0.5f, 0.5f, 0.55f)));
                noFill();
                float iw = 20, ih = 14;
                drawRect(startX + (orientBtnW_ - iw) / 2,
                         y + (orientBtnH_ - ih) / 2, iw, ih);
            }

            // Portrait button
            {
                float px = startX + orientBtnW_ + 8;
                orientPortBtnX_ = px;
                bool sel = !isLandscape_ && !grayed;
                setColor(sel ? Color(0.2f, 0.35f, 0.55f) : Color(0.15f, 0.15f, 0.17f));
                fill();
                drawRect(px, y, orientBtnW_, orientBtnH_);
                setColor(grayed ? Color(0.25f, 0.25f, 0.28f) :
                         (sel ? Color(0.9f, 0.9f, 0.95f) : Color(0.5f, 0.5f, 0.55f)));
                noFill();
                float iw = 14, ih = 20;
                drawRect(px + (orientBtnW_ - iw) / 2,
                         y + (orientBtnH_ - ih) / 2, iw, ih);
            }
            y += orientBtnH_ + 8;
        }

        // --- Aspect Ratio ---
        setColor(0.45f, 0.45f, 0.5f);
        drawBitmapString("Aspect Ratio", pad, y);
        y += 13;

        const char* labels[] = { "Original", "16:9", "4:3", "3:2", "1:1", "5:4", "Free" };
        aspectButtonY_ = y;
        for (int i = 0; i < 7; i++) {
            bool selected = (i == (int)currentAspect_);
            float btnH = 26;

            if (selected) {
                setColor(0.2f, 0.35f, 0.55f);
                fill();
                drawRect(pad, y, w - pad * 2, btnH);
            }

            setColor(selected ? Color(0.9f, 0.9f, 0.95f) : Color(0.6f, 0.6f, 0.65f));
            drawBitmapString(labels[i], pad + 10, y + (btnH - 12) / 2);
            y += btnH + 2;
        }

        y += 8;

        // Separator
        setColor(0.25f, 0.25f, 0.28f);
        drawLine(pad, y, w - pad, y);
        y += 8;

        // --- Output size ---
        if (outputW_ > 0 && outputH_ > 0) {
            setColor(0.45f, 0.45f, 0.5f);
            drawBitmapString("Output", pad, y);
            y += 13;
            setColor(0.55f, 0.55f, 0.6f);
            string sizeStr = to_string(outputW_) + " x " + to_string(outputH_);
            drawBitmapString(sizeStr, pad + 10, y);
            y += 13;
        }

        y += 8;

        // --- Buttons ---
        float btnW = w - pad * 2;
        float btnH = 30;

        // Reset button
        resetBtnY_ = y;
        setColor(0.18f, 0.18f, 0.2f);
        fill();
        drawRect(pad, y, btnW, btnH);
        setColor(0.7f, 0.7f, 0.75f);
        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString("Reset", pad + btnW / 2, y + btnH / 2);
        popStyle();
        y += btnH + 6;

        // Cancel button
        cancelBtnY_ = y;
        setColor(0.18f, 0.18f, 0.2f);
        fill();
        drawRect(pad, y, btnW, btnH);
        setColor(0.7f, 0.7f, 0.75f);
        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString("Cancel", pad + btnW / 2, y + btnH / 2);
        popStyle();
        y += btnH + 6;

        // Done button
        doneBtnY_ = y;
        setColor(0.2f, 0.4f, 0.65f);
        fill();
        drawRect(pad, y, btnW, btnH);
        setColor(0.95f, 0.95f, 1.0f);
        pushStyle();
        setTextAlign(Direction::Center, Direction::Center);
        drawBitmapString("Done", pad + btnW / 2, y + btnH / 2);
        popStyle();
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;
        float pad = 12;
        float w = getWidth();
        float btnW = w - pad * 2;
        float btnH = 30;

        // Orientation buttons
        {
            bool grayed = (currentAspect_ == Free || currentAspect_ == A1_1);
            if (!grayed) {
                if (pos.x >= orientLandBtnX_ && pos.x <= orientLandBtnX_ + orientBtnW_ &&
                    pos.y >= orientBtnY_ && pos.y <= orientBtnY_ + orientBtnH_) {
                    if (!isLandscape_) {
                        isLandscape_ = true;
                        if (onOrientationChanged) onOrientationChanged(true);
                    }
                    return true;
                }
                if (pos.x >= orientPortBtnX_ && pos.x <= orientPortBtnX_ + orientBtnW_ &&
                    pos.y >= orientBtnY_ && pos.y <= orientBtnY_ + orientBtnH_) {
                    if (isLandscape_) {
                        isLandscape_ = false;
                        if (onOrientationChanged) onOrientationChanged(false);
                    }
                    return true;
                }
            }
        }

        // Aspect ratio buttons
        float y = aspectButtonY_;
        for (int i = 0; i < 7; i++) {
            float h = 26;
            if (pos.x >= pad && pos.x <= pad + btnW &&
                pos.y >= y && pos.y <= y + h) {
                currentAspect_ = (Aspect)i;
                if (onAspectChanged) onAspectChanged(currentAspect_);
                return true;
            }
            y += h + 2;
        }

        // Reset button
        if (pos.x >= pad && pos.x <= pad + btnW &&
            pos.y >= resetBtnY_ && pos.y <= resetBtnY_ + btnH) {
            if (onReset) onReset();
            return true;
        }

        // Cancel button
        if (pos.x >= pad && pos.x <= pad + btnW &&
            pos.y >= cancelBtnY_ && pos.y <= cancelBtnY_ + btnH) {
            if (onCancel) onCancel();
            return true;
        }

        // Done button
        if (pos.x >= pad && pos.x <= pad + btnW &&
            pos.y >= doneBtnY_ && pos.y <= doneBtnY_ + btnH) {
            if (onDone) onDone();
            return true;
        }

        return true; // consume all clicks in panel
    }

private:
    Aspect currentAspect_ = Aspect::Original;
    bool isLandscape_ = true;

    // Preview
    bool hasPreview_ = false;
    sg_view previewView_ = {};
    sg_sampler previewSampler_ = {};
    float previewU0_ = 0, previewV0_ = 0, previewU1_ = 1, previewV1_ = 1;
    int outputW_ = 0, outputH_ = 0;

    // Orientation button layout
    static constexpr float orientBtnW_ = 36;
    static constexpr float orientBtnH_ = 28;
    float orientLandBtnX_ = 0, orientPortBtnX_ = 0, orientBtnY_ = 0;

    // Button Y positions (computed in draw)
    float aspectButtonY_ = 0;
    float resetBtnY_ = 0;
    float cancelBtnY_ = 0;
    float doneBtnY_ = 0;
};
