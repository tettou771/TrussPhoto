#pragma once

// =============================================================================
// LensCorrector - Lens correction using lensfun XML database
// =============================================================================
// Self-contained implementation. No lensfun library dependency.
// Parses lensfun XML data files and applies corrections on CPU Pixels.
// Correction models: PTLens (distortion), PA (vignetting), poly3 (TCA)
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
        float cropFactor = 1.0f;
        for (auto& cam : cameras_) {
            if (containsIgnoreCase(cam.model, cameraModel) ||
                containsIgnoreCase(cameraModel, cam.model)) {
                cropFactor = cam.cropFactor;
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

        // Interpolate correction parameters for this focal length / aperture
        interpDistortion(*found, focalLength);
        interpVignetting(*found, focalLength, aperture);
        interpTca(*found, focalLength);

        width_ = width;
        height_ = height;
        cropFactor_ = cropFactor;
        hasDistortion_ = !found->distortion.empty();
        hasVignetting_ = !found->vignetting.empty();
        hasTca_ = !found->tca.empty();
        ready_ = hasDistortion_ || hasVignetting_ || hasTca_;

        if (ready_) {
            logNotice() << "[LensCorrector] Corrections: "
                        << (hasDistortion_ ? "distortion " : "")
                        << (hasVignetting_ ? "vignetting " : "")
                        << (hasTca_ ? "TCA" : "");
        }
        return ready_;
    }

    bool isReady() const { return ready_; }

    // Apply lens corrections to RGBA pixels
    bool apply(Pixels& pixels) {
        if (!ready_) return false;

        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        unsigned char* data = pixels.getData();
        float cx = w * 0.5f;
        float cy = h * 0.5f;
        float rNorm = min(cx, cy); // normalization radius

        // 1. Vignetting correction (in-place)
        if (hasVignetting_) {
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    float dx = (x - cx) / rNorm;
                    float dy = (y - cy) / rNorm;
                    float r2 = dx * dx + dy * dy;
                    float r4 = r2 * r2;
                    float r6 = r4 * r2;
                    float v = 1.0f + vig_.k1 * r2 + vig_.k2 * r4 + vig_.k3 * r6;
                    if (v > 0.0f) {
                        float correction = 1.0f / v;
                        int idx = (y * w + x) * ch;
                        for (int c = 0; c < 3; c++) {
                            float val = data[idx + c] * correction;
                            data[idx + c] = (unsigned char)clamp(val, 0.0f, 255.0f);
                        }
                    }
                }
            }
        }

        // 2. Distortion + TCA correction (coordinate remap)
        if (hasDistortion_ || hasTca_) {
            Pixels corrected;
            corrected.allocate(w, h, ch);
            unsigned char* dst = corrected.getData();

            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int dstIdx = (y * w + x) * ch;

                    // Normalized coordinates from center
                    float dx = (x - cx) / rNorm;
                    float dy = (y - cy) / rNorm;
                    float r = sqrt(dx * dx + dy * dy);

                    // Per-channel coordinate remap
                    for (int c = 0; c < 3; c++) {
                        float channelR = r;

                        // TCA: scale radius per channel
                        if (hasTca_) {
                            if (c == 0) channelR *= tca_.vr;      // Red
                            else if (c == 2) channelR *= tca_.vb; // Blue
                            // Green: no change
                        }

                        // Distortion: PTLens model
                        float newR = channelR;
                        if (hasDistortion_ && channelR > 0.0001f) {
                            float r2 = channelR * channelR;
                            float r3 = r2 * channelR;
                            float d = 1.0f - dist_.a - dist_.b - dist_.c;
                            newR = dist_.a * r3 + dist_.b * r2 + dist_.c * channelR + d * channelR;
                        }

                        // Map back to pixel coordinates
                        float scale = (r > 0.0001f) ? (newR / r) : 1.0f;
                        float sx = cx + dx * scale * rNorm;
                        float sy = cy + dy * scale * rNorm;

                        dst[dstIdx + c] = sampleBilinear(data, w, h, ch, c, sx, sy);
                    }
                    dst[dstIdx + 3] = 255; // Alpha
                }
            }

            pixels = std::move(corrected);
        }

        return true;
    }

private:
    vector<LensCameraInfo> cameras_;
    vector<LensProfile> lenses_;

    bool ready_ = false;
    int width_ = 0, height_ = 0;
    float cropFactor_ = 1.0f;
    bool hasDistortion_ = false, hasVignetting_ = false, hasTca_ = false;

    // Interpolated parameters for current setup
    struct { float a = 0, b = 0, c = 0; } dist_;
    struct { float k1 = 0, k2 = 0, k3 = 0; } vig_;
    struct { float vr = 1.0f, vb = 1.0f; } tca_;

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

        // Find bracketing entries and lerp
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

        // Deduplicate by (focal, aperture) — XML has distance=10 and distance=1000 duplicates
        // Just pick unique (focal, aperture) pairs
        struct FocalAperture { float focal, aperture, k1, k2, k3; };
        vector<FocalAperture> unique;
        for (auto& v : lens.vignetting) {
            bool found = false;
            for (auto& u : unique) {
                if (u.focal == v.focal && u.aperture == v.aperture) { found = true; break; }
            }
            if (!found) unique.push_back({v.focal, v.aperture, v.k1, v.k2, v.k3});
        }

        // Collect unique focal lengths and apertures
        vector<float> focals, apertures;
        for (auto& u : unique) {
            if (std::find(focals.begin(), focals.end(), u.focal) == focals.end())
                focals.push_back(u.focal);
            if (std::find(apertures.begin(), apertures.end(), u.aperture) == apertures.end())
                apertures.push_back(u.aperture);
        }
        sort(focals.begin(), focals.end());
        sort(apertures.begin(), apertures.end());

        // Find data for a given focal+aperture
        auto findData = [&](float f, float a) -> FocalAperture* {
            for (auto& u : unique) {
                if (u.focal == f && u.aperture == a) return &u;
            }
            return nullptr;
        };

        // Bilinear interpolation over focal and aperture
        // First interpolate over aperture for the two bracketing focals
        auto interpAperture = [&](float f, float apt) -> tuple<float, float, float> {
            // Find entries at this focal
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

        // Clamp focal
        focal = clamp(focal, focals.front(), focals.back());
        aperture = clamp(aperture, apertures.front(), apertures.back());

        // Find bracketing focals
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

    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    // -- Helpers --------------------------------------------------------------

    static bool containsIgnoreCase(const string& haystack, const string& needle) {
        if (needle.empty() || haystack.empty()) return false;
        string h = haystack, n = needle;
        transform(h.begin(), h.end(), h.begin(), ::tolower);
        transform(n.begin(), n.end(), n.begin(), ::tolower);
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
};
