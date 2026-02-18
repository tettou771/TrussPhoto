#pragma once

// =============================================================================
// LensCorrector - Lens correction using lensfun XML database
// =============================================================================
// Self-contained implementation. Parses lensfun XML data files and applies
// corrections on CPU Pixels.
// Correction models: PTLens (distortion), PA (vignetting), poly3 (TCA)
//
// Coordinate approach (lensfun v0.3.4 compatible):
//   XML version_1 database coefficients are used directly (no rescaling).
//   NormScale = 2.0 / min(W,H) * coordinate_correction
//   where coordinate_correction adjusts for crop/AR mismatch between
//   the calibration sensor and the runtime sensor.
//
// PTLens distortion model:
//   r_dist = r_undist * (a*r³ + b*r² + c*r + d)   where d = 1-a-b-c
//   For correction: source = output * poly(r_norm)  (forward polynomial)
//   r_norm = pixel_distance * NormScale
// =============================================================================

#include <TrussC.h>
#include <pugixml/pugixml.hpp>
#include <exiv2/exiv2.hpp>
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
    float cr = 0, cb = 0, br = 0, bb = 0;
};

struct LensProfile {
    string maker, model, mount;
    float cropFactor = 1.0f;
    float aspectRatio = 1.5f;  // default 3:2
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

    // Load lens aliases from JSON file (EXIF name -> lensfun name)
    void loadAliases(const string& jsonPath) {
        lensAliases_.clear();
        if (!fs::exists(jsonPath)) return;

        ifstream f(jsonPath);
        if (!f.is_open()) return;
        string content((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        f.close();

        // Minimal JSON object parser for {"key": "value", ...}
        size_t pos = content.find('{');
        if (pos == string::npos) return;
        pos++;
        while (pos < content.size()) {
            size_t ks = content.find('"', pos);
            if (ks == string::npos) break;
            size_t ke = content.find('"', ks + 1);
            if (ke == string::npos) break;
            string key = content.substr(ks + 1, ke - ks - 1);
            size_t vs = content.find('"', ke + 1);
            if (vs == string::npos) break;
            size_t ve = content.find('"', vs + 1);
            if (ve == string::npos) break;
            string val = content.substr(vs + 1, ve - vs - 1);
            lensAliases_[key] = val;
            pos = ve + 1;
        }
        if (!lensAliases_.empty()) {
            logNotice() << "[LensCorrector] Loaded " << lensAliases_.size() << " lens aliases";
        }
    }

    // Setup correction for a specific camera + lens + shooting parameters
    bool setup(const string& cameraMake, const string& cameraModel,
               const string& lensModel, float focalLength, float aperture,
               int width, int height) {
        ready_ = false;
        useExif_ = false;
        if (lenses_.empty()) return false;

        // Trim whitespace from lens model
        string trimmedLens = lensModel;
        while (!trimmedLens.empty() && trimmedLens.back() == ' ') trimmedLens.pop_back();

        // Apply lens alias
        string resolvedLens = trimmedLens;
        auto aliasIt = lensAliases_.find(trimmedLens);
        if (aliasIt != lensAliases_.end()) {
            resolvedLens = aliasIt->second;
            logNotice() << "[LensCorrector] Alias: " << trimmedLens << " -> " << resolvedLens;
        }

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
            if (containsIgnoreCase(lens.model, resolvedLens) ||
                containsIgnoreCase(resolvedLens, lens.model)) {
                found = &lens;
                break;
            }
        }

        if (!found) {
            logWarning() << "[LensCorrector] Lens not found: " << trimmedLens;
            return false;
        }

        logNotice() << "[LensCorrector] Found lens: " << found->model;

        // Interpolate raw XML coefficients for this focal length / aperture
        interpDistortion(*found, focalLength);
        interpVignetting(*found, focalLength, aperture);
        interpTca(*found, focalLength);

        // Calibration parameters
        float lensCrop = found->cropFactor > 0 ? found->cropFactor : 1.0f;
        float lensAR = found->aspectRatio > 0 ? found->aspectRatio : 1.5f;

        width_ = width;
        height_ = height;
        hasDistortion_ = !found->distortion.empty();
        hasVignetting_ = !found->vignetting.empty();
        hasTca_ = !found->tca.empty();

        // Constant term from raw interpolated coefficients
        distD_ = 1.0f - dist_.a - dist_.b - dist_.c;

        // NormScale: lensfun v0.3.4 formula
        // "size" = min dimension (pixel count - 1)
        float W = width >= 2 ? (float)(width - 1) : 1.0f;
        float H = height >= 2 ? (float)(height - 1) : 1.0f;
        float size = min(W, H);
        float imageAR = (W < H) ? H / W : W / H;

        // coordinate_correction maps image sensor coords → calibration sensor coords
        // cc = 1/sqrt(imageAR²+1) * calibCrop/cameraCrop * sqrt(lensAR²+1)
        float cc = (1.0f / sqrt(imageAR * imageAR + 1.0f))
                 * (lensCrop / cameraCrop)
                 * sqrt(lensAR * lensAR + 1.0f);

        normScale_ = 2.0f / size * cc;
        normUnScale_ = size * 0.5f / cc;

        // Vignetting uses coordinate system (2): r=1 at image corner
        // vigScale_ = NormScale / AspectRatioCorrection
        // In lensfun 0.3.4, this strips the lensAR factor from NormScale
        float vigCC = (1.0f / sqrt(imageAR * imageAR + 1.0f))
                    * (lensCrop / cameraCrop);
        vigScale_ = 2.0f / size * vigCC;

        // No coefficient rescaling — XML version_1 data used as-is

        ready_ = hasDistortion_ || hasVignetting_ || hasTca_;

        // Compute auto-scale to crop black borders from distortion correction
        autoScale_ = 1.0f;
        if (hasDistortion_) {
            computeAutoScale();
        }

        if (ready_) {
            logNotice() << "[LensCorrector] Corrections: "
                        << (hasDistortion_ ? "distortion " : "")
                        << (hasVignetting_ ? "vignetting " : "")
                        << (hasTca_ ? "TCA" : "")
                        << " autoScale=" << autoScale_
                        << " normScale=" << normScale_
                        << " distD=" << distD_
                        << " dist=[" << dist_.a << "," << dist_.b << "," << dist_.c << "]";
        }
        return ready_;
    }

    bool isReady() const { return ready_; }
    bool isExifMode() const { return useExif_; }

    // Reset correction state (call before starting new background load)
    void reset() { ready_ = false; useExif_ = false; }

    // Setup from Sony EXIF embedded correction data (priority over lensfun)
    // Returns true if valid EXIF correction data was found.
    bool setupFromExif(const string& rawFilePath, int width, int height) {
        ready_ = false;
        useExif_ = false;
        hasExifTca_ = false;
        hasExifVig_ = false;
        exifKnotCount_ = 0;

        try {
            auto image = Exiv2::ImageFactory::open(rawFilePath);
            image->readMetadata();
            auto& exif = image->exifData();

            // Distortion correction (required)
            auto distTag = exif.findKey(Exiv2::ExifKey("Exif.SubImage1.DistortionCorrParams"));
            if (distTag == exif.end()) return false;

            int nc = (int)distTag->toInt64(0);
            if (nc < 2 || nc > 16) return false;

            // Knot positions: (i + 0.5) / (nc - 1), diagonal-normalized radius
            // Correction factor: raw * 2^(-14) + 1 (magnification scale)
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

            useExif_ = true;
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

    // Apply lens corrections to RGBA pixels (auto-detects U8/F32, EXIF/lensfun)
    bool apply(Pixels& pixels) {
        if (!ready_) return false;
        if (useExif_) {
            return pixels.isFloat() ? applyExifFloat(pixels) : applyExifU8(pixels);
        }
        if (pixels.isFloat()) return applyFloat(pixels);
        return applyU8(pixels);
    }

private:
    vector<LensCameraInfo> cameras_;
    vector<LensProfile> lenses_;
    unordered_map<string, string> lensAliases_;

    bool ready_ = false;
    int width_ = 0, height_ = 0;
    bool hasDistortion_ = false, hasVignetting_ = false, hasTca_ = false;
    float autoScale_ = 1.0f;

    // NormScale coordinate conversion (lensfun 0.3.4 compatible)
    float normScale_ = 0;    // pixel → NormScale coord (1)
    float normUnScale_ = 0;  // NormScale → pixel
    float vigScale_ = 0;     // pixel → vignetting coord (2)

    // Raw interpolated parameters (no rescaling, used directly)
    struct { float a = 0, b = 0, c = 0; } dist_;
    float distD_ = 1.0f;  // d = 1-a-b-c
    struct { float k1 = 0, k2 = 0, k3 = 0; } vig_;
    struct { float vr = 1.0f, vb = 1.0f, cr = 0, cb = 0, br = 0, bb = 0; } tca_;

    // EXIF embedded correction data (Sony ARW)
    bool useExif_ = false;
    int exifKnotCount_ = 0;
    float exifKnots_[16] = {};
    float exifDistortion_[16] = {};   // magnification factor per knot
    float exifCaR_[16] = {};          // R channel TCA multiplier
    float exifCaB_[16] = {};          // B channel TCA multiplier
    float exifVignetting_[16] = {};   // vignetting correction factor
    bool hasExifTca_ = false;
    bool hasExifVig_ = false;

    // -- AutoScale ------------------------------------------------------------
    // Forward polynomial: source = output * poly(r_norm)
    // poly > 1 at boundary → source extends beyond image → need shrinking.
    // poly < 1 at boundary → barrel correction, no black borders.

    void computeAutoScale() {
        float a = dist_.a, b = dist_.b, c = dist_.c, dd = distD_;
        float cx = (width_ - 1) * 0.5f;
        float cy = (height_ - 1) * 0.5f;
        float ns = normScale_;

        // Sample boundary points (pixel distance from center)
        float pts[][2] = {
            { cx, 0},    {-cx, 0},
            {0,  cy},    {0, -cy},
            { cx,  cy},  {-cx,  cy},
            { cx, -cy},  {-cx, -cy},
        };

        float maxPoly = 0;
        for (auto& p : pts) {
            float nx = p[0] * ns;
            float ny = p[1] * ns;
            float r2 = nx * nx + ny * ny;
            float r = sqrt(r2);
            if (r < 1e-8f) continue;
            float poly = a * r2 * r + b * r2 + c * r + dd;
            if (poly > maxPoly) maxPoly = poly;
        }

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

        for (auto node : root.children("camera")) {
            LensCameraInfo cam;
            cam.maker = node.child_value("maker");
            cam.model = node.child_value("model");
            cam.mount = node.child_value("mount");
            cam.cropFactor = node.child("cropfactor").text().as_float(1.0f);
            if (!cam.model.empty()) cameras_.push_back(cam);
        }

        for (auto node : root.children("lens")) {
            LensProfile lens;
            lens.maker = node.child_value("maker");
            lens.model = node.child_value("model");
            lens.mount = node.child_value("mount");
            lens.cropFactor = node.child("cropfactor").text().as_float(1.0f);
            lens.aspectRatio = parseAspectRatio(node.child("aspect-ratio").text().as_string("3:2"));

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
                    td.cr = t.attribute("cr").as_float(0.0f);
                    td.cb = t.attribute("cb").as_float(0.0f);
                    td.br = t.attribute("br").as_float(0.0f);
                    td.bb = t.attribute("bb").as_float(0.0f);
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

        struct FocalAperture { float focal, aperture, k1, k2, k3; };
        vector<FocalAperture> unique;
        for (auto& v : lens.vignetting) {
            bool found = false;
            for (auto& u : unique) {
                if (u.focal == v.focal && u.aperture == v.aperture) { found = true; break; }
            }
            if (!found) unique.push_back({v.focal, v.aperture, v.k1, v.k2, v.k3});
        }

        vector<float> focals, apertures;
        for (auto& u : unique) {
            if (std::find(focals.begin(), focals.end(), u.focal) == focals.end())
                focals.push_back(u.focal);
            if (std::find(apertures.begin(), apertures.end(), u.aperture) == apertures.end())
                apertures.push_back(u.aperture);
        }
        sort(focals.begin(), focals.end());
        sort(apertures.begin(), apertures.end());

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
        tca_ = {1.0f, 1.0f, 0, 0, 0, 0};
        if (lens.tca.empty()) return;

        auto& data = lens.tca;
        if (data.size() == 1 || focal <= data.front().focal) {
            tca_ = {data.front().vr, data.front().vb,
                    data.front().cr, data.front().cb,
                    data.front().br, data.front().bb};
            return;
        }
        if (focal >= data.back().focal) {
            tca_ = {data.back().vr, data.back().vb,
                    data.back().cr, data.back().cb,
                    data.back().br, data.back().bb};
            return;
        }

        for (size_t i = 0; i + 1 < data.size(); i++) {
            if (focal >= data[i].focal && focal <= data[i + 1].focal) {
                float t = (focal - data[i].focal) / (data[i + 1].focal - data[i].focal);
                tca_.vr = lerp(data[i].vr, data[i + 1].vr, t);
                tca_.vb = lerp(data[i].vb, data[i + 1].vb, t);
                tca_.cr = lerp(data[i].cr, data[i + 1].cr, t);
                tca_.cb = lerp(data[i].cb, data[i + 1].cb, t);
                tca_.br = lerp(data[i].br, data[i + 1].br, t);
                tca_.bb = lerp(data[i].bb, data[i + 1].bb, t);
                return;
            }
        }
    }

    // -- Apply corrections to U8 pixels (multi-threaded) ----------------------
    bool applyU8(Pixels& pixels) {
        if (pixels.isFloat()) return false;
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        unsigned char* data = pixels.getData();
        float cx = (w - 1) * 0.5f;
        float cy = (h - 1) * 0.5f;
        int nThreads = max(1u, std::thread::hardware_concurrency());

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

            float vs = vigScale_;
            float vk1 = vig_.k1, vk2 = vig_.k2, vk3 = vig_.k3;

            parallelRows(h, nThreads, [=](int y) {
                float dy = (y - cy) * vs;
                float dy2 = dy * dy;
                for (int x = 0; x < w; x++) {
                    float dx = (x - cx) * vs;
                    float r2 = dx * dx + dy2;
                    float r4 = r2 * r2;
                    float r6 = r4 * r2;
                    float c = 1.0f + vk1 * r2 + vk2 * r4 + vk3 * r6;
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
            });
        }

        // 2. Distortion + TCA correction
        if (hasDistortion_ || hasTca_) {
            Pixels corrected;
            corrected.allocate(w, h, ch);
            unsigned char* dst = corrected.getData();

            float a = dist_.a, b = dist_.b, c_coeff = dist_.c, dd = distD_;
            float as = autoScale_, ns = normScale_;
            float tvr = tca_.vr, tvb = tca_.vb;
            float tcr = tca_.cr, tcb = tca_.cb;
            float tbr = tca_.br, tbb = tca_.bb;
            bool doDist = hasDistortion_, doTca = hasTca_;

            parallelRows(h, nThreads, [=](int y) {
                for (int x = 0; x < w; x++) {
                    int dstIdx = (y * w + x) * ch;
                    float px = (x - cx);
                    float py = (y - cy);
                    float pxs = px * as;
                    float pys = py * as;

                    // Radius in NormScale coordinates (1)
                    float nx = pxs * ns;
                    float ny = pys * ns;
                    float r2 = nx * nx + ny * ny;
                    float r = sqrt(r2);

                    // Forward polynomial: poly = a*r³ + b*r² + c*r + d
                    float poly = dd;
                    if (doDist && r > 1e-8f) {
                        poly = a * r2 * r + b * r2 + c_coeff * r + dd;
                    }

                    // Source pixel = output_pixel * poly (NormScale cancels)
                    for (int i = 0; i < 3; i++) {
                        float tcaScale = 1.0f;
                        if (doTca) {
                            if (i == 0) tcaScale = tbr * r2 + tcr * r + tvr;
                            else if (i == 2) tcaScale = tbb * r2 + tcb * r + tvb;
                        }
                        float sx = cx + pxs * poly * tcaScale;
                        float sy = cy + pys * poly * tcaScale;
                        dst[dstIdx + i] = sampleBilinear(data, w, h, ch, i, sx, sy);
                    }
                    dst[dstIdx + 3] = 255;
                }
            });

            pixels = std::move(corrected);
        }

        return true;
    }

    // -- Apply corrections to F32 pixels (high precision, multi-threaded) -----
    bool applyFloat(Pixels& pixels) {
        int w = pixels.getWidth();
        int h = pixels.getHeight();
        int ch = pixels.getChannels();
        if (w != width_ || h != height_ || ch != 4) return false;

        float* data = pixels.getDataF32();
        float cx = (w - 1) * 0.5f;
        float cy = (h - 1) * 0.5f;
        int nThreads = max(1u, std::thread::hardware_concurrency());
        auto t0 = std::chrono::high_resolution_clock::now();

        // 1. Vignetting correction (in-place, linear light)
        if (hasVignetting_) {
            auto tv0 = std::chrono::high_resolution_clock::now();
            float vs = vigScale_;
            float vk1 = vig_.k1, vk2 = vig_.k2, vk3 = vig_.k3;

            parallelRows(h, nThreads, [=](int y) {
                float dy = (y - cy) * vs;
                float dy2 = dy * dy;
                for (int x = 0; x < w; x++) {
                    float dx = (x - cx) * vs;
                    float r2 = dx * dx + dy2;
                    float r4 = r2 * r2;
                    float r6 = r4 * r2;
                    float c = 1.0f + vk1 * r2 + vk2 * r4 + vk3 * r6;
                    if (c < 0.01f) c = 0.01f;
                    float correction = 1.0f / c;
                    int idx = (y * w + x) * ch;
                    for (int i = 0; i < 3; i++) {
                        float v = data[idx + i];
                        float lin = (v <= 0.04045f)
                            ? v / 12.92f
                            : powf((v + 0.055f) / 1.055f, 2.4f);
                        lin *= correction;
                        float s = (lin <= 0.0031308f)
                            ? lin * 12.92f
                            : 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f;
                        data[idx + i] = clamp(s, 0.0f, 1.0f);
                    }
                }
            });
            auto tv1 = std::chrono::high_resolution_clock::now();
            logNotice() << "[LensCorrector] Vignetting: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(tv1 - tv0).count() << "ms";
        }

        // 2. Distortion + TCA correction (float bilinear sampling)
        if (hasDistortion_ || hasTca_) {
            auto td0 = std::chrono::high_resolution_clock::now();
            Pixels corrected;
            corrected.allocate(w, h, ch, PixelFormat::F32);
            float* dst = corrected.getDataF32();

            float a = dist_.a, b = dist_.b, c_coeff = dist_.c, dd = distD_;
            float as = autoScale_, ns = normScale_;
            float tvr = tca_.vr, tvb = tca_.vb;
            float tcr = tca_.cr, tcb = tca_.cb;
            float tbr = tca_.br, tbb = tca_.bb;
            bool doDist = hasDistortion_, doTca = hasTca_;

            parallelRows(h, nThreads, [=](int y) {
                for (int x = 0; x < w; x++) {
                    int dstIdx = (y * w + x) * ch;
                    float px = (x - cx);
                    float py = (y - cy);
                    float pxs = px * as;
                    float pys = py * as;

                    // Radius in NormScale coordinates (1)
                    float nx = pxs * ns;
                    float ny = pys * ns;
                    float r2 = nx * nx + ny * ny;
                    float r = sqrt(r2);

                    // Forward polynomial: poly = a*r³ + b*r² + c*r + d
                    float poly = dd;
                    if (doDist && r > 1e-8f) {
                        poly = a * r2 * r + b * r2 + c_coeff * r + dd;
                    }

                    // Source pixel = output_pixel * poly (NormScale cancels)
                    for (int i = 0; i < 3; i++) {
                        float tcaScale = 1.0f;
                        if (doTca) {
                            if (i == 0) tcaScale = tbr * r2 + tcr * r + tvr;
                            else if (i == 2) tcaScale = tbb * r2 + tcb * r + tvb;
                        }
                        float sx = cx + pxs * poly * tcaScale;
                        float sy = cy + pys * poly * tcaScale;
                        dst[dstIdx + i] = sampleBilinearF32(data, w, h, ch, i, sx, sy);
                    }
                    dst[dstIdx + 3] = 1.0f;
                }
            });

            auto td1 = std::chrono::high_resolution_clock::now();
            logNotice() << "[LensCorrector] Distortion+TCA: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count() << "ms";
            pixels = std::move(corrected);
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        logNotice() << "[LensCorrector] Total: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
            << "ms (" << nThreads << " threads, " << w << "x" << h << ")";
        return true;
    }

    // -- EXIF embedded correction (Sony) -------------------------------------

    // Linear interpolation over spline knots
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

        // Pass 1: Vignetting (in-place, operates in linear light)
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

        // Pass 2: Distortion + TCA (remap into new buffer)
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

    // -- Utility functions ----------------------------------------------------

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

    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    static float parseAspectRatio(const string& s) {
        auto pos = s.find(':');
        if (pos != string::npos && pos > 0 && pos + 1 < s.size()) {
            float num = strtof(s.c_str(), nullptr);
            float den = strtof(s.c_str() + pos + 1, nullptr);
            if (den > 0) return num / den;
        }
        return 1.5f;
    }

    static bool containsIgnoreCase(const string& haystack, const string& needle) {
        if (needle.empty() || haystack.empty()) return false;
        string h(haystack.size(), ' ');
        string n(needle.size(), ' ');
        transform(haystack.begin(), haystack.end(), h.begin(), ::tolower);
        transform(needle.begin(), needle.end(), n.begin(), ::tolower);
        return h.find(n) != string::npos;
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
