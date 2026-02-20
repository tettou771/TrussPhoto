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

// Area-averaging downscale (U8 RGBA)
// Each output pixel averages ALL source pixels in the corresponding region.
// Equivalent to OpenCV INTER_AREA — no pixel skipping, no aliasing.
inline void resizeU8(const Pixels& src, Pixels& dst, int newW, int newH) {
    int srcW = src.getWidth(), srcH = src.getHeight();
    int ch = src.getChannels();
    dst.allocate(newW, newH, ch);
    auto* srcData = src.getData();
    auto* dstData = dst.getData();

    float scaleX = (float)srcW / newW;
    float scaleY = (float)srcH / newH;

    for (int y = 0; y < newH; y++) {
        float srcY0 = y * scaleY;
        float srcY1 = (y + 1) * scaleY;
        int iy0 = (int)srcY0;
        int iy1 = min((int)srcY1, srcH - 1);

        for (int x = 0; x < newW; x++) {
            float srcX0 = x * scaleX;
            float srcX1 = (x + 1) * scaleX;
            int ix0 = (int)srcX0;
            int ix1 = min((int)srcX1, srcW - 1);

            float sum[4] = {0, 0, 0, 0};
            float totalWeight = 0;

            for (int sy = iy0; sy <= iy1; sy++) {
                // Vertical coverage of this source row
                float wy = 1.0f;
                if (sy == iy0) wy = 1.0f - (srcY0 - iy0);
                if (sy == iy1 && iy1 != iy0) wy = srcY1 - iy1;
                if (sy == iy0 && sy == iy1) wy = srcY1 - srcY0;

                for (int sx = ix0; sx <= ix1; sx++) {
                    float wx = 1.0f;
                    if (sx == ix0) wx = 1.0f - (srcX0 - ix0);
                    if (sx == ix1 && ix1 != ix0) wx = srcX1 - ix1;
                    if (sx == ix0 && sx == ix1) wx = srcX1 - srcX0;

                    float w = wx * wy;
                    int idx = (sy * srcW + sx) * ch;
                    for (int c = 0; c < ch; c++) {
                        sum[c] += srcData[idx + c] * w;
                    }
                    totalWeight += w;
                }
            }

            int outIdx = (y * newW + x) * ch;
            float invW = 1.0f / totalWeight;
            for (int c = 0; c < ch; c++) {
                dstData[outIdx + c] = (unsigned char)clamp(sum[c] * invW, 0.f, 255.f);
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
