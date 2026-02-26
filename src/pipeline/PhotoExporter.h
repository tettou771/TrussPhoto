#pragma once

// =============================================================================
// PhotoExporter.h - Export developed photos to JPEG
// =============================================================================
// Reads the DevelopShader FBO (RGB10A2) via Metal blit, optionally resizes,
// and saves as JPEG using stbi_write_jpg.
// =============================================================================

#include <TrussC.h>
#include "DevelopShader.h"
#include "PhotoEntry.h"

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

// Bilinear UV transform with adaptive supersampling.
// corners: {u0,v0, u1,v1, u2,v2, u3,v3} = TL, TR, BR, BL
// Supersampling factor is derived from the quad's max source-pixel footprint
// (min 2x2 for rotation quality, scales up for perspective).
inline void transformU8(const Pixels& src, Pixels& dst,
                        const float corners[8], int outW, int outH) {
    int srcW = src.getWidth(), srcH = src.getHeight();
    int channels = src.getChannels();
    dst.allocate(outW, outH, channels);
    auto* srcData = src.getData();
    auto* dstData = dst.getData();

    float u0 = corners[0], v0 = corners[1]; // TL
    float u1 = corners[2], v1 = corners[3]; // TR
    float u2 = corners[4], v2 = corners[5]; // BR
    float u3 = corners[6], v3 = corners[7]; // BL

    // Estimate max source-pixel footprint per output pixel from quad edges
    float topSpan  = hypot((u1-u0)*srcW, (v1-v0)*srcH) / outW;
    float botSpan  = hypot((u2-u3)*srcW, (v2-v3)*srcH) / outW;
    float leftSpan = hypot((u3-u0)*srcW, (v3-v0)*srcH) / outH;
    float rightSpan= hypot((u2-u1)*srcW, (v2-v1)*srcH) / outH;
    float maxFoot = max({topSpan, botSpan, leftSpan, rightSpan});
    int ss = clamp((int)ceil(maxFoot), 2, 8);
    float invSS2 = 1.0f / (ss * ss);

    for (int y = 0; y < outH; y++) {
        for (int x = 0; x < outW; x++) {
            float sum[4] = {0, 0, 0, 0};

            for (int sj = 0; sj < ss; sj++) {
                float ty = (y + (sj + 0.5f) / ss) / outH;
                float lU = u0 + (u3-u0)*ty, lV = v0 + (v3-v0)*ty;
                float rU = u1 + (u2-u1)*ty, rV = v1 + (v2-v1)*ty;

                for (int si = 0; si < ss; si++) {
                    float tx = (x + (si + 0.5f) / ss) / outW;
                    float u = lU + (rU-lU)*tx;
                    float v = lV + (rV-lV)*tx;

                    float sx = u * srcW - 0.5f;
                    float sy = v * srcH - 0.5f;
                    int ix = (int)floor(sx), iy = (int)floor(sy);
                    float fx = sx - ix, fy = sy - iy;

                    for (int c = 0; c < channels; c++) {
                        auto sample = [&](int px, int py) -> float {
                            px = clamp(px, 0, srcW - 1);
                            py = clamp(py, 0, srcH - 1);
                            return srcData[(py * srcW + px) * channels + c];
                        };
                        sum[c] += sample(ix,iy)*(1-fx)*(1-fy)
                                + sample(ix+1,iy)*fx*(1-fy)
                                + sample(ix,iy+1)*(1-fx)*fy
                                + sample(ix+1,iy+1)*fx*fy;
                    }
                }
            }

            int outIdx = (y * outW + x) * channels;
            for (int c = 0; c < channels; c++) {
                dstData[outIdx + c] = (unsigned char)clamp(sum[c] * invSS2, 0.f, 255.f);
            }
        }
    }
}

// Per-pixel UV transform for perspective export.
// Uses PhotoEntry::getCropUV() for each output pixel with adaptive supersampling.
inline void transformPerspU8(const Pixels& src, Pixels& dst,
                              const PhotoEntry& entry, int outW, int outH) {
    int srcW = src.getWidth(), srcH = src.getHeight();
    int channels = src.getChannels();
    dst.allocate(outW, outH, channels);
    auto* srcData = src.getData();
    auto* dstData = dst.getData();

    // Estimate supersampling from perspective magnitude
    float maxPersp = max({fabs(entry.userPerspV), fabs(entry.userPerspH), fabs(entry.userShear)});
    int ss = clamp((int)ceil(2.0f + maxPersp * 4.0f), 2, 6);
    float invSS2 = 1.0f / (ss * ss);

    for (int y = 0; y < outH; y++) {
        for (int x = 0; x < outW; x++) {
            float sum[4] = {0, 0, 0, 0};

            for (int sj = 0; sj < ss; sj++) {
                for (int si = 0; si < ss; si++) {
                    float tx = (x + (si + 0.5f) / ss) / outW;
                    float ty = (y + (sj + 0.5f) / ss) / outH;
                    auto [u, v] = entry.getCropUV(tx, ty, srcW, srcH);

                    float sx = u * srcW - 0.5f;
                    float sy = v * srcH - 0.5f;
                    int ix = (int)floor(sx), iy = (int)floor(sy);
                    float fx = sx - ix, fy = sy - iy;

                    for (int c = 0; c < channels; c++) {
                        auto sample = [&](int px, int py) -> float {
                            px = clamp(px, 0, srcW - 1);
                            py = clamp(py, 0, srcH - 1);
                            return srcData[(py * srcW + px) * channels + c];
                        };
                        sum[c] += sample(ix,iy)*(1-fx)*(1-fy)
                                + sample(ix+1,iy)*fx*(1-fy)
                                + sample(ix,iy+1)*(1-fx)*fy
                                + sample(ix+1,iy+1)*fx*fy;
                    }
                }
            }

            int outIdx = (y * outW + x) * channels;
            for (int c = 0; c < channels; c++) {
                dstData[outIdx + c] = (unsigned char)clamp(sum[c] * invSS2, 0.f, 255.f);
            }
        }
    }
}

// Full export pipeline (4-corner UV quad for crop+rotation)
inline bool exportJpeg(const DevelopShader& shader,
                       const string& outPath,
                       const ExportSettings& settings,
                       const float corners[8], int outW, int outH) {
    if (!shader.isFboReady()) return false;

    // 1. Read FBO
    Pixels pixels;
    if (!readFboPixels(shader.getFboImage(),
                       shader.getFboWidth(), shader.getFboHeight(), pixels))
        return false;

    // 1b. Apply UV transform (crop + rotation)
    Pixels transformed;
    Pixels* srcPtr = &pixels;
    bool isIdentity = (outW == pixels.getWidth() && outH == pixels.getHeight() &&
                       corners[0] == 0 && corners[1] == 0 &&
                       corners[2] == 1 && corners[3] == 0 &&
                       corners[4] == 1 && corners[5] == 1 &&
                       corners[6] == 0 && corners[7] == 1);
    if (!isIdentity) {
        transformU8(pixels, transformed, corners, outW, outH);
        srcPtr = &transformed;
    }

    // 2. Resize if needed
    Pixels* outPtr = srcPtr;
    Pixels resized;
    if (settings.maxEdge > 0) {
        int w = outPtr->getWidth(), h = outPtr->getHeight();
        int longEdge = max(w, h);
        if (longEdge > settings.maxEdge) {
            float scale = (float)settings.maxEdge / longEdge;
            int newW = max(1, (int)round(w * scale));
            int newH = max(1, (int)round(h * scale));
            resizeU8(*outPtr, resized, newW, newH);
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

// Full export pipeline with perspective support via PhotoEntry
inline bool exportJpeg(const DevelopShader& shader,
                       const string& outPath,
                       const ExportSettings& settings,
                       const PhotoEntry& entry) {
    if (!shader.isFboReady()) return false;

    int srcW = shader.getFboWidth();
    int srcH = shader.getFboHeight();
    auto [outW, outH] = entry.getCropOutputSize(srcW, srcH);

    // 1. Read FBO
    Pixels pixels;
    if (!readFboPixels(shader.getFboImage(), srcW, srcH, pixels))
        return false;

    // 1b. Choose transform path
    Pixels transformed;
    Pixels* srcPtr = &pixels;

    bool hasCropOrRot = entry.hasCrop() || entry.hasRotation();
    if (entry.hasPerspective()) {
        // Per-pixel UV for perspective
        transformPerspU8(pixels, transformed, entry, outW, outH);
        srcPtr = &transformed;
    } else if (hasCropOrRot) {
        // 4-corner bilinear for rotation only
        auto quad = entry.getCropQuad(srcW, srcH);
        bool isIdentity = (outW == srcW && outH == srcH &&
                           quad[0] == 0 && quad[1] == 0 &&
                           quad[2] == 1 && quad[3] == 0 &&
                           quad[4] == 1 && quad[5] == 1 &&
                           quad[6] == 0 && quad[7] == 1);
        if (!isIdentity) {
            transformU8(pixels, transformed, quad.data(), outW, outH);
            srcPtr = &transformed;
        }
    }

    // 2. Resize if needed
    Pixels* outPtr = srcPtr;
    Pixels resized;
    if (settings.maxEdge > 0) {
        int w = outPtr->getWidth(), h = outPtr->getHeight();
        int longEdge = max(w, h);
        if (longEdge > settings.maxEdge) {
            float scale = (float)settings.maxEdge / longEdge;
            int newW = max(1, (int)round(w * scale));
            int newH = max(1, (int)round(h * scale));
            resizeU8(*outPtr, resized, newW, newH);
            outPtr = &resized;
        }
    }

    // 3. Create output directory
    fs::create_directories(fs::path(outPath).parent_path());

    // 4. Save JPEG (RGB, drop alpha)
    int w = outPtr->getWidth(), h = outPtr->getHeight();
    int ch = outPtr->getChannels();
    if (ch == 4) {
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

// Generate a developed thumbnail (crop+rotation+develop applied, 512px Q60)
inline bool generateThumbnail(const DevelopShader& shader,
                               const string& outPath,
                               const PhotoEntry& entry) {
    ExportSettings settings;
    settings.maxEdge = THUMBNAIL_MAX_SIZE;
    settings.quality = THUMBNAIL_JPEG_QUALITY;
    return exportJpeg(shader, outPath, settings, entry);
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
