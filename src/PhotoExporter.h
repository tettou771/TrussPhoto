#pragma once

// =============================================================================
// PhotoExporter.h - Export developed photos to JPEG
// =============================================================================
// Reads the DevelopShader FBO (RGB10A2) via Metal blit, optionally resizes,
// and saves as JPEG using stbi_write_jpg.
// =============================================================================

#include <TrussC.h>
#include "DevelopShader.h"

using namespace std;
using namespace tc;

struct ExportSettings {
    int maxEdge = 2560;    // 0 = no resize
    int quality = 92;      // JPEG quality (1-100)
};

namespace PhotoExporter {

// Metal readback: RGB10A2 sg_image -> Pixels U8 RGBA
// Implemented in PhotoExporter.mm (Metal API)
bool readFboPixels(sg_image fboImg, int w, int h, Pixels& outPixels);

// Bilinear downscale (U8 RGBA)
inline void resizeU8(const Pixels& src, Pixels& dst, int newW, int newH) {
    int srcW = src.getWidth(), srcH = src.getHeight();
    int ch = src.getChannels();
    dst.allocate(newW, newH, ch);
    auto* srcData = src.getData();
    auto* dstData = dst.getData();
    for (int y = 0; y < newH; y++) {
        float srcYf = (y + 0.5f) * srcH / newH - 0.5f;
        int y0 = max(0, (int)floor(srcYf));
        int y1 = min(srcH - 1, y0 + 1);
        float fy = srcYf - y0;
        for (int x = 0; x < newW; x++) {
            float srcXf = (x + 0.5f) * srcW / newW - 0.5f;
            int x0 = max(0, (int)floor(srcXf));
            int x1 = min(srcW - 1, x0 + 1);
            float fx = srcXf - x0;
            for (int c = 0; c < ch; c++) {
                float v00 = srcData[(y0 * srcW + x0) * ch + c];
                float v10 = srcData[(y0 * srcW + x1) * ch + c];
                float v01 = srcData[(y1 * srcW + x0) * ch + c];
                float v11 = srcData[(y1 * srcW + x1) * ch + c];
                float v = v00*(1-fx)*(1-fy) + v10*fx*(1-fy)
                        + v01*(1-fx)*fy + v11*fx*fy;
                dstData[(y * newW + x) * ch + c] = (unsigned char)clamp(v, 0.f, 255.f);
            }
        }
    }
}

// Full export pipeline
inline bool exportJpeg(const DevelopShader& shader,
                       const string& outPath,
                       const ExportSettings& settings = {}) {
    if (!shader.isFboReady()) return false;

    // 1. Read FBO
    Pixels pixels;
    if (!readFboPixels(shader.getFboImage(),
                       shader.getFboWidth(), shader.getFboHeight(), pixels))
        return false;

    // 2. Resize if needed
    Pixels* outPtr = &pixels;
    Pixels resized;
    if (settings.maxEdge > 0) {
        int w = pixels.getWidth(), h = pixels.getHeight();
        int longEdge = max(w, h);
        if (longEdge > settings.maxEdge) {
            float scale = (float)settings.maxEdge / longEdge;
            int newW = max(1, (int)round(w * scale));
            int newH = max(1, (int)round(h * scale));
            resizeU8(pixels, resized, newW, newH);
            outPtr = &resized;
        }
    }

    // 3. Create output directory
    fs::create_directories(fs::path(outPath).parent_path());

    // 4. Save JPEG (RGB, drop alpha)
    int w = outPtr->getWidth(), h = outPtr->getHeight();
    int ch = outPtr->getChannels();
    if (ch == 4) {
        // Strip alpha: RGBA -> RGB for JPEG
        vector<unsigned char> rgb(w * h * 3);
        auto* src = outPtr->getData();
        for (int i = 0; i < w * h; i++) {
            rgb[i*3+0] = src[i*4+0];
            rgb[i*3+1] = src[i*4+1];
            rgb[i*3+2] = src[i*4+2];
        }
        return stbi_write_jpg(outPath.c_str(), w, h, 3,
                              rgb.data(), settings.quality) != 0;
    }
    return stbi_write_jpg(outPath.c_str(), w, h, ch,
                          outPtr->getData(), settings.quality) != 0;
}

// Export path: catalog/exports/stem.jpg (auto-increment if exists)
inline string makeExportPath(const string& catalogPath,
                             const string& originalFilename) {
    string stem = fs::path(originalFilename).stem().string();
    string dir = catalogPath + "/exports";
    fs::create_directories(dir);

    string path = dir + "/" + stem + ".jpg";
    if (!fs::exists(path)) return path;

    for (int i = 2; i < 10000; i++) {
        path = dir + "/" + stem + "_" + to_string(i) + ".jpg";
        if (!fs::exists(path)) return path;
    }
    return path;
}

} // namespace PhotoExporter
