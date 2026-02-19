#pragma once

// =============================================================================
// LensCorrector - Lens correction from EXIF / DNG embedded data
// =============================================================================
// Supports three correction sources:
//   1. Sony ARW: EXIF SubImage1 spline-based distortion/TCA/vignetting
//   2. DNG: OpcodeList WarpRectilinear (polynomial per-plane) + GainMap
//   3. Fujifilm RAF: MakerNote spline-based (same apply as Sony)
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

    // "sony", "dng", "fuji", or "none"
    string correctionSource() const {
        if (!ready_) return "none";
        if (useDng_) return "dng";
        if (useFuji_) return "fuji";
        return "sony";
    }

    // Reset correction state
    void reset() {
        ready_ = false;
        useDng_ = false;
        useFuji_ = false;
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

            bool correctionFound = false;

            // --- Try Sony SubImage1 correction params ---
            auto distTag = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DistortionCorrParams"));
            if (distTag != exif.end()) {
                int nc = (int)distTag->toInt64(0);
                if (nc >= 2 && nc <= 16) {
                    for (int i = 0; i < nc; i++) {
                        exifKnots_[i] = (float)(i + 0.5f) / (float)(nc - 1);
                        exifDistortion_[i] = (float)distTag->toInt64(i + 1) * powf(2.0f, -14.0f) + 1.0f;
                    }
                    exifKnotCount_ = nc;
                    correctionFound = true;

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
                                exifVignetting_[i] = v;
                            }
                            hasExifVig_ = true;
                        }
                    }
                    logNotice() << "[LensCorrector] Using Sony EXIF correction";
                }
            }

            // --- Try DNG OpcodeList (Sigma etc.) ---
            if (!correctionFound) {
                correctionFound = setupDngFromExif(exif);
            }

            // --- Try Fujifilm MakerNote correction params ---
            if (!correctionFound) {
                correctionFound = setupFujiFromExif(exif);
            }

            if (!correctionFound) return false;

            // --- DefaultCropOrigin/Size (common to Sony and DNG) ---
            readDefaultCropFromExif(exif, width, height);

            intW_ = width;
            intH_ = height;
            width_ = width;
            height_ = height;
            ready_ = true;
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
            } else if (type == "fuji") {
                return setupFujiFromJson(j, width, height);
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
        // Sony and Fuji use the same spline-based apply (data pre-computed to same format)
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

    // Source flags
    bool useDng_ = false;
    bool useFuji_ = false;
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
    // Fujifilm JSON restore
    // =========================================================================
    bool setupFujiFromJson(const nlohmann::json& j, int w, int h) {
        auto knotsArr = j.value("knots", nlohmann::json::array());
        auto distArr = j.value("dist", nlohmann::json::array());
        auto caRArr = j.value("caR", nlohmann::json::array());
        auto caBArr = j.value("caB", nlohmann::json::array());
        auto vigArr = j.value("vig", nlohmann::json::array());

        int nc = (int)knotsArr.size();
        if (nc < 2 || nc > 16) return false;

        exifKnotCount_ = nc;

        // Fuji stores explicit knot positions
        for (int i = 0; i < nc; i++)
            exifKnots_[i] = knotsArr[i].get<float>();

        // Distortion: pre-computed scale factors (value/100 + 1)
        if ((int)distArr.size() == nc) {
            for (int i = 0; i < nc; i++)
                exifDistortion_[i] = distArr[i].get<float>();
        } else {
            for (int i = 0; i < nc; i++)
                exifDistortion_[i] = 1.0f;
        }

        // CA: pre-computed factors (value + 1)
        if ((int)caRArr.size() == nc && (int)caBArr.size() == nc) {
            for (int i = 0; i < nc; i++) {
                exifCaR_[i] = caRArr[i].get<float>();
                exifCaB_[i] = caBArr[i].get<float>();
            }
            hasExifTca_ = true;
        }

        // Vignetting: pre-computed fractional brightness (value/100)
        if ((int)vigArr.size() == nc) {
            for (int i = 0; i < nc; i++)
                exifVignetting_[i] = vigArr[i].get<float>();
            hasExifVig_ = true;
        }

        useFuji_ = true;
        width_ = w;
        height_ = h;
        ready_ = true;
        logNotice() << "[LensCorrector] Restored Fuji correction from JSON (nc=" << nc << ")";
        return true;
    }

    // =========================================================================
    // Spline-based apply (shared by Sony and Fuji, F32 and U8)
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

    // Compute minimum auto-scale so distortion remap never samples outside
    // the DefaultCrop bounds (manufacturer's declared effective pixel area).
    // For no-crop case, cropCx/cropCy = image center and bounds = full image.
    float computeExifAutoScale(int srcW, int srcH, int outW, int outH,
                                float cropCx, float cropCy,
                                float srcCx, float srcCy, float invDiag) const {
        float outHalfW = (outW - 1) * 0.5f;
        float outHalfH = (outH - 1) * 0.5f;
        int nc = exifKnotCount_;
        const float* knots = exifKnots_;
        const float* distVals = exifDistortion_;
        const float* caR = exifCaR_;
        const float* caB = exifCaB_;
        bool doTca = hasExifTca_;

        // 4 corners + 4 edge midpoints
        struct Pt { float x, y; };
        Pt tests[] = {
            {0, 0}, {(float)(outW-1), 0},
            {0, (float)(outH-1)}, {(float)(outW-1), (float)(outH-1)},
            {outHalfW, 0}, {outHalfW, (float)(outH-1)},
            {0, outHalfH}, {(float)(outW-1), outHalfH},
        };
        constexpr int nTests = 8;
        // Valid sampling area = DefaultCrop bounds (effective pixels only).
        // For no-crop: cropCx ± outHalfW naturally gives [0, w-1].
        float validMinX = cropCx - outHalfW;
        float validMinY = cropCy - outHalfH;
        float validMaxX = cropCx + outHalfW;
        float validMaxY = cropCy + outHalfH;

        auto checkScale = [&](float s) -> bool {
            float inv = 1.0f / s;
            for (int t = 0; t < nTests; t++) {
                float ix = cropCx + (tests[t].x - outHalfW) * inv;
                float iy = cropCy + (tests[t].y - outHalfH) * inv;
                float px = ix - srcCx, py = iy - srcCy;
                float radius = sqrt(px * px + py * py) * invDiag;
                float dr = interpSpline(knots, distVals, nc, radius);
                float sx = dr * px + srcCx;
                float sy = dr * py + srcCy;
                if (sx < validMinX || sx > validMaxX ||
                    sy < validMinY || sy > validMaxY) return false;
                if (doTca) {
                    float drR = dr * interpSpline(knots, caR, nc, radius);
                    if (drR * px + srcCx < validMinX || drR * px + srcCx > validMaxX ||
                        drR * py + srcCy < validMinY || drR * py + srcCy > validMaxY) return false;
                    float drB = dr * interpSpline(knots, caB, nc, radius);
                    if (drB * px + srcCx < validMinX || drB * px + srcCx > validMaxX ||
                        drB * py + srcCy < validMinY || drB * py + srcCy > validMaxY) return false;
                }
            }
            return true;
        };

        if (checkScale(1.0f)) return 1.0f;
        float lo = 1.0f, hi = 1.5f;
        for (int i = 0; i < 20; i++) {
            float mid = (lo + hi) * 0.5f;
            if (checkScale(mid)) hi = mid; else lo = mid;
        }
        return hi;
    }

    bool applyExifFloat(Pixels& pixels) {
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        float* data = pixels.getDataF32();
        float srcCx = (w - 1) * 0.5f, srcCy = (h - 1) * 0.5f;
        float invDiag = 1.0f / sqrt(srcCx * srcCx + srcCy * srcCy);
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

        // Output dimensions: DefaultCrop if available, else same as source.
        // For smart preview, scale crop coords to match SP resolution.
        int outW, outH;
        float cropCx, cropCy;
        if (hasDefaultCrop_) {
            float scaleX = 1.0f, scaleY = 1.0f;
            if (intW_ > 0 && intH_ > 0 && w != intW_) {
                scaleX = (float)w / intW_;
                scaleY = (float)h / intH_;
            }
            outW = max(1, (int)round(cropW_ * scaleX));
            outH = max(1, (int)round(cropH_ * scaleY));
            cropCx = cropX_ * scaleX + (outW - 1) * 0.5f;
            cropCy = cropY_ * scaleY + (outH - 1) * 0.5f;
        } else {
            outW = w;
            outH = h;
            cropCx = srcCx;
            cropCy = srcCy;
        }

        float autoScale = computeExifAutoScale(w, h, outW, outH,
                                                cropCx, cropCy,
                                                srcCx, srcCy, invDiag);

        // Pass 1: Vignetting (in-place on source)
        if (doVig) {
            parallelRows(h, nThreads, [=](int y) {
                float dy = y - srcCy;
                for (int x = 0; x < w; x++) {
                    float dx = x - srcCx;
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

        // Pass 2: Distortion + TCA + Crop + AutoScale (single remap)
        Pixels corrected;
        corrected.allocate(outW, outH, ch, PixelFormat::F32);
        float* dst = corrected.getDataF32();
        float outHalfW = (outW - 1) * 0.5f;
        float outHalfH = (outH - 1) * 0.5f;
        float invScale = 1.0f / autoScale;

        parallelRows(outH, nThreads, [=](int y) {
            for (int x = 0; x < outW; x++) {
                // Map output pixel to intermediate image coords via crop center + auto-scale
                float ix = cropCx + (x - outHalfW) * invScale;
                float iy = cropCy + (y - outHalfH) * invScale;
                float px = ix - srcCx, py = iy - srcCy;
                float radius = sqrt(px * px + py * py) * invDiag;

                for (int c = 0; c < 3; c++) {
                    float dr = interpSpline(knots, distVals, nc, radius);
                    if (doTca) {
                        if (c == 0) dr *= interpSpline(knots, caR, nc, radius);
                        if (c == 2) dr *= interpSpline(knots, caB, nc, radius);
                    }
                    float sx = dr * px + srcCx;
                    float sy = dr * py + srcCy;
                    dst[(y * outW + x) * ch + c] = sampleBilinearF32(data, w, h, ch, c, sx, sy);
                }
                dst[(y * outW + x) * ch + 3] = 1.0f;
            }
        });

        auto t1 = std::chrono::high_resolution_clock::now();
        logNotice() << "[LensCorrector] EXIF correction: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
            << "ms (" << nThreads << " threads, " << w << "x" << h
            << " -> " << outW << "x" << outH
            << " scale=" << autoScale << ")";

        pixels = std::move(corrected);
        return true;
    }

    bool applyExifU8(Pixels& pixels) {
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        unsigned char* data = pixels.getData();
        float srcCx = (w - 1) * 0.5f, srcCy = (h - 1) * 0.5f;
        float invDiag = 1.0f / sqrt(srcCx * srcCx + srcCy * srcCy);
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

        // Output dimensions (same logic as F32 path)
        int outW, outH;
        float cropCx, cropCy;
        if (hasDefaultCrop_) {
            float scaleX = 1.0f, scaleY = 1.0f;
            if (intW_ > 0 && intH_ > 0 && w != intW_) {
                scaleX = (float)w / intW_;
                scaleY = (float)h / intH_;
            }
            outW = max(1, (int)round(cropW_ * scaleX));
            outH = max(1, (int)round(cropH_ * scaleY));
            cropCx = cropX_ * scaleX + (outW - 1) * 0.5f;
            cropCy = cropY_ * scaleY + (outH - 1) * 0.5f;
        } else {
            outW = w;
            outH = h;
            cropCx = srcCx;
            cropCy = srcCy;
        }

        float autoScale = computeExifAutoScale(w, h, outW, outH,
                                                cropCx, cropCy,
                                                srcCx, srcCy, invDiag);

        // Pass 1: Vignetting (in-place, linear light)
        if (doVig) {
            parallelRows(h, nThreads, [=](int y) {
                float dy = y - srcCy;
                for (int x = 0; x < w; x++) {
                    float dx = x - srcCx;
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

        // Pass 2: Distortion + TCA + Crop + AutoScale (single remap)
        Pixels corrected;
        corrected.allocate(outW, outH, ch);
        unsigned char* dst = corrected.getData();
        float outHalfW = (outW - 1) * 0.5f;
        float outHalfH = (outH - 1) * 0.5f;
        float invScale = 1.0f / autoScale;

        parallelRows(outH, nThreads, [=](int y) {
            for (int x = 0; x < outW; x++) {
                float ix = cropCx + (x - outHalfW) * invScale;
                float iy = cropCy + (y - outHalfH) * invScale;
                float px = ix - srcCx, py = iy - srcCy;
                float radius = sqrt(px * px + py * py) * invDiag;

                for (int c = 0; c < 3; c++) {
                    float dr = interpSpline(knots, distVals, nc, radius);
                    if (doTca) {
                        if (c == 0) dr *= interpSpline(knots, caR, nc, radius);
                        if (c == 2) dr *= interpSpline(knots, caB, nc, radius);
                    }
                    float sx = dr * px + srcCx;
                    float sy = dr * py + srcCy;
                    dst[(y * outW + x) * ch + c] = sampleBilinear(data, w, h, ch, c, sx, sy);
                }
                dst[(y * outW + x) * ch + 3] = 255;
            }
        });

        pixels = std::move(corrected);
        return true;
    }

    // =========================================================================
    // DNG apply (WarpRectilinear + GainMap)
    // =========================================================================

    // Compute minimum auto-scale for DNG WarpRectilinear remap.
    float computeDngAutoScale(int srcW, int srcH, int outW, int outH,
                               float cropCx, float cropCy) const {
        float outHalfW = (outW - 1) * 0.5f;
        float outHalfH = (outH - 1) * 0.5f;
        int np = dngWarpPlanes_;
        DngWarpPlane wp[3];
        for (int i = 0; i < 3; i++) wp[i] = dngWarp_[min(i, np - 1)];
        double cx = dngCx_, cy = dngCy_;
        // Valid sampling area = DefaultCrop bounds
        float validMinX = cropCx - outHalfW;
        float validMinY = cropCy - outHalfH;
        float validMaxX = cropCx + outHalfW;
        float validMaxY = cropCy + outHalfH;

        struct Pt { float x, y; };
        Pt tests[] = {
            {0, 0}, {(float)(outW-1), 0},
            {0, (float)(outH-1)}, {(float)(outW-1), (float)(outH-1)},
            {outHalfW, 0}, {outHalfW, (float)(outH-1)},
            {0, outHalfH}, {(float)(outW-1), outHalfH},
        };
        constexpr int nTests = 8;

        auto checkScale = [&](float s) -> bool {
            float inv = 1.0f / s;
            for (int t = 0; t < nTests; t++) {
                float ix = cropCx + (tests[t].x - outHalfW) * inv;
                float iy = cropCy + (tests[t].y - outHalfH) * inv;
                double nx = (double)ix / (srcW - 1) - cx;
                double ny = (double)iy / (srcH - 1) - cy;
                double r2 = nx * nx + ny * ny;
                double r4 = r2 * r2;
                double r6 = r4 * r2;
                for (int c = 0; c < 3; c++) {
                    const auto& p = wp[c];
                    double factor = p.kr[0] + p.kr[1] * r2 + p.kr[2] * r4 + p.kr[3] * r6;
                    float sx = (float)((factor * nx + cx) * (srcW - 1));
                    float sy = (float)((factor * ny + cy) * (srcH - 1));
                    if (sx < validMinX || sx > validMaxX ||
                        sy < validMinY || sy > validMaxY) return false;
                }
            }
            return true;
        };

        if (checkScale(1.0f)) return 1.0f;
        float lo = 1.0f, hi = 1.5f;
        for (int i = 0; i < 20; i++) {
            float mid = (lo + hi) * 0.5f;
            if (checkScale(mid)) hi = mid; else lo = mid;
        }
        return hi;
    }

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

        // Pass 2: WarpRectilinear + Crop + AutoScale (single remap)
        if (dngWarpPlanes_ > 0) {
            // Output dimensions
            int outW, outH;
            float cropCx, cropCy;
            if (hasDefaultCrop_) {
                float scaleX = 1.0f, scaleY = 1.0f;
                if (intW_ > 0 && intH_ > 0 && w != intW_) {
                    scaleX = (float)w / intW_;
                    scaleY = (float)h / intH_;
                }
                outW = max(1, (int)round(cropW_ * scaleX));
                outH = max(1, (int)round(cropH_ * scaleY));
                cropCx = cropX_ * scaleX + (outW - 1) * 0.5f;
                cropCy = cropY_ * scaleY + (outH - 1) * 0.5f;
            } else {
                outW = w;
                outH = h;
                cropCx = (w - 1) * 0.5f;
                cropCy = (h - 1) * 0.5f;
            }

            float autoScale = computeDngAutoScale(w, h, outW, outH, cropCx, cropCy);

            Pixels corrected;
            corrected.allocate(outW, outH, ch, PixelFormat::F32);
            float* dst = corrected.getDataF32();

            double cx = dngCx_, cy = dngCy_;
            int np = dngWarpPlanes_;
            DngWarpPlane wp[3];
            for (int i = 0; i < 3; i++) wp[i] = dngWarp_[min(i, np - 1)];
            float outHalfW = (outW - 1) * 0.5f;
            float outHalfH = (outH - 1) * 0.5f;
            float invScale = 1.0f / autoScale;

            parallelRows(outH, nThreads, [=](int y) {
                for (int x = 0; x < outW; x++) {
                    float ix = cropCx + (x - outHalfW) * invScale;
                    float iy = cropCy + (y - outHalfH) * invScale;
                    double nx = (double)ix / (w - 1) - cx;
                    double ny = (double)iy / (h - 1) - cy;

                    int dstIdx = (y * outW + x) * ch;
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
                        dst[dstIdx + c] = sampleBilinearF32(data, w, h, ch, c, px, py);
                    }
                    dst[dstIdx + 3] = 1.0f;
                }
            });

            auto t1 = std::chrono::high_resolution_clock::now();
            logNotice() << "[LensCorrector] DNG correction: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                << "ms (" << nThreads << " threads, " << w << "x" << h
                << " -> " << outW << "x" << outH
                << " scale=" << autoScale << ")";

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

        // Pass 2: WarpRectilinear + Crop + AutoScale (single remap)
        if (dngWarpPlanes_ > 0) {
            int outW, outH;
            float cropCx, cropCy;
            if (hasDefaultCrop_) {
                float scaleX = 1.0f, scaleY = 1.0f;
                if (intW_ > 0 && intH_ > 0 && w != intW_) {
                    scaleX = (float)w / intW_;
                    scaleY = (float)h / intH_;
                }
                outW = max(1, (int)round(cropW_ * scaleX));
                outH = max(1, (int)round(cropH_ * scaleY));
                cropCx = cropX_ * scaleX + (outW - 1) * 0.5f;
                cropCy = cropY_ * scaleY + (outH - 1) * 0.5f;
            } else {
                outW = w;
                outH = h;
                cropCx = (w - 1) * 0.5f;
                cropCy = (h - 1) * 0.5f;
            }

            float autoScale = computeDngAutoScale(w, h, outW, outH, cropCx, cropCy);

            Pixels corrected;
            corrected.allocate(outW, outH, ch);
            unsigned char* dst = corrected.getData();

            double cx = dngCx_, cy = dngCy_;
            int np = dngWarpPlanes_;
            DngWarpPlane wp[3];
            for (int i = 0; i < 3; i++) wp[i] = dngWarp_[min(i, np - 1)];
            float outHalfW = (outW - 1) * 0.5f;
            float outHalfH = (outH - 1) * 0.5f;
            float invScale = 1.0f / autoScale;

            parallelRows(outH, nThreads, [=](int y) {
                for (int x = 0; x < outW; x++) {
                    float ix = cropCx + (x - outHalfW) * invScale;
                    float iy = cropCy + (y - outHalfH) * invScale;
                    double nx = (double)ix / (w - 1) - cx;
                    double ny = (double)iy / (h - 1) - cy;

                    int dstIdx = (y * outW + x) * ch;
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

    // =========================================================================
    // DNG OpcodeList binary parsing (direct from EXIF, no JSON intermediate)
    // =========================================================================

    static uint32_t readBE32(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    static double readBE64f(const uint8_t* p) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
        double d;
        memcpy(&d, &v, 8);
        return d;
    }
    static float readBE32f(const uint8_t* p) {
        uint32_t v = readBE32(p);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }

    // Parse Fujifilm MakerNote lens correction params directly from EXIF
    // Values are pre-computed to Sony-compatible format so applyExifFloat/U8 works as-is
    bool setupFujiFromExif(const Exiv2::ExifData& exif) {
        auto distIt = exif.findKey(Exiv2::ExifKey("Exif.Fujifilm.GeometricDistortionParams"));
        if (distIt == exif.end()) return false;

        int count = (int)distIt->count();
        // X-Trans IV/V: 19 values (1 header + 9 knots + 9 coeffs)
        // X-Trans I/II/III: 23 values (1 header + 11 knots + 11 coeffs)
        int nc;
        if (count == 19) nc = 9;
        else if (count == 23) nc = 11;
        else return false;

        exifKnotCount_ = nc;

        // Read knot positions (values[1..nc]) — explicit normalized radius
        for (int i = 0; i < nc; i++)
            exifKnots_[i] = distIt->toFloat(1 + i);

        // Distortion: values[nc+1..2*nc] → factor = value / 100 + 1
        for (int i = 0; i < nc; i++)
            exifDistortion_[i] = distIt->toFloat(1 + nc + i) / 100.0f + 1.0f;

        logNotice() << "[LensCorrector] Fuji distortion: nc=" << nc
                    << " first=" << exifDistortion_[0]
                    << " last=" << exifDistortion_[nc - 1];

        // Chromatic Aberration (R + B channels)
        auto caIt = exif.findKey(Exiv2::ExifKey("Exif.Fujifilm.ChromaticAberrationParams"));
        if (caIt != exif.end()) {
            int caCount = (int)caIt->count();
            // IV/V: 29 = 1 header + 9 knots + 9 R + 9 B + 1 trailer
            // I/II/III: 31 = 1 header + 10 knots + 10 R + 10 B (knot 0 at index 0)
            if ((nc == 9 && caCount == 29) || (nc == 11 && caCount >= 31)) {
                for (int i = 0; i < nc; i++) {
                    exifCaR_[i] = caIt->toFloat(1 + nc + i) + 1.0f;
                    exifCaB_[i] = caIt->toFloat(1 + nc * 2 + i) + 1.0f;
                }
                hasExifTca_ = true;
            }
        }

        // Vignetting
        auto vigIt = exif.findKey(Exiv2::ExifKey("Exif.Fujifilm.VignettingParams"));
        if (vigIt != exif.end()) {
            int vigCount = (int)vigIt->count();
            if (vigCount == count) {  // same structure as distortion
                for (int i = 0; i < nc; i++) {
                    // Fuji vignetting is percentage brightness (e.g., 95.7 = 95.7%)
                    // Convert to fractional: 95.7 / 100 = 0.957
                    exifVignetting_[i] = vigIt->toFloat(1 + nc + i) / 100.0f;
                }
                hasExifVig_ = true;
            }
        }

        useFuji_ = true;
        logNotice() << "[LensCorrector] Using Fuji EXIF correction"
                    << " tca=" << hasExifTca_ << " vig=" << hasExifVig_;
        return true;
    }

    // Parse DNG OpcodeList3/2 directly from EXIF data into member variables
    bool setupDngFromExif(const Exiv2::ExifData& exif) {
        bool hasWarp = false, hasGain = false;

        // OpcodeList3: WarpRectilinear (distortion + TCA)
        auto op3It = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.OpcodeList3"));
        if (op3It != exif.end()) {
            auto& val = op3It->value();
            size_t sz = val.size();
            vector<uint8_t> buf(sz);
            val.copy((Exiv2::byte*)buf.data(), Exiv2::invalidByteOrder);

            if (sz >= 4) {
                uint32_t numOps = readBE32(buf.data());
                size_t pos = 4;
                for (uint32_t op = 0; op < numOps && pos + 16 <= sz; op++) {
                    uint32_t opcodeId = readBE32(buf.data() + pos);
                    uint32_t paramBytes = readBE32(buf.data() + pos + 12);
                    pos += 16;
                    if (opcodeId == 1 && pos + paramBytes <= sz) {
                        const uint8_t* d = buf.data() + pos;
                        uint32_t nPlanes = readBE32(d);
                        if (nPlanes >= 1 && nPlanes <= 3) {
                            dngWarpPlanes_ = nPlanes;
                            size_t off = 4;
                            for (uint32_t p = 0; p < nPlanes; p++) {
                                for (int k = 0; k < 4; k++) {
                                    dngWarp_[p].kr[k] = readBE64f(d + off);
                                    off += 8;
                                }
                                for (int k = 0; k < 2; k++) {
                                    dngWarp_[p].kt[k] = readBE64f(d + off);
                                    off += 8;
                                }
                            }
                            dngCx_ = readBE64f(d + off); off += 8;
                            dngCy_ = readBE64f(d + off);
                            hasWarp = true;
                        }
                    }
                    pos += paramBytes;
                }
            }
        }

        // OpcodeList2: GainMap (vignetting)
        auto op2It = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.OpcodeList2"));
        if (op2It != exif.end()) {
            auto& val = op2It->value();
            size_t sz = val.size();
            vector<uint8_t> buf(sz);
            val.copy((Exiv2::byte*)buf.data(), Exiv2::invalidByteOrder);

            if (sz >= 4) {
                uint32_t numOps = readBE32(buf.data());
                size_t pos = 4;
                for (uint32_t op = 0; op < numOps && pos + 16 <= sz; op++) {
                    uint32_t opcodeId = readBE32(buf.data() + pos);
                    uint32_t paramBytes = readBE32(buf.data() + pos + 12);
                    pos += 16;
                    if (opcodeId == 9 && pos + paramBytes <= sz && paramBytes >= 76) {
                        const uint8_t* d = buf.data() + pos;
                        uint32_t rows = readBE32(d + 32);
                        uint32_t cols = readBE32(d + 36);
                        uint32_t mapPlanes = readBE32(d + 72);
                        uint32_t totalPts = rows * cols * mapPlanes;
                        if (76 + totalPts * 4 <= paramBytes && totalPts > 0 && totalPts < 100000) {
                            dngGainRows_ = rows;
                            dngGainCols_ = cols;
                            dngGainMapPlanes_ = mapPlanes;
                            dngGainMap_.resize(totalPts);
                            for (uint32_t i = 0; i < totalPts; i++) {
                                dngGainMap_[i] = readBE32f(d + 76 + i * 4);
                            }
                            hasGain = true;
                        }
                    }
                    pos += paramBytes;
                }
            }
        }

        if (!hasWarp && !hasGain) return false;

        useDng_ = true;
        logNotice() << "[LensCorrector] DNG OpcodeList from EXIF: warp="
                    << dngWarpPlanes_ << "planes gain="
                    << dngGainRows_ << "x" << dngGainCols_;
        return true;
    }

    // Read DefaultCropOrigin/Size from EXIF and set crop members.
    // Handles portrait rotation (EXIF stores in landscape orientation).
    void readDefaultCropFromExif(const Exiv2::ExifData& exif, int width, int height) {
        auto cropOrig = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DefaultCropOrigin"));
        auto cropSize = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DefaultCropSize"));
        if (cropOrig == exif.end() || cropSize == exif.end() ||
            cropOrig->count() < 2 || cropSize->count() < 2) return;

        int cx = (int)cropOrig->toInt64(0);
        int cy = (int)cropOrig->toInt64(1);
        int cw = (int)cropSize->toInt64(0);
        int ch = (int)cropSize->toInt64(1);

        // Detect rotation: if crop is landscape but image is portrait,
        // the image was rotated by LibRaw.
        bool cropIsLandscape = (cw > ch);
        bool imageIsPortrait = (width < height);

        if (cropIsLandscape && imageIsPortrait) {
            int orient = 1;
            auto orientTag = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
            if (orientTag != exif.end()) orient = (int)orientTag->toInt64();

            if (orient == 6) {
                cropX_ = width - cy - ch;
                cropY_ = cx;
                cropW_ = ch;
                cropH_ = cw;
            } else if (orient == 8) {
                cropX_ = cy;
                cropY_ = height - cx - cw;
                cropW_ = ch;
                cropH_ = cw;
            } else {
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
};
