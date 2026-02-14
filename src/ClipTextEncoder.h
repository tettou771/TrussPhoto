#pragma once

// =============================================================================
// ClipTextEncoder.h - Text encoder for semantic search (SigLIP2)
// =============================================================================
// Currently: waon-siglip2-base-patch16-256 (SentencePiece/Gemma, 768-dim)
// Extensible: add new Mode + loadXxx() + encode branch for future models.

#include "OnnxRunner.h"
#include "SentencePieceTokenizer.h"
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
    // Embedding dim (set after load)
    int EMBED_DIM = 768;

    // SigLIP2 model files
    static constexpr const char* SIGLIP2_MODEL_FILE = "waon-siglip2-text.onnx";
    static constexpr const char* SIGLIP2_SPIECE_FILE = "waon-siglip2-spiece.model";

    enum class Mode { None, SigLIP2 };

    // Load tokenizer + model in background thread
    void loadAsync(const string& modelDir) {
        modelDir_ = modelDir;
        fs::create_directories(modelDir);

        initThread_ = thread([this]() {
            string modelPath = modelDir_ + "/" + SIGLIP2_MODEL_FILE;
            string spiecePath = modelDir_ + "/" + SIGLIP2_SPIECE_FILE;
            if (fs::exists(modelPath) && fs::exists(spiecePath)) {
                loadSigLIP2(modelPath, spiecePath);
            } else {
                logError() << "[TextEncoder] SigLIP2 model not found in " << modelDir_;
                logError() << "[TextEncoder] Run: python scripts/export_siglip2.py";
            }
        });
        initThread_.detach();
    }

    bool isReady() const { return ready_; }
    Mode getMode() const { return mode_; }
    bool isMultilingual() const { return mode_ != Mode::None; }

    // Encode text -> L2-normalized embedding
    vector<float> encode(const string& text) {
        if (!ready_) return {};

        // Check cache
        {
            auto it = cache_.find(text);
            if (it != cache_.end()) return it->second;
        }

        vector<float> output;

        if (mode_ == Mode::SigLIP2) {
            // SigLIP2: SentencePiece (Gemma) -> [1, 64] + attention_mask (all ones!)
            // SigLIP2 handles PAD internally; passing a real mask breaks embeddings.
            auto tokens = spTokenizer_.encode(text);
            int seqLen = spTokenizer_.getMaxSeqLen();
            vector<int64_t> mask(seqLen, 1);
            vector<int64_t> shape = {1, seqLen};
            output = runner_.runInt64x2(tokens, mask, shape);
        }

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
    SentencePieceTokenizer spTokenizer_;
    string modelDir_;
    atomic<bool> ready_{false};
    Mode mode_ = Mode::None;
    thread initThread_;
    unordered_map<string, vector<float>> cache_;

    void loadSigLIP2(const string& modelPath, const string& spiecePath) {
        logNotice() << "[TextEncoder] SigLIP2 mode: loading SentencePiece...";

        if (!spTokenizer_.load(spiecePath)) {
            logError() << "[TextEncoder] Failed to load SigLIP2 SentencePiece model";
            return;
        }

        // GemmaTokenizer config: PAD=0, EOS=1, BOS=2, UNK=3
        // No CLS prefix, add EOS suffix, lowercase
        spTokenizer_.configure(
            -1,   // CLS (unused)
            1,    // EOS
            0,    // PAD
            3,    // UNK
            64,   // max_seq_len
            false, // no CLS prefix
            true,  // add EOS suffix
            true   // lowercase
        );

        if (!runner_.load(modelPath)) {
            logError() << "[TextEncoder] Failed to load SigLIP2 text model";
            return;
        }
        runner_.printModelInfo();

        mode_ = Mode::SigLIP2;
        EMBED_DIM = 768;
        ready_ = true;
        logNotice() << "[TextEncoder] SigLIP2 mode ready (SentencePiece/Gemma, 768-dim)";
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
