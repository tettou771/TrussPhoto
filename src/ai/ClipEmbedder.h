#pragma once

// =============================================================================
// ClipEmbedder.h - Image embedding (SigLIP2)
// =============================================================================
// Currently: waon-siglip2-base-patch16-256 (ViT-B/16, 256px input, 768-dim)
// Extensible: add new model constants + preprocess branch for future models.
// Input:  Pixels (U8 RGBA, any size) -> resize to inputSize_ -> normalize
// Output: L2-normalized float vector

#include "OnnxRunner.h"
#include <TrussC.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <thread>
#include <atomic>

using namespace std;
using namespace tc;

namespace fs = std::filesystem;

class ClipEmbedder {
public:
    // SigLIP2 waon
    static constexpr int SIGLIP2_INPUT_SIZE = 256;
    static constexpr int SIGLIP2_EMBED_DIM = 768;
    static constexpr const char* SIGLIP2_MODEL_NAME = "waon-siglip2";
    static constexpr const char* SIGLIP2_MODEL_FILE = "waon-siglip2-vision.onnx";

    // Active model info (set after load)
    const char* MODEL_NAME = SIGLIP2_MODEL_NAME;
    int EMBED_DIM = SIGLIP2_EMBED_DIM;

    // Start loading in background (non-blocking)
    void loadAsync(const string& modelDir) {
        modelDir_ = modelDir;
        fs::create_directories(modelDir);

        initThread_ = thread([this]() {
            string modelPath = modelDir_ + "/" + SIGLIP2_MODEL_FILE;
            if (fs::exists(modelPath)) {
                loadingModel_ = true;
                loadingStatus_ = "Loading SigLIP2 model...";
                if (runner_.load(modelPath)) {
                    runner_.printModelInfo();
                    MODEL_NAME = SIGLIP2_MODEL_NAME;
                    EMBED_DIM = SIGLIP2_EMBED_DIM;
                    inputSize_ = SIGLIP2_INPUT_SIZE;
                    ready_ = true;
                    loadingModel_ = false;
                    logNotice() << "[CLIP] Ready (" << MODEL_NAME << ", " << EMBED_DIM << "-dim)";
                    return;
                }
                loadingModel_ = false;
                logError() << "[CLIP] SigLIP2 model failed to load";
            } else {
                logError() << "[CLIP] SigLIP2 ONNX not found: " << modelPath;
                logError() << "[CLIP] Run: python scripts/export_siglip2.py";
            }
        });
    }

    ~ClipEmbedder() {
        if (initThread_.joinable()) initThread_.join();
    }

    bool isReady() const { return ready_; }
    bool isLoading() const { return loadingModel_; }
    bool isInitializing() const { return loadingModel_; }
    const string& getStatusText() const { return loadingStatus_; }

    // Release ONNX session to free memory (after all embeddings are generated)
    void unload() {
        runner_.unload();
        ready_ = false;
        logNotice() << "[CLIP] Vision model unloaded";
    }

    // Generate embedding from U8 pixels (any size, any channel count)
    vector<float> embed(const Pixels& pixels) {
        if (!ready_ || !pixels.isAllocated()) return {};

        auto input = preprocess(pixels);
        if (input.empty()) return {};

        return infer(input);
    }

    // Preprocess pixels to float tensor (thread-safe, can run on multiple threads)
    vector<float> preprocessPixels(const Pixels& pixels) const {
        if (!ready_ || !pixels.isAllocated()) return {};
        return preprocess(pixels);
    }

    // Run inference on preprocessed tensor (NOT thread-safe, call from single thread)
    vector<float> infer(const vector<float>& input) {
        vector<int64_t> shape = {1, 3, inputSize_, inputSize_};
        auto output = runner_.run(input, shape);
        if (output.empty()) return {};

        // L2 normalize
        l2Normalize(output);
        return output;
    }

    // Batch inference: multiple preprocessed tensors → multiple embeddings
    // Each input tensor is [3 * inputSize_ * inputSize_] floats
    vector<vector<float>> inferBatch(const vector<vector<float>>& inputs) {
        if (inputs.empty()) return {};
        if (inputs.size() == 1) {
            auto result = infer(inputs[0]);
            if (result.empty()) return {};
            return {std::move(result)};
        }

        int batchSize = (int)inputs.size();
        int tensorSize = 3 * inputSize_ * inputSize_;

        // Concatenate into single [N, 3, H, W] tensor
        vector<float> batched(batchSize * tensorSize);
        for (int i = 0; i < batchSize; i++) {
            memcpy(batched.data() + i * tensorSize,
                   inputs[i].data(), tensorSize * sizeof(float));
        }

        vector<int64_t> shape = {batchSize, 3, inputSize_, inputSize_};
        auto output = runner_.run(batched, shape);
        if (output.empty()) return {};

        // Split output [N, EMBED_DIM] into individual embeddings
        vector<vector<float>> results;
        results.reserve(batchSize);
        for (int i = 0; i < batchSize; i++) {
            vector<float> emb(output.begin() + i * EMBED_DIM,
                              output.begin() + (i + 1) * EMBED_DIM);
            l2Normalize(emb);
            results.push_back(std::move(emb));
        }
        return results;
    }

private:
    OnnxRunner runner_;
    string modelDir_;
    string loadingStatus_;
    atomic<bool> ready_{false};
    atomic<bool> loadingModel_{false};
    thread initThread_;
    int inputSize_ = SIGLIP2_INPUT_SIZE;

    // Preprocess pixels to model input format
    // SigLIP2: simple resize to 256x256, normalize (v - 0.5) / 0.5
    vector<float> preprocess(const Pixels& src) const {
        int srcW = src.getWidth();
        int srcH = src.getHeight();
        int srcCh = src.getChannels();
        if (srcW <= 0 || srcH <= 0) return {};

        bool isF32 = (src.getFormat() == PixelFormat::F32);
        const unsigned char* u8Data = isF32 ? nullptr : src.getData();
        const float* f32Data = isF32 ? src.getDataF32() : nullptr;

        const int S = inputSize_;
        vector<float> result(3 * S * S);

        // SigLIP2: simple bilinear resize + normalize (v - 0.5) / 0.5
        for (int oy = 0; oy < S; oy++) {
            float srcYf = (oy + 0.5f) * srcH / S - 0.5f;
            for (int ox = 0; ox < S; ox++) {
                float srcXf = (ox + 0.5f) * srcW / S - 0.5f;

                int x0 = (int)floor(srcXf);
                int y0 = (int)floor(srcYf);
                int x1 = x0 + 1;
                int y1 = y0 + 1;
                float fx = srcXf - x0;
                float fy = srcYf - y0;

                x0 = clamp(x0, 0, srcW - 1);
                x1 = clamp(x1, 0, srcW - 1);
                y0 = clamp(y0, 0, srcH - 1);
                y1 = clamp(y1, 0, srcH - 1);

                for (int c = 0; c < 3; c++) {
                    float v00, v10, v01, v11;
                    if (isF32) {
                        v00 = f32Data[(y0 * srcW + x0) * srcCh + c];
                        v10 = f32Data[(y0 * srcW + x1) * srcCh + c];
                        v01 = f32Data[(y1 * srcW + x0) * srcCh + c];
                        v11 = f32Data[(y1 * srcW + x1) * srcCh + c];
                    } else {
                        v00 = u8Data[(y0 * srcW + x0) * srcCh + c] / 255.0f;
                        v10 = u8Data[(y0 * srcW + x1) * srcCh + c] / 255.0f;
                        v01 = u8Data[(y1 * srcW + x0) * srcCh + c] / 255.0f;
                        v11 = u8Data[(y1 * srcW + x1) * srcCh + c] / 255.0f;
                    }

                    float val = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy) +
                                v01 * (1-fx) * fy     + v11 * fx * fy;
                    // SigLIP2 normalize: (v - 0.5) / 0.5 = v * 2.0 - 1.0
                    val = val * 2.0f - 1.0f;
                    result[c * S * S + oy * S + ox] = val;
                }
            }
        }
        return result;
    }

    static void l2Normalize(vector<float>& vec) {
        float norm = 0;
        for (float v : vec) norm += v * v;
        norm = sqrtf(norm);
        if (norm > 1e-8f) {
            for (float& v : vec) v /= norm;
        }
    }
};
