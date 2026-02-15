#pragma once

// =============================================================================
// SmartPreview.h - JPEG XL float16 lossy encode/decode for smart previews
// Uses XYB color space for perceptually optimized compression
// =============================================================================

#include <TrussC.h>
#include <jxl/encode.h>
#include <jxl/decode.h>
#include <jxl/color_encoding.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/resizable_parallel_runner.h>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <cmath>

using namespace std;
using namespace tc;
namespace fs = std::filesystem;

class SmartPreview {
public:
    static constexpr int MAX_EDGE = 3072;
    static constexpr float ENCODE_DISTANCE = 2.0f; // 0=lossless, 1.0=visually lossless, 2.0=high quality
    static constexpr int ENCODE_EFFORT = 3;          // 1=fastest, 7=default, 9=slowest
    static constexpr size_t ENCODE_THREADS = 4;      // threads per encode (multiple encodes run in parallel)

    // Encode F32 Pixels to float16 lossy JPEG XL with XYB transform (resized to MAX_EDGE)
    // Preserves HDR values above 1.0 (highlight headroom)
    static bool encode(const Pixels& srcF32, const string& outPath) {
        if (!srcF32.isAllocated() || srcF32.getFormat() != PixelFormat::F32) {
            logWarning() << "[SmartPreview] encode: need F32 pixels";
            return false;
        }

        int srcW = srcF32.getWidth();
        int srcH = srcF32.getHeight();
        int srcCh = srcF32.getChannels();

        // Calculate target size (fit within MAX_EDGE)
        int dstW = srcW, dstH = srcH;
        if (srcW > MAX_EDGE || srcH > MAX_EDGE) {
            float scale = (float)MAX_EDGE / max(srcW, srcH);
            dstW = (int)(srcW * scale);
            dstH = (int)(srcH * scale);
        }

        // Resize F32 to target size as RGB (3ch), no clamping (preserves HDR)
        vector<float> rgbBuf(dstW * dstH * 3);
        const float* src = srcF32.getDataF32();

        for (int y = 0; y < dstH; y++) {
            float srcY = (float)y * srcH / dstH;
            int y0 = min((int)srcY, srcH - 1);
            int y1 = min(y0 + 1, srcH - 1);
            float fy = srcY - y0;

            for (int x = 0; x < dstW; x++) {
                float srcX = (float)x * srcW / dstW;
                int x0 = min((int)srcX, srcW - 1);
                int x1 = min(x0 + 1, srcW - 1);
                float fx = srcX - x0;

                int dstIdx = (y * dstW + x) * 3;
                for (int c = 0; c < 3; c++) {
                    // Bilinear interpolation
                    float v00 = src[(y0 * srcW + x0) * srcCh + c];
                    float v10 = src[(y0 * srcW + x1) * srcCh + c];
                    float v01 = src[(y1 * srcW + x0) * srcCh + c];
                    float v11 = src[(y1 * srcW + x1) * srcCh + c];
                    rgbBuf[dstIdx + c] = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy)
                                       + v01 * (1-fx) * fy + v11 * fx * fy;
                }
            }
        }

        // Create parallel runner (limit threads since multiple encodes run concurrently)
        void* runner = JxlResizableParallelRunnerCreate(nullptr);
        JxlResizableParallelRunnerSetThreads(runner, ENCODE_THREADS);

        // Create encoder
        JxlEncoder* enc = JxlEncoderCreate(nullptr);
        if (!enc) {
            JxlResizableParallelRunnerDestroy(runner);
            return false;
        }
        JxlEncoderSetParallelRunner(enc, JxlResizableParallelRunner, runner);

        // Basic info: float16 with XYB transform
        JxlBasicInfo info;
        JxlEncoderInitBasicInfo(&info);
        info.xsize = dstW;
        info.ysize = dstH;
        info.bits_per_sample = 16;
        info.exponent_bits_per_sample = 5; // IEEE float16
        info.num_color_channels = 3;
        info.num_extra_channels = 0;
        info.alpha_bits = 0;
        info.uses_original_profile = JXL_FALSE; // Enable XYB transform

        if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) {
            logWarning() << "[SmartPreview] Failed to set basic info";
            JxlEncoderDestroy(enc);
            JxlResizableParallelRunnerDestroy(runner);
            return false;
        }

        // Tell encoder the input is sRGB (gamma-encoded from LibRaw)
        JxlColorEncoding colorEnc;
        JxlColorEncodingSetToSRGB(&colorEnc, JXL_FALSE);
        JxlEncoderSetColorEncoding(enc, &colorEnc);

        // Frame settings: lossy with configurable distance
        JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
        if (ENCODE_DISTANCE == 0.0f) {
            JxlEncoderSetFrameLossless(settings, JXL_TRUE);
        }
        JxlEncoderSetFrameDistance(settings, ENCODE_DISTANCE);
        // Effort 3 = fast encode (default 7 is too slow for batch SP generation)
        JxlEncoderFrameSettingsSetOption(settings,
            JXL_ENC_FRAME_SETTING_EFFORT, ENCODE_EFFORT);

        // Pixel format: FLOAT32 input, 3 channels (encoder stores as float16)
        JxlPixelFormat pixfmt = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};

        if (JxlEncoderAddImageFrame(settings, &pixfmt,
                rgbBuf.data(), rgbBuf.size() * sizeof(float)) != JXL_ENC_SUCCESS) {
            logWarning() << "[SmartPreview] Failed to add image frame";
            JxlEncoderDestroy(enc);
            JxlResizableParallelRunnerDestroy(runner);
            return false;
        }

        JxlEncoderCloseInput(enc);

        // Process output
        vector<uint8_t> compressed(256 * 1024); // 256KB initial (XYB float16 ~300KB typical)
        uint8_t* next = compressed.data();
        size_t avail = compressed.size();

        JxlEncoderStatus status;
        while ((status = JxlEncoderProcessOutput(enc, &next, &avail)) == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t used = next - compressed.data();
            compressed.resize(compressed.size() * 2);
            next = compressed.data() + used;
            avail = compressed.size() - used;
        }

        JxlEncoderDestroy(enc);
        JxlResizableParallelRunnerDestroy(runner);

        if (status != JXL_ENC_SUCCESS) {
            logWarning() << "[SmartPreview] Encode failed";
            return false;
        }

        size_t finalSize = next - compressed.data();

        // Write to file
        fs::create_directories(fs::path(outPath).parent_path());
        ofstream out(outPath, ios::binary);
        if (!out) {
            logWarning() << "[SmartPreview] Failed to write: " << outPath;
            return false;
        }
        out.write(reinterpret_cast<const char*>(compressed.data()), finalSize);
        out.close();

        logNotice() << "[SmartPreview] Encoded " << dstW << "x" << dstH
                    << " -> " << (finalSize / 1024) << "KB: " << outPath;
        return true;
    }

    // Decode JPEG XL file to F32 Pixels (RGBA)
    static bool decode(const string& jxlPath, Pixels& outF32) {
        // Read file
        ifstream file(jxlPath, ios::binary | ios::ate);
        if (!file) return false;
        auto fileSize = file.tellg();
        file.seekg(0, ios::beg);
        vector<uint8_t> data(fileSize);
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
        file.close();

        // Create parallel runner
        void* runner = JxlResizableParallelRunnerCreate(nullptr);

        // Create decoder
        JxlDecoder* dec = JxlDecoderCreate(nullptr);
        if (!dec) {
            JxlResizableParallelRunnerDestroy(runner);
            return false;
        }
        JxlDecoderSetParallelRunner(dec, JxlResizableParallelRunner, runner);

        int events = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE;
        JxlDecoderSubscribeEvents(dec, events);
        JxlDecoderSetInput(dec, data.data(), data.size());
        JxlDecoderCloseInput(dec);

        // Output format: FLOAT32, RGBA
        JxlPixelFormat pixfmt = {4, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
        JxlBasicInfo info = {};
        vector<float> pixelBuf;

        JxlDecoderStatus status;
        while ((status = JxlDecoderProcessInput(dec)) != JXL_DEC_SUCCESS) {
            if (status == JXL_DEC_BASIC_INFO) {
                JxlDecoderGetBasicInfo(dec, &info);
                JxlResizableParallelRunnerSetThreads(runner,
                    JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
                pixelBuf.resize(info.xsize * info.ysize * 4);
            } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                JxlDecoderSetImageOutBuffer(dec, &pixfmt,
                    pixelBuf.data(), pixelBuf.size() * sizeof(float));
            } else if (status == JXL_DEC_FULL_IMAGE) {
                // Image decoded, will get JXL_DEC_SUCCESS next
            } else {
                logWarning() << "[SmartPreview] Decode error: status=" << (int)status;
                JxlDecoderDestroy(dec);
                JxlResizableParallelRunnerDestroy(runner);
                return false;
            }
        }

        JxlDecoderDestroy(dec);
        JxlResizableParallelRunnerDestroy(runner);

        if (info.xsize == 0 || info.ysize == 0) return false;

        // Copy to Pixels F32
        outF32.allocate(info.xsize, info.ysize, 4, PixelFormat::F32);
        float* dst = outF32.getDataF32();

        if (info.num_color_channels == 3 && info.alpha_bits == 0) {
            // Input was 3ch, output is 4ch RGBA — libjxl fills alpha=1.0 for us
        }
        memcpy(dst, pixelBuf.data(), pixelBuf.size() * sizeof(float));

        logNotice() << "[SmartPreview] Decoded " << info.xsize << "x" << info.ysize
                    << " from: " << jxlPath;
        return true;
    }
};
