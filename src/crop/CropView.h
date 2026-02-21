#pragma once

// =============================================================================
// CropView.h - Lightroom-style crop editing with rotation
// =============================================================================
// Image rotates underneath a screen-horizontal crop frame.
// Crop coordinates are relative to the bounding box of the rotated image.
// The bright crop content is rendered with per-vertex UV mapping (inverse
// rotation) so the texture appears correctly within the screen-aligned quad.
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
            updateBoundingBox();
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

        updateBoundingBox();
        constrainCropToBounds();

        // Determine initial orientation from crop shape (in BB pixels)
        float pixelCropW = cropW_ * bbW_;
        float pixelCropH = cropH_ * bbH_;
        isLandscape_ = (pixelCropW >= pixelCropH);
        panel_->setOrientation(isLandscape_);
        panel_->setAngle(angle_);

        // Save initial state for cancel (after constraint)
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
        updateBoundingBox();
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
        updateBoundingBox();
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

        // Image area: fit rotated bounding box
        if (fboW_ <= 0 || fboH_ <= 0) return;

        float padding = 40;
        float availW = imgAreaW - padding * 2;
        float availH = h - padding * 2;

        float totalRot = rotation90_ * TAU / 4 + angle_;
        updateBoundingBox();

        // Fit bounding box to available area
        float fitScale = min(availW / bbW_, availH / bbH_);
        float bbDrawW = bbW_ * fitScale;
        float bbDrawH = bbH_ * fitScale;
        float imgDrawW = (float)fboW_ * fitScale;
        float imgDrawH = (float)fboH_ * fitScale;

        // Center of display area = center of BB = center of image
        float centerX = padding + availW / 2;
        float centerY = padding + availH / 2;

        float bbDrawX = centerX - bbDrawW / 2;
        float bbDrawY = centerY - bbDrawH / 2;

        // Store for mouse hit testing
        bbRect_ = { bbDrawX, bbDrawY, bbDrawW, bbDrawH };

        // --- Step 1: Draw rotated image (dimmed) ---
        pushMatrix();
        translate(centerX, centerY);
        rotate(totalRot);
        translate(-centerX, -centerY);

        float imgX = centerX - imgDrawW / 2;
        float imgY = centerY - imgDrawH / 2;

        setColor(0.3f, 0.3f, 0.3f);
        sgl_enable_texture();
        sgl_texture(fboView_, fboSampler_);
        Color col = getDefaultContext().getColor();
        sgl_begin_quads();
        sgl_c4f(col.r, col.g, col.b, col.a);
        sgl_v2f_t2f(imgX, imgY, 0, 0);
        sgl_v2f_t2f(imgX + imgDrawW, imgY, 1, 0);
        sgl_v2f_t2f(imgX + imgDrawW, imgY + imgDrawH, 1, 1);
        sgl_v2f_t2f(imgX, imgY + imgDrawH, 0, 1);
        sgl_end();
        sgl_disable_texture();

        popMatrix();

        // --- Step 2: Crop rect in screen coords (relative to BB) ---
        float cx = bbDrawX + cropX_ * bbDrawW;
        float cy = bbDrawY + cropY_ * bbDrawH;
        float cw = cropW_ * bbDrawW;
        float ch = cropH_ * bbDrawH;

        // UV mapping: screen point -> texture UV via inverse rotation
        auto screenToUV = [&](float sx, float sy) -> pair<float,float> {
            float dx = sx - centerX;
            float dy = sy - centerY;
            float cosR = cos(-totalRot), sinR = sin(-totalRot);
            float rx = dx * cosR - dy * sinR;
            float ry = dx * sinR + dy * cosR;
            float u = rx / imgDrawW + 0.5f;
            float v = ry / imgDrawH + 0.5f;
            return {u, v};
        };

        // --- Step 3: Draw bright crop area with rotated UV ---
        auto [u_tl_x, u_tl_y] = screenToUV(cx, cy);
        auto [u_tr_x, u_tr_y] = screenToUV(cx + cw, cy);
        auto [u_br_x, u_br_y] = screenToUV(cx + cw, cy + ch);
        auto [u_bl_x, u_bl_y] = screenToUV(cx, cy + ch);

        setColor(1, 1, 1);
        sgl_enable_texture();
        sgl_texture(fboView_, fboSampler_);
        col = getDefaultContext().getColor();
        sgl_begin_quads();
        sgl_c4f(col.r, col.g, col.b, col.a);
        sgl_v2f_t2f(cx, cy, u_tl_x, u_tl_y);
        sgl_v2f_t2f(cx + cw, cy, u_tr_x, u_tr_y);
        sgl_v2f_t2f(cx + cw, cy + ch, u_br_x, u_br_y);
        sgl_v2f_t2f(cx, cy + ch, u_bl_x, u_bl_y);
        sgl_end();
        sgl_disable_texture();

        // --- Step 4: Rule of thirds grid ---
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

        // Update panel preview (pass per-vertex UVs)
        int outputW = (int)round(bbW_ * cropW_);
        int outputH = (int)round(bbH_ * cropH_);
        panel_->setPreviewInfo(fboView_, fboSampler_,
                               u_tl_x, u_tl_y, u_tr_x, u_tr_y,
                               u_br_x, u_br_y, u_bl_x, u_bl_y,
                               outputW, outputH);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        if (button != 0) return false;

        // Crop rect in screen coords
        float cx = bbRect_.x + cropX_ * bbRect_.w;
        float cy = bbRect_.y + cropY_ * bbRect_.h;
        float cw = cropW_ * bbRect_.w;
        float ch = cropH_ * bbRect_.h;

        float hs = handleSize_ * 1.5f;

        dragMode_ = DragMode::None;

        // Test handles (corners first, then edges, then interior)
        if (hitTest(pos, cx, cy, hs))                dragMode_ = DragMode::TL;
        else if (hitTest(pos, cx + cw, cy, hs))      dragMode_ = DragMode::TR;
        else if (hitTest(pos, cx, cy + ch, hs))       dragMode_ = DragMode::BL;
        else if (hitTest(pos, cx + cw, cy + ch, hs))  dragMode_ = DragMode::BR;
        else if (hitTest(pos, cx + cw/2, cy, hs))     dragMode_ = DragMode::T;
        else if (hitTest(pos, cx + cw/2, cy + ch, hs)) dragMode_ = DragMode::B;
        else if (hitTest(pos, cx, cy + ch/2, hs))      dragMode_ = DragMode::L;
        else if (hitTest(pos, cx + cw, cy + ch/2, hs)) dragMode_ = DragMode::R;
        // Interior
        else if (pos.x >= cx && pos.x <= cx + cw &&
                 pos.y >= cy && pos.y <= cy + ch) {
            dragMode_ = DragMode::Move;
        }
        // Outside crop -> Rotate (if near the image area)
        else {
            float margin = 60;
            if (pos.x >= bbRect_.x - margin && pos.x <= bbRect_.x + bbRect_.w + margin &&
                pos.y >= bbRect_.y - margin && pos.y <= bbRect_.y + bbRect_.h + margin) {
                dragMode_ = DragMode::Rotate;
            }
        }

        if (dragMode_ != DragMode::None) {
            pushUndo();
            dragStart_ = pos;
            dragStartCrop_ = { cropX_, cropY_, cropW_, cropH_, angle_, rotation90_ };

            if (dragMode_ == DragMode::Rotate) {
                float imgCenterX = bbRect_.x + bbRect_.w / 2;
                float imgCenterY = bbRect_.y + bbRect_.h / 2;
                dragStartMouseAngle_ = atan2(pos.y - imgCenterY, pos.x - imgCenterX);
            }
            return true;
        }

        return false;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (button != 0 || dragMode_ == DragMode::None) return false;

        if (dragMode_ == DragMode::Rotate) {
            // Rotation drag: use screen coords relative to BB center
            float imgCenterX = bbRect_.x + bbRect_.w / 2;
            float imgCenterY = bbRect_.y + bbRect_.h / 2;
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

        // For crop operations, use screen coords converted to BB-normalized
        float dx = (pos.x - dragStart_.x) / bbRect_.w;
        float dy = (pos.y - dragStart_.y) / bbRect_.h;

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
                            float ccx = s.x + s.w / 2;
                            nw = nh * targetAR;
                            float maxW = min(ccx, 1.0f - ccx) * 2;
                            if (nw > maxW) { nw = maxW; nh = nw / targetAR; }
                            nx = ccx - nw / 2;
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
                // Corner drag
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
                    float anchorSX = bbRect_.x + anchorNX * bbRect_.w;
                    float anchorSY = bbRect_.y + anchorNY * bbRect_.h;

                    float adx = abs(pos.x - anchorSX) / bbRect_.w;
                    float ady = abs(pos.y - anchorSY) / bbRect_.h;
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

        constrainCropToBounds();
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
        // Hit test against screen-aligned crop rect
        float cx = bbRect_.x + cropX_ * bbRect_.w;
        float cy = bbRect_.y + cropY_ * bbRect_.h;
        float cw = cropW_ * bbRect_.w;
        float ch = cropH_ * bbRect_.h;
        if (pos.x < cx || pos.x > cx + cw || pos.y < cy || pos.y > cy + ch)
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

        constrainCropToBounds();
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

    // Bounding box of rotated image (in source pixels)
    float bbW_ = 1, bbH_ = 1;
    float bbAspect_ = 1;

    // Orientation (landscape = pixel width >= height)
    bool isLandscape_ = true;

    // Current crop (normalized 0-1, relative to bounding box)
    float cropX_ = 0, cropY_ = 0, cropW_ = 1, cropH_ = 1;

    // Rotation state
    float angle_ = 0;       // fine rotation (radians, +/-TAU/8)
    int rotation90_ = 0;    // 90 degree steps (0-3)

    // Initial state for cancel
    CropState initialCrop_ = {0, 0, 1, 1, 0, 0};

    // Undo stack
    vector<CropState> undoStack_;

    // Drag state
    DragMode dragMode_ = DragMode::None;
    Vec2 dragStart_;
    CropState dragStartCrop_ = {0, 0, 1, 1, 0, 0};
    float dragStartMouseAngle_ = 0;

    // Cached BB draw rect (screen coords)
    struct Rect { float x, y, w, h; };
    Rect bbRect_ = {0, 0, 0, 0};

    float handleSize_ = 4.0f;

    // --- Bounding box computation ---

    void updateBoundingBox() {
        float totalRot = rotation90_ * TAU / 4 + angle_;
        float absRot = abs(totalRot);
        float cosA = cos(absRot), sinA = sin(absRot);
        bbW_ = (float)fboW_ * cosA + (float)fboH_ * sinA;
        bbH_ = (float)fboW_ * sinA + (float)fboH_ * cosA;
        bbAspect_ = bbW_ / max(1.0f, bbH_);
    }

    // Transform crop coords when bounding box changes (keeps crop in same
    // physical screen position while the BB rescales around the fixed center)
    void rescaleCropForBBChange(float oldBBW, float oldBBH,
                                float newBBW, float newBBH) {
        float scaleX = oldBBW / max(1.0f, newBBW);
        float scaleY = oldBBH / max(1.0f, newBBH);

        // Crop center offset from BB center (in old normalized coords)
        float offsetX = (cropX_ + cropW_ / 2) - 0.5f;
        float offsetY = (cropY_ + cropH_ / 2) - 0.5f;

        cropW_ *= scaleX;
        cropH_ *= scaleY;
        cropX_ = (0.5f + offsetX * scaleX) - cropW_ / 2;
        cropY_ = (0.5f + offsetY * scaleY) - cropH_ / 2;
    }

    // --- Rotation helpers ---

    void setAngle(float a) {
        float oldBBW = bbW_;
        float oldBBH = bbH_;

        angle_ = clamp(a, -TAU / 8, TAU / 8);
        updateBoundingBox();

        rescaleCropForBBChange(oldBBW, oldBBH, bbW_, bbH_);

        panel_->setAngle(angle_);
        constrainCropToBounds();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    void rotate90(int dir) {
        pushUndo();
        float oldBBW = bbW_;
        float oldBBH = bbH_;

        rotation90_ = (rotation90_ + dir + 4) % 4;
        updateBoundingBox();

        rescaleCropForBBChange(oldBBW, oldBBH, bbW_, bbH_);

        constrainCropToBounds();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    // Constrain crop so all 4 corners stay inside the rotated image.
    // When fine rotation is 0 (pure 0/90/180/270), BB = image, simple clamp.
    // Otherwise, compute inscribed rect budget analytically.
    void constrainCropToBounds() {
        cropW_ = clamp(cropW_, 0.02f, 1.0f);
        cropH_ = clamp(cropH_, 0.02f, 1.0f);

        // No fine tilt → BB matches image exactly, simple clamp
        if (abs(angle_) < 0.0001f) {
            cropX_ = clamp(cropX_, 0.0f, 1.0f - cropW_);
            cropY_ = clamp(cropY_, 0.0f, 1.0f - cropH_);
            return;
        }

        float totalRot = rotation90_ * TAU / 4 + angle_;
        float cosR = cos(totalRot), sinR = sin(totalRot);
        float absC = abs(cosR), absS = abs(sinR);
        float W2 = (float)fboW_ / 2, H2 = (float)fboH_ / 2;

        float hw = cropW_ / 2, hh = cropH_ / 2;

        // Max corner offset from crop center in image space
        float max_off_ix = hw * bbW_ * absC + hh * bbH_ * absS;
        float max_off_iy = hw * bbW_ * absS + hh * bbH_ * absC;

        // Budget: how far the center can be from image center
        float budget_x = W2 - max_off_ix;
        float budget_y = H2 - max_off_iy;

        // If budget negative, crop is too large → shrink to fit centered
        if (budget_x < 0 || budget_y < 0) {
            float sx = W2 / max(max_off_ix, 0.01f);
            float sy = H2 / max(max_off_iy, 0.01f);
            float s = min(sx, sy);
            s = max(s, 0.0f);

            cropW_ = max(cropW_ * s, 0.02f);
            cropH_ = max(cropH_ * s, 0.02f);
            hw = cropW_ / 2;
            hh = cropH_ / 2;

            max_off_ix = hw * bbW_ * absC + hh * bbH_ * absS;
            max_off_iy = hw * bbW_ * absS + hh * bbH_ * absC;
            budget_x = W2 - max_off_ix;
            budget_y = H2 - max_off_iy;
        }

        budget_x = max(budget_x, 0.0f);
        budget_y = max(budget_y, 0.0f);

        // Crop center in image space (inverse rotation)
        float cx = cropX_ + cropW_ / 2;
        float cy = cropY_ + cropH_ / 2;
        float cx_px = (cx - 0.5f) * bbW_;
        float cy_px = (cy - 0.5f) * bbH_;
        float ci_x = cx_px * cosR + cy_px * sinR;
        float ci_y = -cx_px * sinR + cy_px * cosR;

        // Clamp center to budget
        ci_x = clamp(ci_x, -budget_x, budget_x);
        ci_y = clamp(ci_y, -budget_y, budget_y);

        // Reconstruct BB-norm position (forward rotation: image → BB)
        cx_px = ci_x * cosR - ci_y * sinR;
        cy_px = ci_x * sinR + ci_y * cosR;
        cx = cx_px / bbW_ + 0.5f;
        cy = cy_px / bbH_ + 0.5f;

        cropX_ = cx - cropW_ / 2;
        cropY_ = cy - cropH_ / 2;

        // Final safety clamp
        cropX_ = clamp(cropX_, 0.0f, 1.0f - cropW_);
        cropY_ = clamp(cropY_, 0.0f, 1.0f - cropH_);
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

    // Get target aspect ratio in BB-normalized space (crop w/h in 0-1 coords)
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
        // Convert from physical aspect to BB-normalized aspect
        return ar / bbAspect_;
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
        constrainCropToBounds();
    }
};
