#pragma once

// =============================================================================
// Lut3DCPU.h - CPU-only 3D LUT parser + trilinear interpolation
// =============================================================================
// Reads .cube files and applies LUT color grading without GPU.
// Used by DevelopPipelineCPU for headless/server-side export.
// =============================================================================

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>

using namespace std;

class Lut3DCPU {
public:
    Lut3DCPU() = default;

    bool load(const string& path) {
        ifstream file(path);
        if (!file) return false;

        string line;
        int size = 0;
        vector<float> data;

        while (getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty()) continue;

            istringstream iss(line);
            string keyword;
            iss >> keyword;

            if (keyword == "TITLE" || keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX") {
                continue;
            } else if (keyword == "LUT_3D_SIZE") {
                iss >> size;
                if (size < 2 || size > 256) return false;
                data.reserve(size * size * size * 3);
            } else if (keyword == "LUT_1D_SIZE") {
                return false; // 1D LUT not supported
            } else {
                float r, g, b;
                istringstream dataIss(line);
                if (dataIss >> r >> g >> b) {
                    data.push_back(r);
                    data.push_back(g);
                    data.push_back(b);
                }
            }
        }

        if (size == 0 || (int)data.size() != size * size * size * 3)
            return false;

        size_ = size;
        data_ = std::move(data);
        return true;
    }

    bool isLoaded() const { return size_ > 0; }
    int getSize() const { return size_; }

    // Trilinear interpolation (matches develop.glsl L222-228)
    void apply(float& r, float& g, float& b) const {
        if (size_ <= 0) return;

        float scale = (float)(size_ - 1) / size_;
        float offset = 0.5f / size_;

        float cr = clamp(r, 0.0f, 1.0f) * scale + offset;
        float cg = clamp(g, 0.0f, 1.0f) * scale + offset;
        float cb = clamp(b, 0.0f, 1.0f) * scale + offset;

        // Convert to grid coordinates
        float fr = cr * size_ - 0.5f;
        float fg = cg * size_ - 0.5f;
        float fb = cb * size_ - 0.5f;

        int ir = clamp((int)floor(fr), 0, size_ - 2);
        int ig = clamp((int)floor(fg), 0, size_ - 2);
        int ib = clamp((int)floor(fb), 0, size_ - 2);

        float dr = fr - ir;
        float dg = fg - ig;
        float db = fb - ib;

        // 8-corner lookup (R varies fastest, then G, then B)
        auto sample = [&](int ri, int gi, int bi) -> const float* {
            int idx = (bi * size_ * size_ + gi * size_ + ri) * 3;
            return &data_[idx];
        };

        const float* c000 = sample(ir,   ig,   ib);
        const float* c100 = sample(ir+1, ig,   ib);
        const float* c010 = sample(ir,   ig+1, ib);
        const float* c110 = sample(ir+1, ig+1, ib);
        const float* c001 = sample(ir,   ig,   ib+1);
        const float* c101 = sample(ir+1, ig,   ib+1);
        const float* c011 = sample(ir,   ig+1, ib+1);
        const float* c111 = sample(ir+1, ig+1, ib+1);

        // Trilinear interpolation for each channel
        for (int ch = 0; ch < 3; ch++) {
            float c00 = c000[ch] * (1-dr) + c100[ch] * dr;
            float c10 = c010[ch] * (1-dr) + c110[ch] * dr;
            float c01 = c001[ch] * (1-dr) + c101[ch] * dr;
            float c11 = c011[ch] * (1-dr) + c111[ch] * dr;

            float c0 = c00 * (1-dg) + c10 * dg;
            float c1 = c01 * (1-dg) + c11 * dg;

            float val = c0 * (1-db) + c1 * db;
            if (ch == 0) r = val;
            else if (ch == 1) g = val;
            else b = val;
        }
    }

private:
    int size_ = 0;
    vector<float> data_; // RGB interleaved, R varies fastest
};
