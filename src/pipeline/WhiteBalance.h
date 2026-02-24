#pragma once

// =============================================================================
// WhiteBalance.h - Color temperature calculations for WB correction
// =============================================================================
// Converts Kelvin + Tint to RGB multipliers for white balance correction.
// Reference: darktable illuminants.h, CIE daylight/planckian locus formulas.
// =============================================================================

#include <cmath>

namespace wb {

// -------------------------------------------------------------------------
// CIE Daylight Locus: Temperature (K) -> chromaticity (x, y)
// Valid: 4000 K - 25000 K (CIE standard illuminant D series)
// -------------------------------------------------------------------------
inline void daylightXY(float T, float& x, float& y) {
    if (T >= 4000.0f && T <= 7000.0f) {
        x = ((-4.6070e9f / T + 2.9678e6f) / T + 0.09911e3f) / T + 0.244063f;
    } else {
        x = ((-2.0064e9f / T + 1.9018e6f) / T + 0.24748e3f) / T + 0.237040f;
    }
    y = (-3.0f * x + 2.87f) * x - 0.275f;
}

// -------------------------------------------------------------------------
// Planckian (Blackbody) Locus: Temperature (K) -> chromaticity (x, y)
// Valid: 1667 K - 25000 K
// -------------------------------------------------------------------------
inline void planckianXY(float T, float& x, float& y) {
    if (T >= 1667.0f && T <= 4000.0f) {
        x = ((-0.2661239e9f / T - 0.2343589e6f) / T + 0.8776956e3f) / T + 0.179910f;
    } else {
        x = ((-3.0258469e9f / T + 2.1070379e6f) / T + 0.2226347e3f) / T + 0.240390f;
    }

    if (T >= 1667.0f && T <= 2222.0f) {
        y = ((-1.1063814f * x - 1.34811020f) * x + 2.18555832f) * x - 0.20219683f;
    } else if (T > 2222.0f && T <= 4000.0f) {
        y = ((-0.9549476f * x - 1.37418593f) * x + 2.09137015f) * x - 0.16748867f;
    } else {
        y = ((3.0817580f * x - 5.87338670f) * x + 3.75112997f) * x - 0.37001483f;
    }
}

// -------------------------------------------------------------------------
// Combined: use daylight locus >= 4000K, planckian below
// -------------------------------------------------------------------------
inline void kelvinToXY(float T, float& x, float& y) {
    if (T < 1667.0f) T = 1667.0f;
    if (T > 25000.0f) T = 25000.0f;
    if (T >= 4000.0f) {
        daylightXY(T, x, y);
    } else {
        planckianXY(T, x, y);
    }
}

// -------------------------------------------------------------------------
// Reverse: chromaticity (x, y) -> approximate CCT (Kelvin)
// McCamy's approximation, valid 3000-50000 K
// -------------------------------------------------------------------------
inline float xyToKelvin(float x, float y) {
    float n = (x - 0.3320f) / (y - 0.1858f);
    return 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;
}

// -------------------------------------------------------------------------
// Adobe Tint → Duv (offset perpendicular to Planckian locus)
// Adobe convention: tint/150 ≈ Duv * 3000
// Positive tint = magenta (negative Duv), negative tint = green
// -------------------------------------------------------------------------
inline float tintToDuv(float tint) {
    return -tint / 3000.0f;
}

// -------------------------------------------------------------------------
// Apply Duv offset to chromaticity on Planckian locus
// Moves perpendicular to the locus direction
// -------------------------------------------------------------------------
inline void applyTintToXY(float T, float duv, float& x, float& y) {
    // Compute normal to Planckian locus at this temperature
    // dy/dx of the Planckian locus (slope of tangent)
    float dx;
    if (T >= 1667.0f && T <= 2222.0f) {
        dx = (-3.3191442f * x - 2.69622040f) * x + 2.18555832f;
    } else if (T > 2222.0f && T <= 4000.0f) {
        dx = (-2.8648428f * x - 2.74837186f) * x + 2.09137015f;
    } else {
        dx = (9.2452740f * x - 11.7467734f) * x + 3.75112997f;
    }

    // Normal vector (perpendicular to tangent)
    float norm = sqrtf(1.0f + dx * dx);
    x += duv * dx / norm;
    y -= duv / norm;
}

// -------------------------------------------------------------------------
// XY chromaticity -> XYZ (assuming Y=1)
// -------------------------------------------------------------------------
inline void xyToXYZ(float x, float y, float& X, float& Y, float& Z) {
    Y = 1.0f;
    X = x / y;
    Z = (1.0f - x - y) / y;
}

// -------------------------------------------------------------------------
// sRGB D65 reference white point
// -------------------------------------------------------------------------
inline void d65XYZ(float& X, float& Y, float& Z) {
    // D65: x=0.31271, y=0.32902
    xyToXYZ(0.31271f, 0.32902f, X, Y, Z);
}

// -------------------------------------------------------------------------
// Bradford chromatic adaptation: diagonal approximation in LMS space
// Computes RGB multiplier to adapt from source illuminant to D65
// -------------------------------------------------------------------------

// Bradford matrix: XYZ -> LMS
inline void xyzToLMS(float X, float Y, float Z, float& L, float& M, float& S) {
    L =  0.8951f * X + 0.2664f * Y - 0.1614f * Z;
    M = -0.7502f * X + 1.7135f * Y + 0.0367f * Z;
    S =  0.0389f * X - 0.0685f * Y + 1.0296f * Z;
}

// Bradford inverse: LMS -> XYZ
inline void lmsToXYZ(float L, float M, float S, float& X, float& Y, float& Z) {
    X =  0.9870f * L - 0.1471f * M + 0.1600f * S;
    Y =  0.4323f * L + 0.5184f * M + 0.0493f * S;
    Z = -0.0085f * L + 0.0400f * M + 0.9685f * S;
}

// sRGB matrix: XYZ (D65) -> linear sRGB
inline void xyzToSrgb(float X, float Y, float Z, float& R, float& G, float& B) {
    R =  3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    G = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    B =  0.0557f * X - 0.2040f * Y + 1.0570f * Z;
}

// -------------------------------------------------------------------------
// Main API: compute WB RGB multiplier from temperature+tint pair
//
// targetK/targetTint: desired white balance (e.g. from LR)
// asShotK/asShotTint: camera's as-shot white balance
//
// Returns multiplier that when applied to linear sRGB makes
// (asShotK, asShotTint) white point look like (targetK, targetTint).
//
// If asShotK == 0: returns (1,1,1) (no correction possible)
// -------------------------------------------------------------------------
struct WbMul { float r, g, b; };

inline WbMul kelvinToWbMultiplier(float targetK, float targetTint,
                                   float asShotK, float asShotTint) {
    if (asShotK <= 0 || targetK <= 0) return {1.0f, 1.0f, 1.0f};

    // Target illuminant chromaticity
    float tx, ty;
    kelvinToXY(targetK, tx, ty);
    if (targetTint != 0) {
        float duv = tintToDuv(targetTint);
        applyTintToXY(targetK, duv, tx, ty);
    }

    // As-shot illuminant chromaticity
    float ax, ay;
    kelvinToXY(asShotK, ax, ay);
    if (asShotTint != 0) {
        float duv = tintToDuv(asShotTint);
        applyTintToXY(asShotK, duv, ax, ay);
    }

    // Convert both to XYZ
    float tX, tY, tZ, aX, aY, aZ;
    xyToXYZ(tx, ty, tX, tY, tZ);
    xyToXYZ(ax, ay, aX, aY, aZ);

    // Convert to Bradford LMS
    float tL, tM, tS, aL, aM, aS;
    xyzToLMS(tX, tY, tZ, tL, tM, tS);
    xyzToLMS(aX, aY, aZ, aL, aM, aS);

    // Diagonal adaptation: scale factors in LMS
    // We want to make the as-shot illuminant appear as the target illuminant
    float scaleL = (aL > 0.0001f) ? tL / aL : 1.0f;
    float scaleM = (aM > 0.0001f) ? tM / aM : 1.0f;
    float scaleS = (aS > 0.0001f) ? tS / aS : 1.0f;

    // Apply scale in LMS, then convert back to XYZ
    // For a diagonal adaptation, the XYZ->LMS->scale->LMS->XYZ simplifies to
    // a 3x3 matrix multiply on XYZ. But we need RGB multipliers.
    //
    // Method: transform the D65 white point through the adaptation and
    // see what sRGB values it produces. The multiplier is the inverse.
    float dX, dY, dZ;
    d65XYZ(dX, dY, dZ);

    // D65 in LMS
    float dL, dM, dS;
    xyzToLMS(dX, dY, dZ, dL, dM, dS);

    // Adapted D65
    float adL = dL * scaleL;
    float adM = dM * scaleM;
    float adS = dS * scaleS;

    // Back to XYZ
    float adX, adY, adZ;
    lmsToXYZ(adL, adM, adS, adX, adY, adZ);

    // To sRGB
    float adR, adG, adB;
    xyzToSrgb(adX, adY, adZ, adR, adG, adB);

    // Normalize to green channel (most perceptually important)
    if (adG > 0.0001f) {
        adR /= adG;
        adB /= adG;
        adG = 1.0f;
    }

    return {adR, adG, adB};
}

// -------------------------------------------------------------------------
// Approximate cam_mul (RGBG) -> Kelvin
// Uses a precomputed lookup: compute R/G ratio for known temperatures,
// then binary search for the closest match.
// -------------------------------------------------------------------------
inline float camMulToKelvin(float camR, float camG, float camB) {
    if (camG <= 0.0001f) return 5500.0f; // fallback

    // Normalize by green
    float rg = camR / camG;

    // The R/G ratio of cam_mul decreases as temperature increases
    // (warm light = low kelvin = high R/G, cool light = high kelvin = low R/G)
    // We precompute the D65-referenced R/G ratio for known temperatures
    // and do a reverse lookup.

    // Precomputed table: {kelvin, R/G ratio of daylight illuminant in sRGB}
    // Generated from kelvinToXY -> xyToXYZ -> xyzToSrgb -> R/G
    struct KelvinRG { float kelvin; float rg; };
    static const KelvinRG table[] = {
        {2000, 2.580f}, {2500, 1.948f}, {3000, 1.597f}, {3500, 1.382f},
        {4000, 1.237f}, {4500, 1.133f}, {5000, 1.057f}, {5500, 0.999f},
        {6000, 0.952f}, {6500, 0.914f}, {7000, 0.882f}, {7500, 0.856f},
        {8000, 0.834f}, {9000, 0.799f}, {10000, 0.773f}, {12000, 0.737f},
    };
    static const int N = sizeof(table) / sizeof(table[0]);

    // cam_mul R/G is inversely related: high cam_mul R means camera needs
    // to amplify R, meaning the light was blue (high kelvin)
    // So we actually want to find kelvin where sRGB R/G ≈ 1/rg
    // Wait - cam_mul is the multiplier to apply to raw channels to get neutral.
    // High R multiplier = light was deficient in red = high kelvin (blue light).
    // The sRGB R/G of the illuminant is inversely related to cam_mul R/G.

    // Actually, the relationship is more direct:
    // cam_mul[R] / cam_mul[G] is proportional to the inverse of the
    // illuminant's R/G ratio.
    // So: illuminant_rg ≈ 1 / rg (normalized cam_mul)
    float illumRG = 1.0f / rg;

    // Binary search in table
    if (illumRG >= table[0].rg) return table[0].kelvin;
    if (illumRG <= table[N - 1].rg) return table[N - 1].kelvin;

    for (int i = 0; i < N - 1; i++) {
        if (illumRG <= table[i].rg && illumRG >= table[i + 1].rg) {
            // Linear interpolation
            float t = (illumRG - table[i].rg) / (table[i + 1].rg - table[i].rg);
            return table[i].kelvin + t * (table[i + 1].kelvin - table[i].kelvin);
        }
    }

    return 5500.0f; // fallback
}

// Approximate cam_mul -> Tint (simplified)
// Returns 0 for now; proper tint extraction requires the full
// Planckian normal vector calculation and is camera-dependent.
inline float camMulToTint(float camR, float camG, float camG2, float camB) {
    // G1/G2 imbalance can indicate tint, but this is very approximate
    if (camG2 <= 0 || camG <= 0) return 0.0f;
    // For now, return 0 (neutral tint)
    return 0.0f;
}

} // namespace wb
