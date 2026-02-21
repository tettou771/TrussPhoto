#pragma once

// =============================================================================
// CropView.h - Interactive crop editing view
// =============================================================================
// Displays the developed FBO image with a draggable crop rectangle overlay.
// Left area: image + crop overlay. Right 220px: CropPanel.
// =============================================================================

#include <TrussC.h>
#include "ViewContainer.h"
#include "SingleView.h"
#include "CropPanel.h"
#include "CropTypes.h"

using namespace std;
using namespace tc;

// Flip threshold: TAU/8 * this value (~47°). Slightly above 45° for hysteresis.
static constexpr float kFlipThreshold = 1.05f;

class CropView : public ViewContainer {
public:
    using Ptr = shared_ptr<CropView>;

    // Drag handle identification
    enum class DragMode {
        None, Move,
        TL, T, TR, L, R, BL, B, BR
    };

    // Undo history entry
    struct CropState {
        float x, y, w, h;
    };

    void beginView(ViewContext& ctx) override {
        ctx_ = &ctx;
    }

    void endView() override {
        singleView_ = nullptr;
        ctx_ = nullptr;
        undoStack_.clear();
    }

    bool wantsSearchBar() const override { return false; }
    bool wantsLeftSidebar() const override { return false; }

    CropView() {
        // Create panel in constructor so it's available before setup()
        panel_ = make_shared<CropPanel>();

        panel_->onAspectChanged = [this](CropAspect a) {
            applyAspect(a);
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        };
        panel_->onOrientationChanged = [this](bool landscape) {
            isLandscape_ = landscape;
            applyAspect(panel_->aspect());
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        };
        panel_->onReset = [this]() {
            pushUndo();
            cropX_ = 0; cropY_ = 0; cropW_ = 1; cropH_ = 1;
            isLandscape_ = (originalAspect_ >= 1.0f);
            panel_->setOrientation(isLandscape_);
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        };
        panel_->onDone = [this]() {
            commitCrop();
            if (onDone_) onDone_();
        };
        panel_->onCancel = [this]() {
            cancelCrop();
            if (onDone_) onDone_();
        };
    }

    void setup() override {
        enableEvents();
        setClipping(true);
        addChild(panel_);
    }

    void setSingleView(shared_ptr<SingleView> sv) {
        singleView_ = sv;
    }

    // Called when entering crop mode from SingleView
    void enterCrop() {
        if (!singleView_ || !singleView_->hasFbo()) return;
        if (!ctx_) return;

        // Borrow FBO handles
        fboView_ = singleView_->fboView();
        fboSampler_ = singleView_->fboSampler();
        fboW_ = singleView_->displayWidth();
        fboH_ = singleView_->displayHeight();
        if (fboW_ <= 0 || fboH_ <= 0) {
            fboW_ = singleView_->fboWidth();
            fboH_ = singleView_->fboHeight();
        }
        originalAspect_ = (float)fboW_ / max(1, fboH_);

        // Load current crop from entry
        string pid = singleView_->currentPhotoId();
        auto* entry = ctx_->provider->getPhoto(pid);
        if (entry) {
            cropX_ = entry->userCropX;
            cropY_ = entry->userCropY;
            cropW_ = entry->userCropW;
            cropH_ = entry->userCropH;
        } else {
            cropX_ = 0; cropY_ = 0; cropW_ = 1; cropH_ = 1;
        }

        // Determine initial orientation from crop shape
        float pixelCropW = cropW_ * fboW_;
        float pixelCropH = cropH_ * fboH_;
        isLandscape_ = (pixelCropW >= pixelCropH);
        panel_->setOrientation(isLandscape_);

        // Save initial state for cancel
        initialCrop_ = { cropX_, cropY_, cropW_, cropH_ };
        undoStack_.clear();
    }

    // Save crop to DB (Done / Enter)
    void commitCrop() {
        if (!ctx_ || !singleView_) return;
        string pid = singleView_->currentPhotoId();
        if (!pid.empty()) {
            ctx_->provider->setUserCrop(pid, cropX_, cropY_, cropW_, cropH_);
        }
    }

    // Revert to initial crop (Cancel / ESC)
    void cancelCrop() {
        cropX_ = initialCrop_.x;
        cropY_ = initialCrop_.y;
        cropW_ = initialCrop_.w;
        cropH_ = initialCrop_.h;
        // Also save reverted state to DB
        commitCrop();
    }

    // Check if crop has been modified from initial state
    bool hasChanges() const {
        return cropX_ != initialCrop_.x || cropY_ != initialCrop_.y ||
               cropW_ != initialCrop_.w || cropH_ != initialCrop_.h;
    }

    // Undo last drag operation (Cmd+Z)
    void undo() {
        if (undoStack_.empty()) return;
        auto s = undoStack_.back();
        undoStack_.pop_back();
        cropX_ = s.x; cropY_ = s.y;
        cropW_ = s.w; cropH_ = s.h;
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    // Callbacks
    function<void()> onDone_;

    // Handle key input (Cmd+Z is handled by tcApp)
    bool handleKey(int key) {
        (void)key;
        return false;
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.07f, 0.07f, 0.09f);
        fill();
        drawRect(0, 0, w, h);

        // Layout: panel on right
        float panelW = 220;
        float imgAreaW = w - panelW;

        panel_->setRect(imgAreaW, 0, panelW, h);

        // Image area: fit FBO image
        if (fboW_ <= 0 || fboH_ <= 0) return;

        float padding = 40;
        float availW = imgAreaW - padding * 2;
        float availH = h - padding * 2;

        float imgAspect = (float)fboW_ / fboH_;
        float fitScale = min(availW / fboW_, availH / fboH_);
        float drawW = fboW_ * fitScale;
        float drawH = fboH_ * fitScale;
        float imgX = padding + (availW - drawW) / 2;
        float imgY = padding + (availH - drawH) / 2;

        // Store for hit testing
        imgRect_ = { imgX, imgY, drawW, drawH };

        // Draw full image (dimmed)
        setColor(0.6f, 0.6f, 0.6f);
        sgl_enable_texture();
        sgl_texture(fboView_, fboSampler_);
        Color col = getDefaultContext().getColor();
        sgl_begin_quads();
        sgl_c4f(col.r, col.g, col.b, col.a);
        sgl_v2f_t2f(imgX, imgY, 0, 0);
        sgl_v2f_t2f(imgX + drawW, imgY, 1, 0);
        sgl_v2f_t2f(imgX + drawW, imgY + drawH, 1, 1);
        sgl_v2f_t2f(imgX, imgY + drawH, 0, 1);
        sgl_end();
        sgl_disable_texture();

        // Crop rectangle in screen coords
        float cx = imgX + cropX_ * drawW;
        float cy = imgY + cropY_ * drawH;
        float cw = cropW_ * drawW;
        float ch = cropH_ * drawH;

        // Dark overlay (4 rects around crop)
        setColor(0, 0, 0, 0.45f);
        fill();
        drawRect(imgX, imgY, drawW, cy - imgY);                         // top
        drawRect(imgX, cy + ch, drawW, (imgY + drawH) - (cy + ch));     // bottom
        drawRect(imgX, cy, cx - imgX, ch);                               // left
        drawRect(cx + cw, cy, (imgX + drawW) - (cx + cw), ch);          // right

        // Draw crop area at full brightness
        setColor(1, 1, 1);
        sgl_enable_texture();
        sgl_texture(fboView_, fboSampler_);
        col = getDefaultContext().getColor();
        sgl_begin_quads();
        sgl_c4f(col.r, col.g, col.b, col.a);
        sgl_v2f_t2f(cx, cy, cropX_, cropY_);
        sgl_v2f_t2f(cx + cw, cy, cropX_ + cropW_, cropY_);
        sgl_v2f_t2f(cx + cw, cy + ch, cropX_ + cropW_, cropY_ + cropH_);
        sgl_v2f_t2f(cx, cy + ch, cropX_, cropY_ + cropH_);
        sgl_end();
        sgl_disable_texture();

        // Rule of thirds grid
        setColor(1, 1, 1, 0.25f);
        noFill();
        for (int i = 1; i <= 2; i++) {
            float gx = cx + cw * i / 3.0f;
            float gy = cy + ch * i / 3.0f;
            drawLine(gx, cy, gx, cy + ch);
            drawLine(cx, gy, cx + cw, gy);
        }

        // Crop border
        setColor(1, 1, 1, 0.8f);
        noFill();
        drawRect(cx, cy, cw, ch);

        // 8 handles
        float hs = handleSize_;
        setColor(1, 1, 1, 0.9f);
        fill();
        // Corners
        drawRect(cx - hs, cy - hs, hs * 2, hs * 2);
        drawRect(cx + cw - hs, cy - hs, hs * 2, hs * 2);
        drawRect(cx - hs, cy + ch - hs, hs * 2, hs * 2);
        drawRect(cx + cw - hs, cy + ch - hs, hs * 2, hs * 2);
        // Edge midpoints
        drawRect(cx + cw / 2 - hs, cy - hs, hs * 2, hs * 2);
        drawRect(cx + cw / 2 - hs, cy + ch - hs, hs * 2, hs * 2);
        drawRect(cx - hs, cy + ch / 2 - hs, hs * 2, hs * 2);
        drawRect(cx + cw - hs, cy + ch / 2 - hs, hs * 2, hs * 2);

        // Update panel preview
        int outputW = (int)round(fboW_ * cropW_);
        int outputH = (int)round(fboH_ * cropH_);
        panel_->setPreviewInfo(fboView_, fboSampler_,
                               cropX_, cropY_,
                               cropX_ + cropW_, cropY_ + cropH_,
                               outputW, outputH);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;

        // Check if inside image area
        float cx = imgRect_.x + cropX_ * imgRect_.w;
        float cy = imgRect_.y + cropY_ * imgRect_.h;
        float cw = cropW_ * imgRect_.w;
        float ch = cropH_ * imgRect_.h;

        float hs = handleSize_ * 1.5f; // larger hit area

        // Test handles (corners first, then edges, then interior)
        dragMode_ = DragMode::None;

        // Corners
        if (hitTest(pos, cx, cy, hs))                dragMode_ = DragMode::TL;
        else if (hitTest(pos, cx + cw, cy, hs))      dragMode_ = DragMode::TR;
        else if (hitTest(pos, cx, cy + ch, hs))       dragMode_ = DragMode::BL;
        else if (hitTest(pos, cx + cw, cy + ch, hs))  dragMode_ = DragMode::BR;
        // Edge midpoints
        else if (hitTest(pos, cx + cw/2, cy, hs))     dragMode_ = DragMode::T;
        else if (hitTest(pos, cx + cw/2, cy + ch, hs)) dragMode_ = DragMode::B;
        else if (hitTest(pos, cx, cy + ch/2, hs))      dragMode_ = DragMode::L;
        else if (hitTest(pos, cx + cw, cy + ch/2, hs)) dragMode_ = DragMode::R;
        // Interior
        else if (pos.x >= cx && pos.x <= cx + cw &&
                 pos.y >= cy && pos.y <= cy + ch) {
            dragMode_ = DragMode::Move;
        }

        if (dragMode_ != DragMode::None) {
            pushUndo();
            dragStart_ = pos;
            dragStartCrop_ = { cropX_, cropY_, cropW_, cropH_ };
            return true;
        }

        return false;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (button != 0 || dragMode_ == DragMode::None) return false;

        float dx = (pos.x - dragStart_.x) / imgRect_.w;
        float dy = (pos.y - dragStart_.y) / imgRect_.h;

        auto& s = dragStartCrop_;
        bool locked = panel_->aspect() != CropAspect::Free;

        if (dragMode_ == DragMode::Move) {
            cropX_ = clamp(s.x + dx, 0.0f, 1.0f - cropW_);
            cropY_ = clamp(s.y + dy, 0.0f, 1.0f - cropH_);
        } else {
            constexpr float minSize = 0.02f;
            bool isEdge = (dragMode_ == DragMode::T || dragMode_ == DragMode::B ||
                           dragMode_ == DragMode::L || dragMode_ == DragMode::R);

            if (isEdge) {
                // Edge drag: drag axis — dragged side follows, opposite fixed.
                // Orthogonal axis — symmetric from center (when AR locked).
                float nx = s.x, ny = s.y, nw = s.w, nh = s.h;
                bool isHoriz = (dragMode_ == DragMode::L || dragMode_ == DragMode::R);

                // Drag axis: one side follows mouse, opposite anchored
                if (dragMode_ == DragMode::L) { nx = s.x + dx; nw = s.w - dx; }
                if (dragMode_ == DragMode::R) { nw = s.w + dx; }
                if (dragMode_ == DragMode::T) { ny = s.y + dy; nh = s.h - dy; }
                if (dragMode_ == DragMode::B) { nh = s.h + dy; }

                nw = max(nw, minSize);
                nh = max(nh, minSize);
                if (dragMode_ == DragMode::L && nw <= minSize) nx = s.x + s.w - minSize;
                if (dragMode_ == DragMode::T && nh <= minSize) ny = s.y + s.h - minSize;

                // Orthogonal axis: symmetric from center (AR locked)
                if (locked) {
                    float targetAR = getTargetAspectNorm();
                    if (targetAR > 0) {
                        if (isHoriz) {
                            // Width changed -> adjust height symmetrically
                            float cy = s.y + s.h / 2;
                            nh = nw / targetAR;
                            float maxH = min(cy, 1.0f - cy) * 2;
                            if (nh > maxH) { nh = maxH; nw = nh * targetAR; }
                            ny = cy - nh / 2;
                            if (dragMode_ == DragMode::L) nx = s.x + s.w - nw;
                        } else {
                            // Height changed -> adjust width symmetrically
                            float cx = s.x + s.w / 2;
                            nw = nh * targetAR;
                            float maxW = min(cx, 1.0f - cx) * 2;
                            if (nw > maxW) { nw = maxW; nh = nw / targetAR; }
                            nx = cx - nw / 2;
                            if (dragMode_ == DragMode::T) ny = s.y + s.h - nh;
                        }
                    }
                }

                // Clamp to image bounds
                nx = clamp(nx, 0.0f, 1.0f - minSize);
                ny = clamp(ny, 0.0f, 1.0f - minSize);
                nw = clamp(nw, minSize, 1.0f - nx);
                nh = clamp(nh, minSize, 1.0f - ny);

                cropX_ = nx; cropY_ = ny;
                cropW_ = nw; cropH_ = nh;
            } else {
                // Corner drag: anchor opposite corner
                float nx = s.x, ny = s.y, nw = s.w, nh = s.h;

                bool moveLeft = (dragMode_ == DragMode::TL || dragMode_ == DragMode::BL);
                bool moveRight = (dragMode_ == DragMode::TR || dragMode_ == DragMode::BR);
                bool moveTop = (dragMode_ == DragMode::TL || dragMode_ == DragMode::TR);
                bool moveBottom = (dragMode_ == DragMode::BL || dragMode_ == DragMode::BR);

                if (moveLeft)   { nx = s.x + dx; nw = s.w - dx; }
                if (moveRight)  { nw = s.w + dx; }
                if (moveTop)    { ny = s.y + dy; nh = s.h - dy; }
                if (moveBottom) { nh = s.h + dy; }

                // Enforce minimum size
                if (nw < minSize) { if (moveLeft) nx = s.x + s.w - minSize; nw = minSize; }
                if (nh < minSize) { if (moveTop) ny = s.y + s.h - minSize; nh = minSize; }

                // Auto-flip orientation during corner drag (only when AR locked, not 1:1)
                if (locked && panel_->aspect() != CropAspect::A1_1) {
                    // Anchor corner in screen coords
                    float anchorNX = moveLeft ? (s.x + s.w) : s.x;
                    float anchorNY = moveTop  ? (s.y + s.h) : s.y;
                    float anchorSX = imgRect_.x + anchorNX * imgRect_.w;
                    float anchorSY = imgRect_.y + anchorNY * imgRect_.h;

                    float sdx = abs(pos.x - anchorSX);
                    float sdy = abs(pos.y - anchorSY);
                    float angle = atan2(sdy, sdx); // angle from horizontal

                    // Angle from current orientation's axis
                    float a = isLandscape_ ? atan2(sdy, sdx) : atan2(sdx, sdy);
                    bool shouldFlip = (a > TAU / 8 * kFlipThreshold);

                    if (shouldFlip) {
                        isLandscape_ = !isLandscape_;
                        panel_->setOrientation(isLandscape_);
                    }
                }

                // Aspect ratio constraint
                if (locked) {
                    float targetAR = getTargetAspectNorm();
                    if (targetAR > 0) {
                        if (nw / nh > targetAR) {
                            nw = nh * targetAR;
                        } else {
                            nh = nw / targetAR;
                        }
                        if (moveTop && !moveBottom) ny = s.y + s.h - nh;
                        if (moveLeft && !moveRight) nx = s.x + s.w - nw;
                    }
                }

                // Clamp to image bounds
                nx = clamp(nx, 0.0f, 1.0f - minSize);
                ny = clamp(ny, 0.0f, 1.0f - minSize);
                nw = clamp(nw, minSize, 1.0f - nx);
                nh = clamp(nh, minSize, 1.0f - ny);

                cropX_ = nx; cropY_ = ny;
                cropW_ = nw; cropH_ = nh;
            }
        }

        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 0) {
            dragMode_ = DragMode::None;
            return true;
        }
        return false;
    }

    bool onMouseScroll(Vec2 pos, Vec2 scroll) override {
        // Only respond if pointer is inside the crop rect (screen coords)
        float cx = imgRect_.x + cropX_ * imgRect_.w;
        float cy = imgRect_.y + cropY_ * imgRect_.h;
        float cw = cropW_ * imgRect_.w;
        float ch = cropH_ * imgRect_.h;
        if (pos.x < cx || pos.x > cx + cw || pos.y < cy || pos.y > cy + ch)
            return false;

        pushUndo();

        // Scale factor: scroll up = shrink crop (zoom in), scroll down = expand
        float factor = 1.0f - scroll.y * 0.03f;
        factor = clamp(factor, 0.8f, 1.2f);

        constexpr float minSize = 0.02f;
        bool locked = panel_->aspect() != CropAspect::Free;

        float centerX = cropX_ + cropW_ / 2;
        float centerY = cropY_ + cropH_ / 2;
        float nw = cropW_ * factor;
        float nh = cropH_ * factor;

        // Enforce min/max
        nw = clamp(nw, minSize, 1.0f);
        nh = clamp(nh, minSize, 1.0f);

        // Maintain aspect ratio if locked
        if (locked) {
            float targetAR = getTargetAspectNorm();
            if (targetAR > 0) {
                if (nw / nh > targetAR) {
                    nw = nh * targetAR;
                } else {
                    nh = nw / targetAR;
                }
            }
        }

        // Center the crop
        float nx = centerX - nw / 2;
        float ny = centerY - nh / 2;

        // Clamp to image bounds
        nx = clamp(nx, 0.0f, 1.0f - nw);
        ny = clamp(ny, 0.0f, 1.0f - nh);
        nw = min(nw, 1.0f - nx);
        nh = min(nh, 1.0f - ny);

        cropX_ = nx; cropY_ = ny;
        cropW_ = nw; cropH_ = nh;

        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        return true;
    }

private:
    ViewContext* ctx_ = nullptr;
    shared_ptr<SingleView> singleView_;
    CropPanel::Ptr panel_;

    // Borrowed FBO handles
    sg_view fboView_ = {};
    sg_sampler fboSampler_ = {};
    int fboW_ = 0, fboH_ = 0;
    float originalAspect_ = 1.0f;

    // Orientation (landscape = pixel width >= height)
    bool isLandscape_ = true;

    // Current crop (normalized 0-1)
    float cropX_ = 0, cropY_ = 0, cropW_ = 1, cropH_ = 1;

    // Initial crop for cancel
    CropState initialCrop_ = {0, 0, 1, 1};

    // Undo stack
    vector<CropState> undoStack_;

    // Drag state
    DragMode dragMode_ = DragMode::None;
    Vec2 dragStart_;
    CropState dragStartCrop_ = {0, 0, 1, 1};

    // Cached image rect (screen coords)
    struct Rect { float x, y, w, h; };
    Rect imgRect_ = {0, 0, 0, 0};

    float handleSize_ = 4.0f;

    void pushUndo() {
        undoStack_.push_back({cropX_, cropY_, cropW_, cropH_});
        // Limit stack size
        if (undoStack_.size() > 50) {
            undoStack_.erase(undoStack_.begin());
        }
    }

    bool hitTest(Vec2 pos, float cx, float cy, float radius) {
        return abs(pos.x - cx) <= radius && abs(pos.y - cy) <= radius;
    }

    // Get target aspect ratio in normalized space (w/h in 0-1 coords)
    float getTargetAspectNorm() {
        float ar = 0;
        switch (panel_->aspect()) {
            case CropAspect::Original: ar = originalAspect_; break;
            case CropAspect::A16_9:    ar = 16.0f / 9.0f; break;
            case CropAspect::A4_3:     ar = 4.0f / 3.0f; break;
            case CropAspect::A3_2:     ar = 3.0f / 2.0f; break;
            case CropAspect::A1_1:     ar = 1.0f; break;
            case CropAspect::A5_4:     ar = 5.0f / 4.0f; break;
            case CropAspect::Free:     return 0;
        }
        // Flip for portrait orientation
        if (!isLandscape_) ar = 1.0f / ar;
        // Convert from pixel aspect to normalized aspect
        return ar / originalAspect_;
    }

    void applyAspect(CropAspect a) {
        if (a == CropAspect::Free) return;

        pushUndo();
        float normAR = getTargetAspectNorm();
        if (normAR <= 0) return;

        float centerX = cropX_ + cropW_ / 2;
        float centerY = cropY_ + cropH_ / 2;

        // Compute maximum possible crop at this AR within image bounds
        float maxW, maxH;
        if (normAR >= 1.0f) {
            maxW = 1.0f; maxH = 1.0f / normAR;
        } else {
            maxH = 1.0f; maxW = normAR;
        }

        // Scale down to preserve current crop's larger dimension
        float currentMax = max(cropW_, cropH_);
        float newMax = max(maxW, maxH);
        float newW = maxW, newH = maxH;
        if (newMax > currentMax) {
            float s = currentMax / newMax;
            newW *= s;
            newH *= s;
        }

        cropX_ = clamp(centerX - newW / 2, 0.0f, 1.0f - newW);
        cropY_ = clamp(centerY - newH / 2, 0.0f, 1.0f - newH);
        cropW_ = newW;
        cropH_ = newH;
    }
};
