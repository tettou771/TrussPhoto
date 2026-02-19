#pragma once

// =============================================================================
// GuidedFilter.h - Guided filter noise reduction for RAW images
// =============================================================================
// Edge-preserving filter using integral images for O(n) per-pixel cost.
// Operates in YCbCr space: luma guide preserves edges in chroma channels.
// =============================================================================

#include <TrussC.h>
#include <vector>
#include <cmath>
#include <thread>
#include <algorithm>

using namespace std;
using namespace tc;

namespace tp {

// Forward declare MPS GPU version (implemented in GuidedFilterMPS.mm)
#ifdef __APPLE__
bool guidedDenoiseMPS(float* rgbaData, int w, int h,
                      float chromaStrength, float lumaStrength, int radius);
#endif

// Guided filter on a single channel using integral images.
// guide and input are row-major float arrays of size w*h.
// output is written to out (must be pre-allocated w*h).
inline void guidedFilterChannel(const float* guide, const float* input,
                                float* out, int w, int h, int radius, float eps) {
    int r = radius;
    int N = w * h;

    // Integral images: sumI, sumP, sumII, sumIP, sumOne
    vector<double> sumI(N), sumP(N), sumII(N), sumIP(N);

    // Build integral images
    auto integralSum = [&](const float* a, const float* b, double* dst) {
        // dst[y*w+x] = sum of a[i]*b[i] for all i in [0..y][0..x]
        for (int y = 0; y < h; y++) {
            double rowSum = 0;
            for (int x = 0; x < w; x++) {
                int idx = y * w + x;
                rowSum += (double)a[idx] * (double)b[idx];
                dst[idx] = rowSum + (y > 0 ? dst[(y - 1) * w + x] : 0.0);
            }
        }
    };

    auto integralSumSingle = [&](const float* a, double* dst) {
        for (int y = 0; y < h; y++) {
            double rowSum = 0;
            for (int x = 0; x < w; x++) {
                int idx = y * w + x;
                rowSum += (double)a[idx];
                dst[idx] = rowSum + (y > 0 ? dst[(y - 1) * w + x] : 0.0);
            }
        }
    };

    integralSumSingle(guide, sumI.data());
    integralSumSingle(input, sumP.data());
    integralSum(guide, guide, sumII.data());
    integralSum(guide, input, sumIP.data());

    // Box filter query from integral image
    auto boxQuery = [&](const double* intImg, int x1, int y1, int x2, int y2) -> double {
        x1 = max(x1, 0); y1 = max(y1, 0);
        x2 = min(x2, w - 1); y2 = min(y2, h - 1);
        double d = intImg[y2 * w + x2];
        if (x1 > 0) d -= intImg[y2 * w + (x1 - 1)];
        if (y1 > 0) d -= intImg[(y1 - 1) * w + x2];
        if (x1 > 0 && y1 > 0) d += intImg[(y1 - 1) * w + (x1 - 1)];
        return d;
    };

    // Compute a, b coefficients
    vector<float> a_coeff(N), b_coeff(N);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int x1 = x - r, y1 = y - r;
            int x2 = x + r, y2 = y + r;
            // Count of pixels in window (clamped to image bounds)
            int wx1 = max(x1, 0), wy1 = max(y1, 0);
            int wx2 = min(x2, w - 1), wy2 = min(y2, h - 1);
            double count = (double)(wx2 - wx1 + 1) * (wy2 - wy1 + 1);

            double meanI = boxQuery(sumI.data(), x1, y1, x2, y2) / count;
            double meanP = boxQuery(sumP.data(), x1, y1, x2, y2) / count;
            double meanII = boxQuery(sumII.data(), x1, y1, x2, y2) / count;
            double meanIP = boxQuery(sumIP.data(), x1, y1, x2, y2) / count;

            double varI = meanII - meanI * meanI;
            double covIP = meanIP - meanI * meanP;

            double ak = covIP / (varI + eps);
            double bk = meanP - ak * meanI;

            int idx = y * w + x;
            a_coeff[idx] = (float)ak;
            b_coeff[idx] = (float)bk;
        }
    }

    // Mean of a and b coefficients (second box filter pass)
    // Build integral images of a and b
    vector<double> sumA(N), sumB(N);
    integralSumSingle(a_coeff.data(), sumA.data());
    integralSumSingle(b_coeff.data(), sumB.data());

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int x1 = x - r, y1 = y - r;
            int x2 = x + r, y2 = y + r;
            int wx1 = max(x1, 0), wy1 = max(y1, 0);
            int wx2 = min(x2, w - 1), wy2 = min(y2, h - 1);
            double count = (double)(wx2 - wx1 + 1) * (wy2 - wy1 + 1);

            double meanA = boxQuery(sumA.data(), x1, y1, x2, y2) / count;
            double meanB = boxQuery(sumB.data(), x1, y1, x2, y2) / count;

            int idx = y * w + x;
            out[idx] = (float)(meanA * guide[idx] + meanB);
        }
    }
}

// Apply guided filter noise reduction to F32 RGBA pixels.
// chromaStrength: 0 = no chroma NR, 1 = strong chroma NR
// lumaStrength:   0 = no luma NR, 1 = strong luma NR
// radius: 0 = auto (based on image size)
inline void guidedDenoise(Pixels& pixels, float chromaStrength, float lumaStrength, int radius = 0) {
    if (chromaStrength <= 0 && lumaStrength <= 0) return;
    if (!pixels.isFloat() || pixels.getChannels() != 4) return;

    int w = pixels.getWidth();
    int h = pixels.getHeight();
    if (w <= 0 || h <= 0) return;

    float* data = pixels.getDataF32();
    int N = w * h;

#ifdef __APPLE__
    // MPS handles its own radius computation (separate luma/chroma radii)
    if (guidedDenoiseMPS(data, w, h, chromaStrength, lumaStrength, radius)) {
        return; // MPS GPU path succeeded
    }
    // Fall through to CPU path if MPS unavailable
#endif

    // Auto radius: scale with image size (CPU path only)
    if (radius <= 0) {
        int longEdge = max(w, h);
        radius = max(1, (int)round(5.0 * longEdge / 7000.0));
    }

    // Extract Y, Cb, Cr channels (BT.601)
    vector<float> Y(N), Cb(N), Cr(N);
    for (int i = 0; i < N; i++) {
        float r = data[i * 4 + 0];
        float g = data[i * 4 + 1];
        float b = data[i * 4 + 2];
        Y[i]  =  0.299f * r + 0.587f * g + 0.114f * b;
        Cb[i] = -0.169f * r - 0.331f * g + 0.500f * b;
        Cr[i] =  0.500f * r - 0.419f * g - 0.081f * b;
    }

    // Filter channels in parallel
    vector<float> filtY(N), filtCb(N), filtCr(N);

    auto filterLuma = [&]() {
        if (lumaStrength > 0) {
            float eps = lumaStrength * 0.01f;
            guidedFilterChannel(Y.data(), Y.data(), filtY.data(), w, h, radius, eps);
        } else {
            filtY = Y;
        }
    };

    auto filterCb_fn = [&]() {
        if (chromaStrength > 0) {
            float eps = chromaStrength * 0.1f;
            guidedFilterChannel(Y.data(), Cb.data(), filtCb.data(), w, h, radius, eps);
        } else {
            filtCb = Cb;
        }
    };

    auto filterCr_fn = [&]() {
        if (chromaStrength > 0) {
            float eps = chromaStrength * 0.1f;
            guidedFilterChannel(Y.data(), Cr.data(), filtCr.data(), w, h, radius, eps);
        } else {
            filtCr = Cr;
        }
    };

    // Run up to 3 threads
    thread t1(filterLuma);
    thread t2(filterCb_fn);
    filterCr_fn();  // run in current thread
    t1.join();
    t2.join();

    // Convert back to sRGB
    for (int i = 0; i < N; i++) {
        float y  = filtY[i];
        float cb = filtCb[i];
        float cr = filtCr[i];
        data[i * 4 + 0] = y + 1.402f * cr;
        data[i * 4 + 1] = y - 0.344f * cb - 0.714f * cr;
        data[i * 4 + 2] = y + 1.772f * cb;
        // alpha unchanged
    }
}

} // namespace tp
