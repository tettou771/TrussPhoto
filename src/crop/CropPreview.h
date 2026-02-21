#pragma once

// =============================================================================
// CropPreview.h - Crop preview texture display widget
// =============================================================================

#include <TrussC.h>

using namespace std;
using namespace tc;

class CropPreview : public RectNode {
public:
    using Ptr = shared_ptr<CropPreview>;

    void setPreviewInfo(sg_view view, sg_sampler sampler,
                        float u0, float v0, float u1, float v1,
                        int outputW, int outputH) {
        view_ = view;
        sampler_ = sampler;
        u0_ = u0; v0_ = v0; u1_ = u1; v1_ = v1;
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
            sgl_v2f_t2f(px, py, u0_, v0_);
            sgl_v2f_t2f(px + fitW, py, u1_, v0_);
            sgl_v2f_t2f(px + fitW, py + fitH, u1_, v1_);
            sgl_v2f_t2f(px, py + fitH, u0_, v1_);
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
    float u0_ = 0, v0_ = 0, u1_ = 1, v1_ = 1;
    int outputW_ = 0, outputH_ = 0;
};
