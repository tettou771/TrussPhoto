#pragma once

// =============================================================================
// ClipTextEncoder.h - CLIP text encoder (ViT-B/32 text branch)
// =============================================================================
// Tokenizes text with BPE, runs ONNX text model → 512-dim L2-normalized vector.
// Auto-downloads text model + vocab/merges if missing.

#include "OnnxRunner.h"
#include "ClipTokenizer.h"
#include <TrussC.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <cmath>
#include <unordered_map>

using namespace std;
using namespace tc;

namespace fs = std::filesystem;

class ClipTextEncoder {
public:
    static constexpr int EMBED_DIM = 512;
    static constexpr const char* MODEL_FILE = "clip-vit-b32-text.onnx";
    static constexpr const char* MODEL_URL =
        "https://huggingface.co/Qdrant/clip-ViT-B-32-text/resolve/main/model.onnx";
    static constexpr const char* VOCAB_FILE = "clip-vit-b32-vocab.json";
    static constexpr const char* MERGES_FILE = "clip-vit-b32-merges.txt";
    static constexpr const char* VOCAB_URL =
        "https://huggingface.co/openai/clip-vit-base-patch32/resolve/main/vocab.json";
    static constexpr const char* MERGES_URL =
        "https://huggingface.co/openai/clip-vit-base-patch32/resolve/main/merges.txt";

    // Load tokenizer + model (blocking — call from background thread)
    void loadAsync(const string& modelDir) {
        modelDir_ = modelDir;
        fs::create_directories(modelDir);

        initThread_ = thread([this]() {
            // 1. Load tokenizer (download vocab/merges if needed)
            string vocabPath = modelDir_ + "/" + VOCAB_FILE;
            string mergesPath = modelDir_ + "/" + MERGES_FILE;

            if (!fs::exists(vocabPath)) {
                downloadFile(VOCAB_URL, vocabPath);
            }
            if (!fs::exists(mergesPath)) {
                downloadFile(MERGES_URL, mergesPath);
            }

            if (!tokenizer_.load(vocabPath, mergesPath)) {
                logError() << "[TextEncoder] Failed to load tokenizer";
                return;
            }

            // 2. Load ONNX model (download if needed)
            string modelPath = modelDir_ + "/" + MODEL_FILE;
            if (!fs::exists(modelPath)) {
                logNotice() << "[TextEncoder] Downloading text model...";
                downloadFile(MODEL_URL, modelPath);
            }

            if (!runner_.load(modelPath)) {
                logError() << "[TextEncoder] Failed to load ONNX model";
                return;
            }
            runner_.printModelInfo();

            ready_ = true;
            logNotice() << "[TextEncoder] Ready";
        });
        initThread_.detach();
    }

    bool isReady() const { return ready_; }

    // Encode text → 512-dim L2-normalized embedding
    vector<float> encode(const string& text) {
        if (!ready_) return {};

        // Check cache
        {
            auto it = cache_.find(text);
            if (it != cache_.end()) return it->second;
        }

        // Tokenize
        auto tokens = tokenizer_.encode(text);

        // Build attention mask (1 for non-padding, 0 for padding)
        vector<int64_t> attentionMask(ClipTokenizer::CONTEXT_LEN);
        for (int i = 0; i < ClipTokenizer::CONTEXT_LEN; i++) {
            attentionMask[i] = (tokens[i] != 0) ? 1 : 0;
        }

        // Run inference
        vector<int64_t> shape = {1, ClipTokenizer::CONTEXT_LEN};
        auto output = runner_.runInt64x2(tokens, attentionMask, shape);
        if (output.empty()) return {};

        // L2 normalize
        l2Normalize(output);

        // Cache result
        cache_[text] = output;
        return output;
    }

    // Clear text embedding cache
    void clearCache() { cache_.clear(); }

private:
    OnnxRunner runner_;
    ClipTokenizer tokenizer_;
    string modelDir_;
    atomic<bool> ready_{false};
    thread initThread_;
    unordered_map<string, vector<float>> cache_;

    static void l2Normalize(vector<float>& vec) {
        float norm = 0;
        for (float v : vec) norm += v * v;
        norm = sqrtf(norm);
        if (norm > 1e-8f) {
            for (float& v : vec) v /= norm;
        }
    }

    static bool downloadFile(const char* url, const string& destPath) {
        string tmpPath = destPath + ".download";
        string cmd = "curl -L -f -s -o '" + tmpPath + "' '" + string(url) + "'";
        int ret = system(cmd.c_str());
        if (ret != 0) {
            logError() << "[TextEncoder] Download failed: " << url;
            try { fs::remove(tmpPath); } catch (...) {}
            return false;
        }
        try {
            fs::rename(tmpPath, destPath);
        } catch (...) {
            return false;
        }
        return true;
    }
};
