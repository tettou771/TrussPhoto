#pragma once

// =============================================================================
// DevelopPipelineCPU.h - CPU port of develop.glsl pixel operations
// =============================================================================
// 1:1 port of the GPU develop shader for headless/server-side processing.
// Operates on F32 RGBA Pixels in-place. Thread-parallelized by row chunks.
//
// Processing order (matches develop.glsl exactly):
//   1. sRGB → linear
//   2. Exposure
//   3. White balance
//   4. Contrast
//   5. Blacks
//   6. Whites
//   7. Highlights/Shadows
//   8. linear → sRGB
//   9. Saturation
//  10. Vibrance
//  11. LUT (via Lut3DCPU)
// =============================================================================

#include <TrussC.h>
#include "WhiteBalance.h"
#include "Lut3DCPU.h"
#include "PhotoEntry.h"
#include <cmath>
#include <algorithm>
#include <thread>

using namespace std;
using namespace tc;

namespace DevelopPipelineCPU {

// Helper: smoothstep
inline float smoothstep(float edge0, float edge1, float x) {
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Process a row range [yStart, yEnd) of F32 RGBA pixels in-place
inline void processRows(float* data, int w, int yStart, int yEnd,
                        float exposure, float wbR, float wbG, float wbB,
                        float contrast, float blacks, float whites,
                        float highlights, float shadows,
                        float saturation, float vibrance,
                        float lutBlend, const Lut3DCPU* lut) {

    for (int y = yStart; y < yEnd; y++) {
        for (int x = 0; x < w; x++) {
            int i = (y * w + x) * 4;
            float r = data[i+0];
            float g = data[i+1];
            float b = data[i+2];

            // 1. sRGB → linear
            r = powf(max(r, 0.0f), 2.2f);
            g = powf(max(g, 0.0f), 2.2f);
            b = powf(max(b, 0.0f), 2.2f);

            // 2. Exposure
            if (exposure != 0.0f) {
                float gain = powf(2.0f, exposure);
                r *= gain; g *= gain; b *= gain;
            }

            // 3. White balance
            r *= wbR; g *= wbG; b *= wbB;

            // 4. Contrast: power-curve around 18% gray pivot
            if (contrast != 0.0f) {
                float c = contrast / 100.0f;
                float p = 1.0f + c * 1.5f;
                float pivot = 0.18f;
                r = pivot * powf(max(r, 0.00001f) / pivot, p);
                g = pivot * powf(max(g, 0.00001f) / pivot, p);
                b = pivot * powf(max(b, 0.00001f) / pivot, p);
            }

            // 5. Blacks
            if (blacks != 0.0f) {
                float bk = blacks / 100.0f;
                if (bk < 0.0f) {
                    float thresh = -bk * 0.06f;
                    r = max((r - thresh) / (1.0f - thresh), 0.0f);
                    g = max((g - thresh) / (1.0f - thresh), 0.0f);
                    b = max((b - thresh) / (1.0f - thresh), 0.0f);
                } else {
                    float lift = bk * 0.06f;
                    r = r * (1.0f - lift) + lift;
                    g = g * (1.0f - lift) + lift;
                    b = b * (1.0f - lift) + lift;
                }
            }

            // 6. Whites
            if (whites != 0.0f) {
                float wh = whites / 100.0f;
                if (wh > 0.0f) {
                    float div = 1.0f - wh * 0.06f;
                    r /= div; g /= div; b /= div;
                } else {
                    float mul = 1.0f + wh * 0.06f;
                    r *= mul; g *= mul; b *= mul;
                }
            }

            // 7. Highlights / Shadows
            float lum = clamp(r * 0.2126f + g * 0.7152f + b * 0.0722f, 0.0f, 1.0f);
            float adj = 0.0f;

            if (highlights != 0.0f) {
                float mask = smoothstep(0.55f, 0.90f, lum);
                adj += highlights / 100.0f * 0.25f * mask;
            }
            if (shadows != 0.0f) {
                float mask = smoothstep(0.02f, 0.08f, lum) * (1.0f - smoothstep(0.20f, 0.40f, lum));
                adj += shadows / 100.0f * 0.30f * mask;
            }

            if (adj != 0.0f) {
                float lumPost = r * 0.2126f + g * 0.7152f + b * 0.0722f;
                float target = max(lumPost + adj, 0.0f);
                float blend = smoothstep(0.02f, 0.10f, lumPost);

                float addR = max(r + adj, 0.0f);
                float addG = max(g + adj, 0.0f);
                float addB = max(b + adj, 0.0f);

                if (lumPost > 0.0001f) {
                    float ratio = target / lumPost;
                    r = addR * (1.0f - blend) + (r * ratio) * blend;
                    g = addG * (1.0f - blend) + (g * ratio) * blend;
                    b = addB * (1.0f - blend) + (b * ratio) * blend;
                } else {
                    r = addR; g = addG; b = addB;
                }
            }

            // 8. linear → sRGB
            r = powf(max(r, 0.0f), 1.0f / 2.2f);
            g = powf(max(g, 0.0f), 1.0f / 2.2f);
            b = powf(max(b, 0.0f), 1.0f / 2.2f);

            // 9. Saturation
            if (saturation != 0.0f) {
                float gray = r * 0.2126f + g * 0.7152f + b * 0.0722f;
                float s = saturation / 100.0f;
                float factor = 1.0f + s;
                r = gray + (r - gray) * factor;
                g = gray + (g - gray) * factor;
                b = gray + (b - gray) * factor;
            }

            // 10. Vibrance
            if (vibrance != 0.0f) {
                float gray = r * 0.2126f + g * 0.7152f + b * 0.0722f;
                float maxC = max({r, g, b});
                float minC = min({r, g, b});
                float range = maxC - minC;
                float sat = (maxC > 0.001f) ? range / maxC : 0.0f;

                float weight = powf(1.0f - sat, 1.5f);

                float skinProtect = 1.0f;
                if (range > 0.001f) {
                    float hue;
                    if (maxC == r)
                        hue = fmodf((g - b) / range, 6.0f) / 6.0f;
                    else if (maxC == g)
                        hue = ((b - r) / range + 2.0f) / 6.0f;
                    else
                        hue = ((r - g) / range + 4.0f) / 6.0f;

                    float d = fabsf(hue - 0.055f);
                    d = min(d, 1.0f - d);
                    skinProtect = 1.0f - 0.6f * expf(-(d * d) / (2.0f * 0.042f * 0.042f));
                }

                float v = vibrance / 100.0f;
                float factor = 1.0f + v * weight * skinProtect;
                r = gray + (r - gray) * factor;
                g = gray + (g - gray) * factor;
                b = gray + (b - gray) * factor;
            }

            // 11. LUT
            if (lut && lutBlend > 0.0f) {
                float lr = r, lg = g, lb = b;
                lut->apply(lr, lg, lb);
                r = r * (1.0f - lutBlend) + lr * lutBlend;
                g = g * (1.0f - lutBlend) + lg * lutBlend;
                b = b * (1.0f - lutBlend) + lb * lutBlend;
            }

            data[i+0] = r;
            data[i+1] = g;
            data[i+2] = b;
        }
    }
}

// Main API: develop F32 RGBA pixels in-place
// Lens correction should be applied before calling this.
inline void develop(Pixels& pixels, const PhotoEntry& entry,
                    float asShotTemp, float asShotTint,
                    const Lut3DCPU* lut = nullptr, float lutBlend = 1.0f) {

    int w = pixels.getWidth();
    int h = pixels.getHeight();
    float* data = pixels.getDataF32();
    if (!data) return;

    // Compute WB multiplier
    if (asShotTemp <= 0) asShotTemp = 5500.0f;
    float temperature = (entry.devTemperature > 0) ? entry.devTemperature : asShotTemp;
    auto wbMul = wb::kelvinToWbMultiplier(temperature, entry.devTint, asShotTemp, asShotTint);

    // Parallel row processing
    int numThreads = clamp((int)thread::hardware_concurrency(), 1, 16);
    int rowsPerThread = (h + numThreads - 1) / numThreads;

    vector<thread> threads;
    for (int t = 0; t < numThreads; t++) {
        int yStart = t * rowsPerThread;
        int yEnd = min(yStart + rowsPerThread, h);
        if (yStart >= yEnd) break;

        threads.emplace_back(processRows, data, w, yStart, yEnd,
            entry.devExposure, wbMul.r, wbMul.g, wbMul.b,
            entry.devContrast, entry.devBlacks, entry.devWhites,
            entry.devHighlights, entry.devShadows,
            entry.devSaturation, entry.devVibrance,
            lutBlend, lut);
    }
    for (auto& t : threads) t.join();
}

// Convert F32 RGBA → U8 RGBA (clamp to 0-255)
inline void toU8(const Pixels& f32, Pixels& u8) {
    int w = f32.getWidth();
    int h = f32.getHeight();
    int ch = f32.getChannels();
    u8.allocate(w, h, ch);

    const float* src = f32.getDataF32();
    auto* dst = u8.getData();
    int total = w * h * ch;

    for (int i = 0; i < total; i++) {
        dst[i] = (unsigned char)clamp(src[i] * 255.0f, 0.0f, 255.0f);
    }
}

} // namespace DevelopPipelineCPU
