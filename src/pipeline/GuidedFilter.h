#pragma once

// =============================================================================
// GuidedFilter.h - Noise reduction for RAW images
// =============================================================================
// Local adaptive Wiener filter (primary) + guided filter (legacy CPU fallback).
// Operates in YCbCr space for independent chroma/luma control.
// =============================================================================

#include <TrussC.h>
#include <vector>
#include <cmath>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace std;
using namespace tc;

namespace tp {

// =============================================================================
// Local adaptive Wiener filter (MATLAB wiener2 equivalent)
// =============================================================================
// For each pixel, estimates local mean and variance in a small window.
// Where variance ≈ noise → output = local mean (smooth).
// Where variance >> noise → output ≈ input (preserve edges).
// Uses integral images for O(1) per-pixel box queries.

inline void wienerFilterChannel(float* data, int w, int h, int radius, float noiseVar) {
    int N = w * h;

    // Build integral images: sum and sum-of-squares (double for precision)
    vector<double> sum(N), sum2(N);
    for (int y = 0; y < h; y++) {
        double rs = 0, rs2 = 0;
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            double v = data[idx];
            rs += v;
            rs2 += v * v;
            sum[idx] = rs + (y > 0 ? sum[idx - w] : 0.0);
            sum2[idx] = rs2 + (y > 0 ? sum2[idx - w] : 0.0);
        }
    }

    // Box query helper
    auto boxQuery = [&](const double* img, int x1, int y1, int x2, int y2) -> double {
        x1 = max(x1, 0); y1 = max(y1, 0);
        x2 = min(x2, w - 1); y2 = min(y2, h - 1);
        double d = img[y2 * w + x2];
        if (x1 > 0) d -= img[y2 * w + (x1 - 1)];
        if (y1 > 0) d -= img[(y1 - 1) * w + x2];
        if (x1 > 0 && y1 > 0) d += img[(y1 - 1) * w + (x1 - 1)];
        return d;
    };

    // Apply Wiener filter in-place
    double nv = (double)noiseVar;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int x1 = x - radius, y1 = y - radius;
            int x2 = x + radius, y2 = y + radius;
            int cx1 = max(x1, 0), cy1 = max(y1, 0);
            int cx2 = min(x2, w - 1), cy2 = min(y2, h - 1);
            double count = (double)(cx2 - cx1 + 1) * (cy2 - cy1 + 1);

            double s = boxQuery(sum.data(), x1, y1, x2, y2);
            double s2 = boxQuery(sum2.data(), x1, y1, x2, y2);
            double mean = s / count;
            double var = s2 / count - mean * mean;

            // Wiener weight: 0 = full smoothing, 1 = preserve original
            double wt = max(0.0, (var - nv) / max(var, 1e-10));

            int idx = y * w + x;
            data[idx] = (float)(mean + wt * (data[idx] - mean));
        }
    }
}

// Apply Wiener noise reduction to F32 RGBA pixels.
// chromaStrength: 0 = no chroma NR, 1 = strong chroma NR
// lumaStrength:   0 = no luma NR, 1 = strong luma NR
inline void guidedDenoise(Pixels& pixels, float chromaStrength, float lumaStrength, int /*radius*/ = 0) {
    if (chromaStrength <= 0 && lumaStrength <= 0) return;
    if (!pixels.isFloat() || pixels.getChannels() != 4) return;

    int w = pixels.getWidth();
    int h = pixels.getHeight();
    if (w <= 0 || h <= 0) return;

    auto t0 = chrono::steady_clock::now();

    float* data = pixels.getDataF32();
    int N = w * h;

    // Small fixed radius — Wiener adapts via noise threshold, not window size
    int radius = 3;  // 7×7 window

    // Split RGBA → YCbCr (BT.601)
    vector<float> Y(N), Cb(N), Cr(N);
    for (int i = 0; i < N; i++) {
        float r = data[i * 4 + 0];
        float g = data[i * 4 + 1];
        float b = data[i * 4 + 2];
        Y[i]  =  0.299f * r + 0.587f * g + 0.114f * b;
        Cb[i] = -0.169f * r - 0.331f * g + 0.500f * b;
        Cr[i] =  0.500f * r - 0.419f * g - 0.081f * b;
    }

    auto t1 = chrono::steady_clock::now();

    // Noise variance from slider (quadratic for finer low-end control)
    // Measured chroma noise variance at ISO 10000: ~0.0004
    float chromaNV = chromaStrength * chromaStrength * 0.005f;
    float lumaNV   = lumaStrength * lumaStrength * 0.001f;

    // Filter channels in parallel (each uses its own integral images)
    auto filterCb = [&]() {
        if (chromaStrength > 0) wienerFilterChannel(Cb.data(), w, h, radius, chromaNV);
    };
    auto filterCr = [&]() {
        if (chromaStrength > 0) wienerFilterChannel(Cr.data(), w, h, radius, chromaNV);
    };
    auto filterY = [&]() {
        if (lumaStrength > 0) wienerFilterChannel(Y.data(), w, h, radius, lumaNV);
    };

    thread t_cb(filterCb);
    thread t_cr(filterCr);
    filterY();  // run in current thread
    t_cb.join();
    t_cr.join();

    auto t2 = chrono::steady_clock::now();

    // YCbCr → RGBA (BT.601 inverse)
    for (int i = 0; i < N; i++) {
        float y  = Y[i];
        float cb = Cb[i];
        float cr = Cr[i];
        data[i * 4 + 0] = y + 1.402f * cr;
        data[i * 4 + 1] = y - 0.344f * cb - 0.714f * cr;
        data[i * 4 + 2] = y + 1.772f * cb;
        // alpha unchanged
    }

    auto t3 = chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };
    fprintf(stderr, "[Wiener NR] %dx%d r=%d chromaNV=%.6f lumaNV=%.6f | split=%lldms filter=%lldms merge=%lldms total=%lldms\n",
            w, h, radius, chromaNV, lumaNV, ms(t0,t1), ms(t1,t2), ms(t2,t3), ms(t0,t3));
}

} // namespace tp
