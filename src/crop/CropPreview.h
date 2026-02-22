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

            // Tessellated grid for correct perspective (homography) preview.
            // Bilinear UV interpolation from 4 corner UVs:
            //   TL(uv_[0,1]) --- TR(uv_[2,3])
            //    |                   |
            //   BL(uv_[6,7]) --- BR(uv_[4,5])
            constexpr int N = 8;
            setColor(1, 1, 1);
            sgl_enable_texture();
            sgl_texture(view_, sampler_);
            Color col = getDefaultContext().getColor();
            sgl_begin_triangles();
            sgl_c4f(col.r, col.g, col.b, col.a);

            for (int j = 0; j < N; j++) {
                for (int i = 0; i < N; i++) {
                    float s0 = (float)i / N, s1 = (float)(i+1) / N;
                    float t0 = (float)j / N, t1 = (float)(j+1) / N;

                    auto uvAt = [&](float s, float t, float& u, float& v) {
                        // Bilinear: lerp(lerp(TL,TR,s), lerp(BL,BR,s), t)
                        float topU = uv_[0] + (uv_[2] - uv_[0]) * s;
                        float topV = uv_[1] + (uv_[3] - uv_[1]) * s;
                        float botU = uv_[6] + (uv_[4] - uv_[6]) * s;
                        float botV = uv_[7] + (uv_[5] - uv_[7]) * s;
                        u = topU + (botU - topU) * t;
                        v = topV + (botV - topV) * t;
                    };

                    float u00, v00, u10, v10, u11, v11, u01, v01;
                    uvAt(s0, t0, u00, v00);
                    uvAt(s1, t0, u10, v10);
                    uvAt(s1, t1, u11, v11);
                    uvAt(s0, t1, u01, v01);

                    float sx00 = px + s0 * fitW, sy00 = py + t0 * fitH;
                    float sx10 = px + s1 * fitW, sy10 = py + t0 * fitH;
                    float sx11 = px + s1 * fitW, sy11 = py + t1 * fitH;
                    float sx01 = px + s0 * fitW, sy01 = py + t1 * fitH;

                    // Triangle 1: TL, TR, BR
                    sgl_v2f_t2f(sx00, sy00, u00, v00);
                    sgl_v2f_t2f(sx10, sy10, u10, v10);
                    sgl_v2f_t2f(sx11, sy11, u11, v11);
                    // Triangle 2: TL, BR, BL
                    sgl_v2f_t2f(sx00, sy00, u00, v00);
                    sgl_v2f_t2f(sx11, sy11, u11, v11);
                    sgl_v2f_t2f(sx01, sy01, u01, v01);
                }
            }

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
