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
#include "views/ViewContainer.h"
#include "views/SingleView.h"
#include "CropPanel.h"
#include "CropTypes.h"

using namespace std;
using namespace tc;

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
        float perspV, perspH, shear;
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
        perspVListener_ = panel_->perspVChanged.listen([this](float& v) {
            setPerspV(v);
        });
        perspHListener_ = panel_->perspHChanged.listen([this](float& v) {
            setPerspH(v);
        });
        shearListener_ = panel_->shearChanged.listen([this](float& v) {
            setShear(v);
        });
        focalListener_ = panel_->focalChanged.listen([this](int& mm) {
            focalLength35mm_ = mm;
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        });
        centerizeListener_ = panel_->centerizeEvent.listen([this]() {
            centerize();
        });
        resetListener_ = panel_->resetEvent.listen([this]() {
            pushUndo();
            cropX_ = 0; cropY_ = 0; cropW_ = 1; cropH_ = 1;
            angle_ = 0; rotation90_ = 0;
            perspV_ = 0; perspH_ = 0; shear_ = 0;
            updateBoundingBox();
            syncViewAnchor();
            isLandscape_ = (originalAspect_ >= 1.0f);
            panel_->setOrientation(isLandscape_);
            panel_->setAngle(0);
            panel_->setPerspV(0);
            panel_->setPerspH(0);
            panel_->setShear(0);
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
        initRotateCursor();

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

        // Load current crop + rotation + perspective from entry
        string pid = singleView_->currentPhotoId();
        auto* entry = ctx_->provider->getPhoto(pid);
        if (entry) {
            cropX_ = entry->userCropX;
            cropY_ = entry->userCropY;
            cropW_ = entry->userCropW;
            cropH_ = entry->userCropH;
            angle_ = entry->userAngle;
            rotation90_ = entry->userRotation90;
            perspV_ = entry->userPerspV;
            perspH_ = entry->userPerspH;
            shear_ = entry->userShear;
        } else {
            cropX_ = 0; cropY_ = 0; cropW_ = 1; cropH_ = 1;
            angle_ = 0; rotation90_ = 0;
            perspV_ = 0; perspH_ = 0; shear_ = 0;
        }

        // Read focal length from entry
        if (entry) {
            focalLength35mm_ = entry->focalLength35mm;
        } else {
            focalLength35mm_ = 0;
        }

        viewZoom_ = 1.0f;
        updateBoundingBox();
        constrainCropToBounds();
        syncViewAnchor();

        // Determine initial orientation from crop shape (in BB pixels)
        float pixelCropW = cropW_ * bbW_;
        float pixelCropH = cropH_ * bbH_;
        isLandscape_ = (pixelCropW >= pixelCropH);
        panel_->setOrientation(isLandscape_);
        panel_->setAngle(angle_);
        panel_->setPerspV(perspV_);
        panel_->setPerspH(perspH_);
        panel_->setShear(shear_);
        panel_->setFocalLength(focalLength35mm_, focalLength35mm_ > 0);

        // Save initial state for cancel (after constraint)
        initialCrop_ = { cropX_, cropY_, cropW_, cropH_, angle_, rotation90_,
                         perspV_, perspH_, shear_ };
        undoStack_.clear();
    }

    // Save crop + rotation + perspective to DB (Done / Enter)
    void commitCrop() {
        if (!ctx_ || !singleView_) return;
        setCursor(Cursor::Default);
        string pid = singleView_->currentPhotoId();
        if (!pid.empty()) {
            ctx_->provider->setUserCrop(pid, cropX_, cropY_, cropW_, cropH_);
            ctx_->provider->setUserRotation(pid, angle_, rotation90_);
            ctx_->provider->setUserPerspective(pid, perspV_, perspH_, shear_);
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
        perspV_ = initialCrop_.perspV;
        perspH_ = initialCrop_.perspH;
        shear_ = initialCrop_.shear;
        updateBoundingBox();
        // Also save reverted state to DB
        commitCrop();
    }

    // Check if crop has been modified from initial state
    bool hasChanges() const {
        return cropX_ != initialCrop_.x || cropY_ != initialCrop_.y ||
               cropW_ != initialCrop_.w || cropH_ != initialCrop_.h ||
               angle_ != initialCrop_.angle || rotation90_ != initialCrop_.rot90 ||
               perspV_ != initialCrop_.perspV || perspH_ != initialCrop_.perspH ||
               shear_ != initialCrop_.shear;
    }

    // Undo last drag operation (Cmd+Z)
    void undo() {
        if (undoStack_.empty()) return;
        auto s = undoStack_.back();
        undoStack_.pop_back();
        cropX_ = s.x; cropY_ = s.y;
        cropW_ = s.w; cropH_ = s.h;
        angle_ = s.angle; rotation90_ = s.rot90;
        perspV_ = s.perspV; perspH_ = s.perspH; shear_ = s.shear;
        updateBoundingBox();
        syncViewAnchor();
        panel_->setAngle(angle_);
        panel_->setPerspV(perspV_);
        panel_->setPerspH(perspH_);
        panel_->setShear(shear_);
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    Event<void> doneEvent;

    // Update cursor based on mouse hover position
    void updateHoverCursor(Vec2 pos) {
        if (dragMode_ != DragMode::None) return;  // dragging overrides

        float cx = bbRect_.x + cropX_ * bbRect_.w;
        float cy = bbRect_.y + cropY_ * bbRect_.h;
        float cw = cropW_ * bbRect_.w;
        float ch = cropH_ * bbRect_.h;
        float hs = handleSize_ * 1.5f;

        if (hitTest(pos, cx, cy, hs) || hitTest(pos, cx + cw, cy + ch, hs))
            setCursor(Cursor::ResizeNWSE);
        else if (hitTest(pos, cx + cw, cy, hs) || hitTest(pos, cx, cy + ch, hs))
            setCursor(Cursor::ResizeNESW);
        else if (hitTest(pos, cx + cw/2, cy, hs) || hitTest(pos, cx + cw/2, cy + ch, hs))
            setCursor(Cursor::ResizeNS);
        else if (hitTest(pos, cx, cy + ch/2, hs) || hitTest(pos, cx + cw, cy + ch/2, hs))
            setCursor(Cursor::ResizeEW);
        else if (pos.x >= cx && pos.x <= cx + cw && pos.y >= cy && pos.y <= cy + ch) {
            bool shiftHeld = ctx_ && ctx_->shiftDown && *ctx_->shiftDown;
            if (shiftHeld)
                setCursor(Cursor::Hand);
            else
                setCursor(Cursor::ResizeAll);
        } else {
            float margin = 60;
            if (pos.x >= bbRect_.x - margin && pos.x <= bbRect_.x + bbRect_.w + margin &&
                pos.y >= bbRect_.y - margin && pos.y <= bbRect_.y + bbRect_.h + margin)
                setCursor(Cursor::Rotate);
            else
                setCursor(Cursor::Default);
        }
    }

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
        bool hasPersp = (perspV_ != 0 || perspH_ != 0 || shear_ != 0);

        // Fit using rotation-only BB (perspective doesn't shrink the view)
        float cosA = fabs(cos(totalRot)), sinA = fabs(sin(totalRot));
        float rotBBW = (float)fboW_ * cosA + (float)fboH_ * sinA;
        float rotBBH = (float)fboW_ * sinA + (float)fboH_ * cosA;
        float fitScale = min(availW / rotBBW, availH / rotBBH) * viewZoom_;
        float bbDrawW = bbW_ * fitScale;
        float bbDrawH = bbH_ * fitScale;
        float imgDrawW = (float)fboW_ * fitScale;
        float imgDrawH = (float)fboH_ * fitScale;

        // viewAnchor = BB-norm point that appears at screen center.
        // Move/perspective update it incrementally; resize leaves it frozen.
        float centerX = padding + availW / 2 + (0.5f - viewAnchorX_) * bbDrawW;
        float centerY = padding + availH / 2 + (0.5f - viewAnchorY_) * bbDrawH;

        float bbDrawX = centerX - bbDrawW / 2;
        float bbDrawY = centerY - bbDrawH / 2;

        // Store for mouse hit testing
        bbRect_ = { bbDrawX, bbDrawY, bbDrawW, bbDrawH };

        // --- Step 1: Draw rotated image (dimmed) ---
        auto tmpEntry = makeTmpEntry();

        if (!hasPersp) {
            // Simple rotation: draw as rotated quad
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
        } else {
            // Perspective: tessellated grid for dimmed background
            int gridN = 16;
            float cosR = cos(totalRot), sinR = sin(totalRot);
            setColor(0.3f, 0.3f, 0.3f);
            sgl_enable_texture();
            sgl_texture(fboView_, fboSampler_);
            Color col = getDefaultContext().getColor();

            auto srcToScreen = [&](float u, float v) -> pair<float,float> {
                auto [wu, wv] = tmpEntry.forwardWarp(u, v);
                float px = (wu - 0.5f) * fboW_;
                float py = (wv - 0.5f) * fboH_;
                float rx = px * cosR - py * sinR;
                float ry = px * sinR + py * cosR;
                return {centerX + rx * fitScale, centerY + ry * fitScale};
            };

            sgl_begin_triangles();
            sgl_c4f(col.r, col.g, col.b, col.a);
            for (int j = 0; j < gridN; j++) {
                for (int i = 0; i < gridN; i++) {
                    float u0 = (float)i / gridN, u1 = (float)(i+1) / gridN;
                    float v0 = (float)j / gridN, v1 = (float)(j+1) / gridN;

                    auto [sx00, sy00] = srcToScreen(u0, v0);
                    auto [sx10, sy10] = srcToScreen(u1, v0);
                    auto [sx11, sy11] = srcToScreen(u1, v1);
                    auto [sx01, sy01] = srcToScreen(u0, v1);

                    // Triangle 1: TL, TR, BR
                    sgl_v2f_t2f(sx00, sy00, u0, v0);
                    sgl_v2f_t2f(sx10, sy10, u1, v0);
                    sgl_v2f_t2f(sx11, sy11, u1, v1);
                    // Triangle 2: TL, BR, BL
                    sgl_v2f_t2f(sx00, sy00, u0, v0);
                    sgl_v2f_t2f(sx11, sy11, u1, v1);
                    sgl_v2f_t2f(sx01, sy01, u0, v1);
                }
            }
            sgl_end();
            sgl_disable_texture();
        }

        // --- Step 2: Crop rect in screen coords (relative to BB) ---
        float cx = bbDrawX + cropX_ * bbDrawW;
        float cy = bbDrawY + cropY_ * bbDrawH;
        float cw = cropW_ * bbDrawW;
        float ch = cropH_ * bbDrawH;

        // UV mapping: screen point -> texture UV via inverse rotation + inverse perspective
        auto screenToUV = [&](float sx, float sy) -> pair<float,float> {
            float dx = sx - centerX;
            float dy = sy - centerY;
            float cosR = cos(-totalRot), sinR = sin(-totalRot);
            float rx = dx / fitScale;
            float ry = dy / fitScale;
            float ix = rx * cosR - ry * sinR;
            float iy = rx * sinR + ry * cosR;
            float wu = ix / fboW_ + 0.5f;
            float wv = iy / fboH_ + 0.5f;
            if (hasPersp) {
                return tmpEntry.inverseWarp(wu, wv);
            }
            return {wu, wv};
        };

        // --- Step 3: Draw bright crop area ---
        if (!hasPersp) {
            // Simple rotation: 4-corner quad
            auto [u_tl_x, u_tl_y] = screenToUV(cx, cy);
            auto [u_tr_x, u_tr_y] = screenToUV(cx + cw, cy);
            auto [u_br_x, u_br_y] = screenToUV(cx + cw, cy + ch);
            auto [u_bl_x, u_bl_y] = screenToUV(cx, cy + ch);

            setColor(1, 1, 1);
            sgl_enable_texture();
            sgl_texture(fboView_, fboSampler_);
            Color col = getDefaultContext().getColor();
            sgl_begin_quads();
            sgl_c4f(col.r, col.g, col.b, col.a);
            sgl_v2f_t2f(cx, cy, u_tl_x, u_tl_y);
            sgl_v2f_t2f(cx + cw, cy, u_tr_x, u_tr_y);
            sgl_v2f_t2f(cx + cw, cy + ch, u_br_x, u_br_y);
            sgl_v2f_t2f(cx, cy + ch, u_bl_x, u_bl_y);
            sgl_end();
            sgl_disable_texture();
        } else {
            // Perspective: tessellated crop area for correct UV mapping
            int tessN = 16;
            setColor(1, 1, 1);
            sgl_enable_texture();
            sgl_texture(fboView_, fboSampler_);
            Color col = getDefaultContext().getColor();

            sgl_begin_triangles();
            sgl_c4f(col.r, col.g, col.b, col.a);
            for (int j = 0; j < tessN; j++) {
                for (int i = 0; i < tessN; i++) {
                    float tx0 = (float)i / tessN, tx1 = (float)(i+1) / tessN;
                    float ty0 = (float)j / tessN, ty1 = (float)(j+1) / tessN;

                    float sx00 = cx + tx0 * cw, sy00 = cy + ty0 * ch;
                    float sx10 = cx + tx1 * cw, sy10 = cy + ty0 * ch;
                    float sx11 = cx + tx1 * cw, sy11 = cy + ty1 * ch;
                    float sx01 = cx + tx0 * cw, sy01 = cy + ty1 * ch;

                    auto [u00, v00] = screenToUV(sx00, sy00);
                    auto [u10, v10] = screenToUV(sx10, sy10);
                    auto [u11, v11] = screenToUV(sx11, sy11);
                    auto [u01, v01] = screenToUV(sx01, sy01);

                    sgl_v2f_t2f(sx00, sy00, u00, v00);
                    sgl_v2f_t2f(sx10, sy10, u10, v10);
                    sgl_v2f_t2f(sx11, sy11, u11, v11);

                    sgl_v2f_t2f(sx00, sy00, u00, v00);
                    sgl_v2f_t2f(sx11, sy11, u11, v11);
                    sgl_v2f_t2f(sx01, sy01, u01, v01);
                }
            }
            sgl_end();
            sgl_disable_texture();
        }

        // --- Step 4: Rule of thirds grid ---
        setColor(1, 1, 1, 0.25f);
        noFill();
        for (int i = 1; i <= 2; i++) {
            float gx = cx + cw * i / 3.0f;
            float gy = cy + ch * i / 3.0f;
            drawLine(gx, cy, gx, cy + ch);
            drawLine(cx, gy, cx + cw, gy);
        }

        // Crop border (yellow when Shift held = perspective drag mode)
        bool shiftHeld = ctx_ && ctx_->shiftDown && *ctx_->shiftDown;
        if (shiftHeld)
            setColor(1.0f, 0.9f, 0.2f, 0.9f);
        else
            setColor(1, 1, 1, 0.8f);
        noFill();
        drawRect(cx, cy, cw, ch);

        // 8 handles
        float hs = handleSize_;
        if (shiftHeld)
            setColor(1.0f, 0.9f, 0.2f, 0.95f);
        else
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

        // Update panel preview (pass per-vertex UVs for rotation-only preview)
        auto [u_tl_x, u_tl_y] = screenToUV(cx, cy);
        auto [u_tr_x, u_tr_y] = screenToUV(cx + cw, cy);
        auto [u_br_x, u_br_y] = screenToUV(cx + cw, cy + ch);
        auto [u_bl_x, u_bl_y] = screenToUV(cx, cy + ch);
        int outputW = (int)round(bbW_ * cropW_);
        int outputH = (int)round(bbH_ * cropH_);
        panel_->setPreviewInfo(fboView_, fboSampler_,
                               u_tl_x, u_tl_y, u_tr_x, u_tr_y,
                               u_br_x, u_br_y, u_bl_x, u_bl_y,
                               outputW, outputH);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        // Right-click: start view pan (no crop changes)
        if (button == 1) {
            viewPanDragging_ = true;
            viewPanStart_ = pos;
            viewPanAnchorStartX_ = viewAnchorX_;
            viewPanAnchorStartY_ = viewAnchorY_;
            setCursor(Cursor::Hand);
            return true;
        }
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
            dragStartCrop_ = { cropX_, cropY_, cropW_, cropH_, angle_, rotation90_,
                               perspV_, perspH_, shear_ };
            dragStartBBRect_ = bbRect_;
            dragStartBBW_ = bbW_;
            dragStartBBH_ = bbH_;
            viewAnchorStartX_ = viewAnchorX_;
            viewAnchorStartY_ = viewAnchorY_;
            updateCursorForDrag();

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
        // Right-drag: view pan
        if (button == 1 && viewPanDragging_) {
            float dx = pos.x - viewPanStart_.x;
            float dy = pos.y - viewPanStart_.y;
            // bbRect_.w = bbW_ * fitScale * viewZoom_, independent of viewAnchor
            viewAnchorX_ = viewPanAnchorStartX_ - dx / max(1.0f, bbRect_.w);
            viewAnchorY_ = viewPanAnchorStartY_ - dy / max(1.0f, bbRect_.h);
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
            return true;
        }
        if (button != 0 || dragMode_ == DragMode::None) return false;

        if (dragMode_ == DragMode::Rotate) {
            float imgCenterX = bbRect_.x + bbRect_.w / 2;
            float imgCenterY = bbRect_.y + bbRect_.h / 2;
            float mouseAngle = atan2(pos.y - imgCenterY, pos.x - imgCenterX);
            float delta = mouseAngle - dragStartMouseAngle_;
            while (delta > TAU / 2) delta -= TAU;
            while (delta < -TAU / 2) delta += TAU;
            float newAngle = clamp(dragStartCrop_.angle + delta, -TAU / 8, TAU / 8);
            setAngle(newAngle);
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
            return true;
        }

        // Use drag-start bbRect for stable coordinate mapping
        // (prevents feedback loop when perspective changes BB mid-drag)
        auto& bb = dragStartBBRect_;
        float dx = (pos.x - dragStart_.x) / bb.w;
        float dy = (pos.y - dragStart_.y) / bb.h;

        auto& s = dragStartCrop_;
        bool locked = panel_->aspect() != CropAspect::Free;
        constexpr float minSize = 0.02f;

        float nx = s.x, ny = s.y, nw = s.w, nh = s.h;
        bool shiftPerspDrag = false;

        bool movePanDrag = false;

        if (dragMode_ == DragMode::Move) {
            bool isShift = ctx_ && ctx_->shiftDown && *ctx_->shiftDown;
            if (isShift) {
                shiftPerspDrag = true;  // perspective block handles it
            } else {
                movePanDrag = true;     // translation block handles it
            }
        } else {
            bool isEdge = (dragMode_ == DragMode::T || dragMode_ == DragMode::B ||
                           dragMode_ == DragMode::L || dragMode_ == DragMode::R);

            if (isEdge) {
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
                            ny = cy - nh / 2;
                            if (dragMode_ == DragMode::L) nx = s.x + s.w - nw;
                        } else {
                            float cx = s.x + s.w / 2;
                            nw = nh * targetAR;
                            nx = cx - nw / 2;
                            if (dragMode_ == DragMode::T) ny = s.y + s.h - nh;
                        }
                    }
                }
            } else {
                // Corner drag
                bool shiftHeld = ctx_ && ctx_->shiftDown && *ctx_->shiftDown;
                bool moveLeft = (dragMode_ == DragMode::TL || dragMode_ == DragMode::BL);
                bool moveTop = (dragMode_ == DragMode::TL || dragMode_ == DragMode::TR);
                float anchorNX = moveLeft ? (s.x + s.w) : s.x;
                float anchorNY = moveTop  ? (s.y + s.h) : s.y;

                if (shiftHeld) {
                    // Shift+corner: uniform scale from center
                    float centerNX = s.x + s.w / 2;
                    float centerNY = s.y + s.h / 2;
                    float centerSX = bbRect_.x + centerNX * bbRect_.w;
                    float centerSY = bbRect_.y + centerNY * bbRect_.h;

                    float distX = abs(pos.x - centerSX) / max(bbRect_.w, 1.0f);
                    float distY = abs(pos.y - centerSY) / max(bbRect_.h, 1.0f);
                    float origHW = s.w / 2, origHH = s.h / 2;
                    float scaleX = distX / max(origHW, 0.001f);
                    float scaleY = distY / max(origHH, 0.001f);
                    float scale = max(scaleX, scaleY);
                    scale = max(scale, minSize / max(s.w, s.h));

                    nw = s.w * scale;
                    nh = s.h * scale;

                    if (locked) {
                        float targetAR = getTargetAspectNorm();
                        if (targetAR > 0) {
                            if (nw / nh > targetAR) nw = nh * targetAR;
                            else nh = nw / targetAR;
                        }
                    }

                    nw = max(nw, minSize);
                    nh = max(nh, minSize);
                    nx = centerNX - nw / 2;
                    ny = centerNY - nh / 2;
                } else if (locked) {
                    // AR-locked: compute from anchor-to-mouse distance directly.
                    // Uniform algorithm — orientation flip is seamless because
                    // the crop is always derived from the same anchor-to-mouse vector.
                    float anchorSX = bbRect_.x + anchorNX * bbRect_.w;
                    float anchorSY = bbRect_.y + anchorNY * bbRect_.h;
                    float physDx = abs(pos.x - anchorSX);
                    float physDy = abs(pos.y - anchorSY);

                    // Auto-flip orientation (physical pixels, symmetric hysteresis)
                    if (panel_->aspect() != CropAspect::A1_1) {
                        constexpr float flipK = 0.92f;
                        if (isLandscape_ && physDx < physDy * flipK) {
                            isLandscape_ = false;
                            panel_->setOrientation(isLandscape_);
                        } else if (!isLandscape_ && physDy < physDx * flipK) {
                            isLandscape_ = true;
                            panel_->setOrientation(isLandscape_);
                        }
                    }

                    float targetAR = getTargetAspectNorm();
                    float normDx = physDx / max(bbRect_.w, 1.0f);
                    float normDy = physDy / max(bbRect_.h, 1.0f);

                    // Project onto AR constraint
                    if (targetAR > 0 && normDx > normDy * targetAR) {
                        nw = normDy * targetAR;
                        nh = normDy;
                    } else if (targetAR > 0) {
                        nw = normDx;
                        nh = normDx / targetAR;
                    } else {
                        nw = normDx; nh = normDy;
                    }

                    nw = max(nw, minSize);
                    nh = max(nh, minSize);
                    nx = moveLeft ? (anchorNX - nw) : anchorNX;
                    ny = moveTop ? (anchorNY - nh) : anchorNY;
                } else {
                    // Free aspect: delta-based
                    if (moveLeft) { nx = s.x + dx; nw = s.w - dx; }
                    else          { nw = s.w + dx; }
                    if (moveTop)  { ny = s.y + dy; nh = s.h - dy; }
                    else          { nh = s.h + dy; }

                    if (nw < minSize) { if (moveLeft) nx = anchorNX - minSize; nw = minSize; }
                    if (nh < minSize) { if (moveTop) ny = anchorNY - minSize; nh = minSize; }
                }
            }
        }

        // --- Boundary constraint (mode-specific) ---
        // Skip for Shift+Move (perspective drag) — bounds handled separately
        if (dragMode_ == DragMode::Move && !shiftPerspDrag) {
            // Pass 1: constrain the combined (dx, dy) movement
            float blockNX = 0, blockNY = 0;
            float tMax = computeDragLimit(s, nx, ny, nw, nh, &blockNX, &blockNY);
            nx = s.x + (nx - s.x) * tMax;
            ny = s.y + (ny - s.y) * tMax;

            // Pass 2: project remaining movement onto boundary tangent
            if (tMax < 0.999f && (blockNX != 0 || blockNY != 0)) {
                float remainDx = dx * (1.0f - tMax);
                float remainDy = dy * (1.0f - tMax);
                // Tangent = perpendicular to blocking edge's inward normal
                float tanX = -blockNY, tanY = blockNX;
                float dot = remainDx * tanX + remainDy * tanY;
                float slideDx = dot * tanX;
                float slideDy = dot * tanY;

                if (abs(slideDx) > 0.0001f || abs(slideDy) > 0.0001f) {
                    CropState cur = {nx, ny, nw, nh, s.angle, s.rot90,
                                     s.perspV, s.perspH, s.shear};
                    float tSlide = computeDragLimit(cur,
                        nx + slideDx, ny + slideDy, nw, nh);
                    nx += slideDx * tSlide;
                    ny += slideDy * tSlide;
                }
            }
        } else {
            float tMax = computeDragLimit(s, nx, ny, nw, nh);
            if (tMax < 0.999f) {
                nx = s.x + (nx - s.x) * tMax;
                ny = s.y + (ny - s.y) * tMax;
                nw = s.w + (nw - s.w) * tMax;
                nh = s.h + (nh - s.h) * tMax;

                // AR re-projection: tMax interpolation can break AR lock
                // (e.g. after orientation flip, start and desired have different ARs).
                // Shrinking to restore AR is always safe (moves away from boundary).
                if (locked) {
                    float targetAR = getTargetAspectNorm();
                    if (targetAR > 0 && abs(nw / nh - targetAR) > 0.001f) {
                        bool isCorner = (dragMode_ >= DragMode::TL);
                        bool isShift = ctx_ && ctx_->shiftDown && *ctx_->shiftDown;
                        if (nw / nh > targetAR) nw = nh * targetAR;
                        else nh = nw / targetAR;

                        if (isCorner && !isShift) {
                            // Anchor = opposite corner
                            bool ml = (dragMode_ == DragMode::TL || dragMode_ == DragMode::BL);
                            bool mt = (dragMode_ == DragMode::TL || dragMode_ == DragMode::TR);
                            if (ml) nx = (s.x + s.w) - nw;
                            if (mt) ny = (s.y + s.h) - nh;
                        } else if (isCorner && isShift) {
                            // Anchor = center of start crop
                            nx = (s.x + s.w / 2) - nw / 2;
                            ny = (s.y + s.h / 2) - nh / 2;
                        }
                        // Edge drag: center of orthogonal axis (already correct)
                    }
                }
            }
        }

        cropX_ = nx; cropY_ = ny;
        cropW_ = nw; cropH_ = nh;

        // Shift+Move: perspective correction
        if (shiftPerspDrag) {
            float screenDx = pos.x - dragStart_.x;
            float screenDy = pos.y - dragStart_.y;

            float totalRot = (float)rotation90_ * (TAU / 4.0f) + angle_;
            float cosR = cosf(-totalRot), sinR = sinf(-totalRot);
            float startFitScale = dragStartBBRect_.w / max(1.0f, dragStartBBW_);
            float imgDx = screenDx / startFitScale;
            float imgDy = screenDy / startFitScale;
            float du = (imgDx * cosR - imgDy * sinR) / max(1.0f, (float)fboW_);
            float dv = (imgDx * sinR + imgDy * cosR) / max(1.0f, (float)fboH_);

            float focal = (focalLength35mm_ > 0) ? (float)focalLength35mm_ : 28.0f;
            float fx = focal / 36.0f;
            float fy = focal / 24.0f;
            constexpr float kRad2Deg = 180.0f / 3.14159265f;

            perspH_ = clamp(s.perspH + atanf(du / fx) * kRad2Deg, -45.0f, 45.0f);
            perspV_ = clamp(s.perspV - atanf(dv / fy) * kRad2Deg, -45.0f, 45.0f);
            updateBoundingBox();

            // Keep crop at its original screen position
            auto newBB = predictBBRect();
            auto& sb = dragStartBBRect_;
            float startScreenX = sb.x + s.x * sb.w;
            float startScreenY = sb.y + s.y * sb.h;
            cropX_ = (startScreenX - newBB.x) / newBB.w;
            cropY_ = (startScreenY - newBB.y) / newBB.h;
            cropW_ = (s.w * sb.w) / newBB.w;
            cropH_ = (s.h * sb.h) / newBB.h;

            // Track crop movement so image shifts, crop stays on screen
            float startMidX = s.x + s.w / 2;
            float startMidY = s.y + s.h / 2;
            viewAnchorX_ = viewAnchorStartX_ + ((cropX_ + cropW_ / 2) - startMidX);
            viewAnchorY_ = viewAnchorStartY_ + ((cropY_ + cropH_ / 2) - startMidY);

            panel_->setPerspV(perspV_);
            panel_->setPerspH(perspH_);
        }

        // Normal Move: translate photo behind fixed crop frame
        if (movePanDrag) {
            float screenDx = pos.x - dragStart_.x;
            float screenDy = pos.y - dragStart_.y;
            auto& sb = dragStartBBRect_;

            // Desired crop position (photo moves in drag direction → crop opposite)
            float desiredX = s.x - screenDx / sb.w;
            float desiredY = s.y - screenDy / sb.h;

            // Pass 1: constrain to keep all crop corners inside source image
            float blockNX = 0, blockNY = 0;
            float tMax = computeDragLimit(s, desiredX, desiredY, s.w, s.h,
                                          &blockNX, &blockNY);
            float nx = s.x + (desiredX - s.x) * tMax;
            float ny = s.y + (desiredY - s.y) * tMax;

            // Pass 2: slide remaining movement along blocking edge
            if (tMax < 0.999f && (blockNX != 0 || blockNY != 0)) {
                float cropDx = (desiredX - s.x) * (1.0f - tMax);
                float cropDy = (desiredY - s.y) * (1.0f - tMax);
                float tanX = -blockNY, tanY = blockNX;
                float dot = cropDx * tanX + cropDy * tanY;
                float slideDx = dot * tanX;
                float slideDy = dot * tanY;

                if (fabs(slideDx) > 0.0001f || fabs(slideDy) > 0.0001f) {
                    CropState cur = {nx, ny, s.w, s.h, s.angle, s.rot90,
                                     s.perspV, s.perspH, s.shear};
                    float tSlide = computeDragLimit(cur,
                        nx + slideDx, ny + slideDy, s.w, s.h);
                    nx += slideDx * tSlide;
                    ny += slideDy * tSlide;
                }
            }

            cropX_ = nx;
            cropY_ = ny;
            cropW_ = s.w;
            cropH_ = s.h;

            // Track crop movement so image shifts, crop stays on screen
            float startMidX = s.x + s.w / 2;
            float startMidY = s.y + s.h / 2;
            viewAnchorX_ = viewAnchorStartX_ + ((cropX_ + cropW_ / 2) - startMidX);
            viewAnchorY_ = viewAnchorStartY_ + ((cropY_ + cropH_ / 2) - startMidY);
        }

        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        return true;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 1 && viewPanDragging_) {
            viewPanDragging_ = false;
            setCursor(Cursor::Default);
            return true;
        }
        if (button == 0) {
            dragMode_ = DragMode::None;
            setCursor(Cursor::Default);
            return true;
        }
        return false;
    }

    bool onMouseScroll(Vec2 pos, Vec2 scroll) override {
        // View zoom: scroll changes magnification, not crop parameters
        float factor = 1.0f + scroll.y * 0.05f;
        factor = clamp(factor, 0.8f, 1.25f);

        // Zoom range: min = show full BB, max = 4x
        float totalRot = rotation90_ * TAU / 4 + angle_;
        float cosA = fabs(cos(totalRot)), sinA = fabs(sin(totalRot));
        float rotBBW = (float)fboW_ * cosA + (float)fboH_ * sinA;
        float rotBBH = (float)fboW_ * sinA + (float)fboH_ * cosA;
        float bbZoom = min(rotBBW / max(1.0f, bbW_), rotBBH / max(1.0f, bbH_));
        float minZoom = min(bbZoom, 0.75f);  // allow some zoom-out even without perspective
        constexpr float maxZoom = 4.0f;

        float newZoom = clamp(viewZoom_ * factor, minZoom, maxZoom);
        float actualFactor = newZoom / viewZoom_;
        if (fabs(actualFactor - 1.0f) < 0.001f) return true;

        // Adjust viewAnchor to keep pointer position stable
        float panelW = 220, padding = 40;
        float screenCenterX = padding + (getWidth() - panelW - padding * 2) / 2;
        float screenCenterY = padding + (getHeight() - padding * 2) / 2;

        if (bbRect_.w > 1 && bbRect_.h > 1) {
            float k = 1.0f - 1.0f / actualFactor;
            viewAnchorX_ += (pos.x - screenCenterX) / bbRect_.w * k;
            viewAnchorY_ += (pos.y - screenCenterY) / bbRect_.h * k;
        }

        viewZoom_ = newZoom;
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
    EventListener perspVListener_, perspHListener_, shearListener_;
    EventListener focalListener_, centerizeListener_;
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

    // Perspective / shear state
    float perspV_ = 0;
    float perspH_ = 0;
    float shear_ = 0;

    // Focal length for perspective drag sensitivity
    int focalLength35mm_ = 0;

    // Initial state for cancel
    CropState initialCrop_ = {0, 0, 1, 1, 0, 0, 0, 0, 0};

    // Undo stack
    vector<CropState> undoStack_;

    // Drag state
    DragMode dragMode_ = DragMode::None;
    Vec2 dragStart_;
    CropState dragStartCrop_ = {0, 0, 1, 1, 0, 0, 0, 0, 0};
    float dragStartMouseAngle_ = 0;

    // Cached BB draw rect (screen coords)
    struct Rect { float x, y, w, h; };
    Rect bbRect_ = {0, 0, 0, 0};
    Rect dragStartBBRect_ = {0, 0, 0, 0};  // bbRect_ at drag start (stable during drag)
    float dragStartBBW_ = 0, dragStartBBH_ = 0;  // BB dimensions at drag start

    // View anchor: BB-norm point that maps to screen center.
    // Move/perspective update it incrementally; resize leaves it frozen.
    float viewAnchorX_ = 0.5f, viewAnchorY_ = 0.5f;
    float viewAnchorStartX_ = 0.5f, viewAnchorStartY_ = 0.5f;  // saved at drag start

    // View zoom (1.0 = fit rotation-only BB to screen)
    float viewZoom_ = 1.0f;

    // Right-drag view pan state
    bool viewPanDragging_ = false;
    Vec2 viewPanStart_;
    float viewPanAnchorStartX_ = 0, viewPanAnchorStartY_ = 0;

    float handleSize_ = 4.0f;

    // Sync view anchor to current crop center (re-centers the view)
    void syncViewAnchor() {
        viewAnchorX_ = cropX_ + cropW_ / 2;
        viewAnchorY_ = cropY_ + cropH_ / 2;
    }

    // --- Bounding box computation ---

    // Build a PhotoEntry with current crop/rotation/perspective state
    PhotoEntry makeTmpEntry() const {
        PhotoEntry e;
        e.userAngle = angle_;
        e.userRotation90 = rotation90_;
        e.userPerspV = perspV_;
        e.userPerspH = perspH_;
        e.userShear = shear_;
        e.focalLength35mm = focalLength35mm_;
        return e;
    }

    void updateBoundingBox() {
        auto tmpEntry = makeTmpEntry();
        auto [w, h] = tmpEntry.computeBB(fboW_, fboH_);
        bbW_ = w;
        bbH_ = h;
        bbAspect_ = bbW_ / max(1.0f, bbH_);
    }

    // Predict screen-space BB rect from current bbW_/bbH_ (same formula as draw())
    Rect predictBBRect() const {
        float panelW = 220, padding = 40;
        float availW = getWidth() - panelW - padding * 2;
        float availH = getHeight() - padding * 2;
        // Use rotation-only BB for fitScale (matches draw())
        float totalRot = rotation90_ * TAU / 4 + angle_;
        float cosA = fabs(cos(totalRot)), sinA = fabs(sin(totalRot));
        float rotBBW = (float)fboW_ * cosA + (float)fboH_ * sinA;
        float rotBBH = (float)fboW_ * sinA + (float)fboH_ * cosA;
        float fitScale = min(availW / rotBBW, availH / rotBBH) * viewZoom_;
        float bw = bbW_ * fitScale, bh = bbH_ * fitScale;
        // Use viewAnchor (matches draw())
        float cx = padding + availW / 2 + (0.5f - viewAnchorX_) * bw;
        float cy = padding + availH / 2 + (0.5f - viewAnchorY_) * bh;
        return {cx - bw / 2, cy - bh / 2, bw, bh};
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
        syncViewAnchor();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    // --- Perspective/shear helpers ---

    void setPerspV(float v) {
        pushUndo();
        float oldBBW = bbW_, oldBBH = bbH_;
        perspV_ = clamp(v, -45.0f, 45.0f);
        updateBoundingBox();
        rescaleCropForBBChange(oldBBW, oldBBH, bbW_, bbH_);
        panel_->setPerspV(perspV_);
        constrainCropToBounds();
        syncViewAnchor();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    void setPerspH(float v) {
        pushUndo();
        float oldBBW = bbW_, oldBBH = bbH_;
        perspH_ = clamp(v, -45.0f, 45.0f);
        updateBoundingBox();
        rescaleCropForBBChange(oldBBW, oldBBH, bbW_, bbH_);
        panel_->setPerspH(perspH_);
        constrainCropToBounds();
        syncViewAnchor();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    void setShear(float v) {
        pushUndo();
        float oldBBW = bbW_, oldBBH = bbH_;
        shear_ = clamp(v, -1.0f, 1.0f);
        updateBoundingBox();
        rescaleCropForBBChange(oldBBW, oldBBH, bbW_, bbH_);
        panel_->setShear(shear_);
        constrainCropToBounds();
        syncViewAnchor();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    // Center crop + reset all perspective (keep crop size/ratio)
    void centerize() {
        pushUndo();

        // Preserve physical crop size (pixels)
        float physW = cropW_ * bbW_;
        float physH = cropH_ * bbH_;

        // Reset perspective
        perspV_ = 0;
        perspH_ = 0;
        shear_ = 0;
        updateBoundingBox();

        // Rescale crop to new BB
        cropW_ = clamp(physW / max(1.0f, bbW_), 0.02f, 1.0f);
        cropH_ = clamp(physH / max(1.0f, bbH_), 0.02f, 1.0f);

        // Center
        cropX_ = 0.5f - cropW_ / 2;
        cropY_ = 0.5f - cropH_ / 2;

        // Update panel
        panel_->setPerspV(0);
        panel_->setPerspH(0);
        panel_->setShear(0);

        constrainCropToBounds();
        syncViewAnchor();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    void rotate90(int dir) {
        pushUndo();

        // Save old state for coordinate transformation
        float oldTotalRot = rotation90_ * TAU / 4 + angle_;
        float oldBBW = bbW_, oldBBH = bbH_;

        rotation90_ = (rotation90_ + dir + 4) % 4;
        updateBoundingBox();

        // Transform crop center: old BB-norm → image space → new BB-norm
        float cx = cropX_ + cropW_ / 2;
        float cy = cropY_ + cropH_ / 2;

        // Old BB-norm to centered BB pixel coords
        float cx_bb = (cx - 0.5f) * oldBBW;
        float cy_bb = (cy - 0.5f) * oldBBH;

        // BB pixel → image pixel (inverse of old rotation)
        float cosOld = cos(oldTotalRot), sinOld = sin(oldTotalRot);
        float img_x =  cx_bb * cosOld + cy_bb * sinOld;
        float img_y = -cx_bb * sinOld + cy_bb * cosOld;

        // Image pixel → new BB pixel (forward new rotation)
        float newTotalRot = rotation90_ * TAU / 4 + angle_;
        float cosNew = cos(newTotalRot), sinNew = sin(newTotalRot);
        float new_cx_bb = img_x * cosNew - img_y * sinNew;
        float new_cy_bb = img_x * sinNew + img_y * cosNew;

        // New BB pixel → new BB-norm
        float newCx = new_cx_bb / bbW_ + 0.5f;
        float newCy = new_cy_bb / bbH_ + 0.5f;

        // Swap crop W/H: image rotated 90° under screen-horizontal crop
        float newCropW = cropH_ * oldBBH / bbW_;
        float newCropH = cropW_ * oldBBW / bbH_;

        cropW_ = newCropW;
        cropH_ = newCropH;
        cropX_ = newCx - cropW_ / 2;
        cropY_ = newCy - cropH_ / 2;

        // Flip landscape/portrait to follow image rotation
        isLandscape_ = !isLandscape_;
        panel_->setOrientation(isLandscape_);

        constrainCropToBounds();
        syncViewAnchor();
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    // Constrain crop so all 4 corners stay inside the source image.
    // For rotation-only: analytic inscribed rect budget.
    // For perspective: check inverse-warped corners are in [0,1]² and shrink if needed.
    void constrainCropToBounds() {
        cropW_ = clamp(cropW_, 0.02f, 1.0f);
        cropH_ = clamp(cropH_, 0.02f, 1.0f);

        bool hasPersp = (perspV_ != 0 || perspH_ != 0 || shear_ != 0);

        // No fine tilt and no perspective → BB matches image exactly, simple clamp
        if (abs(angle_) < 0.0001f && !hasPersp) {
            cropX_ = clamp(cropX_, 0.0f, 1.0f - cropW_);
            cropY_ = clamp(cropY_, 0.0f, 1.0f - cropH_);
            return;
        }

        if (hasPersp) {
            // Perspective mode: check crop corners via inverse warp
            // If any corner maps outside [0,1]², shrink crop uniformly around center
            auto tmpEntry = makeTmpEntry();
            auto [bbW, bbH] = tmpEntry.computeBB(fboW_, fboH_);
            float totalRot = tmpEntry.totalRotation();
            float cosR = cos(-totalRot), sinR = sin(-totalRot);

            auto checkCorners = [&]() -> float {
                // Returns max overshoot (>0 means out of bounds)
                float maxOver = 0;
                float corners[4][2] = {
                    {cropX_, cropY_},
                    {cropX_ + cropW_, cropY_},
                    {cropX_ + cropW_, cropY_ + cropH_},
                    {cropX_, cropY_ + cropH_}
                };
                for (auto& c : corners) {
                    // BB-norm → BB pixel (centered)
                    float dx = (c[0] - 0.5f) * bbW;
                    float dy = (c[1] - 0.5f) * bbH;
                    // Inverse rotation → image pixel
                    float ix = dx * cosR - dy * sinR;
                    float iy = dx * sinR + dy * cosR;
                    // Image pixel → warped UV → source UV
                    float wu = ix / fboW_ + 0.5f;
                    float wv = iy / fboH_ + 0.5f;
                    auto [u, v] = tmpEntry.inverseWarp(wu, wv);

                    maxOver = max(maxOver, -u);
                    maxOver = max(maxOver, u - 1.0f);
                    maxOver = max(maxOver, -v);
                    maxOver = max(maxOver, v - 1.0f);
                }
                return maxOver;
            };

            cropX_ = clamp(cropX_, 0.0f, 1.0f - cropW_);
            cropY_ = clamp(cropY_, 0.0f, 1.0f - cropH_);

            float over = checkCorners();
            if (over > 0) {
                // Phase 1: Position-only adjustment (move toward BB center)
                float origX = cropX_, origY = cropY_;
                float targetX = clamp(0.5f - cropW_ / 2, 0.0f, 1.0f - cropW_);
                float targetY = clamp(0.5f - cropH_ / 2, 0.0f, 1.0f - cropH_);

                if (fabs(origX - targetX) > 0.0001f || fabs(origY - targetY) > 0.0001f) {
                    float lo = 0.0f, hi = 1.0f;
                    for (int iter = 0; iter < 16; iter++) {
                        float mid = (lo + hi) / 2;
                        cropX_ = origX + (targetX - origX) * mid;
                        cropY_ = origY + (targetY - origY) * mid;
                        if (checkCorners() > 0.001f) {
                            lo = mid;
                        } else {
                            hi = mid;
                        }
                    }
                    cropX_ = origX + (targetX - origX) * hi;
                    cropY_ = origY + (targetY - origY) * hi;
                }
            }

            // Phase 2: If still out of bounds (crop too large), shrink uniformly
            over = checkCorners();
            if (over > 0) {
                float cx = cropX_ + cropW_ / 2;
                float cy = cropY_ + cropH_ / 2;
                float origW = cropW_, origH = cropH_;

                float lo = 0.02f, hi = 1.0f;
                for (int iter = 0; iter < 16; iter++) {
                    float mid = (lo + hi) / 2;
                    cropW_ = origW * mid;
                    cropH_ = origH * mid;
                    cropX_ = clamp(cx - cropW_ / 2, 0.0f, 1.0f - cropW_);
                    cropY_ = clamp(cy - cropH_ / 2, 0.0f, 1.0f - cropH_);
                    if (checkCorners() > 0.001f) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                cropW_ = origW * lo;
                cropH_ = origH * lo;
                cropX_ = clamp(cx - cropW_ / 2, 0.0f, 1.0f - cropW_);
                cropY_ = clamp(cy - cropH_ / 2, 0.0f, 1.0f - cropH_);
            }
            return;
        }

        // Rotation-only: analytic inscribed rect budget
        float totalRot = rotation90_ * TAU / 4 + angle_;
        float cosR = cos(totalRot), sinR = sin(totalRot);
        float absC = abs(cosR), absS = abs(sinR);
        float W2 = (float)fboW_ / 2, H2 = (float)fboH_ / 2;

        float hw = cropW_ / 2, hh = cropH_ / 2;

        float max_off_ix = hw * bbW_ * absC + hh * bbH_ * absS;
        float max_off_iy = hw * bbW_ * absS + hh * bbH_ * absC;

        float budget_x = W2 - max_off_ix;
        float budget_y = H2 - max_off_iy;

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

        float cx = cropX_ + cropW_ / 2;
        float cy = cropY_ + cropH_ / 2;
        float cx_px = (cx - 0.5f) * bbW_;
        float cy_px = (cy - 0.5f) * bbH_;
        float ci_x = cx_px * cosR + cy_px * sinR;
        float ci_y = -cx_px * sinR + cy_px * cosR;

        ci_x = clamp(ci_x, -budget_x, budget_x);
        ci_y = clamp(ci_y, -budget_y, budget_y);

        cx_px = ci_x * cosR - ci_y * sinR;
        cy_px = ci_x * sinR + ci_y * cosR;
        cx = cx_px / bbW_ + 0.5f;
        cy = cy_px / bbH_ + 0.5f;

        cropX_ = cx - cropW_ / 2;
        cropY_ = cy - cropH_ / 2;

        cropX_ = clamp(cropX_, 0.0f, 1.0f - cropW_);
        cropY_ = clamp(cropY_, 0.0f, 1.0f - cropH_);
    }

    // Analytical drag constraint: find max t ∈ [0,1] where all crop corners
    // stay inside the forward-warped source image boundary (in BB-norm space).
    // Forward-warps 8 source boundary points (corners + edge midpoints) to form
    // an 8-sided polygon, then checks each crop corner's linear path against
    // each polygon edge halfplane.  O(32) dot products, no iteration.
    // blockNormal: if non-null, receives the inward normal of the blocking edge
    float computeDragLimit(const CropState& start,
                           float nx, float ny, float nw, float nh,
                           float* blockNormalX = nullptr,
                           float* blockNormalY = nullptr) {
        auto tmpEntry = makeTmpEntry();
        auto [bbW, bbH] = tmpEntry.computeBB(fboW_, fboH_);
        float totalRot = tmpEntry.totalRotation();
        float cosR = cos(totalRot), sinR = sin(totalRot);

        // Forward warp source UV → BB-norm
        auto srcToBBNorm = [&](float u, float v) -> pair<float,float> {
            auto [wu, wv] = tmpEntry.forwardWarp(u, v);
            float px = (wu - 0.5f) * fboW_;
            float py = (wv - 0.5f) * fboH_;
            float rx = px * cosR - py * sinR;
            float ry = px * sinR + py * cosR;
            return {rx / bbW + 0.5f, ry / bbH + 0.5f};
        };

        // 8-point boundary: corners + edge midpoints (handles perspective curvature)
        constexpr int N = 8;
        float Q[N][2];
        auto set = [&](int i, float u, float v) {
            auto [x, y] = srcToBBNorm(u, v);
            Q[i][0] = x; Q[i][1] = y;
        };
        set(0, 0,0);     set(1, 0.5f,0);   // TL, T-mid
        set(2, 1,0);     set(3, 1,0.5f);   // TR, R-mid
        set(4, 1,1);     set(5, 0.5f,1);   // BR, B-mid
        set(6, 0,1);     set(7, 0,0.5f);   // BL, L-mid

        // Polygon centroid for inward normal orientation
        float qcx = 0, qcy = 0;
        for (int i = 0; i < N; i++) { qcx += Q[i][0]; qcy += Q[i][1]; }
        qcx /= N; qcy /= N;

        // Crop corners at t=0 (start) and t=1 (desired)
        float P0[4][2] = {
            {start.x,           start.y},
            {start.x + start.w, start.y},
            {start.x + start.w, start.y + start.h},
            {start.x,           start.y + start.h}
        };
        float P1[4][2] = {
            {nx,      ny},
            {nx + nw, ny},
            {nx + nw, ny + nh},
            {nx,      ny + nh}
        };

        float tMax = 1.0f;
        float bestNX = 0, bestNY = 0;  // normal of the tightest constraint

        // Check each polygon edge
        for (int e = 0; e < N; e++) {
            int e1 = (e + 1) % N;
            float edgeX = Q[e1][0] - Q[e][0];
            float edgeY = Q[e1][1] - Q[e][1];

            // Inward normal (perpendicular, oriented toward centroid, normalized)
            float inx = -edgeY, iny = edgeX;
            if (inx * (qcx - Q[e][0]) + iny * (qcy - Q[e][1]) < 0) {
                inx = -inx; iny = -iny;
            }
            float nlen = sqrt(inx * inx + iny * iny);
            if (nlen < 1e-8f) continue;  // degenerate edge
            inx /= nlen; iny /= nlen;

            // Check each crop corner against this edge.
            // eps on d0: treats "on boundary" as inside (prevents stuck-on-release).
            // No eps on d1: prevents accumulated penetration during edge-sliding.
            constexpr float eps = 0.0005f;

            for (int c = 0; c < 4; c++) {
                float d0 = (P0[c][0] - Q[e][0]) * inx + (P0[c][1] - Q[e][1]) * iny;
                float d1 = (P1[c][0] - Q[e][0]) * inx + (P1[c][1] - Q[e][1]) * iny;

                if (d0 > eps && d1 < 0) {
                    float t = d0 / (d0 - d1);
                    if (t < tMax) { tMax = t; bestNX = inx; bestNY = iny; }
                } else if (d0 <= eps && d1 < -eps) {
                    if (0.0f < tMax) { tMax = 0; bestNX = inx; bestNY = iny; }
                }
                // d0 <= eps && d1 >= -eps: on boundary, ~parallel → allow
            }
        }

        if (blockNormalX) *blockNormalX = bestNX;
        if (blockNormalY) *blockNormalY = bestNY;
        return max(tMax, 0.0f);
    }

    void pushUndo() {
        undoStack_.push_back({cropX_, cropY_, cropW_, cropH_, angle_, rotation90_,
                              perspV_, perspH_, shear_});
        if (undoStack_.size() > 50) {
            undoStack_.erase(undoStack_.begin());
        }
    }

    bool hitTest(Vec2 pos, float cx, float cy, float radius) {
        return abs(pos.x - cx) <= radius && abs(pos.y - cy) <= radius;
    }

    void updateCursorForDrag() {
        switch (dragMode_) {
            case DragMode::Move:   setCursor(Cursor::ResizeAll); break;
            case DragMode::Rotate: setCursor(Cursor::Rotate); break;
            case DragMode::TL:     setCursor(Cursor::ResizeNWSE); break;
            case DragMode::BR:     setCursor(Cursor::ResizeNWSE); break;
            case DragMode::TR:     setCursor(Cursor::ResizeNESW); break;
            case DragMode::BL:     setCursor(Cursor::ResizeNESW); break;
            case DragMode::T:      setCursor(Cursor::ResizeNS); break;
            case DragMode::B:      setCursor(Cursor::ResizeNS); break;
            case DragMode::L:      setCursor(Cursor::ResizeEW); break;
            case DragMode::R:      setCursor(Cursor::ResizeEW); break;
            default:               setCursor(Cursor::Default); break;
        }
    }

    // Generate and register a circular-arrow rotation cursor icon
    static void initRotateCursor() {
        static bool done = false;
        if (done) return;
        done = true;

        constexpr int S = 32;
        static unsigned char pixels[S * S * 4];
        memset(pixels, 0, sizeof(pixels));

        float cx = S * 0.5f, cy = S * 0.5f;
        float R = 7.0f;

        // Arc with gap at top-right (~1 o'clock)
        float gapAngle = -TAU * 0.15f;   // gap center at ~-54°
        float gapHalf = TAU * 0.08f;     // ~29° each side

        // Arrowhead at CW end of gap
        float cwEnd = gapAngle + gapHalf;
        float tipAngle = cwEnd - TAU * 0.03f;
        float baseAngle = cwEnd + TAU * 0.045f;
        float arrowW = 3.0f;

        float tipX = cx + R * cos(tipAngle);
        float tipY = cy + R * sin(tipAngle);
        float boX = cx + (R + arrowW) * cos(baseAngle);
        float boY = cy + (R + arrowW) * sin(baseAngle);
        float biX = cx + (R - arrowW) * cos(baseAngle);
        float biY = cy + (R - arrowW) * sin(baseAngle);

        // Expanded triangle for outline
        float tcx = (tipX + boX + biX) / 3;
        float tcy = (tipY + boY + biY) / 3;
        auto expandPt = [](float px, float py, float cx, float cy, float amt) {
            float dx = px - cx, dy = py - cy;
            float len = sqrt(dx * dx + dy * dy);
            if (len < 0.001f) return make_pair(px, py);
            return make_pair(px + dx / len * amt, py + dy / len * amt);
        };
        auto [etX, etY] = expandPt(tipX, tipY, tcx, tcy, 1.0f);
        auto [eoX, eoY] = expandPt(boX, boY, tcx, tcy, 1.0f);
        auto [eiX, eiY] = expandPt(biX, biY, tcx, tcy, 1.0f);

        auto inTri = [](float px, float py,
                        float x0, float y0, float x1, float y1, float x2, float y2) {
            float d1 = (px - x1) * (y0 - y1) - (x0 - x1) * (py - y1);
            float d2 = (px - x2) * (y1 - y2) - (x1 - x2) * (py - y2);
            float d3 = (px - x0) * (y2 - y0) - (x2 - x0) * (py - y0);
            return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
        };

        constexpr int SS = 4;
        constexpr float invSS2 = 1.0f / (SS * SS);

        for (int y = 0; y < S; y++) {
            for (int x = 0; x < S; x++) {
                float fillC = 0, outC = 0;

                for (int sj = 0; sj < SS; sj++) {
                    for (int si = 0; si < SS; si++) {
                        float px = x + (si + 0.5f) / SS;
                        float py = y + (sj + 0.5f) / SS;
                        float dx = px - cx, dy = py - cy;
                        float d = sqrt(dx * dx + dy * dy);
                        float a = atan2(dy, dx);

                        float rel = a - gapAngle;
                        while (rel > (float)(TAU / 2)) rel -= (float)TAU;
                        while (rel < -(float)(TAU / 2)) rel += (float)TAU;
                        bool arc = fabs(rel) >= gapHalf;

                        bool fill = (arc && fabs(d - R) <= 1.2f) ||
                                    inTri(px, py, tipX, tipY, boX, boY, biX, biY);
                        bool out  = (arc && fabs(d - R) <= 2.2f) ||
                                    inTri(px, py, etX, etY, eoX, eoY, eiX, eiY);

                        if (fill) fillC++;
                        if (out) outC++;
                    }
                }

                float fA = fillC * invSS2;
                float oA = outC * invSS2;
                int idx = (y * S + x) * 4;

                if (fA > 0.01f) {
                    pixels[idx] = 255; pixels[idx + 1] = 255;
                    pixels[idx + 2] = 255; pixels[idx + 3] = (uint8_t)(fA * 255);
                } else if (oA > 0.01f) {
                    pixels[idx] = 30; pixels[idx + 1] = 30;
                    pixels[idx + 2] = 30; pixels[idx + 3] = (uint8_t)(oA * 255);
                }
            }
        }

        bindCursorImage(Cursor::Rotate, S, S, pixels, S / 2, S / 2);
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
        syncViewAnchor();
    }
};
