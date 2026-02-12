#pragma once

// =============================================================================
// ClipEmbedder.h - CLIP ViT-B/32 image embedding with auto model download
// =============================================================================
// Input:  Pixels (U8 RGBA, any size) → 224×224 bilinear resize → ImageNet norm
// Output: 512-dim L2-normalized float vector

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
    static constexpr int INPUT_SIZE = 224;
    static constexpr int EMBED_DIM = 512;
    static constexpr const char* MODEL_NAME = "clip-vit-b32";
    static constexpr const char* MODEL_FILE = "clip-vit-b32-vision.onnx";
    static constexpr const char* MODEL_URL =
        "https://huggingface.co/Qdrant/clip-ViT-B-32-vision/resolve/main/model.onnx";
    static constexpr int64_t MODEL_SIZE_APPROX = 338 * 1024 * 1024; // ~338MB

    // Start loading in background (non-blocking)
    void loadAsync(const string& modelDir) {
        modelDir_ = modelDir;
        fs::create_directories(modelDir);

        string modelPath = modelDir + "/" + MODEL_FILE;

        if (fs::exists(modelPath)) {
            // Model already downloaded, load session (fast, ~1s)
            loadingModel_ = true;
            loadingStatus_ = "Loading CLIP model...";
            initThread_ = thread([this, modelPath]() {
                if (runner_.load(modelPath)) {
                    runner_.printModelInfo();
                    ready_ = true;
                    logNotice() << "[CLIP] Ready (" << MODEL_NAME << ")";
                }
                loadingModel_ = false;
            });
            initThread_.detach();
        } else {
            // Need to download first
            downloadingModel_ = true;
            downloadProgress_ = 0;
            downloadPath_ = modelPath + ".download";
            loadingStatus_ = "Downloading CLIP model...";
            logNotice() << "[CLIP] Model not found, starting background download";

            initThread_ = thread([this, modelPath]() {
                if (downloadModel(modelPath)) {
                    loadingStatus_ = "Loading CLIP model...";
                    loadingModel_ = true;
                    downloadingModel_ = false;
                    if (runner_.load(modelPath)) {
                        runner_.printModelInfo();
                        ready_ = true;
                        logNotice() << "[CLIP] Ready (" << MODEL_NAME << ")";
                    }
                    loadingModel_ = false;
                } else {
                    downloadingModel_ = false;
                }
            });
            initThread_.detach();
        }
    }

    bool isReady() const { return ready_; }
    bool isDownloading() const { return downloadingModel_; }
    bool isLoading() const { return loadingModel_; }
    bool isInitializing() const { return downloadingModel_ || loadingModel_; }
    const string& getStatusText() const { return loadingStatus_; }

    // Get download progress 0.0-1.0 (check .download file size)
    float getDownloadProgress() {
        if (!downloadingModel_) return ready_ ? 1.0f : 0.0f;
        try {
            if (fs::exists(downloadPath_)) {
                auto sz = fs::file_size(downloadPath_);
                return clamp((float)sz / (float)MODEL_SIZE_APPROX, 0.0f, 0.99f);
            }
        } catch (...) {}
        return 0.0f;
    }

    // Generate embedding from U8 pixels (any size, any channel count)
    vector<float> embed(const Pixels& pixels) {
        if (!ready_ || !pixels.isAllocated()) return {};

        // Preprocess: resize to 224x224, convert to RGB CHW, ImageNet normalize
        auto input = preprocess(pixels);
        if (input.empty()) return {};

        vector<int64_t> shape = {1, 3, INPUT_SIZE, INPUT_SIZE};
        auto output = runner_.run(input, shape);
        if (output.empty()) return {};

        // L2 normalize
        l2Normalize(output);
        return output;
    }

private:
    OnnxRunner runner_;
    string modelDir_;
    string downloadPath_;
    string loadingStatus_;
    atomic<bool> ready_{false};
    atomic<bool> downloadingModel_{false};
    atomic<bool> loadingModel_{false};
    atomic<float> downloadProgress_{0};
    thread initThread_;

    bool downloadModel(const string& destPath) {
        // Use curl command for large file download (150MB+)
        // HttpClient has a 30s timeout which is too short
        string tmpPath = destPath + ".download";

        logNotice() << "[CLIP] Downloading model (~150MB)...";
        logNotice() << "[CLIP] URL: " << MODEL_URL;

        // -L: follow redirects, -f: fail on HTTP errors, --progress-bar: show progress
        string cmd = "curl -L -f --progress-bar -o '"
                     + tmpPath + "' '" + string(MODEL_URL) + "' 2>&1";
        int ret = system(cmd.c_str());

        if (ret != 0) {
            logError() << "[CLIP] Download failed (curl exit code: " << ret << ")";
            // Clean up partial download
            try { fs::remove(tmpPath); } catch (...) {}
            return false;
        }

        // Verify file size (should be >100MB)
        try {
            auto fileSize = fs::file_size(tmpPath);
            if (fileSize < 100 * 1024 * 1024) {
                logError() << "[CLIP] Downloaded file too small: " << fileSize << " bytes";
                fs::remove(tmpPath);
                return false;
            }
            logNotice() << "[CLIP] Downloaded " << (fileSize / (1024*1024)) << " MB";
        } catch (...) {
            logError() << "[CLIP] Cannot verify downloaded file";
            return false;
        }

        // Rename to final path
        try {
            fs::rename(tmpPath, destPath);
        } catch (const exception& e) {
            logError() << "[CLIP] Rename failed: " << e.what();
            return false;
        }

        logNotice() << "[CLIP] Model saved: " << destPath;
        return true;
    }

    // Preprocess pixels to CLIP input format
    // Pixels (any size U8/F32, RGBA/RGB) → [1, 3, 224, 224] float32
    // Steps: bilinear resize to 224x224 → /255 → ImageNet normalize
    static vector<float> preprocess(const Pixels& src) {
        int srcW = src.getWidth();
        int srcH = src.getHeight();
        int srcCh = src.getChannels();
        if (srcW <= 0 || srcH <= 0) return {};

        bool isF32 = (src.getFormat() == PixelFormat::F32);
        const unsigned char* u8Data = isF32 ? nullptr : src.getData();
        const float* f32Data = isF32 ? src.getDataF32() : nullptr;

        // ImageNet mean/std (RGB)
        constexpr float mean[3] = {0.48145466f, 0.4578275f, 0.40821073f};
        constexpr float std[3]  = {0.26862954f, 0.26130258f, 0.27577711f};

        int outSize = 3 * INPUT_SIZE * INPUT_SIZE;
        vector<float> result(outSize);

        // Center crop to square, then resize
        int cropSize = min(srcW, srcH);
        int cropX = (srcW - cropSize) / 2;
        int cropY = (srcH - cropSize) / 2;

        for (int y = 0; y < INPUT_SIZE; y++) {
            float srcYf = cropY + (y + 0.5f) * cropSize / INPUT_SIZE - 0.5f;
            for (int x = 0; x < INPUT_SIZE; x++) {
                float srcXf = cropX + (x + 0.5f) * cropSize / INPUT_SIZE - 0.5f;

                // Bilinear interpolation
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

                    // ImageNet normalize
                    val = (val - mean[c]) / std[c];

                    // CHW layout: [channel][y][x]
                    result[c * INPUT_SIZE * INPUT_SIZE + y * INPUT_SIZE + x] = val;
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
