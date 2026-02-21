#pragma once

// =============================================================================
// CropView.h - Interactive crop editing view with rotation support
// =============================================================================
// Displays the developed FBO image with a draggable crop rectangle overlay.
// Left area: image + crop overlay. Right 220px: CropPanel.
// Supports fine rotation (±45°) via drag outside crop area + slider,
// and 90° step rotation.
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
        None, Move, Rotate,
        TL, T, TR, L, R, BL, B, BR
    };

    // Undo history entry
    struct CropState {
        float x, y, w, h;
        float angle;
        int rot90;
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

        aspectListener_ = panel_->aspectChanged.listen([this](CropAspect& a) {
            applyAspect(a);
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        });
        orientListener_ = panel_->orientationChanged.listen([this](bool& landscape) {
            isLandscape_ = landscape;
            applyAspect(panel_->aspect());
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        });
        angleListener_ = panel_->angleChanged.listen([this](float& a) {
            setAngle(a);
        });
        rotate90Listener_ = panel_->rotate90Event.listen([this](int& dir) {
            rotate90(dir);
        });
        resetListener_ = panel_->resetEvent.listen([this]() {
            pushUndo();
            cropX_ = 0; cropY_ = 0; cropW_ = 1; cropH_ = 1;
            angle_ = 0; rotation90_ = 0;
            isLandscape_ = (originalAspect_ >= 1.0f);
            panel_->setOrientation(isLandscape_);
            panel_->setAngle(0);
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        });
        panelDoneListener_ = panel_->doneEvent.listen([this]() {
            commitCrop();
            doneEvent.notify();
        });
        panelCancelListener_ = panel_->cancelEvent.listen([this]() {
            cancelCrop();
            doneEvent.notify();
        });
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

        // Load current crop + rotation from entry
        string pid = singleView_->currentPhotoId();
        auto* entry = ctx_->provider->getPhoto(pid);
        if (entry) {
            cropX_ = entry->userCropX;
            cropY_ = entry->userCropY;
            cropW_ = entry->userCropW;
            cropH_ = entry->userCropH;
            angle_ = entry->userAngle;
            rotation90_ = entry->userRotation90;
        } else {
            cropX_ = 0; cropY_ = 0; cropW_ = 1; cropH_ = 1;
            angle_ = 0; rotation90_ = 0;
        }

        // Determine initial orientation from crop shape
        float pixelCropW = cropW_ * fboW_;
        float pixelCropH = cropH_ * fboH_;
        isLandscape_ = (pixelCropW >= pixelCropH);
        panel_->setOrientation(isLandscape_);
        panel_->setAngle(angle_);

        // Save initial state for cancel
        initialCrop_ = { cropX_, cropY_, cropW_, cropH_, angle_, rotation90_ };
        undoStack_.clear();
    }

    // Save crop + rotation to DB (Done / Enter)
    void commitCrop() {
        if (!ctx_ || !singleView_) return;
        string pid = singleView_->currentPhotoId();
        if (!pid.empty()) {
            ctx_->provider->setUserCrop(pid, cropX_, cropY_, cropW_, cropH_);
            ctx_->provider->setUserRotation(pid, angle_, rotation90_);
        }
    }

    // Revert to initial state (Cancel / ESC)
    void cancelCrop() {
        cropX_ = initialCrop_.x;
        cropY_ = initialCrop_.y;
        cropW_ = initialCrop_.w;
        cropH_ = initialCrop_.h;
        angle_ = initialCrop_.angle;
        rotation90_ = initialCrop_.rot90;
        // Also save reverted state to DB
        commitCrop();
    }

    // Check if crop has been modified from initial state
    bool hasChanges() const {
        return cropX_ != initialCrop_.x || cropY_ != initialCrop_.y ||
               cropW_ != initialCrop_.w || cropH_ != initialCrop_.h ||
               angle_ != initialCrop_.angle || rotation90_ != initialCrop_.rot90;
    }

    // Undo last drag operation (Cmd+Z)
    void undo() {
        if (undoStack_.empty()) return;
        auto s = undoStack_.back();
        undoStack_.pop_back();
        cropX_ = s.x; cropY_ = s.y;
        cropW_ = s.w; cropH_ = s.h;
        angle_ = s.angle; rotation90_ = s.rot90;
        panel_->setAngle(angle_);
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    Event<void> doneEvent;

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

        // Account for rotation when fitting: the rotated image bounding box is larger
        float totalRot = rotation90_ * TAU / 4 + angle_;
        float absAngle = abs(totalRot);
        float cosA = cos(absAngle), sinA = sin(absAngle);

        // Rotated bounding box size
        float rotBoundsW = (float)fboW_ * cosA + (float)fboH_ * sinA;
        float rotBoundsH = (float)fboW_ * sinA + (float)fboH_ * cosA;

        float fitScale = min(availW / rotBoundsW, availH / rotBoundsH);
        float drawW = fboW_ * fitScale;
        float drawH = fboH_ * fitScale;

        // Center of image area
        float imgCenterX = padding + availW / 2;
        float imgCenterY = padding + availH / 2;

        // Image rect top-left (before rotation)
        float imgX = imgCenterX - drawW / 2;
        float imgY = imgCenterY - drawH / 2;

        // Store for hit testing (pre-rotation image rect)
        imgRect_ = { imgX, imgY, drawW, drawH };

        // Apply rotation around image center
        pushMatrix();
        translate(imgCenterX, imgCenterY);
        rotate(totalRot);
        translate(-imgCenterX, -imgCenterY);

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

        // Crop rectangle in screen coords (within rotated space)
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

        popMatrix();

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

        // Transform mouse to rotated image space
        Vec2 rpos = screenToImageSpace(pos);

        // Check if inside image area
        float cx = imgRect_.x + cropX_ * imgRect_.w;
        float cy = imgRect_.y + cropY_ * imgRect_.h;
        float cw = cropW_ * imgRect_.w;
        float ch = cropH_ * imgRect_.h;

        float hs = handleSize_ * 1.5f; // larger hit area

        // Test handles (corners first, then edges, then interior)
        dragMode_ = DragMode::None;

        // Corners
        if (hitTest(rpos, cx, cy, hs))                dragMode_ = DragMode::TL;
        else if (hitTest(rpos, cx + cw, cy, hs))      dragMode_ = DragMode::TR;
        else if (hitTest(rpos, cx, cy + ch, hs))       dragMode_ = DragMode::BL;
        else if (hitTest(rpos, cx + cw, cy + ch, hs))  dragMode_ = DragMode::BR;
        // Edge midpoints
        else if (hitTest(rpos, cx + cw/2, cy, hs))     dragMode_ = DragMode::T;
        else if (hitTest(rpos, cx + cw/2, cy + ch, hs)) dragMode_ = DragMode::B;
        else if (hitTest(rpos, cx, cy + ch/2, hs))      dragMode_ = DragMode::L;
        else if (hitTest(rpos, cx + cw, cy + ch/2, hs)) dragMode_ = DragMode::R;
        // Interior
        else if (rpos.x >= cx && rpos.x <= cx + cw &&
                 rpos.y >= cy && rpos.y <= cy + ch) {
            dragMode_ = DragMode::Move;
        }
        // Outside crop but inside image area → Rotate
        else if (rpos.x >= imgRect_.x && rpos.x <= imgRect_.x + imgRect_.w &&
                 rpos.y >= imgRect_.y && rpos.y <= imgRect_.y + imgRect_.h) {
            dragMode_ = DragMode::Rotate;
        }

        if (dragMode_ != DragMode::None) {
            pushUndo();
            dragStart_ = pos;           // screen coords for rotation
            dragStartRotated_ = rpos;   // rotated coords for crop ops
            dragStartCrop_ = { cropX_, cropY_, cropW_, cropH_, angle_, rotation90_ };

            if (dragMode_ == DragMode::Rotate) {
                // Calculate initial mouse angle from crop center
                float ccx = imgRect_.x + (cropX_ + cropW_/2) * imgRect_.w;
                float ccy = imgRect_.y + (cropY_ + cropH_/2) * imgRect_.h;
                // Use image center in screen space (before rotation transform)
                float imgCenterX = imgRect_.x + imgRect_.w / 2;
                float imgCenterY = imgRect_.y + imgRect_.h / 2;
                dragStartMouseAngle_ = atan2(pos.y - imgCenterY, pos.x - imgCenterX);
            }
            return true;
        }

        return false;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (button != 0 || dragMode_ == DragMode::None) return false;

        if (dragMode_ == DragMode::Rotate) {
            // Rotation drag: use screen coords relative to image center
            float imgCenterX = imgRect_.x + imgRect_.w / 2;
            float imgCenterY = imgRect_.y + imgRect_.h / 2;
            float mouseAngle = atan2(pos.y - imgCenterY, pos.x - imgCenterX);
            float delta = mouseAngle - dragStartMouseAngle_;
            // Wrap delta to [-TAU/2, TAU/2]
            while (delta > TAU / 2) delta -= TAU;
            while (delta < -TAU / 2) delta += TAU;
            float newAngle = clamp(dragStartCrop_.angle + delta, -TAU / 8, TAU / 8);
            setAngle(newAngle);
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
            return true;
        }

        // For crop operations, use rotated coordinates
        Vec2 rpos = screenToImageSpace(pos);
        float dx = (rpos.x - dragStartRotated_.x) / imgRect_.w;
        float dy = (rpos.y - dragStartRotated_.y) / imgRect_.h;

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
                float nx = s.x, ny = s.y, nw = s.w, nh = s.h;
                bool isHoriz = (dragMode_ == DragMode::L || dragMode_ == DragMode::R);

                if (dragMode_ == DragMode::L) { nx = s.x + dx; nw = s.w - dx; }
                if (dragMode_ == DragMode::R) { nw = s.w + dx; }
                if (dragMode_ == DragMode::T) { ny = s.y + dy; nh = s.h - dy; }
                if (dragMode_ == DragMode::B) { nh = s.h + dy; }

                nw = max(nw, minSize);
                nh = max(nh, minSize);
                if (dragMode_ == DragMode::L && nw <= minSize) nx = s.x + s.w - minSize;
                if (dragMode_ == DragMode::T && nh <= minSize) ny = s.y + s.h - minSize;

                if (locked) {
                    float targetAR = getTargetAspectNorm();
                    if (targetAR > 0) {
                        if (isHoriz) {
                            float cy = s.y + s.h / 2;
                            nh = nw / targetAR;
                            float maxH = min(cy, 1.0f - cy) * 2;
                            if (nh > maxH) { nh = maxH; nw = nh * targetAR; }
                            ny = cy - nh / 2;
                            if (dragMode_ == DragMode::L) nx = s.x + s.w - nw;
                        } else {
                            float cx = s.x + s.w / 2;
                            nw = nh * targetAR;
                            float maxW = min(cx, 1.0f - cx) * 2;
                            if (nw > maxW) { nw = maxW; nh = nw / targetAR; }
                            nx = cx - nw / 2;
                            if (dragMode_ == DragMode::T) ny = s.y + s.h - nh;
                        }
                    }
                }

                nx = clamp(nx, 0.0f, 1.0f - minSize);
                ny = clamp(ny, 0.0f, 1.0f - minSize);
                nw = clamp(nw, minSize, 1.0f - nx);
                nh = clamp(nh, minSize, 1.0f - ny);

                cropX_ = nx; cropY_ = ny;
                cropW_ = nw; cropH_ = nh;
            } else {
                float nx = s.x, ny = s.y, nw = s.w, nh = s.h;

                bool moveLeft = (dragMode_ == DragMode::TL || dragMode_ == DragMode::BL);
                bool moveRight = (dragMode_ == DragMode::TR || dragMode_ == DragMode::BR);
                bool moveTop = (dragMode_ == DragMode::TL || dragMode_ == DragMode::TR);
                bool moveBottom = (dragMode_ == DragMode::BL || dragMode_ == DragMode::BR);

                if (moveLeft)   { nx = s.x + dx; nw = s.w - dx; }
                if (moveRight)  { nw = s.w + dx; }
                if (moveTop)    { ny = s.y + dy; nh = s.h - dy; }
                if (moveBottom) { nh = s.h + dy; }

                if (nw < minSize) { if (moveLeft) nx = s.x + s.w - minSize; nw = minSize; }
                if (nh < minSize) { if (moveTop) ny = s.y + s.h - minSize; nh = minSize; }

                // Auto-flip orientation during corner drag
                if (locked && panel_->aspect() != CropAspect::A1_1) {
                    float anchorNX = moveLeft ? (s.x + s.w) : s.x;
                    float anchorNY = moveTop  ? (s.y + s.h) : s.y;
                    float anchorSX = imgRect_.x + anchorNX * imgRect_.w;
                    float anchorSY = imgRect_.y + anchorNY * imgRect_.h;

                    float adx = abs(rpos.x - anchorSX) / imgRect_.w;
                    float ady = abs(rpos.y - anchorSY) / imgRect_.h;
                    float a = isLandscape_ ? atan2(ady, adx) : atan2(adx, ady);
                    bool shouldFlip = (a > TAU / 8 * kFlipThreshold);

                    if (shouldFlip) {
                        isLandscape_ = !isLandscape_;
                        panel_->setOrientation(isLandscape_);
                    }
                }

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
        // Transform to rotated space for hit testing
        Vec2 rpos = screenToImageSpace(pos);

        float cx = imgRect_.x + cropX_ * imgRect_.w;
        float cy = imgRect_.y + cropY_ * imgRect_.h;
        float cw = cropW_ * imgRect_.w;
        float ch = cropH_ * imgRect_.h;
        if (rpos.x < cx || rpos.x > cx + cw || rpos.y < cy || rpos.y > cy + ch)
            return false;

        pushUndo();

        float factor = 1.0f - scroll.y * 0.03f;
        factor = clamp(factor, 0.8f, 1.2f);

        constexpr float minSize = 0.02f;
        bool locked = panel_->aspect() != CropAspect::Free;

        float centerX = cropX_ + cropW_ / 2;
        float centerY = cropY_ + cropH_ / 2;
        float nw = cropW_ * factor;
        float nh = cropH_ * factor;

        nw = clamp(nw, minSize, 1.0f);
        nh = clamp(nh, minSize, 1.0f);

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

        float nx = centerX - nw / 2;
        float ny = centerY - nh / 2;

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

    // Panel event listeners
    EventListener aspectListener_, orientListener_, resetListener_;
    EventListener angleListener_, rotate90Listener_;
    EventListener panelDoneListener_, panelCancelListener_;

    // Borrowed FBO handles
    sg_view fboView_ = {};
    sg_sampler fboSampler_ = {};
    int fboW_ = 0, fboH_ = 0;
    float originalAspect_ = 1.0f;

    // Orientation (landscape = pixel width >= height)
    bool isLandscape_ = true;

    // Current crop (normalized 0-1)
    float cropX_ = 0, cropY_ = 0, cropW_ = 1, cropH_ = 1;

    // Rotation state
    float angle_ = 0;       // fine rotation (radians, ±TAU/8)
    int rotation90_ = 0;    // 90° steps (0-3)

    // Initial state for cancel
    CropState initialCrop_ = {0, 0, 1, 1, 0, 0};

    // Undo stack
    vector<CropState> undoStack_;

    // Drag state
    DragMode dragMode_ = DragMode::None;
    Vec2 dragStart_;         // screen coords
    Vec2 dragStartRotated_;  // rotated image-space coords
    CropState dragStartCrop_ = {0, 0, 1, 1, 0, 0};
    float dragStartMouseAngle_ = 0;  // atan2 at drag start (rotation mode)

    // Cached image rect (screen coords, pre-rotation)
    struct Rect { float x, y, w, h; };
    Rect imgRect_ = {0, 0, 0, 0};

    float handleSize_ = 4.0f;

    // --- Rotation helpers ---

    void setAngle(float a) {
        angle_ = clamp(a, -TAU / 8, TAU / 8);
        panel_->setAngle(angle_);
        constrainCropToBounds();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    void rotate90(int dir) {
        pushUndo();
        rotation90_ = (rotation90_ + dir + 4) % 4;
        constrainCropToBounds();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    // Constrain crop rect to image bounds (simple clamp)
    void constrainCropToBounds() {
        cropX_ = clamp(cropX_, 0.0f, 1.0f - cropW_);
        cropY_ = clamp(cropY_, 0.0f, 1.0f - cropH_);
    }

    // Transform screen coords to rotated image space (inverse rotation)
    Vec2 screenToImageSpace(Vec2 pos) {
        float totalRot = rotation90_ * TAU / 4 + angle_;
        if (totalRot == 0) return pos;

        float imgCenterX = imgRect_.x + imgRect_.w / 2;
        float imgCenterY = imgRect_.y + imgRect_.h / 2;

        // Inverse rotation around image center
        float dx = pos.x - imgCenterX;
        float dy = pos.y - imgCenterY;
        float cosA = cos(-totalRot);
        float sinA = sin(-totalRot);
        return {
            imgCenterX + dx * cosA - dy * sinA,
            imgCenterY + dx * sinA + dy * cosA
        };
    }

    void pushUndo() {
        undoStack_.push_back({cropX_, cropY_, cropW_, cropH_, angle_, rotation90_});
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
            case CropAspect::Original: ar = max(originalAspect_, 1.0f / originalAspect_); break;
            case CropAspect::A16_9:    ar = 16.0f / 9.0f; break;
            case CropAspect::A4_3:     ar = 4.0f / 3.0f; break;
            case CropAspect::A3_2:     ar = 3.0f / 2.0f; break;
            case CropAspect::A1_1:     ar = 1.0f; break;
            case CropAspect::A5_4:     ar = 5.0f / 4.0f; break;
            case CropAspect::Free:     return 0;
        }
        if (!isLandscape_) ar = 1.0f / ar;
        return ar / originalAspect_;
    }

    void applyAspect(CropAspect a) {
        if (a == CropAspect::Free) return;

        pushUndo();
        float normAR = getTargetAspectNorm();
        if (normAR <= 0) return;

        float centerX = cropX_ + cropW_ / 2;
        float centerY = cropY_ + cropH_ / 2;

        float maxW, maxH;
        if (normAR >= 1.0f) {
            maxW = 1.0f; maxH = 1.0f / normAR;
        } else {
            maxH = 1.0f; maxW = normAR;
        }

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
