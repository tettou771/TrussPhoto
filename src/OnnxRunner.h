#pragma once

// =============================================================================
// OnnxRunner.h - ONNX Runtime session RAII wrapper
// =============================================================================

#include <onnxruntime_cxx_api.h>
#include <TrussC.h>
#include <string>
#include <vector>
#include <memory>

using namespace std;
using namespace tc;

class OnnxRunner {
public:
    OnnxRunner() : env_(ORT_LOGGING_LEVEL_WARNING, "TrussPhoto") {}

    bool load(const string& modelPath) {
        try {
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(2);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            session_ = make_unique<Ort::Session>(env_, modelPath.c_str(), opts);
            logNotice() << "[OnnxRunner] Model loaded: " << modelPath;
            return true;
        } catch (const Ort::Exception& e) {
            logError() << "[OnnxRunner] Failed to load model: " << e.what();
            return false;
        }
    }

    bool isLoaded() const { return session_ != nullptr; }

    // Run inference: single input tensor → single output tensor
    vector<float> run(const vector<float>& input,
                      const vector<int64_t>& inputShape,
                      const char* inputName = "pixel_values",
                      const char* outputName = "image_embeds") {
        if (!session_) return {};

        try {
            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, const_cast<float*>(input.data()), input.size(),
                inputShape.data(), inputShape.size());

            const char* inputNames[] = {inputName};
            const char* outputNames[] = {outputName};

            auto outputs = session_->Run(
                Ort::RunOptions{nullptr},
                inputNames, &inputTensor, 1,
                outputNames, 1);

            // Extract output
            float* outData = outputs[0].GetTensorMutableData<float>();
            auto outInfo = outputs[0].GetTensorTypeAndShapeInfo();
            size_t outSize = outInfo.GetElementCount();

            return vector<float>(outData, outData + outSize);
        } catch (const Ort::Exception& e) {
            logError() << "[OnnxRunner] Inference failed: " << e.what();
            return {};
        }
    }

    // Query input/output names (for debugging)
    void printModelInfo() {
        if (!session_) return;
        Ort::AllocatorWithDefaultOptions alloc;

        size_t numInputs = session_->GetInputCount();
        for (size_t i = 0; i < numInputs; i++) {
            auto name = session_->GetInputNameAllocated(i, alloc);
            auto info = session_->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            logNotice() << "[OnnxRunner] Input " << i << ": " << name.get()
                        << " shape=[" << shapeStr(shape) << "]";
        }

        size_t numOutputs = session_->GetOutputCount();
        for (size_t i = 0; i < numOutputs; i++) {
            auto name = session_->GetOutputNameAllocated(i, alloc);
            auto info = session_->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            logNotice() << "[OnnxRunner] Output " << i << ": " << name.get()
                        << " shape=[" << shapeStr(shape) << "]";
        }
    }

private:
    Ort::Env env_;
    unique_ptr<Ort::Session> session_;

    static string shapeStr(const vector<int64_t>& shape) {
        string s;
        for (size_t i = 0; i < shape.size(); i++) {
            if (i > 0) s += ", ";
            s += to_string(shape[i]);
        }
        return s;
    }
};
