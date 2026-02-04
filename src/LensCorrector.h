#pragma once

// =============================================================================
// LensCorrector - Lens correction using lensfun XML database
// =============================================================================
// Self-contained implementation matching lensfun v0.3.4 behavior.
// Parses lensfun XML data files and applies corrections on CPU Pixels.
// Correction models: PTLens (distortion), PA (vignetting), poly3 (TCA)
//
// Coordinate system (from lensfun docs):
//   (1) Distortion/TCA: r=1 at half of the short edge (normalized)
//   (2) Vignetting:     r=1 at the image corner (half-diagonal)
//   NormScale converts pixel offsets to (1). Extra 1/AspectRatioCorrection
//   converts (1) to (2) for vignetting.
// =============================================================================

#include <TrussC.h>
#include <pugixml/pugixml.hpp>
#include <cstring>
#include <algorithm>
using namespace std;
using namespace tc;

// -- Data structures ----------------------------------------------------------

struct LensCameraInfo {
    string maker, model, mount;
    float cropFactor = 1.0f;
};

struct LensDistortionData {
    float focal = 0, a = 0, b = 0, c = 0;
};

struct LensVignettingData {
    float focal = 0, aperture = 0, k1 = 0, k2 = 0, k3 = 0;
};

struct LensTcaData {
    float focal = 0, vr = 1.0f, vb = 1.0f;
};

struct LensProfile {
    string maker, model, mount;
    float cropFactor = 1.0f;
    vector<LensDistortionData> distortion;
    vector<LensVignettingData> vignetting;
    vector<LensTcaData> tca;
};

// -- LensCorrector ------------------------------------------------------------

class LensCorrector {
public:
    LensCorrector() = default;

    // Load all XML files from a directory (e.g. bin/data/lensfun/)
    bool loadDatabase(const string& dirPath) {
        cameras_.clear();
        lenses_.clear();

        if (!fs::exists(dirPath)) {
            logWarning() << "[LensCorrector] Database directory not found: " << dirPath;
            return false;
        }

        int fileCount = 0;
        for (auto& entry : fs::directory_iterator(dirPath)) {
            if (entry.path().extension() != ".xml") continue;
            parseXmlFile(entry.path().string());
            fileCount++;
        }

        logNotice() << "[LensCorrector] Loaded " << fileCount << " XML files, "
                     << cameras_.size() << " cameras, " << lenses_.size() << " lenses";
        return !lenses_.empty();
    }

    bool isLoaded() const { return !lenses_.empty(); }

    // Setup correction for a specific camera + lens + shooting parameters
    bool setup(const string& cameraMake, const string& cameraModel,
               const string& lensModel, float focalLength, float aperture,
               int width, int height) {
        ready_ = false;
        if (lenses_.empty()) return false;

        // Find camera (for crop factor)
        float cameraCrop = 1.0f;
        for (auto& cam : cameras_) {
            if (containsIgnoreCase(cam.model, cameraModel) ||
                containsIgnoreCase(cameraModel, cam.model)) {
                cameraCrop = cam.cropFactor;
                break;
            }
        }

        // Find lens
        const LensProfile* found = nullptr;
        for (auto& lens : lenses_) {
            if (containsIgnoreCase(lens.model, lensModel) ||
                containsIgnoreCase(lensModel, lens.model)) {
                found = &lens;
                break;
            }
        }

        if (!found) {
            logWarning() << "[LensCorrector] Lens not found: " << lensModel;
            return false;
        }

        logNotice() << "[LensCorrector] Found lens: " << found->model;

        // Interpolate raw XML coefficients for this focal length / aperture
        interpDistortion(*found, focalLength);
        interpVignetting(*found, focalLength, aperture);
        interpTca(*found, focalLength);

        width_ = width;
        height_ = height;
        hasDistortion_ = !found->distortion.empty();
        hasVignetting_ = !found->vignetting.empty();
        hasTca_ = !found->tca.empty();

        // -- Compute NormScale (lensfun v0.3.4 modifier.cpp constructor) --
        // Width/Height measured at pixel centers (width-1, height-1)
        float W = (float)(width >= 2 ? width - 1 : 1);
        float H = (float)(height >= 2 ? height - 1 : 1);
        float size = min(W, H);
        float imageAR = max(W, H) / size;

        // Lens aspect ratio: default 1.5 (3:2) as in lensfun database.cpp
        float lensAR = 1.5f;
        float arCorrection = sqrt(lensAR * lensAR + 1.0f);  // sqrt(3.25) ≈ 1.803

        float lensCrop = found->cropFactor > 0 ? found->cropFactor : 1.0f;

        // coordinate_correction maps from image sensor coords to calibration sensor coords
        float cc = (1.0f / sqrt(imageAR * imageAR + 1.0f))
                  * (lensCrop / cameraCrop)
                  * arCorrection;

        normScale_ = 2.0f / size * cc;
        normUnScale_ = size * 0.5f / cc;
        arCorrection_ = arCorrection;

        // Image extent in normalized coords
        maxX_ = W * 0.5f * normScale_;
        maxY_ = H * 0.5f * normScale_;

        ready_ = hasDistortion_ || hasVignetting_ || hasTca_;

        // Compute auto-scale to crop black borders from undistortion
        autoScale_ = 1.0f;
        if (hasDistortion_) {
            computeAutoScale();
        }

        if (ready_) {
            logNotice() << "[LensCorrector] Corrections: "
                        << (hasDistortion_ ? "distortion " : "")
                        << (hasVignetting_ ? "vignetting " : "")
                        << (hasTca_ ? "TCA" : "")
                        << " autoScale=" << autoScale_;
        }
        return ready_;
    }

    bool isReady() const { return ready_; }

    // Apply lens corrections to RGBA pixels (auto-detects U8/F32)
    bool apply(Pixels& pixels) {
        if (!ready_) return false;
        if (pixels.isFloat()) return applyFloat(pixels);
        return applyU8(pixels);
    }

private:
    vector<LensCameraInfo> cameras_;
    vector<LensProfile> lenses_;

    bool ready_ = false;
    int width_ = 0, height_ = 0;
    bool hasDistortion_ = false, hasVignetting_ = false, hasTca_ = false;
    float autoScale_ = 1.0f;
    float normScale_ = 0.0f;
    float normUnScale_ = 0.0f;
    float arCorrection_ = 1.0f;
    float maxX_ = 0.0f, maxY_ = 0.0f;

    // Interpolated parameters (raw XML values, no rescaling)
    struct { float a = 0, b = 0, c = 0; } dist_;
    struct { float k1 = 0, k2 = 0, k3 = 0; } vig_;
    struct { float vr = 1.0f, vb = 1.0f; } tca_;

    // -- AutoScale (lensfun v0.3.4 correction mode: Scale priority 100, Dist priority 750) --
    // For correction (reverse=false), autoScale is a pre-scale applied BEFORE the polynomial.
    // We find the scale S such that for all boundary points:
    //   S * poly(r_boundary * normScale * S) ≤ 1
    // i.e., the source position stays within the original image bounds.
    //
    // For barrel distortion (poly < 1 at edges), autoScale = 1.0 (no cropping needed).
    // For pincushion or mixed (poly > 1 at some edges), autoScale < 1.

    void computeAutoScale() {
        float a = dist_.a, b = dist_.b, c = dist_.c;
        float d = 1.0f - a - b - c;

        // Sample 8 boundary points in normalized coords
        float pts[][2] = {
            { maxX_, 0},    {-maxX_, 0},      // left/right edge midpoints
            {0,  maxY_},    {0, -maxY_},       // top/bottom edge midpoints
            { maxX_,  maxY_}, {-maxX_,  maxY_}, // corners
            { maxX_, -maxY_}, {-maxX_, -maxY_},
        };

        // Evaluate poly at each boundary point to find max source extension
        float maxPoly = 0;
        for (auto& p : pts) {
            float r2 = p[0] * p[0] + p[1] * p[1];
            float r = sqrt(r2);
            if (r < 1e-8f) continue;
            float poly = a * r2 * r + b * r2 + c * r + d;
            if (poly > maxPoly) maxPoly = poly;
        }

        // If poly > 1 at any boundary, source extends beyond original → need to shrink
        if (maxPoly > 1.0f) {
            autoScale_ = 1.0f / maxPoly;
        } else {
            autoScale_ = 1.0f;
        }
    }

    // -- XML Parsing ----------------------------------------------------------

    void parseXmlFile(const string& path) {
        pugi::xml_document doc;
        if (!doc.load_file(path.c_str())) return;

        auto root = doc.child("lensdatabase");
        if (!root) return;

        // Parse cameras
        for (auto node : root.children("camera")) {
            LensCameraInfo cam;
            cam.maker = node.child_value("maker");
            cam.model = node.child_value("model");
            cam.mount = node.child_value("mount");
            cam.cropFactor = node.child("cropfactor").text().as_float(1.0f);
            if (!cam.model.empty()) cameras_.push_back(cam);
        }

        // Parse lenses
        for (auto node : root.children("lens")) {
            LensProfile lens;
            lens.maker = node.child_value("maker");
            lens.model = node.child_value("model");
            lens.mount = node.child_value("mount");
            lens.cropFactor = node.child("cropfactor").text().as_float(1.0f);

            auto cal = node.child("calibration");
            if (cal) {
                for (auto d : cal.children("distortion")) {
                    if (string(d.attribute("model").as_string()) != "ptlens") continue;
                    LensDistortionData dd;
                    dd.focal = d.attribute("focal").as_float();
                    dd.a = d.attribute("a").as_float();
                    dd.b = d.attribute("b").as_float();
                    dd.c = d.attribute("c").as_float();
                    lens.distortion.push_back(dd);
                }

                for (auto v : cal.children("vignetting")) {
                    if (string(v.attribute("model").as_string()) != "pa") continue;
                    LensVignettingData vd;
                    vd.focal = v.attribute("focal").as_float();
                    vd.aperture = v.attribute("aperture").as_float();
                    vd.k1 = v.attribute("k1").as_float();
                    vd.k2 = v.attribute("k2").as_float();
                    vd.k3 = v.attribute("k3").as_float();
                    lens.vignetting.push_back(vd);
                }

                for (auto t : cal.children("tca")) {
                    if (string(t.attribute("model").as_string()) != "poly3") continue;
                    LensTcaData td;
                    td.focal = t.attribute("focal").as_float();
                    td.vr = t.attribute("vr").as_float(1.0f);
                    td.vb = t.attribute("vb").as_float(1.0f);
                    lens.tca.push_back(td);
                }
            }

            if (!lens.model.empty()) lenses_.push_back(lens);
        }
    }

    // -- Interpolation --------------------------------------------------------

    void interpDistortion(const LensProfile& lens, float focal) {
        dist_ = {0, 0, 0};
        if (lens.distortion.empty()) return;

        auto& data = lens.distortion;
        if (data.size() == 1 || focal <= data.front().focal) {
            dist_ = {data.front().a, data.front().b, data.front().c};
            return;
        }
        if (focal >= data.back().focal) {
            dist_ = {data.back().a, data.back().b, data.back().c};
            return;
        }

        for (size_t i = 0; i + 1 < data.size(); i++) {
            if (focal >= data[i].focal && focal <= data[i + 1].focal) {
                float t = (focal - data[i].focal) / (data[i + 1].focal - data[i].focal);
                dist_.a = lerp(data[i].a, data[i + 1].a, t);
                dist_.b = lerp(data[i].b, data[i + 1].b, t);
                dist_.c = lerp(data[i].c, data[i + 1].c, t);
                return;
            }
        }
    }

    void interpVignetting(const LensProfile& lens, float focal, float aperture) {
        vig_ = {0, 0, 0};
        if (lens.vignetting.empty()) return;

        // Deduplicate by (focal, aperture) — XML has distance=10 and distance=1000
        struct FocalAperture { float focal, aperture, k1, k2, k3; };
        vector<FocalAperture> unique;
        for (auto& v : lens.vignetting) {
            bool found = false;
            for (auto& u : unique) {
                if (u.focal == v.focal && u.aperture == v.aperture) { found = true; break; }
            }
            if (!found) unique.push_back({v.focal, v.aperture, v.k1, v.k2, v.k3});
        }

        // Collect unique focal lengths
        vector<float> focals, apertures;
        for (auto& u : unique) {
            if (std::find(focals.begin(), focals.end(), u.focal) == focals.end())
                focals.push_back(u.focal);
            if (std::find(apertures.begin(), apertures.end(), u.aperture) == apertures.end())
                apertures.push_back(u.aperture);
        }
        sort(focals.begin(), focals.end());
        sort(apertures.begin(), apertures.end());

        // Bilinear interpolation over focal and aperture
        auto interpAperture = [&](float f, float apt) -> tuple<float, float, float> {
            vector<FocalAperture*> atFocal;
            for (auto& u : unique) {
                if (u.focal == f) atFocal.push_back(&u);
            }
            if (atFocal.empty()) return {0, 0, 0};
            if (atFocal.size() == 1 || apt <= atFocal.front()->aperture) {
                return {atFocal.front()->k1, atFocal.front()->k2, atFocal.front()->k3};
            }
            if (apt >= atFocal.back()->aperture) {
                return {atFocal.back()->k1, atFocal.back()->k2, atFocal.back()->k3};
            }
            for (size_t i = 0; i + 1 < atFocal.size(); i++) {
                if (apt >= atFocal[i]->aperture && apt <= atFocal[i + 1]->aperture) {
                    float t = (apt - atFocal[i]->aperture) / (atFocal[i + 1]->aperture - atFocal[i]->aperture);
                    return {
                        lerp(atFocal[i]->k1, atFocal[i + 1]->k1, t),
                        lerp(atFocal[i]->k2, atFocal[i + 1]->k2, t),
                        lerp(atFocal[i]->k3, atFocal[i + 1]->k3, t)
                    };
                }
            }
            return {atFocal.back()->k1, atFocal.back()->k2, atFocal.back()->k3};
        };

        focal = clamp(focal, focals.front(), focals.back());
        aperture = clamp(aperture, apertures.front(), apertures.back());

        float f0 = focals.front(), f1 = focals.front();
        for (size_t i = 0; i + 1 < focals.size(); i++) {
            if (focal >= focals[i] && focal <= focals[i + 1]) {
                f0 = focals[i];
                f1 = focals[i + 1];
                break;
            }
        }

        auto [k1a, k2a, k3a] = interpAperture(f0, aperture);
        if (f0 == f1) {
            vig_ = {k1a, k2a, k3a};
            return;
        }

        auto [k1b, k2b, k3b] = interpAperture(f1, aperture);
        float t = (focal - f0) / (f1 - f0);
        vig_.k1 = lerp(k1a, k1b, t);
        vig_.k2 = lerp(k2a, k2b, t);
        vig_.k3 = lerp(k3a, k3b, t);
    }

    void interpTca(const LensProfile& lens, float focal) {
        tca_ = {1.0f, 1.0f};
        if (lens.tca.empty()) return;

        auto& data = lens.tca;
        if (data.size() == 1 || focal <= data.front().focal) {
            tca_ = {data.front().vr, data.front().vb};
            return;
        }
        if (focal >= data.back().focal) {
            tca_ = {data.back().vr, data.back().vb};
            return;
        }

        for (size_t i = 0; i + 1 < data.size(); i++) {
            if (focal >= data[i].focal && focal <= data[i + 1].focal) {
                float t = (focal - data[i].focal) / (data[i + 1].focal - data[i].focal);
                tca_.vr = lerp(data[i].vr, data[i + 1].vr, t);
                tca_.vb = lerp(data[i].vb, data[i + 1].vb, t);
                return;
            }
        }
    }

    // Apply corrections to U8 pixels (original implementation)
    bool applyU8(Pixels& pixels) {
        if (pixels.isFloat()) return false;
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        unsigned char* data = pixels.getData();
        float cx = (w - 1) * 0.5f;
        float cy = (h - 1) * 0.5f;

        // 1. Vignetting correction (in-place, linear light)
        if (hasVignetting_) {
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

            float vigNorm = normScale_ / arCorrection_;
            for (int y = 0; y < h; y++) {
                float dy = (y - cy) * vigNorm;
                float dy2 = dy * dy;
                for (int x = 0; x < w; x++) {
                    float dx = (x - cx) * vigNorm;
                    float r2 = dx * dx + dy2;
                    float r4 = r2 * r2;
                    float r6 = r4 * r2;
                    float c = 1.0f + vig_.k1 * r2 + vig_.k2 * r4 + vig_.k3 * r6;
                    if (c < 0.01f) c = 0.01f;
                    float correction = 1.0f / c;
                    int idx = (y * w + x) * ch;
                    for (int i = 0; i < 3; i++) {
                        float lin = srgb2lin[data[idx + i]] * correction;
                        float s = (lin <= 0.0031308f)
                            ? lin * 12.92f
                            : 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f;
                        data[idx + i] = (unsigned char)clamp(s * 255.0f, 0.0f, 255.0f);
                    }
                }
            }
        }

        // 2. Distortion + TCA correction
        if (hasDistortion_ || hasTca_) {
            Pixels corrected;
            corrected.allocate(w, h, ch);
            unsigned char* dst = corrected.getData();

            float a = dist_.a, b = dist_.b, c = dist_.c;
            float d = 1.0f - a - b - c;

            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int dstIdx = (y * w + x) * ch;
                    float px = (x - cx);
                    float py = (y - cy);
                    float pxs = px * autoScale_;
                    float pys = py * autoScale_;
                    float nx = pxs * normScale_;
                    float ny = pys * normScale_;
                    float r2 = nx * nx + ny * ny;
                    float r = sqrt(r2);

                    float poly = d;
                    if (hasDistortion_ && r > 1e-8f) {
                        poly = a * r2 * r + b * r2 + c * r + d;
                    }

                    for (int i = 0; i < 3; i++) {
                        float tcaScale = 1.0f;
                        if (hasTca_) {
                            if (i == 0) tcaScale = tca_.vr;
                            else if (i == 2) tcaScale = tca_.vb;
                        }
                        float sx = cx + pxs * poly * tcaScale;
                        float sy = cy + pys * poly * tcaScale;
                        dst[dstIdx + i] = sampleBilinear(data, w, h, ch, i, sx, sy);
                    }
                    dst[dstIdx + 3] = 255;
                }
            }

            pixels = std::move(corrected);
        }

        return true;
    }

    // Apply corrections to F32 pixels (high precision)
    bool applyFloat(Pixels& pixels) {
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        float* data = pixels.getDataF32();
        float cx = (w - 1) * 0.5f;
        float cy = (h - 1) * 0.5f;

        // 1. Vignetting correction (in-place, linear light)
        // Input is sRGB float: decode → linear multiply → re-encode
        // Float precision avoids clipping that occurred with 8bit
        if (hasVignetting_) {
            float vigNorm = normScale_ / arCorrection_;
            for (int y = 0; y < h; y++) {
                float dy = (y - cy) * vigNorm;
                float dy2 = dy * dy;
                for (int x = 0; x < w; x++) {
                    float dx = (x - cx) * vigNorm;
                    float r2 = dx * dx + dy2;
                    float r4 = r2 * r2;
                    float r6 = r4 * r2;
                    float c = 1.0f + vig_.k1 * r2 + vig_.k2 * r4 + vig_.k3 * r6;
                    if (c < 0.01f) c = 0.01f;
                    float correction = 1.0f / c;
                    int idx = (y * w + x) * ch;
                    for (int i = 0; i < 3; i++) {
                        // sRGB decode
                        float v = data[idx + i];
                        float lin = (v <= 0.04045f)
                            ? v / 12.92f
                            : powf((v + 0.055f) / 1.055f, 2.4f);
                        // Linear multiply (can exceed 1.0 in float — no clipping)
                        lin *= correction;
                        // sRGB encode
                        float s = (lin <= 0.0031308f)
                            ? lin * 12.92f
                            : 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f;
                        data[idx + i] = clamp(s, 0.0f, 1.0f);
                    }
                }
            }
        }

        // 2. Distortion + TCA correction (float bilinear sampling)
        if (hasDistortion_ || hasTca_) {
            Pixels corrected;
            corrected.allocate(w, h, ch, PixelFormat::F32);
            float* dst = corrected.getDataF32();

            float a = dist_.a, b = dist_.b, c = dist_.c;
            float d = 1.0f - a - b - c;

            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int dstIdx = (y * w + x) * ch;
                    float px = (x - cx);
                    float py = (y - cy);
                    float pxs = px * autoScale_;
                    float pys = py * autoScale_;
                    float nx = pxs * normScale_;
                    float ny = pys * normScale_;
                    float r2 = nx * nx + ny * ny;
                    float r = sqrt(r2);

                    float poly = d;
                    if (hasDistortion_ && r > 1e-8f) {
                        poly = a * r2 * r + b * r2 + c * r + d;
                    }

                    for (int i = 0; i < 3; i++) {
                        float tcaScale = 1.0f;
                        if (hasTca_) {
                            if (i == 0) tcaScale = tca_.vr;
                            else if (i == 2) tcaScale = tca_.vb;
                        }
                        float sx = cx + pxs * poly * tcaScale;
                        float sy = cy + pys * poly * tcaScale;
                        dst[dstIdx + i] = sampleBilinearF32(data, w, h, ch, i, sx, sy);
                    }
                    dst[dstIdx + 3] = 1.0f;
                }
            }

            pixels = std::move(corrected);
        }

        return true;
    }

    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    // -- Helpers --------------------------------------------------------------

    // Normalize lens/camera name for matching:
    // lowercase, remove slashes, collapse whitespace
    static string normalizeName(const string& s) {
        string out;
        out.reserve(s.size());
        bool lastSpace = false;
        for (char c : s) {
            if (c == '/') continue;  // "f/4" -> "f4"
            char lc = ::tolower(c);
            if (lc == ' ') {
                if (!lastSpace) out += ' ';
                lastSpace = true;
            } else {
                out += lc;
                lastSpace = false;
            }
        }
        return out;
    }

    static bool containsIgnoreCase(const string& haystack, const string& needle) {
        if (needle.empty() || haystack.empty()) return false;
        string h = normalizeName(haystack);
        string n = normalizeName(needle);
        return h.find(n) != string::npos;
    }

    static unsigned char sampleBilinear(const unsigned char* data,
                                         int w, int h, int ch,
                                         int channel, float fx, float fy) {
        int x0 = (int)fx;
        int y0 = (int)fy;
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, w - 1);
        y0 = clamp(y0, 0, h - 1);
        x1 = clamp(x1, 0, w - 1);
        y1 = clamp(y1, 0, h - 1);

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

        return (unsigned char)clamp(v, 0.0f, 255.0f);
    }

    static float sampleBilinearF32(const float* data,
                                    int w, int h, int ch,
                                    int channel, float fx, float fy) {
        int x0 = (int)fx;
        int y0 = (int)fy;
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, w - 1);
        y0 = clamp(y0, 0, h - 1);
        x1 = clamp(x1, 0, w - 1);
        y1 = clamp(y1, 0, h - 1);

        float dx = fx - (int)fx;
        float dy = fy - (int)fy;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;

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
