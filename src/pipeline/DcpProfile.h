#pragma once

// =============================================================================
// DcpProfile.h - DNG Camera Profile (.dcp) parser + CPU color pipeline
// =============================================================================
// Reads Adobe DCP files (TIFF IFD format with magic 0x4352) and applies:
//   1. ForwardMatrix: camera RGB → XYZ D50 → linear sRGB
//   2. ToneCurve: 1D curve on linear sRGB
//   3. sRGB gamma encoding
// LookTable is applied separately at the LUT stage (in HSV of sRGB).
//
// DCP tag layout (from Adobe Camera Raw profiles):
//   50708  UniqueCameraModel   ASCII
//   50721  ColorMatrix1        SRATIONAL(9)  (StdA)
//   50722  ColorMatrix2        SRATIONAL(9)  (D65)
//   50778  CalibrationIlluminant1  SHORT
//   50779  CalibrationIlluminant2  SHORT
//   50932  ProfileCopyright    ASCII
//   50936  ProfileName         ASCII
//   50940  ProfileToneCurve    FLOAT(256)  = 128 (in,out) pairs
//   50964  ForwardMatrix1      SRATIONAL(9)  (StdA)
//   50965  ForwardMatrix2      SRATIONAL(9)  (D65)
//   50981  ProfileLookTableDims  LONG(3)
//   50982  ProfileLookTableData  FLOAT(N)
// =============================================================================

#include <TrussC.h>
#include <string>
#include <vector>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <thread>

using namespace std;
using namespace tc;

class DcpProfile {
public:
    // Metadata
    string cameraModel;
    string profileName;

    // Illuminants
    int illuminant1 = 0, illuminant2 = 0;

    // Forward matrices: camera RGB → XYZ D50 (3×3, row-major)
    bool hasForwardMatrix1 = false, hasForwardMatrix2 = false;
    float forwardMatrix1[9] = {};
    float forwardMatrix2[9] = {};

    // Color matrices: XYZ D50 → camera RGB (3×3, row-major)
    bool hasColorMatrix1 = false, hasColorMatrix2 = false;
    float colorMatrix1[9] = {};
    float colorMatrix2[9] = {};

    // Tone curve: (input, output) pairs, monotonically increasing
    bool hasToneCurve = false;
    vector<float> toneCurve; // interleaved: [in0, out0, in1, out1, ...]

    // Look table (HSV 3D LUT with deltas)
    bool hasLookTable = false;
    int lookDims[3] = {0, 0, 0}; // [hue, sat, val] divisions
    vector<float> lookData; // [hueShift_deg, satScale, valScale] per grid point

    // ---- Load from file ----

    bool load(const string& path) {
        ifstream file(path, ios::binary);
        if (!file) return false;

        file.seekg(0, ios::end);
        size_t fileSize = file.tellg();
        file.seekg(0);

        vector<uint8_t> data(fileSize);
        file.read((char*)data.data(), fileSize);
        if (!file) return false;

        return parse(data.data(), fileSize);
    }

    // ---- Apply DCP color pipeline to F32 RGBA pixels in-place ----
    // Input: linear camera RGB (from LibRaw output_color=0, WB applied)
    // Output: sRGB gamma-encoded RGBA (matches existing develop pipeline input)
    // colorTemp: as-shot color temperature in Kelvin (for matrix interpolation)

    void applyColorPipeline(float* data, int w, int h, float colorTemp) const {
        if (!hasForwardMatrix1 && !hasForwardMatrix2) return;

        // Interpolate forward matrix based on color temperature
        float fm[9];
        interpolateMatrix(fm, colorTemp);

        // XYZ D50 → linear sRGB (Bradford adapted)
        // From: http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
        static const float xyzToSrgb[9] = {
             3.1338561f, -1.6168667f, -0.4906146f,
            -0.9787684f,  1.9161415f,  0.0334540f,
             0.0719453f, -0.1921862f,  1.1002053f,
        };

        // Combined: camera RGB → XYZ D50 → linear sRGB
        float combined[9];
        matMul3x3(xyzToSrgb, fm, combined);

        // Build tone curve LUT (1024 entries for fast lookup)
        vector<float> tcLut;
        if (hasToneCurve && toneCurve.size() >= 4) {
            tcLut = buildToneCurveLut(1024);
        }
        const float* tcPtr = tcLut.empty() ? nullptr : tcLut.data();
        int tcSize = (int)tcLut.size();

        // Parallel row processing
        int numThreads = clamp((int)thread::hardware_concurrency(), 1, 16);
        int rowsPerThread = (h + numThreads - 1) / numThreads;
        vector<thread> threads;

        for (int t = 0; t < numThreads; t++) {
            int yStart = t * rowsPerThread;
            int yEnd = min(yStart + rowsPerThread, h);
            if (yStart >= yEnd) break;

            threads.emplace_back([=]() {
                applyRows(data, w, yStart, yEnd, combined, tcPtr, tcSize);
            });
        }
        for (auto& t : threads) t.join();
    }

    // ---- Apply fallback matrix (cam_xyz from LibRaw) when no DCP available ----
    // Converts camera RGB → XYZ D50 → linear sRGB → sRGB gamma
    // camXyz: 3x3 row-major camera → XYZ matrix (from LibRaw's cam_xyz)
    static void applyFallbackMatrix(float* data, int w, int h, const float camXyz[9]) {
        // XYZ D50 → linear sRGB
        static const float xyzToSrgb[9] = {
             3.1338561f, -1.6168667f, -0.4906146f,
            -0.9787684f,  1.9161415f,  0.0334540f,
             0.0719453f, -0.1921862f,  1.1002053f,
        };

        // Combined: camera RGB → XYZ D50 → linear sRGB
        float combined[9];
        matMul3x3(xyzToSrgb, camXyz, combined);

        int numThreads = clamp((int)thread::hardware_concurrency(), 1, 16);
        int rowsPerThread = (h + numThreads - 1) / numThreads;
        vector<thread> threads;

        for (int t = 0; t < numThreads; t++) {
            int yStart = t * rowsPerThread;
            int yEnd = min(yStart + rowsPerThread, h);
            if (yStart >= yEnd) break;

            threads.emplace_back([=]() {
                for (int y = yStart; y < yEnd; y++) {
                    for (int x = 0; x < w; x++) {
                        int idx = (y * w + x) * 4;
                        float r = data[idx], g = data[idx+1], b = data[idx+2];

                        // Camera RGB → linear sRGB
                        float lr = combined[0]*r + combined[1]*g + combined[2]*b;
                        float lg = combined[3]*r + combined[4]*g + combined[5]*b;
                        float lb = combined[6]*r + combined[7]*g + combined[8]*b;

                        // sRGB gamma
                        data[idx]   = srgbGamma(max(0.0f, lr));
                        data[idx+1] = srgbGamma(max(0.0f, lg));
                        data[idx+2] = srgbGamma(max(0.0f, lb));
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
    }

    // Parse cam_xyz matrix from JSON string "[0.1, 0.2, ...]" (9 floats)
    static bool parseCamXyz(const string& json, float out[9]) {
        if (json.empty()) return false;
        try {
            // Simple parser for "[f,f,f,f,f,f,f,f,f]"
            size_t i = 0;
            int idx = 0;
            while (i < json.size() && idx < 9) {
                if (json[i] == '-' || json[i] == '.' || (json[i] >= '0' && json[i] <= '9')) {
                    size_t end;
                    out[idx++] = stof(json.substr(i), &end);
                    i += end;
                } else {
                    i++;
                }
            }
            if (idx != 9) return false;
            // Reject degenerate (all-zero) matrices: LibRaw leaves cam_xyz
            // zeroed for some DNGs, and applying it would black out the image.
            float absSum = 0;
            for (int k = 0; k < 9; k++) absSum += fabsf(out[k]);
            return absSum > 1e-6f;
        } catch (...) {
            return false;
        }
    }

    // ---- Apply LookTable (HSV 3D LUT) to sRGB pixel ----
    // Call at the same position as .cube LUT (after develop, in sRGB gamma space)

    void applyLookTable(float& r, float& g, float& b) const {
        if (!hasLookTable || lookDims[0] <= 0) return;

        // RGB → HSV
        float h, s, v;
        rgbToHsv(r, g, b, h, s, v);

        // Grid coordinates (hue wraps, sat/val clamped)
        float hCoord = h / 360.0f * lookDims[0];
        float sCoord = s * (lookDims[1] - 1);
        float vCoord = v * (lookDims[2] - 1);

        // Trilinear interpolation of HSV deltas
        float dH, dS, dV;
        interpolateLookTable(hCoord, sCoord, vCoord, dH, dS, dV);

        // Apply deltas
        h = fmodf(h + dH + 360.0f, 360.0f);
        s = clamp(s * dS, 0.0f, 1.0f);
        v = clamp(v * dV, 0.0f, 1e6f); // allow HDR values

        // HSV → RGB
        hsvToRgb(h, s, v, r, g, b);
    }

    // ---- Check if this profile has a look table ----
    bool hasLook() const { return hasLookTable && lookDims[0] > 0; }

private:

    // ---- TIFF IFD Parser ----

    bool parse(const uint8_t* data, size_t size) {
        if (size < 8) return false;

        // Check byte order (only LE supported, all Adobe DCPs are LE)
        if (data[0] != 'I' || data[1] != 'I') return false;

        // DCP magic is 0x4352 ("CR" in LE), not standard TIFF's 42
        uint16_t magic = readU16(data, 2);
        if (magic != 0x4352 && magic != 42) return false;

        uint32_t ifdOffset = readU32(data, 4);
        if (ifdOffset + 2 > size) return false;

        uint16_t numEntries = readU16(data, ifdOffset);
        if (ifdOffset + 2 + numEntries * 12 > size) return false;

        for (int i = 0; i < numEntries; i++) {
            size_t off = ifdOffset + 2 + i * 12;
            uint16_t tag = readU16(data, off);
            uint16_t type = readU16(data, off + 2);
            uint32_t count = readU32(data, off + 4);
            uint32_t valOff = readU32(data, off + 8);

            size_t dataOff = getDataOffset(type, count, off + 8, valOff);
            if (dataOff >= size) continue;

            switch (tag) {
            case 50708: // UniqueCameraModel
                cameraModel = readAscii(data, dataOff, count);
                break;
            case 50936: // ProfileName
                profileName = readAscii(data, dataOff, count);
                break;
            case 50778: // CalibrationIlluminant1
                illuminant1 = readU16(data, dataOff);
                break;
            case 50779: // CalibrationIlluminant2
                illuminant2 = readU16(data, dataOff);
                break;
            case 50721: // ColorMatrix1
                if (type == 10 && count == 9) {
                    readSRationals(data, dataOff, colorMatrix1, 9, size);
                    hasColorMatrix1 = true;
                }
                break;
            case 50722: // ColorMatrix2
                if (type == 10 && count == 9) {
                    readSRationals(data, dataOff, colorMatrix2, 9, size);
                    hasColorMatrix2 = true;
                }
                break;
            case 50964: // ForwardMatrix1
                if (type == 10 && count == 9) {
                    readSRationals(data, dataOff, forwardMatrix1, 9, size);
                    hasForwardMatrix1 = true;
                }
                break;
            case 50965: // ForwardMatrix2
                if (type == 10 && count == 9) {
                    readSRationals(data, dataOff, forwardMatrix2, 9, size);
                    hasForwardMatrix2 = true;
                }
                break;
            case 50940: // ProfileToneCurve
                if (type == 11 && count >= 4) {
                    toneCurve.resize(count);
                    readFloats(data, dataOff, toneCurve.data(), count, size);
                    hasToneCurve = true;
                }
                break;
            case 50981: // ProfileLookTableDims
                if (type == 4 && count == 3) {
                    lookDims[0] = readU32(data, dataOff);
                    lookDims[1] = readU32(data, dataOff + 4);
                    lookDims[2] = readU32(data, dataOff + 8);
                }
                break;
            case 50982: // ProfileLookTableData
                if (type == 11) {
                    lookData.resize(count);
                    readFloats(data, dataOff, lookData.data(), count, size);
                    hasLookTable = true;
                }
                break;
            }
        }

        // Validate look table dimensions
        if (hasLookTable) {
            int expected = lookDims[0] * lookDims[1] * lookDims[2] * 3;
            if ((int)lookData.size() != expected) {
                hasLookTable = false;
                lookData.clear();
            }
        }

        logNotice() << "[DCP] Loaded: " << profileName
                    << " (" << cameraModel << ")"
                    << " FM1=" << hasForwardMatrix1
                    << " FM2=" << hasForwardMatrix2
                    << " TC=" << hasToneCurve << "(" << toneCurve.size()/2 << "pts)"
                    << " LT=" << hasLookTable
                    << (hasLookTable ? " [" + to_string(lookDims[0]) + "x"
                        + to_string(lookDims[1]) + "x" + to_string(lookDims[2]) + "]" : "");

        return hasForwardMatrix1 || hasForwardMatrix2 || hasLookTable;
    }

    // ---- Binary readers (little-endian) ----

    static uint16_t readU16(const uint8_t* data, size_t off) {
        return data[off] | (data[off+1] << 8);
    }

    static uint32_t readU32(const uint8_t* data, size_t off) {
        return data[off] | (data[off+1] << 8) | (data[off+2] << 16) | (data[off+3] << 24);
    }

    static int32_t readS32(const uint8_t* data, size_t off) {
        return (int32_t)readU32(data, off);
    }

    static float readFloat(const uint8_t* data, size_t off) {
        float v;
        memcpy(&v, &data[off], 4);
        return v;
    }

    static string readAscii(const uint8_t* data, size_t off, uint32_t count) {
        string s((const char*)&data[off], count);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

    static void readSRationals(const uint8_t* data, size_t off, float* out, int count, size_t fileSize) {
        for (int i = 0; i < count; i++) {
            size_t pos = off + i * 8;
            if (pos + 8 > fileSize) { out[i] = 0; continue; }
            int32_t num = readS32(data, pos);
            int32_t den = readS32(data, pos + 4);
            out[i] = (den != 0) ? (float)num / (float)den : 0.0f;
        }
    }

    static void readFloats(const uint8_t* data, size_t off, float* out, uint32_t count, size_t fileSize) {
        for (uint32_t i = 0; i < count; i++) {
            size_t pos = off + i * 4;
            if (pos + 4 > fileSize) { out[i] = 0; continue; }
            out[i] = readFloat(data, pos);
        }
    }

    static size_t typeSize(uint16_t type) {
        switch (type) {
            case 1: case 2: case 6: case 7: return 1;
            case 3: case 8: return 2;
            case 4: case 9: case 11: return 4;
            case 5: case 10: case 12: return 8;
            default: return 0;
        }
    }

    static size_t getDataOffset(uint16_t type, uint32_t count, size_t inlineOff, uint32_t valOff) {
        size_t total = typeSize(type) * count;
        return (total <= 4) ? inlineOff : valOff;
    }

    // ---- Illuminant temperature ----

    static float illuminantTemp(int illuminant) {
        switch (illuminant) {
            case 1:  return 5500.0f;  // Daylight
            case 2:  return 3800.0f;  // Fluorescent
            case 3:  return 2856.0f;  // Tungsten
            case 17: return 2856.0f;  // Standard light A
            case 18: return 4874.0f;  // Standard light B
            case 19: return 6774.0f;  // Standard light C
            case 20: return 5503.0f;  // D55
            case 21: return 6504.0f;  // D65
            case 22: return 7504.0f;  // D75
            case 23: return 5003.0f;  // D50
            case 24: return 3200.0f;  // ISO studio tungsten
            default: return 5000.0f;
        }
    }

    // ---- Matrix operations ----

    // Interpolate ForwardMatrix1/2 based on color temperature (mired space)
    void interpolateMatrix(float* out, float colorTemp) const {
        if (hasForwardMatrix1 && !hasForwardMatrix2) {
            memcpy(out, forwardMatrix1, sizeof(float) * 9);
            return;
        }
        if (!hasForwardMatrix1 && hasForwardMatrix2) {
            memcpy(out, forwardMatrix2, sizeof(float) * 9);
            return;
        }

        float temp1 = illuminantTemp(illuminant1);
        float temp2 = illuminantTemp(illuminant2);
        float mired1 = 1e6f / temp1;
        float mired2 = 1e6f / temp2;
        float miredTarget = 1e6f / max(colorTemp, 1000.0f);

        // weight=1 → matrix1 (warmer illuminant), weight=0 → matrix2 (cooler)
        float weight;
        if (fabsf(mired1 - mired2) < 1.0f) {
            weight = 0.5f;
        } else {
            weight = clamp((miredTarget - mired2) / (mired1 - mired2), 0.0f, 1.0f);
        }

        for (int i = 0; i < 9; i++) {
            out[i] = forwardMatrix1[i] * weight + forwardMatrix2[i] * (1.0f - weight);
        }
    }

    // C = A * B (both 3×3 row-major)
    static void matMul3x3(const float* A, const float* B, float* C) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                C[r*3+c] = A[r*3+0]*B[0*3+c] + A[r*3+1]*B[1*3+c] + A[r*3+2]*B[2*3+c];
            }
        }
    }

    // Apply 3×3 matrix to RGB
    static void matApply(const float* M, float r, float g, float b, float& ro, float& go, float& bo) {
        ro = M[0]*r + M[1]*g + M[2]*b;
        go = M[3]*r + M[4]*g + M[5]*b;
        bo = M[6]*r + M[7]*g + M[8]*b;
    }

    // ---- Tone curve ----

    // Build 1D LUT from tone curve control points (linear interpolation)
    vector<float> buildToneCurveLut(int lutSize) const {
        vector<float> lut(lutSize);
        int numPts = (int)toneCurve.size() / 2;
        if (numPts < 2) {
            for (int i = 0; i < lutSize; i++) lut[i] = (float)i / (lutSize - 1);
            return lut;
        }

        int ptIdx = 0;
        for (int i = 0; i < lutSize; i++) {
            float x = (float)i / (lutSize - 1);

            // Advance to the right segment
            while (ptIdx < numPts - 2 && toneCurve[(ptIdx+1)*2] < x) ptIdx++;

            float x0 = toneCurve[ptIdx * 2];
            float y0 = toneCurve[ptIdx * 2 + 1];
            float x1 = toneCurve[(ptIdx + 1) * 2];
            float y1 = toneCurve[(ptIdx + 1) * 2 + 1];

            float t = (fabsf(x1 - x0) > 1e-8f) ? (x - x0) / (x1 - x0) : 0.0f;
            t = clamp(t, 0.0f, 1.0f);
            lut[i] = y0 + t * (y1 - y0);
        }
        return lut;
    }

    // ---- Row processing ----

    static void applyRows(float* data, int w, int yStart, int yEnd,
                          const float* combined, const float* tcLut, int tcSize) {
        for (int y = yStart; y < yEnd; y++) {
            for (int x = 0; x < w; x++) {
                int i = (y * w + x) * 4;
                float r = max(data[i+0], 0.0f);
                float g = max(data[i+1], 0.0f);
                float b = max(data[i+2], 0.0f);

                // 1. Forward matrix: camera RGB → XYZ D50 → linear sRGB
                float sr, sg, sb;
                matApply(combined, r, g, b, sr, sg, sb);
                sr = max(sr, 0.0f);
                sg = max(sg, 0.0f);
                sb = max(sb, 0.0f);

                // 2. Tone curve (on linear sRGB, per-channel)
                if (tcLut) {
                    sr = sampleLut(tcLut, tcSize, sr);
                    sg = sampleLut(tcLut, tcSize, sg);
                    sb = sampleLut(tcLut, tcSize, sb);
                }

                // 3. sRGB gamma encoding
                sr = srgbGamma(sr);
                sg = srgbGamma(sg);
                sb = srgbGamma(sb);

                data[i+0] = sr;
                data[i+1] = sg;
                data[i+2] = sb;
            }
        }
    }

    static float sampleLut(const float* lut, int size, float x) {
        float coord = clamp(x, 0.0f, 1.0f) * (size - 1);
        int idx = min((int)coord, size - 2);
        float frac = coord - idx;
        return lut[idx] * (1.0f - frac) + lut[idx + 1] * frac;
    }

    static float srgbGamma(float x) {
        if (x <= 0.0031308f) return x * 12.92f;
        return 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
    }

    // ---- HSV conversion ----

    static void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
        float maxC = max({r, g, b});
        float minC = min({r, g, b});
        float delta = maxC - minC;
        v = maxC;
        s = (maxC > 1e-6f) ? delta / maxC : 0.0f;

        if (delta < 1e-6f) {
            h = 0.0f;
        } else if (maxC == r) {
            h = 60.0f * fmodf((g - b) / delta + 6.0f, 6.0f);
        } else if (maxC == g) {
            h = 60.0f * ((b - r) / delta + 2.0f);
        } else {
            h = 60.0f * ((r - g) / delta + 4.0f);
        }
    }

    static void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
        float c = v * s;
        float hp = h / 60.0f;
        float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
        float m = v - c;

        if (hp < 1)      { r = c; g = x; b = 0; }
        else if (hp < 2) { r = x; g = c; b = 0; }
        else if (hp < 3) { r = 0; g = c; b = x; }
        else if (hp < 4) { r = 0; g = x; b = c; }
        else if (hp < 5) { r = x; g = 0; b = c; }
        else              { r = c; g = 0; b = x; }

        r += m; g += m; b += m;
    }

    // ---- LookTable trilinear interpolation ----

    void interpolateLookTable(float hCoord, float sCoord, float vCoord,
                              float& dH, float& dS, float& dV) const {
        int hueDivs = lookDims[0];
        int satDivs = lookDims[1];
        int valDivs = lookDims[2];

        // Hue wraps around
        int h0 = ((int)floorf(hCoord)) % hueDivs;
        if (h0 < 0) h0 += hueDivs;
        int h1 = (h0 + 1) % hueDivs;
        float hFrac = hCoord - floorf(hCoord);

        // Saturation/value clamp
        int s0 = clamp((int)floorf(sCoord), 0, satDivs - 2);
        int s1 = s0 + 1;
        float sFrac = clamp(sCoord - s0, 0.0f, 1.0f);

        int v0 = clamp((int)floorf(vCoord), 0, valDivs - 2);
        int v1 = v0 + 1;
        float vFrac = clamp(vCoord - v0, 0.0f, 1.0f);

        // 8-corner lookup
        auto sample = [&](int hi, int si, int vi, int ch) -> float {
            int idx = (hi * satDivs * valDivs + si * valDivs + vi) * 3 + ch;
            return lookData[idx];
        };

        // Trilinear for each of 3 channels (H_shift, S_scale, V_scale)
        for (int ch = 0; ch < 3; ch++) {
            float c000 = sample(h0, s0, v0, ch);
            float c100 = sample(h1, s0, v0, ch);
            float c010 = sample(h0, s1, v0, ch);
            float c110 = sample(h1, s1, v0, ch);
            float c001 = sample(h0, s0, v1, ch);
            float c101 = sample(h1, s0, v1, ch);
            float c011 = sample(h0, s1, v1, ch);
            float c111 = sample(h1, s1, v1, ch);

            float c00 = c000 * (1-hFrac) + c100 * hFrac;
            float c10 = c010 * (1-hFrac) + c110 * hFrac;
            float c01 = c001 * (1-hFrac) + c101 * hFrac;
            float c11 = c011 * (1-hFrac) + c111 * hFrac;

            float c0 = c00 * (1-sFrac) + c10 * sFrac;
            float c1 = c01 * (1-sFrac) + c11 * sFrac;

            float val = c0 * (1-vFrac) + c1 * vFrac;
            if (ch == 0) dH = val;
            else if (ch == 1) dS = val;
            else dV = val;
        }
    }
};
