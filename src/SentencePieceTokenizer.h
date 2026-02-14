#pragma once

// =============================================================================
// SentencePieceTokenizer.h - SentencePiece tokenizer for vision-language models
// =============================================================================
// Configurable via configure(): CLS/EOS/PAD/UNK tokens, max seq len,
// CLS prefix vs EOS suffix, lowercase. Currently used for SigLIP2 (Gemma).

#include <TrussC.h>
#include <sentencepiece_processor.h>
#include <string>
#include <vector>

using namespace std;
using namespace tc;

class SentencePieceTokenizer {
public:
    // Token IDs (defaults: GemmaTokenizer for SigLIP2)
    int CLS_TOKEN_ID = -1;
    int EOS_TOKEN_ID = 1;
    int PAD_TOKEN_ID = 0;
    int UNK_TOKEN_ID = 3;

    bool load(const string& modelPath) {
        auto status = processor_.Load(modelPath);
        if (!status.ok()) {
            logError() << "[SPTokenizer] Failed to load: " << modelPath
                       << " - " << status.ToString();
            return false;
        }
        logNotice() << "[SPTokenizer] Loaded: vocab_size=" << processor_.GetPieceSize();
        return true;
    }

    // Basic configure (token IDs only)
    void configure(int cls, int eos, int pad, int unk) {
        CLS_TOKEN_ID = cls;
        EOS_TOKEN_ID = eos;
        PAD_TOKEN_ID = pad;
        UNK_TOKEN_ID = unk;
    }

    // Extended configure (token IDs + sequence options)
    void configure(int cls, int eos, int pad, int unk,
                   int maxLen, bool clsPrefix, bool eosSuffix, bool lower) {
        CLS_TOKEN_ID = cls;
        EOS_TOKEN_ID = eos;
        PAD_TOKEN_ID = pad;
        UNK_TOKEN_ID = unk;
        maxSeqLen_ = maxLen;
        useCLSPrefix_ = clsPrefix;
        addEOS_ = eosSuffix;
        doLowerCase_ = lower;
    }

    bool isLoaded() const {
        return processor_.GetPieceSize() > 0;
    }

    int getMaxSeqLen() const { return maxSeqLen_; }

    // Encode text -> input_ids [maxSeqLen_]
    // With CLS prefix: [CLS] + tokens + padding
    // With EOS suffix: tokens + [EOS] + padding
    vector<int64_t> encode(const string& text) const {
        // Text preprocessing
        string cleaned = preprocessText(text);
        if (doLowerCase_) cleaned = toLowerASCII(cleaned);

        // SentencePiece encode (without special tokens)
        vector<int> pieces;
        processor_.Encode(cleaned, &pieces);

        vector<int64_t> ids;
        ids.reserve(maxSeqLen_);

        if (useCLSPrefix_) {
            // CLS prefix mode: [CLS, token1, token2, ..., PAD, PAD]
            ids.push_back(CLS_TOKEN_ID);
            int maxTokens = maxSeqLen_ - 1;
            int tokenCount = min((int)pieces.size(), maxTokens);
            for (int i = 0; i < tokenCount; i++) {
                ids.push_back(pieces[i]);
            }
        } else {
            // EOS suffix mode: [token1, token2, ..., EOS, PAD, PAD]
            int maxTokens = addEOS_ ? maxSeqLen_ - 1 : maxSeqLen_;
            int tokenCount = min((int)pieces.size(), maxTokens);
            for (int i = 0; i < tokenCount; i++) {
                ids.push_back(pieces[i]);
            }
            if (addEOS_) {
                ids.push_back(EOS_TOKEN_ID);
            }
        }

        // Pad to maxSeqLen_
        while ((int)ids.size() < maxSeqLen_) {
            ids.push_back(PAD_TOKEN_ID);
        }
        ids.resize(maxSeqLen_);
        return ids;
    }

    // Build attention mask (1 for real tokens, 0 for padding)
    vector<int64_t> attentionMask(const vector<int64_t>& ids) const {
        vector<int64_t> mask(ids.size());
        for (size_t i = 0; i < ids.size(); i++) {
            mask[i] = (ids[i] != PAD_TOKEN_ID) ? 1 : 0;
        }
        return mask;
    }

    // Build position_ids [0, 1, 2, ..., maxSeqLen_-1]
    vector<int64_t> positionIds() const {
        vector<int64_t> pos(maxSeqLen_);
        for (int i = 0; i < maxSeqLen_; i++) {
            pos[i] = i;
        }
        return pos;
    }

private:
    sentencepiece::SentencePieceProcessor processor_;
    int maxSeqLen_ = 64;
    bool useCLSPrefix_ = false;
    bool addEOS_ = true;
    bool doLowerCase_ = true;

    // Simplified text preprocessing (HTML unescape + whitespace clean)
    static string preprocessText(const string& text) {
        string s = text;
        s = replaceAll(s, "&amp;", "&");
        s = replaceAll(s, "&lt;", "<");
        s = replaceAll(s, "&gt;", ">");
        s = replaceAll(s, "&quot;", "\"");
        s = replaceAll(s, "&#39;", "'");
        s = replaceAll(s, "&nbsp;", " ");

        // Whitespace normalization
        string result;
        result.reserve(s.size());
        bool lastSpace = false;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!lastSpace) {
                    result += ' ';
                    lastSpace = true;
                }
            } else {
                result += c;
                lastSpace = false;
            }
        }

        // Trim
        size_t start = result.find_first_not_of(' ');
        size_t end = result.find_last_not_of(' ');
        if (start == string::npos) return "";
        return result.substr(start, end - start + 1);
    }

    // ASCII-only toLowerCase (Japanese/CJK chars are unaffected)
    static string toLowerASCII(const string& s) {
        string result = s;
        for (char& c : result) {
            if (c >= 'A' && c <= 'Z') c += 32;
        }
        return result;
    }

    static string replaceAll(const string& str, const string& from, const string& to) {
        string result = str;
        size_t pos = 0;
        while ((pos = result.find(from, pos)) != string::npos) {
            result.replace(pos, from.length(), to);
            pos += to.length();
        }
        return result;
    }
};
