#pragma once

// =============================================================================
// FaceRecognizer.h - ArcFace face recognition via ONNX Runtime
// Takes detected face landmarks, aligns the face, extracts 512D embedding.
// =============================================================================

#include <onnxruntime_cxx_api.h>
#if defined(__APPLE__)
#include <coreml_provider_factory.h>
#endif
#include <TrussC.h>
#include "FaceDetector.h"
#include <string>
#include <vector>
#include <array>
#include <cmath>

using namespace std;
using namespace tc;

class FaceRecognizer {
public:
    static constexpr int EMBED_DIM = 512;
    static constexpr int FACE_SIZE = 112;

    bool load(const string& modelPath) {
        try {
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(2);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#if defined(__APPLE__)
            OrtSessionOptionsAppendExecutionProvider_CoreML(opts, 0);
#endif

            session_ = make_unique<Ort::Session>(env_, modelPath.c_str(), opts);

            // Discover names
            Ort::AllocatorWithDefaultOptions alloc;
            auto inName = session_->GetInputNameAllocated(0, alloc);
            inputName_ = inName.get();
            auto outName = session_->GetOutputNameAllocated(0, alloc);
            outputName_ = outName.get();

            logNotice() << "[FaceRecognizer] Loaded: " << modelPath;
            return true;
        } catch (const Ort::Exception& e) {
            logError() << "[FaceRecognizer] Failed to load: " << e.what();
            return false;
        }
    }

    bool isLoaded() const { return session_ != nullptr; }

    // Extract face embedding from an image given detected face landmarks
    // imgData: RGB uint8, row-major
    // face: DetectedFace with kps in original image pixel coordinates (not normalized)
    vector<float> getEmbedding(const uint8_t* imgData, int imgW, int imgH,
                               const DetectedFace& face) {
        if (!session_) return {};

        // 1. Align face: compute similarity transform from 5 landmarks → template
        float srcPts[5][2];
        for (int i = 0; i < 5; i++) {
            srcPts[i][0] = face.kps[i * 2];
            srcPts[i][1] = face.kps[i * 2 + 1];
        }

        // Estimate similarity transform (rotation, scale, translation)
        float M[2][3];
        estimateSimilarityTransform(srcPts, ARCFACE_DST, 5, M);

        // 2. Warp affine to 112×112
        vector<uint8_t> aligned(FACE_SIZE * FACE_SIZE * 3, 0);
        warpAffine(imgData, imgW, imgH, aligned.data(), FACE_SIZE, FACE_SIZE, M);

        // 3. Normalize: (pixel - 127.5) / 127.5, NCHW format
        vector<float> blob(3 * FACE_SIZE * FACE_SIZE);
        for (int y = 0; y < FACE_SIZE; y++) {
            for (int x = 0; x < FACE_SIZE; x++) {
                int idx = y * FACE_SIZE + x;
                int srcIdx = idx * 3;
                // ArcFace expects BGR, NCHW
                blob[0 * FACE_SIZE * FACE_SIZE + idx] =
                    (aligned[srcIdx + 2] - 127.5f) / 127.5f;  // B
                blob[1 * FACE_SIZE * FACE_SIZE + idx] =
                    (aligned[srcIdx + 1] - 127.5f) / 127.5f;  // G
                blob[2 * FACE_SIZE * FACE_SIZE + idx] =
                    (aligned[srcIdx + 0] - 127.5f) / 127.5f;  // R
            }
        }

        // 4. Run inference
        try {
            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            int64_t inputShape[] = {1, 3, FACE_SIZE, FACE_SIZE};

            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, blob.data(), blob.size(),
                inputShape, 4);

            const char* inputNames[] = {inputName_.c_str()};
            const char* outputNames[] = {outputName_.c_str()};

            auto outputs = session_->Run(
                Ort::RunOptions{nullptr},
                inputNames, &inputTensor, 1,
                outputNames, 1);

            float* outData = outputs[0].GetTensorMutableData<float>();
            auto outInfo = outputs[0].GetTensorTypeAndShapeInfo();
            size_t outSize = outInfo.GetElementCount();

            vector<float> embedding(outData, outData + outSize);

            // L2 normalize
            float norm = 0;
            for (float v : embedding) norm += v * v;
            norm = sqrtf(norm);
            if (norm > 0) {
                for (float& v : embedding) v /= norm;
            }

            return embedding;
        } catch (const Ort::Exception& e) {
            logError() << "[FaceRecognizer] Inference failed: " << e.what();
            return {};
        }
    }

    // Cosine similarity between two L2-normalized embeddings
    static float similarity(const vector<float>& a, const vector<float>& b) {
        if (a.size() != b.size() || a.empty()) return 0;
        float dot = 0;
        for (size_t i = 0; i < a.size(); i++) dot += a[i] * b[i];
        return dot;
    }

private:
    // ArcFace template landmarks (112×112 image space)
    // left_eye, right_eye, nose, left_mouth, right_mouth
    static constexpr float ARCFACE_DST[5][2] = {
        {38.2946f, 51.6963f},
        {73.5318f, 51.5014f},
        {56.0252f, 71.7366f},
        {41.5493f, 92.3655f},
        {70.7299f, 92.2041f}
    };

    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "FaceRecognizer"};
    unique_ptr<Ort::Session> session_;
    string inputName_;
    string outputName_;

    // =========================================================================
    // Similarity Transform estimation (Umeyama algorithm, simplified)
    // Maps src points to dst points using rotation, uniform scale, translation
    // Result: 2×3 affine matrix M such that dst ≈ M × [src; 1]
    // =========================================================================
    static void estimateSimilarityTransform(const float src[][2], const float dst[][2],
                                            int n, float M[2][3]) {
        // Compute centroids
        float srcMeanX = 0, srcMeanY = 0, dstMeanX = 0, dstMeanY = 0;
        for (int i = 0; i < n; i++) {
            srcMeanX += src[i][0]; srcMeanY += src[i][1];
            dstMeanX += dst[i][0]; dstMeanY += dst[i][1];
        }
        srcMeanX /= n; srcMeanY /= n;
        dstMeanX /= n; dstMeanY /= n;

        // Centered coordinates
        float srcVar = 0;
        float s00 = 0, s01 = 0, s10 = 0, s11 = 0;  // covariance matrix
        for (int i = 0; i < n; i++) {
            float sx = src[i][0] - srcMeanX;
            float sy = src[i][1] - srcMeanY;
            float dx = dst[i][0] - dstMeanX;
            float dy = dst[i][1] - dstMeanY;
            srcVar += sx * sx + sy * sy;
            s00 += dx * sx; s01 += dx * sy;
            s10 += dy * sx; s11 += dy * sy;
        }
        srcVar /= n;

        // SVD of 2×2 covariance: use analytic solution
        // For 2×2: [s00, s01; s10, s11]
        // Compute rotation and scale
        float det = s00 * s11 - s01 * s10;
        float trace = s00 + s11;

        // Scale
        float normS = sqrtf(s00 * s00 + s01 * s01 + s10 * s10 + s11 * s11);
        float scale = (srcVar > 1e-6f) ? normS / (n * srcVar) : 1.0f;

        // For 2×2 case with non-degenerate data, use Procrustes:
        // R = V * diag(1, det(V*U^T)) * U^T
        // Simplified: compute angle from covariance
        // M = scale * R
        // But the analytic SVD for 2×2 is:
        float a = s00 + s11;
        float b = s01 - s10;
        float theta = atan2f(b, a);

        float cosT = cosf(theta);
        float sinT = sinf(theta);

        // Recompute scale more accurately
        // scale = trace(D * S) / var(src) where D comes from SVD
        // For similarity transform: scale = sum(dst_centered dot R*src_centered) / sum(|src_centered|^2)
        float num = 0, den = 0;
        for (int i = 0; i < n; i++) {
            float sx = src[i][0] - srcMeanX;
            float sy = src[i][1] - srcMeanY;
            float dx = dst[i][0] - dstMeanX;
            float dy = dst[i][1] - dstMeanY;
            // R * src
            float rx = cosT * sx - sinT * sy;
            float ry = sinT * sx + cosT * sy;
            num += dx * rx + dy * ry;
            den += sx * sx + sy * sy;
        }
        scale = (den > 1e-6f) ? num / den : 1.0f;

        // Compose: M = [scale*R | t]
        float r00 = scale * cosT;
        float r01 = -scale * sinT;
        float r10 = scale * sinT;
        float r11 = scale * cosT;

        M[0][0] = r00;
        M[0][1] = r01;
        M[0][2] = dstMeanX - r00 * srcMeanX - r01 * srcMeanY;
        M[1][0] = r10;
        M[1][1] = r11;
        M[1][2] = dstMeanY - r10 * srcMeanX - r11 * srcMeanY;
    }

    // =========================================================================
    // Warp affine (bilinear interpolation)
    // =========================================================================
    static void warpAffine(const uint8_t* src, int srcW, int srcH,
                           uint8_t* dst, int dstW, int dstH,
                           const float M[2][3]) {
        // Compute inverse transform: src = M_inv * dst
        // M = [a b tx; c d ty]
        // M_inv = [d -b (b*ty-d*tx); -c a (c*tx-a*ty)] / det
        float a = M[0][0], b = M[0][1], tx = M[0][2];
        float c = M[1][0], d = M[1][1], ty = M[1][2];
        float det = a * d - b * c;
        if (fabsf(det) < 1e-8f) return;
        float invDet = 1.0f / det;

        float ia = d * invDet, ib = -b * invDet;
        float ic = -c * invDet, id = a * invDet;
        float itx = (b * ty - d * tx) * invDet;
        float ity = (c * tx - a * ty) * invDet;

        for (int y = 0; y < dstH; y++) {
            for (int x = 0; x < dstW; x++) {
                float sx = ia * x + ib * y + itx;
                float sy = ic * x + id * y + ity;

                int x0 = (int)floorf(sx);
                int y0 = (int)floorf(sy);
                float fx = sx - x0;
                float fy = sy - y0;

                if (x0 < 0 || x0 >= srcW - 1 || y0 < 0 || y0 >= srcH - 1) {
                    dst[(y * dstW + x) * 3 + 0] = 0;
                    dst[(y * dstW + x) * 3 + 1] = 0;
                    dst[(y * dstW + x) * 3 + 2] = 0;
                    continue;
                }

                for (int ch = 0; ch < 3; ch++) {
                    float v00 = src[(y0 * srcW + x0) * 3 + ch];
                    float v01 = src[(y0 * srcW + (x0 + 1)) * 3 + ch];
                    float v10 = src[((y0 + 1) * srcW + x0) * 3 + ch];
                    float v11 = src[((y0 + 1) * srcW + (x0 + 1)) * 3 + ch];
                    float v = v00 * (1 - fx) * (1 - fy) + v01 * fx * (1 - fy)
                            + v10 * (1 - fx) * fy + v11 * fx * fy;
                    dst[(y * dstW + x) * 3 + ch] = (uint8_t)max(0.0f, min(255.0f, v + 0.5f));
                }
            }
        }
    }
};
