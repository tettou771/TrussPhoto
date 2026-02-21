#pragma once

// =============================================================================
// CropPreview.h - Crop preview texture display widget
// Supports per-vertex UV mapping for rotated crop display.
// =============================================================================

#include <TrussC.h>

using namespace std;
using namespace tc;

class CropPreview : public RectNode {
public:
    using Ptr = shared_ptr<CropPreview>;

    // Set preview with per-vertex UVs (supports rotation)
    // UV order: TL, TR, BR, BL
    void setPreviewInfo(sg_view view, sg_sampler sampler,
                        float u0, float v0,   // top-left
                        float u1, float v1,   // top-right
                        float u2, float v2,   // bottom-right
                        float u3, float v3,   // bottom-left
                        int outputW, int outputH) {
        view_ = view;
        sampler_ = sampler;
        uv_[0] = u0; uv_[1] = v0;
        uv_[2] = u1; uv_[3] = v1;
        uv_[4] = u2; uv_[5] = v2;
        uv_[6] = u3; uv_[7] = v3;
        outputW_ = outputW;
        outputH_ = outputH;
        hasPreview_ = true;
    }

    void clearPreview() { hasPreview_ = false; }
    int outputW() const { return outputW_; }
    int outputH() const { return outputH_; }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        if (hasPreview_) {
            float cropAspect = (float)outputW_ / max(1, outputH_);
            float fitW, fitH;
            if (cropAspect > w / h) {
                fitW = w;
                fitH = w / cropAspect;
            } else {
                fitH = h;
                fitW = h * cropAspect;
            }
            float px = (w - fitW) / 2;
            float py = (h - fitH) / 2;

            setColor(1, 1, 1);
            sgl_enable_texture();
            sgl_texture(view_, sampler_);
            Color col = getDefaultContext().getColor();
            sgl_begin_quads();
            sgl_c4f(col.r, col.g, col.b, col.a);
            sgl_v2f_t2f(px, py, uv_[0], uv_[1]);
            sgl_v2f_t2f(px + fitW, py, uv_[2], uv_[3]);
            sgl_v2f_t2f(px + fitW, py + fitH, uv_[4], uv_[5]);
            sgl_v2f_t2f(px, py + fitH, uv_[6], uv_[7]);
            sgl_end();
            sgl_disable_texture();
        } else {
            setColor(0.15f, 0.15f, 0.17f);
            fill();
            drawRect(0, 0, w, h);
        }
    }

private:
    bool hasPreview_ = false;
    sg_view view_ = {};
    sg_sampler sampler_ = {};
    float uv_[8] = {0, 0, 1, 0, 1, 1, 0, 1};  // TL, TR, BR, BL
    int outputW_ = 0, outputH_ = 0;
};
