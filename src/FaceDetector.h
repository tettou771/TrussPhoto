#pragma once

// =============================================================================
// FaceDetector.h - SCRFD face detection via ONNX Runtime
// Detects faces and 5-point landmarks from an image.
// Input: SP or any image as raw pixel buffer (RGB, uint8)
// Output: vector of DetectedFace (bbox + landmarks + score)
// =============================================================================

#include "OnnxRunner.h"  // for getSharedOrtEnv()
#include <TrussC.h>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <numeric>

using namespace std;
using namespace tc;

struct DetectedFace {
    float x1, y1, x2, y2;     // bbox in original image coordinates
    float score;
    array<float, 10> kps;     // 5 landmarks: [x0,y0, x1,y1, ... x4,y4]

    // Normalized bbox (0-1) relative to image dimensions
    void normalize(int imgW, int imgH) {
        x1 /= imgW; y1 /= imgH;
        x2 /= imgW; y2 /= imgH;
        for (int i = 0; i < 5; i++) {
            kps[i * 2] /= imgW;
            kps[i * 2 + 1] /= imgH;
        }
    }

    float width() const { return x2 - x1; }
    float height() const { return y2 - y1; }
    float area() const { return width() * height(); }
};

class FaceDetector {
public:
    bool load(const string& modelPath) {
        try {
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(2);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#if defined(__APPLE__)
            opts.AppendExecutionProvider("CoreML", {
                {kCoremlProviderOption_ModelCacheDirectory, getCoreMLCacheDir()}
            });
#endif

            session_ = make_unique<Ort::Session>(getSharedOrtEnv(), modelPath.c_str(), opts);

            // Discover input/output names
            Ort::AllocatorWithDefaultOptions alloc;
            auto inName = session_->GetInputNameAllocated(0, alloc);
            inputName_ = inName.get();

            size_t numOutputs = session_->GetOutputCount();
            outputNames_.reserve(numOutputs);
            for (size_t i = 0; i < numOutputs; i++) {
                auto name = session_->GetOutputNameAllocated(i, alloc);
                outputNames_.push_back(name.get());
            }
            // Build pointer array after all names added (push_back may reallocate)
            outputNamePtrs_.reserve(numOutputs);
            for (const auto& n : outputNames_) {
                outputNamePtrs_.push_back(n.c_str());
            }

            // det_10g has 9 outputs: 3×scores + 3×bbox + 3×kps
            // stride 8: 12800 anchors, stride 16: 3200, stride 32: 800
            // (for 640×640 input)
            hasKps_ = (numOutputs == 9);

            logNotice() << "[FaceDetector] Loaded: " << modelPath
                        << " outputs=" << numOutputs
                        << " kps=" << (hasKps_ ? "yes" : "no");
            return true;
        } catch (const Ort::Exception& e) {
            logError() << "[FaceDetector] Failed to load: " << e.what();
            return false;
        }
    }

    bool isLoaded() const { return session_ != nullptr; }

    // Detect faces in an RGB uint8 image buffer
    // imgData: RGB pixels, row-major, 3 bytes per pixel
    vector<DetectedFace> detect(const uint8_t* imgData, int imgW, int imgH,
                                float scoreThresh = 0.5f, int maxFaces = 0) {
        if (!session_) return {};

        // Preprocess: resize keeping aspect ratio, pad to INPUT_SIZE
        float imRatio = (float)imgH / imgW;
        float modelRatio = (float)INPUT_SIZE / INPUT_SIZE;  // 1.0 for square
        int newW, newH;
        if (imRatio > modelRatio) {
            newH = INPUT_SIZE;
            newW = (int)(newH / imRatio);
        } else {
            newW = INPUT_SIZE;
            newH = (int)(newW * imRatio);
        }
        float detScale = (float)newH / imgH;

        // Resize with bilinear interpolation (simple implementation)
        vector<uint8_t> resized(newW * newH * 3);
        bilinearResize(imgData, imgW, imgH, resized.data(), newW, newH);

        // Pad to INPUT_SIZE × INPUT_SIZE, fill with 0
        // Normalize: (pixel - 127.5) / 128.0, and convert RGB→BGR (SCRFD expects BGR)
        vector<float> blob(3 * INPUT_SIZE * INPUT_SIZE, 0.0f);
        for (int y = 0; y < newH; y++) {
            for (int x = 0; x < newW; x++) {
                int srcIdx = (y * newW + x) * 3;
                // NCHW format, BGR order
                blob[0 * INPUT_SIZE * INPUT_SIZE + y * INPUT_SIZE + x] =
                    (resized[srcIdx + 2] - 127.5f) / 128.0f;  // B
                blob[1 * INPUT_SIZE * INPUT_SIZE + y * INPUT_SIZE + x] =
                    (resized[srcIdx + 1] - 127.5f) / 128.0f;  // G
                blob[2 * INPUT_SIZE * INPUT_SIZE + y * INPUT_SIZE + x] =
                    (resized[srcIdx + 0] - 127.5f) / 128.0f;  // R
            }
        }

        // Run inference
        vector<float> scores, bboxes, kpsData;
        if (!runInference(blob, scores, bboxes, kpsData, scoreThresh)) {
            return {};
        }

        // Scale back to original image coordinates
        int numDet = (int)scores.size();
        vector<DetectedFace> faces;
        faces.reserve(numDet);
        for (int i = 0; i < numDet; i++) {
            DetectedFace f;
            f.score = scores[i];
            f.x1 = bboxes[i * 4 + 0] / detScale;
            f.y1 = bboxes[i * 4 + 1] / detScale;
            f.x2 = bboxes[i * 4 + 2] / detScale;
            f.y2 = bboxes[i * 4 + 3] / detScale;
            if (hasKps_) {
                for (int k = 0; k < 10; k++) {
                    f.kps[k] = kpsData[i * 10 + k] / detScale;
                }
            }
            faces.push_back(f);
        }

        // NMS
        faces = nms(faces, NMS_THRESH);

        // Limit number of faces
        if (maxFaces > 0 && (int)faces.size() > maxFaces) {
            // Keep largest + most centered faces
            sort(faces.begin(), faces.end(), [&](const DetectedFace& a, const DetectedFace& b) {
                return a.area() > b.area();
            });
            faces.resize(maxFaces);
        }

        return faces;
    }

private:
    static constexpr int INPUT_SIZE = 640;
    static constexpr float NMS_THRESH = 0.4f;

    // FPN config for det_10g: 3 strides, 2 anchors each
    static constexpr int FMC = 3;
    static constexpr int NUM_ANCHORS = 2;
    static constexpr int STRIDES[3] = {8, 16, 32};

    unique_ptr<Ort::Session> session_;
    string inputName_;
    vector<string> outputNames_;
    vector<const char*> outputNamePtrs_;
    bool hasKps_ = false;

    bool runInference(const vector<float>& blob,
                      vector<float>& outScores,
                      vector<float>& outBboxes,
                      vector<float>& outKps,
                      float scoreThresh) {
        try {
            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            int64_t inputShape[] = {1, 3, INPUT_SIZE, INPUT_SIZE};

            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, const_cast<float*>(blob.data()), blob.size(),
                inputShape, 4);

            const char* inputNames[] = {inputName_.c_str()};

            auto outputs = session_->Run(
                Ort::RunOptions{nullptr},
                inputNames, &inputTensor, 1,
                outputNamePtrs_.data(), outputNamePtrs_.size());

            // Decode FPN outputs
            // Output order: scores[0..2], bboxes[3..5], kps[6..8]
            // For each stride level
            for (int idx = 0; idx < FMC; idx++) {
                int stride = STRIDES[idx];
                int fmH = INPUT_SIZE / stride;
                int fmW = INPUT_SIZE / stride;
                int K = fmH * fmW * NUM_ANCHORS;

                // Scores
                float* scoreData = outputs[idx].GetTensorMutableData<float>();
                // Bbox predictions
                float* bboxData = outputs[idx + FMC].GetTensorMutableData<float>();
                // Kps predictions (if available)
                float* kpsPtr = hasKps_ ? outputs[idx + FMC * 2].GetTensorMutableData<float>() : nullptr;

                // Generate anchor centers
                vector<float> anchorCenters = generateAnchors(fmH, fmW, stride);

                for (int a = 0; a < K; a++) {
                    if (scoreData[a] < scoreThresh) continue;

                    float cx = anchorCenters[a * 2];
                    float cy = anchorCenters[a * 2 + 1];

                    // distance2bbox: center - left/top + right/bottom
                    float bx1 = cx - bboxData[a * 4 + 0] * stride;
                    float by1 = cy - bboxData[a * 4 + 1] * stride;
                    float bx2 = cx + bboxData[a * 4 + 2] * stride;
                    float by2 = cy + bboxData[a * 4 + 3] * stride;

                    outScores.push_back(scoreData[a]);
                    outBboxes.push_back(bx1);
                    outBboxes.push_back(by1);
                    outBboxes.push_back(bx2);
                    outBboxes.push_back(by2);

                    if (kpsPtr) {
                        for (int k = 0; k < 5; k++) {
                            float kx = cx + kpsPtr[a * 10 + k * 2] * stride;
                            float ky = cy + kpsPtr[a * 10 + k * 2 + 1] * stride;
                            outKps.push_back(kx);
                            outKps.push_back(ky);
                        }
                    }
                }
            }

            return true;
        } catch (const Ort::Exception& e) {
            logError() << "[FaceDetector] Inference failed: " << e.what();
            return false;
        }
    }

    // Generate anchor center coordinates for a feature map
    vector<float> generateAnchors(int fmH, int fmW, int stride) {
        vector<float> centers;
        centers.reserve(fmH * fmW * NUM_ANCHORS * 2);
        for (int y = 0; y < fmH; y++) {
            for (int x = 0; x < fmW; x++) {
                float cx = x * stride;
                float cy = y * stride;
                for (int a = 0; a < NUM_ANCHORS; a++) {
                    centers.push_back(cx);
                    centers.push_back(cy);
                }
            }
        }
        return centers;
    }

    // Non-Maximum Suppression
    static vector<DetectedFace> nms(vector<DetectedFace>& faces, float thresh) {
        sort(faces.begin(), faces.end(), [](const DetectedFace& a, const DetectedFace& b) {
            return a.score > b.score;
        });

        vector<bool> suppressed(faces.size(), false);
        vector<DetectedFace> result;

        for (int i = 0; i < (int)faces.size(); i++) {
            if (suppressed[i]) continue;
            result.push_back(faces[i]);

            for (int j = i + 1; j < (int)faces.size(); j++) {
                if (suppressed[j]) continue;
                float iou = computeIoU(faces[i], faces[j]);
                if (iou > thresh) suppressed[j] = true;
            }
        }
        return result;
    }

    static float computeIoU(const DetectedFace& a, const DetectedFace& b) {
        float ix1 = max(a.x1, b.x1);
        float iy1 = max(a.y1, b.y1);
        float ix2 = min(a.x2, b.x2);
        float iy2 = min(a.y2, b.y2);
        float iw = max(0.0f, ix2 - ix1);
        float ih = max(0.0f, iy2 - iy1);
        float inter = iw * ih;
        float unionArea = a.area() + b.area() - inter;
        return unionArea > 0 ? inter / unionArea : 0;
    }

    // Simple bilinear resize (RGB)
    static void bilinearResize(const uint8_t* src, int srcW, int srcH,
                               uint8_t* dst, int dstW, int dstH) {
        float xRatio = (float)srcW / dstW;
        float yRatio = (float)srcH / dstH;

        for (int y = 0; y < dstH; y++) {
            float srcY = y * yRatio;
            int y0 = (int)srcY;
            int y1 = min(y0 + 1, srcH - 1);
            float fy = srcY - y0;

            for (int x = 0; x < dstW; x++) {
                float srcX = x * xRatio;
                int x0 = (int)srcX;
                int x1 = min(x0 + 1, srcW - 1);
                float fx = srcX - x0;

                for (int c = 0; c < 3; c++) {
                    float v00 = src[(y0 * srcW + x0) * 3 + c];
                    float v01 = src[(y0 * srcW + x1) * 3 + c];
                    float v10 = src[(y1 * srcW + x0) * 3 + c];
                    float v11 = src[(y1 * srcW + x1) * 3 + c];
                    float v = v00 * (1 - fx) * (1 - fy) + v01 * fx * (1 - fy)
                            + v10 * (1 - fx) * fy + v11 * fx * fy;
                    dst[(y * dstW + x) * 3 + c] = (uint8_t)(v + 0.5f);
                }
            }
        }
    }
};
