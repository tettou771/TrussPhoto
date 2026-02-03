#pragma once

// =============================================================================
// tcxLensfun - Lens correction (distortion, vignetting, CA) using lensfun
// =============================================================================
// Applies corrections on CPU Pixels before GPU upload.
// Requires: brew install lensfun
// =============================================================================

#include <TrussC.h>

#ifdef TCX_HAS_LENSFUN
#include <lensfun/lensfun.h>
#endif

#include <memory>
#include <cstring>
using namespace std;
using namespace tc;

namespace tcx {

class LensCorrector {
public:
    LensCorrector() {
#ifdef TCX_HAS_LENSFUN
        ldb_ = lf_db_new();
        lf_db_load(ldb_);
#endif
    }

    ~LensCorrector() {
#ifdef TCX_HAS_LENSFUN
        if (modifier_) lf_modifier_destroy(modifier_);
        if (ldb_) lf_db_destroy(ldb_);
#endif
    }

    // Non-copyable
    LensCorrector(const LensCorrector&) = delete;
    LensCorrector& operator=(const LensCorrector&) = delete;

    // Setup correction parameters. Returns true if camera+lens were found.
    bool setup(const string& cameraMake, const string& cameraModel,
               const string& lensModel, float focalLength, float aperture,
               int width, int height) {
#ifdef TCX_HAS_LENSFUN
        if (!ldb_) return false;

        // Find camera
        const lfCamera** cameras = lf_db_find_cameras(ldb_,
            cameraMake.c_str(), cameraModel.c_str());
        if (!cameras || !cameras[0]) {
            logWarning() << "[Lensfun] Camera not found: " << cameraMake << " " << cameraModel;
            return false;
        }
        const lfCamera* camera = cameras[0];
        float cropFactor = camera->CropFactor;

        // Find lens
        const lfLens** lenses = lf_db_find_lenses_hd(ldb_, camera,
            nullptr, lensModel.c_str(), 0);
        if (!lenses || !lenses[0]) {
            logWarning() << "[Lensfun] Lens not found: " << lensModel;
            lf_free(cameras);
            return false;
        }
        const lfLens* lens = lenses[0];

        logNotice() << "[Lensfun] Found: " << camera->Model << " + " << lens->Model;

        // Create and initialize modifier (lensfun 0.3.x API)
        if (modifier_) lf_modifier_destroy(modifier_);
        modifier_ = lf_modifier_new(lens, cropFactor, width, height);
        if (!modifier_) {
            lf_free(lenses);
            lf_free(cameras);
            return false;
        }

        // Initialize with all corrections enabled
        int modFlags = lf_modifier_initialize(modifier_, lens, LF_PF_U8,
            focalLength, aperture, 1000.0f, // distance (estimate)
            0.0f,                            // scale (0 = auto)
            LF_RECTILINEAR,                 // target geometry
            LF_MODIFY_ALL,                  // enable all corrections
            0);                              // not reverse

        width_ = width;
        height_ = height;
        ready_ = (modFlags != 0);

        lf_free(lenses);
        lf_free(cameras);

        if (ready_) {
            logNotice() << "[Lensfun] Corrections enabled (flags: " << modFlags << ")";
        }
        return ready_;
#else
        (void)cameraMake; (void)cameraModel; (void)lensModel;
        (void)focalLength; (void)aperture; (void)width; (void)height;
        return false;
#endif
    }

    bool isReady() const { return ready_; }

    // Apply lens corrections to RGBA pixels (in-place)
    bool apply(Pixels& pixels) {
#ifdef TCX_HAS_LENSFUN
        if (!ready_ || !modifier_) return false;

        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        unsigned char* data = pixels.getData();

        // 1. Apply vignetting correction (in-place, per-row)
        for (int y = 0; y < h; y++) {
            lf_modifier_apply_color_modification(modifier_,
                data + y * w * ch, 0.0f, (float)y, w, 1,
                LF_CR_4(RED, GREEN, BLUE, UNKNOWN), w * ch);
        }

        // 2. Apply geometry (distortion) + TCA correction
        // Needs a coordinate remap buffer: 3 channels (R, G, B), each with (x, y)
        vector<float> coords(w * 2 * 3);
        Pixels corrected;
        corrected.allocate(w, h, ch);
        unsigned char* dst = corrected.getData();

        for (int y = 0; y < h; y++) {
            bool ok = lf_modifier_apply_subpixel_geometry_distortion(modifier_,
                0.0f, (float)y, w, 1, coords.data());
            if (!ok) {
                // No distortion data, copy row as-is
                memcpy(dst + y * w * ch, data + y * w * ch, w * ch);
                continue;
            }

            for (int x = 0; x < w; x++) {
                int idx = x * 2 * 3;
                // Sample each channel from its remapped coordinate
                for (int c = 0; c < 3; c++) {
                    float sx = coords[idx + c * 2 + 0];
                    float sy = coords[idx + c * 2 + 1];
                    dst[(y * w + x) * ch + c] = sampleBilinear(data, w, h, ch, c, sx, sy);
                }
                dst[(y * w + x) * ch + 3] = 255;  // Alpha
            }
        }

        pixels = std::move(corrected);
        return true;
#else
        (void)pixels;
        return false;
#endif
    }

private:
    bool ready_ = false;
    int width_ = 0;
    int height_ = 0;

#ifdef TCX_HAS_LENSFUN
    lfDatabase* ldb_ = nullptr;
    lfModifier* modifier_ = nullptr;
#endif

    // Bilinear sampling from source image
    static unsigned char sampleBilinear(const unsigned char* data,
                                         int w, int h, int ch,
                                         int channel, float fx, float fy) {
        int x0 = (int)fx;
        int y0 = (int)fy;
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        // Clamp to image bounds
        x0 = (x0 < 0) ? 0 : (x0 >= w ? w - 1 : x0);
        y0 = (y0 < 0) ? 0 : (y0 >= h ? h - 1 : y0);
        x1 = (x1 < 0) ? 0 : (x1 >= w ? w - 1 : x1);
        y1 = (y1 < 0) ? 0 : (y1 >= h ? h - 1 : y1);

        float dx = fx - (int)fx;
        float dy = fy - (int)fy;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;

        float v00 = data[(y0 * w + x0) * ch + channel];
        float v10 = data[(y0 * w + x1) * ch + channel];
        float v01 = data[(y1 * w + x0) * ch + channel];
        float v11 = data[(y1 * w + x1) * ch + channel];

        float v = v00 * (1 - dx) * (1 - dy)
                + v10 * dx * (1 - dy)
                + v01 * (1 - dx) * dy
                + v11 * dx * dy;

        return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
};

} // namespace tcx

// Convenience alias
using tcx::LensCorrector;
