#pragma once

// =============================================================================
// LensCorrector - Lens correction from EXIF / DNG embedded data
// =============================================================================
// Supports two correction sources:
//   1. Sony ARW: EXIF SubImage1 spline-based distortion/TCA/vignetting
//   2. DNG: OpcodeList WarpRectilinear (polynomial per-plane) + GainMap
//
// Data can be loaded directly from RAW file (setupFromExif) or restored
// from DB-cached JSON (setupFromJson).
// =============================================================================

#include <TrussC.h>
#include <exiv2/exiv2.hpp>
#include <nlohmann/json.hpp>
using namespace std;
using namespace tc;

class LensCorrector {
public:
    LensCorrector() = default;

    bool isReady() const { return ready_; }

    // "sony", "dng", or "none"
    string correctionSource() const {
        if (!ready_) return "none";
        if (useDng_) return "dng";
        return "sony";
    }

    // Reset correction state
    void reset() {
        ready_ = false;
        useDng_ = false;
        hasExifTca_ = false;
        hasExifVig_ = false;
        exifKnotCount_ = 0;
        dngGainRows_ = 0;
        dngGainCols_ = 0;
        dngGainMap_.clear();
        hasDefaultCrop_ = false;
        cropX_ = cropY_ = cropW_ = cropH_ = 0;
        intW_ = intH_ = 0;
    }

    // DefaultCrop from EXIF/DNG (applied after lens correction)
    bool hasDefaultCrop() const { return hasDefaultCrop_; }
    int intermediateWidth() const { return intW_; }
    int intermediateHeight() const { return intH_; }
    int cropX() const { return cropX_; }
    int cropY() const { return cropY_; }
    int cropW() const { return cropW_; }
    int cropH() const { return cropH_; }

    // Apply DefaultCrop to pixels. Handles two cases:
    // 1. Full-size: pixels match intermediate dimensions → direct crop
    // 2. Smart preview: pixels are scaled down → scale crop coordinates
    bool applyDefaultCrop(Pixels& pixels) {
        if (!hasDefaultCrop_) return false;
        int w = pixels.getWidth(), h = pixels.getHeight();

        int cx, cy, cw, ch_crop;

        // Full-size path
        if (w >= cropX_ + cropW_ && h >= cropY_ + cropH_) {
            if (w == cropW_ && h == cropH_) return false;  // already at target
            cx = cropX_;
            cy = cropY_;
            cw = cropW_;
            ch_crop = cropH_;
        }
        // Scaled path (smart preview): need intW/intH to compute scale
        else if (intW_ > 0 && intH_ > 0 && w > 0 && h > 0) {
            float scaleX = (float)w / intW_;
            float scaleY = (float)h / intH_;
            cx = (int)round(cropX_ * scaleX);
            cy = (int)round(cropY_ * scaleY);
            cw = (int)round(cropW_ * scaleX);
            ch_crop = (int)round(cropH_ * scaleY);
            // Clamp to pixel bounds
            if (cx + cw > w) cw = w - cx;
            if (cy + ch_crop > h) ch_crop = h - cy;
            if (cw <= 0 || ch_crop <= 0) return false;
            if (cw == w && ch_crop == h) return false;  // no meaningful crop
        } else {
            return false;
        }

        int nch = pixels.getChannels();
        Pixels cropped;
        if (pixels.isFloat()) {
            cropped.allocate(cw, ch_crop, nch, PixelFormat::F32);
            const float* src = pixels.getDataF32();
            float* dst = cropped.getDataF32();
            for (int y = 0; y < ch_crop; y++) {
                memcpy(dst + y * cw * nch,
                       src + ((y + cy) * w + cx) * nch,
                       cw * nch * sizeof(float));
            }
        } else {
            cropped.allocate(cw, ch_crop, nch);
            const unsigned char* src = pixels.getData();
            unsigned char* dst = cropped.getData();
            for (int y = 0; y < ch_crop; y++) {
                memcpy(dst + y * cw * nch,
                       src + ((y + cy) * w + cx) * nch,
                       cw * nch * sizeof(unsigned char));
            }
        }
        pixels = std::move(cropped);
        return true;
    }

    // =========================================================================
    // Setup from RAW file EXIF (Sony ARW SubImage1 tags)
    // =========================================================================
    bool setupFromExif(const string& rawFilePath, int width, int height) {
        reset();

        try {
            auto image = Exiv2::ImageFactory::open(rawFilePath);
            image->readMetadata();
            auto& exif = image->exifData();

            // Distortion correction (required)
            auto distTag = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DistortionCorrParams"));
            if (distTag == exif.end()) return false;

            int nc = (int)distTag->toInt64(0);
            if (nc < 2 || nc > 16) return false;

            for (int i = 0; i < nc; i++) {
                exifKnots_[i] = (float)(i + 0.5f) / (float)(nc - 1);
                exifDistortion_[i] = (float)distTag->toInt64(i + 1) * powf(2.0f, -14.0f) + 1.0f;
            }
            exifKnotCount_ = nc;

            logNotice() << "[LensCorrector] EXIF distortion: nc=" << nc
                        << " first=" << exifDistortion_[0]
                        << " last=" << exifDistortion_[nc - 1];

            // Chromatic Aberration (R/B channels, optional)
            auto caTag = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.ChromaticAberrationCorrParams"));
            if (caTag != exif.end()) {
                int caNc = (int)caTag->toInt64(0);
                if (caNc == nc * 2) {
                    for (int i = 0; i < nc; i++) {
                        exifCaR_[i] = (float)caTag->toInt64(i + 1) * powf(2.0f, -21.0f) + 1.0f;
                        exifCaB_[i] = (float)caTag->toInt64(nc + i + 1) * powf(2.0f, -21.0f) + 1.0f;
                    }
                    hasExifTca_ = true;
                    logNotice() << "[LensCorrector] EXIF TCA: R[0]=" << exifCaR_[0]
                                << " B[0]=" << exifCaB_[0];
                }
            }

            // Vignetting (optional)
            auto vigTag = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.VignettingCorrParams"));
            if (vigTag != exif.end()) {
                int vigNc = (int)vigTag->toInt64(0);
                if (vigNc == nc) {
                    for (int i = 0; i < nc; i++) {
                        float raw = (float)vigTag->toInt64(i + 1);
                        float v = powf(2.0f, 0.5f - powf(2.0f, raw * powf(2.0f, -13.0f) - 1.0f));
                        exifVignetting_[i] = v * v;
                    }
                    hasExifVig_ = true;
                    logNotice() << "[LensCorrector] EXIF vignetting: first=" << exifVignetting_[0]
                                << " last=" << exifVignetting_[nc - 1];
                }
            }

            // Read DefaultCropOrigin/Size for post-correction crop.
            // EXIF stores these in the sensor's native (landscape) orientation.
            // LibRaw rotates output according to EXIF Orientation, so for portrait
            // photos we must transform crop coordinates to match the rotated image.
            auto cropOrig = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DefaultCropOrigin"));
            auto cropSize = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DefaultCropSize"));
            if (cropOrig != exif.end() && cropSize != exif.end() &&
                cropOrig->count() >= 2 && cropSize->count() >= 2) {
                int cx = (int)cropOrig->toInt64(0);
                int cy = (int)cropOrig->toInt64(1);
                int cw = (int)cropSize->toInt64(0);
                int ch = (int)cropSize->toInt64(1);

                // Detect rotation: if crop is landscape but image is portrait,
                // the image was rotated by LibRaw.
                bool cropIsLandscape = (cw > ch);
                bool imageIsPortrait = (width < height);

                if (cropIsLandscape && imageIsPortrait) {
                    // Read EXIF Orientation to determine rotation direction
                    int orient = 1;
                    auto orientTag = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
                    if (orientTag != exif.end()) orient = (int)orientTag->toInt64();

                    if (orient == 6) {
                        // 90° CW: landscape (x,y,w,h) → portrait
                        // In landscape space, the "height" axis = width in portrait
                        cropX_ = width - cy - ch;
                        cropY_ = cx;
                        cropW_ = ch;
                        cropH_ = cw;
                    } else if (orient == 8) {
                        // 90° CCW: landscape (x,y,w,h) → portrait
                        cropX_ = cy;
                        cropY_ = height - cx - cw;
                        cropW_ = ch;
                        cropH_ = cw;
                    } else {
                        // Unknown rotation, use as-is
                        cropX_ = cx; cropY_ = cy; cropW_ = cw; cropH_ = ch;
                    }

                    logNotice() << "[LensCorrector] DefaultCrop (rotated orient=" << orient
                                << "): origin=(" << cropX_ << "," << cropY_
                                << ") size=" << cropW_ << "x" << cropH_;
                } else {
                    cropX_ = cx; cropY_ = cy; cropW_ = cw; cropH_ = ch;
                    logNotice() << "[LensCorrector] DefaultCrop: origin=(" << cropX_ << "," << cropY_
                                << ") size=" << cropW_ << "x" << cropH_;
                }
                hasDefaultCrop_ = true;
            }

            intW_ = width;
            intH_ = height;
            width_ = width;
            height_ = height;
            ready_ = true;
            logNotice() << "[LensCorrector] Using EXIF embedded correction (" << width << "x" << height << ")";
            return true;
        } catch (const Exiv2::Error& e) {
            logWarning() << "[LensCorrector] EXIF read error: " << e.what();
            return false;
        }
    }

    // =========================================================================
    // Setup from DB-cached JSON (restored Sony or DNG data)
    // =========================================================================
    bool setupFromJson(const string& jsonStr, int width, int height) {
        reset();
        if (jsonStr.empty()) return false;

        try {
            auto j = nlohmann::json::parse(jsonStr);

            // Parse DefaultCrop info (common to Sony/DNG)
            if (j.contains("cropX") && j.contains("cropW")) {
                cropX_ = j.value("cropX", 0);
                cropY_ = j.value("cropY", 0);
                cropW_ = j.value("cropW", 0);
                cropH_ = j.value("cropH", 0);
                hasDefaultCrop_ = (cropW_ > 0 && cropH_ > 0);
            }

            // Intermediate image dimensions (for scaling crop to SP)
            intW_ = j.value("intW", 0);
            intH_ = j.value("intH", 0);

            // Crop coords in JSON may be in EXIF's native landscape orientation.
            // If the image pixels are portrait but crop is landscape, transform.
            // After processRawLoadCompletion writes back transformed coords,
            // this branch won't trigger (cropW < cropH matches portrait pixels).
            if (hasDefaultCrop_ && cropW_ > cropH_ && width < height) {
                int orient = j.value("orient", 1);
                int cx = cropX_, cy = cropY_, cw = cropW_, ch = cropH_;
                // Need intermediate dims for transformation.
                // Use intW/intH if available, otherwise approximate from pixel dims.
                int iw = (intW_ > 0) ? intW_ : width;
                int ih = (intH_ > 0) ? intH_ : height;
                if (orient == 6) {
                    cropX_ = iw - cy - ch;
                    cropY_ = cx;
                    cropW_ = ch;
                    cropH_ = cw;
                } else if (orient == 8) {
                    cropX_ = cy;
                    cropY_ = ih - cx - cw;
                    cropW_ = ch;
                    cropH_ = cw;
                }
            }

            string type = j.value("type", string(""));

            if (type == "sony") {
                return setupSonyFromJson(j, width, height);
            } else if (type == "dng") {
                return setupDngFromJson(j, width, height);
            }
        } catch (const exception& e) {
            logWarning() << "[LensCorrector] JSON parse error: " << e.what();
        }
        return false;
    }

    // =========================================================================
    // Apply corrections (auto-dispatch by source and pixel format)
    // =========================================================================
    bool apply(Pixels& pixels) {
        if (!ready_) return false;
        if (useDng_) {
            return pixels.isFloat() ? applyDngFloat(pixels) : applyDngU8(pixels);
        }
        return pixels.isFloat() ? applyExifFloat(pixels) : applyExifU8(pixels);
    }

private:
    bool ready_ = false;
    int width_ = 0, height_ = 0;

    // DefaultCrop (post-correction crop to EXIF declared dimensions)
    bool hasDefaultCrop_ = false;
    int cropX_ = 0, cropY_ = 0, cropW_ = 0, cropH_ = 0;

    // Intermediate image dimensions (zero-cropped, before DefaultCrop).
    // Needed to scale crop coordinates for smart preview display.
    int intW_ = 0, intH_ = 0;

    // Sony EXIF spline data
    int exifKnotCount_ = 0;
    float exifKnots_[16] = {};
    float exifDistortion_[16] = {};
    float exifCaR_[16] = {};
    float exifCaB_[16] = {};
    float exifVignetting_[16] = {};
    bool hasExifTca_ = false;
    bool hasExifVig_ = false;

    // DNG data
    bool useDng_ = false;
    struct DngWarpPlane { double kr[4] = {}; double kt[2] = {}; };
    int dngWarpPlanes_ = 0;
    DngWarpPlane dngWarp_[3] = {};
    double dngCx_ = 0.5, dngCy_ = 0.5;
    int dngGainRows_ = 0, dngGainCols_ = 0;
    int dngGainMapPlanes_ = 1;
    vector<float> dngGainMap_;

    // =========================================================================
    // Sony JSON restore
    // =========================================================================
    bool setupSonyFromJson(const nlohmann::json& j, int w, int h) {
        auto distArr = j.value("dist", nlohmann::json::array());
        auto caRArr = j.value("caR", nlohmann::json::array());
        auto caBArr = j.value("caB", nlohmann::json::array());
        auto vigArr = j.value("vig", nlohmann::json::array());

        int nc = (int)distArr.size();
        if (nc < 2 || nc > 16) {
            // Might only have vignetting
            nc = max({(int)caRArr.size(), (int)caBArr.size(), (int)vigArr.size()});
            if (nc < 2 || nc > 16) return false;
        }

        exifKnotCount_ = nc;
        for (int i = 0; i < nc; i++) {
            exifKnots_[i] = (float)(i + 0.5f) / (float)(nc - 1);
        }

        if ((int)distArr.size() == nc) {
            for (int i = 0; i < nc; i++)
                exifDistortion_[i] = distArr[i].get<float>();
        } else {
            // No distortion data, set identity
            for (int i = 0; i < nc; i++)
                exifDistortion_[i] = 1.0f;
        }

        if ((int)caRArr.size() == nc && (int)caBArr.size() == nc) {
            for (int i = 0; i < nc; i++) {
                exifCaR_[i] = caRArr[i].get<float>();
                exifCaB_[i] = caBArr[i].get<float>();
            }
            hasExifTca_ = true;
        }

        if ((int)vigArr.size() == nc) {
            for (int i = 0; i < nc; i++)
                exifVignetting_[i] = vigArr[i].get<float>();
            hasExifVig_ = true;
        }

        width_ = w;
        height_ = h;
        ready_ = true;
        logNotice() << "[LensCorrector] Restored Sony correction from JSON (nc=" << nc << ")";
        return true;
    }

    // =========================================================================
    // DNG JSON restore
    // =========================================================================
    bool setupDngFromJson(const nlohmann::json& j, int w, int h) {
        bool hasWarp = false, hasGain = false;

        if (j.contains("warp")) {
            auto& warp = j["warp"];
            dngWarpPlanes_ = warp.value("planes", 0);
            dngCx_ = warp.value("cx", 0.5);
            dngCy_ = warp.value("cy", 0.5);
            auto& coeffs = warp["coeffs"];
            for (int p = 0; p < dngWarpPlanes_ && p < 3; p++) {
                auto& plane = coeffs[p];
                for (int k = 0; k < 4 && k < (int)plane.size(); k++)
                    dngWarp_[p].kr[k] = plane[k].get<double>();
                for (int k = 4; k < 6 && k < (int)plane.size(); k++)
                    dngWarp_[p].kt[k - 4] = plane[k].get<double>();
            }
            hasWarp = (dngWarpPlanes_ > 0);
        }

        if (j.contains("gain")) {
            auto& gain = j["gain"];
            dngGainRows_ = gain.value("rows", 0);
            dngGainCols_ = gain.value("cols", 0);
            dngGainMapPlanes_ = gain.value("mapPlanes", 1);
            auto& data = gain["data"];
            dngGainMap_.resize(data.size());
            for (int i = 0; i < (int)data.size(); i++)
                dngGainMap_[i] = data[i].get<float>();
            hasGain = (dngGainRows_ > 0 && dngGainCols_ > 0 && !dngGainMap_.empty());
        }

        if (!hasWarp && !hasGain) return false;

        useDng_ = true;
        width_ = w;
        height_ = h;
        ready_ = true;
        logNotice() << "[LensCorrector] Restored DNG correction from JSON"
                    << " warp=" << dngWarpPlanes_ << "planes"
                    << " gain=" << dngGainRows_ << "x" << dngGainCols_;
        return true;
    }

    // =========================================================================
    // Sony EXIF apply (F32 and U8)
    // =========================================================================

    static float interpSpline(const float* knots, const float* values, int nc, float r) {
        if (r <= knots[0]) return values[0];
        for (int i = 1; i < nc; i++) {
            if (r <= knots[i]) {
                float t = (r - knots[i - 1]) / (knots[i] - knots[i - 1]);
                return values[i - 1] + t * (values[i] - values[i - 1]);
            }
        }
        return values[nc - 1];
    }

    bool applyExifFloat(Pixels& pixels) {
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        float* data = pixels.getDataF32();
        float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
        float invDiag = 1.0f / sqrt(cx * cx + cy * cy);
        int nThreads = max(1u, std::thread::hardware_concurrency());
        auto t0 = std::chrono::high_resolution_clock::now();

        int nc = exifKnotCount_;
        const float* knots = exifKnots_;
        const float* distVals = exifDistortion_;
        const float* caR = exifCaR_;
        const float* caB = exifCaB_;
        const float* vigVals = exifVignetting_;
        bool doTca = hasExifTca_;
        bool doVig = hasExifVig_;

        // Pass 1: Vignetting (in-place)
        if (doVig) {
            parallelRows(h, nThreads, [=](int y) {
                float dy = y - cy;
                for (int x = 0; x < w; x++) {
                    float dx = x - cx;
                    float radius = sqrt(dx * dx + dy * dy) * invDiag;
                    float correction = interpSpline(knots, vigVals, nc, radius);
                    if (correction < 0.01f) correction = 0.01f;
                    float factor = 1.0f / correction;
                    int idx = (y * w + x) * ch;
                    for (int c = 0; c < 3; c++) {
                        data[idx + c] *= factor;
                    }
                }
            });
        }

        // Pass 2: Distortion + TCA (remap)
        Pixels corrected;
        corrected.allocate(w, h, ch, PixelFormat::F32);
        float* dst = corrected.getDataF32();

        parallelRows(h, nThreads, [=](int y) {
            for (int x = 0; x < w; x++) {
                float px = x - cx, py = y - cy;
                float radius = sqrt(px * px + py * py) * invDiag;

                for (int c = 0; c < 3; c++) {
                    float dr = interpSpline(knots, distVals, nc, radius);
                    if (doTca) {
                        if (c == 0) dr *= interpSpline(knots, caR, nc, radius);
                        if (c == 2) dr *= interpSpline(knots, caB, nc, radius);
                    }
                    float sx = dr * px + cx;
                    float sy = dr * py + cy;
                    dst[(y * w + x) * ch + c] = sampleBilinearF32(data, w, h, ch, c, sx, sy);
                }
                dst[(y * w + x) * ch + 3] = 1.0f;
            }
        });

        auto t1 = std::chrono::high_resolution_clock::now();
        logNotice() << "[LensCorrector] EXIF correction: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
            << "ms (" << nThreads << " threads, " << w << "x" << h << ")";

        pixels = std::move(corrected);
        return true;
    }

    bool applyExifU8(Pixels& pixels) {
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        unsigned char* data = pixels.getData();
        float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
        float invDiag = 1.0f / sqrt(cx * cx + cy * cy);
        int nThreads = max(1u, std::thread::hardware_concurrency());

        int nc = exifKnotCount_;
        const float* knots = exifKnots_;
        const float* distVals = exifDistortion_;
        const float* caR = exifCaR_;
        const float* caB = exifCaB_;
        const float* vigVals = exifVignetting_;
        bool doTca = hasExifTca_;
        bool doVig = hasExifVig_;

        // sRGB linearization LUT
        static float srgb2lin[256] = {};
        static bool lutReady = false;
        if (!lutReady) {
            for (int i = 0; i < 256; i++) {
                float v = i / 255.0f;
                srgb2lin[i] = (v <= 0.04045f)
                    ? v / 12.92f
                    : powf((v + 0.055f) / 1.055f, 2.4f);
            }
            lutReady = true;
        }

        // Pass 1: Vignetting (in-place, linear light)
        if (doVig) {
            parallelRows(h, nThreads, [=](int y) {
                float dy = y - cy;
                for (int x = 0; x < w; x++) {
                    float dx = x - cx;
                    float radius = sqrt(dx * dx + dy * dy) * invDiag;
                    float correction = interpSpline(knots, vigVals, nc, radius);
                    if (correction < 0.01f) correction = 0.01f;
                    float factor = 1.0f / correction;
                    int idx = (y * w + x) * ch;
                    for (int c = 0; c < 3; c++) {
                        float lin = srgb2lin[data[idx + c]] * factor;
                        float s = (lin <= 0.0031308f)
                            ? lin * 12.92f
                            : 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f;
                        data[idx + c] = (unsigned char)clamp(s * 255.0f, 0.0f, 255.0f);
                    }
                }
            });
        }

        // Pass 2: Distortion + TCA (remap)
        Pixels corrected;
        corrected.allocate(w, h, ch);
        unsigned char* dst = corrected.getData();

        parallelRows(h, nThreads, [=](int y) {
            for (int x = 0; x < w; x++) {
                float px = x - cx, py = y - cy;
                float radius = sqrt(px * px + py * py) * invDiag;

                for (int c = 0; c < 3; c++) {
                    float dr = interpSpline(knots, distVals, nc, radius);
                    if (doTca) {
                        if (c == 0) dr *= interpSpline(knots, caR, nc, radius);
                        if (c == 2) dr *= interpSpline(knots, caB, nc, radius);
                    }
                    float sx = dr * px + cx;
                    float sy = dr * py + cy;
                    dst[(y * w + x) * ch + c] = sampleBilinear(data, w, h, ch, c, sx, sy);
                }
                dst[(y * w + x) * ch + 3] = 255;
            }
        });

        pixels = std::move(corrected);
        return true;
    }

    // =========================================================================
    // DNG apply (WarpRectilinear + GainMap)
    // =========================================================================

    bool applyDngFloat(Pixels& pixels) {
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        float* data = pixels.getDataF32();
        int nThreads = max(1u, std::thread::hardware_concurrency());
        auto t0 = std::chrono::high_resolution_clock::now();

        // Pass 1: GainMap vignetting (in-place)
        if (dngGainRows_ > 0 && dngGainCols_ > 0 && !dngGainMap_.empty()) {
            int gr = dngGainRows_, gc = dngGainCols_;
            int mp = dngGainMapPlanes_;
            const float* gm = dngGainMap_.data();

            parallelRows(h, nThreads, [=](int y) {
                float gy = (float)y / (h - 1) * (gr - 1);
                int gy0 = clamp((int)gy, 0, gr - 2);
                float fy = gy - gy0;
                for (int x = 0; x < w; x++) {
                    float gx = (float)x / (w - 1) * (gc - 1);
                    int gx0 = clamp((int)gx, 0, gc - 2);
                    float fx = gx - gx0;

                    int idx = (y * w + x) * ch;
                    for (int c = 0; c < 3; c++) {
                        // Select gain map plane
                        int p = (mp >= 3) ? c : 0;
                        int stride = gc * mp;
                        float g00 = gm[gy0 * stride + gx0 * mp + p];
                        float g10 = gm[gy0 * stride + (gx0 + 1) * mp + p];
                        float g01 = gm[(gy0 + 1) * stride + gx0 * mp + p];
                        float g11 = gm[(gy0 + 1) * stride + (gx0 + 1) * mp + p];
                        float gain = g00 * (1 - fx) * (1 - fy) + g10 * fx * (1 - fy)
                                   + g01 * (1 - fx) * fy + g11 * fx * fy;
                        data[idx + c] *= gain;
                    }
                }
            });
        }

        // Pass 2: WarpRectilinear (per-channel polynomial remap)
        if (dngWarpPlanes_ > 0) {
            Pixels corrected;
            corrected.allocate(w, h, ch, PixelFormat::F32);
            float* dst = corrected.getDataF32();

            double cx = dngCx_, cy = dngCy_;
            int np = dngWarpPlanes_;
            DngWarpPlane wp[3];
            for (int i = 0; i < 3; i++) wp[i] = dngWarp_[min(i, np - 1)];

            parallelRows(h, nThreads, [=](int y) {
                double ny = (double)y / (h - 1) - cy;
                for (int x = 0; x < w; x++) {
                    double nx = (double)x / (w - 1) - cx;

                    int dstIdx = (y * w + x) * ch;
                    for (int c = 0; c < 3; c++) {
                        const auto& p = wp[c];
                        double r2 = nx * nx + ny * ny;
                        double r4 = r2 * r2;
                        double r6 = r4 * r2;
                        double factor = p.kr[0] + p.kr[1] * r2 + p.kr[2] * r4 + p.kr[3] * r6;

                        double sx = factor * nx + cx;
                        double sy = factor * ny + cy;

                        // Convert normalized to pixel
                        float px = (float)(sx * (w - 1));
                        float py = (float)(sy * (h - 1));
                        dst[dstIdx + c] = sampleBilinearF32(data, w, h, ch, c, px, py);
                    }
                    dst[dstIdx + 3] = 1.0f;
                }
            });

            auto t1 = std::chrono::high_resolution_clock::now();
            logNotice() << "[LensCorrector] DNG correction: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                << "ms (" << nThreads << " threads, " << w << "x" << h << ")";

            pixels = std::move(corrected);
        }

        return true;
    }

    bool applyDngU8(Pixels& pixels) {
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        unsigned char* data = pixels.getData();
        int nThreads = max(1u, std::thread::hardware_concurrency());

        // sRGB linearization LUT
        static float srgb2lin[256] = {};
        static bool lutReady = false;
        if (!lutReady) {
            for (int i = 0; i < 256; i++) {
                float v = i / 255.0f;
                srgb2lin[i] = (v <= 0.04045f)
                    ? v / 12.92f
                    : powf((v + 0.055f) / 1.055f, 2.4f);
            }
            lutReady = true;
        }

        // Pass 1: GainMap vignetting (in-place, linear light)
        if (dngGainRows_ > 0 && dngGainCols_ > 0 && !dngGainMap_.empty()) {
            int gr = dngGainRows_, gc = dngGainCols_;
            int mp = dngGainMapPlanes_;
            const float* gm = dngGainMap_.data();

            parallelRows(h, nThreads, [=](int y) {
                float gy = (float)y / (h - 1) * (gr - 1);
                int gy0 = clamp((int)gy, 0, gr - 2);
                float fy = gy - gy0;
                for (int x = 0; x < w; x++) {
                    float gx = (float)x / (w - 1) * (gc - 1);
                    int gx0 = clamp((int)gx, 0, gc - 2);
                    float fx = gx - gx0;

                    int idx = (y * w + x) * ch;
                    for (int c = 0; c < 3; c++) {
                        int p = (mp >= 3) ? c : 0;
                        int stride = gc * mp;
                        float g00 = gm[gy0 * stride + gx0 * mp + p];
                        float g10 = gm[gy0 * stride + (gx0 + 1) * mp + p];
                        float g01 = gm[(gy0 + 1) * stride + gx0 * mp + p];
                        float g11 = gm[(gy0 + 1) * stride + (gx0 + 1) * mp + p];
                        float gain = g00 * (1 - fx) * (1 - fy) + g10 * fx * (1 - fy)
                                   + g01 * (1 - fx) * fy + g11 * fx * fy;
                        float lin = srgb2lin[data[idx + c]] * gain;
                        float s = (lin <= 0.0031308f)
                            ? lin * 12.92f
                            : 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f;
                        data[idx + c] = (unsigned char)clamp(s * 255.0f, 0.0f, 255.0f);
                    }
                }
            });
        }

        // Pass 2: WarpRectilinear (per-channel polynomial remap)
        if (dngWarpPlanes_ > 0) {
            Pixels corrected;
            corrected.allocate(w, h, ch);
            unsigned char* dst = corrected.getData();

            double cx = dngCx_, cy = dngCy_;
            int np = dngWarpPlanes_;
            DngWarpPlane wp[3];
            for (int i = 0; i < 3; i++) wp[i] = dngWarp_[min(i, np - 1)];

            parallelRows(h, nThreads, [=](int y) {
                double ny = (double)y / (h - 1) - cy;
                for (int x = 0; x < w; x++) {
                    double nx = (double)x / (w - 1) - cx;

                    int dstIdx = (y * w + x) * ch;
                    for (int c = 0; c < 3; c++) {
                        const auto& p = wp[c];
                        double r2 = nx * nx + ny * ny;
                        double r4 = r2 * r2;
                        double r6 = r4 * r2;
                        double factor = p.kr[0] + p.kr[1] * r2 + p.kr[2] * r4 + p.kr[3] * r6;

                        double sx = factor * nx + cx;
                        double sy = factor * ny + cy;

                        float px = (float)(sx * (w - 1));
                        float py = (float)(sy * (h - 1));
                        dst[dstIdx + c] = sampleBilinear(data, w, h, ch, c, px, py);
                    }
                    dst[dstIdx + 3] = 255;
                }
            });

            pixels = std::move(corrected);
        }

        return true;
    }

    // =========================================================================
    // Utility functions
    // =========================================================================

    template<typename Func>
    static void parallelRows(int height, int nThreads, Func func) {
        if (nThreads <= 1) {
            for (int y = 0; y < height; y++) func(y);
            return;
        }
        vector<std::thread> threads;
        threads.reserve(nThreads);
        int rowsPerThread = (height + nThreads - 1) / nThreads;
        for (int t = 0; t < nThreads; t++) {
            int y0 = t * rowsPerThread;
            int y1 = min(y0 + rowsPerThread, height);
            if (y0 >= y1) break;
            threads.emplace_back([=]() {
                for (int y = y0; y < y1; y++) func(y);
            });
        }
        for (auto& th : threads) th.join();
    }

    static unsigned char sampleBilinear(const unsigned char* data,
                                         int w, int h, int ch,
                                         int channel, float fx, float fy) {
        int x0 = (int)floor(fx);
        int y0 = (int)floor(fy);
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, w - 1);
        y0 = clamp(y0, 0, h - 1);
        x1 = clamp(x1, 0, w - 1);
        y1 = clamp(y1, 0, h - 1);

        float dx = fx - floor(fx);
        float dy = fy - floor(fy);

        float v00 = data[(y0 * w + x0) * ch + channel];
        float v10 = data[(y0 * w + x1) * ch + channel];
        float v01 = data[(y1 * w + x0) * ch + channel];
        float v11 = data[(y1 * w + x1) * ch + channel];

        float v = v00 * (1 - dx) * (1 - dy)
                + v10 * dx * (1 - dy)
                + v01 * (1 - dx) * dy
                + v11 * dx * dy;

        return (unsigned char)clamp(v, 0.0f, 255.0f);
    }

    static float sampleBilinearF32(const float* data,
                                    int w, int h, int ch,
                                    int channel, float fx, float fy) {
        int x0 = (int)floor(fx);
        int y0 = (int)floor(fy);
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, w - 1);
        y0 = clamp(y0, 0, h - 1);
        x1 = clamp(x1, 0, w - 1);
        y1 = clamp(y1, 0, h - 1);

        float dx = fx - floor(fx);
        float dy = fy - floor(fy);

        float v00 = data[(y0 * w + x0) * ch + channel];
        float v10 = data[(y0 * w + x1) * ch + channel];
        float v01 = data[(y1 * w + x0) * ch + channel];
        float v11 = data[(y1 * w + x1) * ch + channel];

        return v00 * (1 - dx) * (1 - dy)
             + v10 * dx * (1 - dy)
             + v01 * (1 - dx) * dy
             + v11 * dx * dy;
    }
};
