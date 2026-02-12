#pragma once

// =============================================================================
// ClipTokenizer.h - CLIP BPE tokenizer (byte-level, GPT-2 style)
// =============================================================================
// Loads vocab.json + merges.txt, tokenizes text for CLIP text encoder.

#include <TrussC.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <climits>

using namespace std;
using namespace tc;

class ClipTokenizer {
public:
    static constexpr int SOT_TOKEN = 49406;  // <|startoftext|>
    static constexpr int EOT_TOKEN = 49407;  // <|endoftext|>
    static constexpr int CONTEXT_LEN = 77;

    bool load(const string& vocabPath, const string& mergesPath) {
        // Load vocab.json
        ifstream vf(vocabPath);
        if (!vf.is_open()) {
            logError() << "[ClipTokenizer] Failed to open: " << vocabPath;
            return false;
        }
        nlohmann::json j = nlohmann::json::parse(vf, nullptr, false);
        if (j.is_discarded()) {
            logError() << "[ClipTokenizer] Failed to parse vocab.json";
            return false;
        }
        for (auto& [key, val] : j.items()) {
            vocab_[key] = val.get<int>();
        }

        // Load merges.txt
        ifstream mf(mergesPath);
        if (!mf.is_open()) {
            logError() << "[ClipTokenizer] Failed to open: " << mergesPath;
            return false;
        }
        string line;
        getline(mf, line); // skip header "#version: 0.2"
        int rank = 0;
        while (getline(mf, line)) {
            if (line.empty()) continue;
            auto sp = line.find(' ');
            if (sp == string::npos) continue;
            string a = line.substr(0, sp);
            string b = line.substr(sp + 1);
            mergeRanks_[a + "\t" + b] = rank++;
        }

        buildByteEncoder();

        logNotice() << "[ClipTokenizer] Loaded: " << vocab_.size()
                    << " tokens, " << mergeRanks_.size() << " merges";
        return true;
    }

    // Encode text → token IDs [CONTEXT_LEN], padded with 0
    vector<int64_t> encode(const string& text) {
        vector<int64_t> tokens;
        tokens.push_back(SOT_TOKEN);

        string cleaned = toLower(text);

        // Split into words and BPE-encode each
        auto words = splitWords(cleaned);
        for (const auto& word : words) {
            // Convert bytes through byte encoder
            string encoded;
            for (unsigned char c : word) {
                encoded += byteEncoder_[c];
            }

            // Apply BPE with </w> suffix on last char
            auto bpeTokens = bpe(encoded);
            for (const auto& t : bpeTokens) {
                auto it = vocab_.find(t);
                if (it != vocab_.end()) {
                    tokens.push_back(it->second);
                }
            }

            if ((int)tokens.size() >= CONTEXT_LEN - 1) break;
        }

        tokens.push_back(EOT_TOKEN);

        // Pad to CONTEXT_LEN
        while ((int)tokens.size() < CONTEXT_LEN) {
            tokens.push_back(0);
        }
        tokens.resize(CONTEXT_LEN);
        return tokens;
    }

private:
    unordered_map<string, int> vocab_;
    unordered_map<string, int> mergeRanks_;  // "a\tb" → rank
    unordered_map<string, vector<string>> bpeCache_;
    string byteEncoder_[256];

    // Build byte-to-unicode mapping (OpenAI GPT-2 style)
    void buildByteEncoder() {
        vector<int> bs, cs;
        // Printable ranges that map to themselves
        for (int i = 33; i <= 126; i++) { bs.push_back(i); cs.push_back(i); }
        for (int i = 161; i <= 172; i++) { bs.push_back(i); cs.push_back(i); }
        for (int i = 174; i <= 255; i++) { bs.push_back(i); cs.push_back(i); }

        // Remaining bytes map to 256+
        int n = 0;
        for (int b = 0; b < 256; b++) {
            bool found = false;
            for (int x : bs) { if (x == b) { found = true; break; } }
            if (!found) {
                bs.push_back(b);
                cs.push_back(256 + n);
                n++;
            }
        }

        for (size_t i = 0; i < bs.size(); i++) {
            byteEncoder_[bs[i]] = codepointToUtf8(cs[i]);
        }
    }

    static string codepointToUtf8(int cp) {
        string s;
        if (cp < 0x80) {
            s += (char)cp;
        } else if (cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
        return s;
    }

    // Split text into "words" for BPE (simplified CLIP pattern)
    // Groups: ASCII letters | digits | non-ASCII (CJK etc.) | punctuation
    static vector<string> splitWords(const string& text) {
        vector<string> words;
        string current;
        enum Mode { NONE, ALPHA, DIGIT, NONASCII, PUNCT };
        Mode mode = NONE;

        size_t i = 0;
        while (i < text.size()) {
            unsigned char c = text[i];

            if (c <= ' ') {
                // Whitespace
                if (!current.empty()) { words.push_back(current); current.clear(); }
                mode = NONE;
                i++;
            } else if (c >= 0x80) {
                // Non-ASCII bytes: group together
                if (mode != NONASCII && !current.empty()) {
                    words.push_back(current); current.clear();
                }
                int len = utf8CharLen(c);
                current += text.substr(i, len);
                mode = NONASCII;
                i += len;
            } else if (isalpha(c)) {
                if (mode != ALPHA && !current.empty()) {
                    words.push_back(current); current.clear();
                }
                current += c;
                mode = ALPHA;
                i++;
            } else if (isdigit(c)) {
                if (mode != DIGIT && !current.empty()) {
                    words.push_back(current); current.clear();
                }
                current += c;
                mode = DIGIT;
                i++;
            } else {
                // Punctuation
                if (mode != PUNCT && !current.empty()) {
                    words.push_back(current); current.clear();
                }
                current += c;
                mode = PUNCT;
                i++;
            }
        }
        if (!current.empty()) words.push_back(current);
        return words;
    }

    static int utf8CharLen(unsigned char c) {
        if (c < 0x80) return 1;
        if (c < 0xC0) return 1; // continuation byte
        if (c < 0xE0) return 2;
        if (c < 0xF0) return 3;
        return 4;
    }

    // BPE algorithm: split word into subword tokens
    vector<string> bpe(const string& token) {
        auto cacheIt = bpeCache_.find(token);
        if (cacheIt != bpeCache_.end()) return cacheIt->second;

        // Split into UTF-8 characters
        vector<string> word;
        size_t i = 0;
        while (i < token.size()) {
            int len = utf8CharLen((unsigned char)token[i]);
            word.push_back(token.substr(i, len));
            i += len;
        }

        if (word.empty()) return {};

        // Append </w> to last character (CLIP convention)
        word.back() += "</w>";

        while (word.size() > 1) {
            // Find pair with lowest merge rank
            int bestRank = INT_MAX;
            int bestIdx = -1;
            for (size_t j = 0; j + 1 < word.size(); j++) {
                string pairKey = word[j] + "\t" + word[j + 1];
                auto it = mergeRanks_.find(pairKey);
                if (it != mergeRanks_.end() && it->second < bestRank) {
                    bestRank = it->second;
                    bestIdx = (int)j;
                }
            }

            if (bestIdx < 0) break;

            // Merge all occurrences of the best pair
            string first = word[bestIdx];
            string second = word[bestIdx + 1];
            string merged = first + second;

            vector<string> newWord;
            size_t j = 0;
            while (j < word.size()) {
                if (j + 1 < word.size() && word[j] == first && word[j + 1] == second) {
                    newWord.push_back(merged);
                    j += 2;
                } else {
                    newWord.push_back(word[j]);
                    j++;
                }
            }
            word = std::move(newWord);
        }

        bpeCache_[token] = word;
        return word;
    }

    static string toLower(const string& s) {
        string result = s;
        transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }
};
