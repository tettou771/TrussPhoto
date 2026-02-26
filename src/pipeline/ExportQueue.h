#pragma once

// =============================================================================
// ExportQueue.h - Background 3-stage export pipeline
// =============================================================================
// Loader thread:  SP/RAW decode + lens correction setup (background)
// Main thread:    GPU texture upload + develop shader render + Metal readback
// Saver thread:   CPU crop/rotation transform + resize + JPEG encode (background)
//
// Usage:
//   exportQueue_.start();
//   exportQueue_.enqueueThumbnail(job);
//   if (exportQueue_.processMainThread()) redraw();  // in update()
//   exportQueue_.stop();                              // in exit()
// =============================================================================

#include <TrussC.h>
#include <tcLut.h>
#include <tcxLibRaw.h>
#include "DevelopShader.h"
#include "LensCorrector.h"
#include "CameraProfileManager.h"
#include "WhiteBalance.h"
#include "GuidedFilter.h"
#include "SmartPreview.h"
#include "PhotoExporter.h"
#include "PhotoEntry.h"
#include <thread>
#include <atomic>
#include <mutex>

using namespace std;
using namespace tc;
using namespace tcx::lut;

// ---- Job types ----

enum class ExportJobType { Thumbnail, Jpeg };

struct ExportJobRequest {
    ExportJobType type = ExportJobType::Thumbnail;
    string photoId;
    string outPath;
    ExportSettings settings;
    PhotoEntry entry;     // snapshot of develop params at enqueue time
    string spPath;        // smart preview path (preferred source)
    string rawPath;       // RAW file path (fallback)
    string lensCorrectionParams; // JSON from DB
};

// Loader -> Main thread
struct ExportLoaderResult {
    ExportJobRequest job;
    Pixels sourcePixels;  // F32 RGBA (SP or RAW decoded)
    bool lensReady = false;
    LensCorrector lensCorrector; // ready to generate GPU data
    bool success = false;
};

// Main thread -> Saver thread
struct ExportSaverJob {
    ExportJobRequest job;
    Pixels fboPixels;     // U8 RGBA from readFboPixels
    int fboW = 0, fboH = 0;
};

// Saver -> Main thread (result notification)
struct ExportResult {
    string photoId;
    string outPath;
    ExportJobType type;
    bool success = false;
};

// ---- ExportQueue ----

class ExportQueue {
public:
    // Events (fired on main thread via processMainThread)
    Event<string> thumbnailReady;      // photoId
    Event<ExportResult> exportDone;    // full result

    ExportQueue() = default;

    ~ExportQueue() { stop(); }

    void setProfileDir(const string& dir) {
        profileManager_.setProfileDir(dir);
    }

    void start() {
        if (running_) return;
        running_ = true;

        // Initialize dedicated develop shader (must be on main thread)
        exportShader_.load();

        loaderThread_ = thread([this]() { loaderFunc(); });
        saverThread_ = thread([this]() { saverFunc(); });
        logNotice() << "[ExportQueue] Started";
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        requestQueue_.close();
        loadedQueue_.close();
        saverQueue_.close();
        resultQueue_.close();

        if (loaderThread_.joinable()) loaderThread_.join();
        if (saverThread_.joinable()) saverThread_.join();

        logNotice() << "[ExportQueue] Stopped";
    }

    // Enqueue a thumbnail generation job (thread-safe, call from main thread)
    void enqueueThumbnail(const ExportJobRequest& job) {
        total_++;
        pending_++;
        requestQueue_.send(job);
    }

    // Enqueue a JPEG export job
    void enqueueExport(const ExportJobRequest& job) {
        total_++;
        pending_++;
        requestQueue_.send(job);
    }

    // Process up to 1 loaded job on the main thread (GPU render + readback).
    // Also drain result queue (notify events).
    // Returns true if any work was done (caller should redraw).
    bool processMainThread() {
        bool didWork = false;

        // 1. Drain result queue -> fire events
        ExportResult result;
        while (resultQueue_.tryReceive(result)) {
            pending_--;
            completed_++;
            if (result.type == ExportJobType::Thumbnail && result.success) {
                thumbnailReady.notify(result.photoId);
            }
            exportDone.notify(result);
            didWork = true;
        }

        // 2. Process one loaded job (GPU render + readback)
        ExportLoaderResult loaded;
        if (loadedQueue_.tryReceive(loaded)) {
            processOneJob(loaded);
            didWork = true;
        }

        return didWork;
    }

    int pendingCount() const { return pending_.load(); }
    int completedCount() const { return completed_.load(); }
    int totalCount() const { return total_.load(); }
    bool isIdle() const { return pending_ == 0; }

    // Reset counters (call after batch completes)
    void resetCounters() {
        completed_ = 0;
        total_ = pending_.load();
    }

private:
    DevelopShader exportShader_;
    CameraProfileManager profileManager_;
    Lut3D profileLut_;
    string currentProfilePath_;

    thread loaderThread_;
    thread saverThread_;

    ThreadChannel<ExportJobRequest> requestQueue_;
    ThreadChannel<ExportLoaderResult> loadedQueue_;
    ThreadChannel<ExportSaverJob> saverQueue_;
    ThreadChannel<ExportResult> resultQueue_;

    atomic<int> pending_{0};
    atomic<int> completed_{0};
    atomic<int> total_{0};
    atomic<bool> running_{false};

    // ---- Loader thread (background) ----
    // Loads SP or RAW pixels + sets up lens correction from cached JSON
    void loaderFunc() {
        while (running_) {
            ExportJobRequest job;
            if (!requestQueue_.tryReceive(job, 200)) continue;

            ExportLoaderResult result;
            result.job = job;

            // Try smart preview first (faster, smaller)
            bool loaded = false;
            if (!job.spPath.empty() && fs::exists(job.spPath)) {
                loaded = SmartPreview::decode(job.spPath, result.sourcePixels);
                if (loaded) {
                    logNotice() << "[ExportQueue] Loaded SP: " << job.photoId;
                }
            }

            // Fallback to RAW
            if (!loaded && !job.rawPath.empty() && fs::exists(job.rawPath)) {
                if (job.entry.isRaw) {
                    loaded = RawLoader::loadFloat(job.rawPath, result.sourcePixels);
                } else {
                    // JPEG/HEIC: load as U8 then convert to F32
                    Pixels u8;
                    if (u8.load(job.rawPath)) {
                        int w = u8.getWidth(), h = u8.getHeight(), ch = u8.getChannels();
                        result.sourcePixels.allocate(w, h, 4, PixelFormat::F32);
                        float* dst = result.sourcePixels.getDataF32();
                        auto* src = u8.getData();
                        for (int i = 0; i < w * h; i++) {
                            dst[i*4+0] = src[i*ch+0] / 255.0f;
                            dst[i*4+1] = src[i*ch+1] / 255.0f;
                            dst[i*4+2] = (ch >= 3) ? src[i*ch+2] / 255.0f : dst[i*4+0];
                            dst[i*4+3] = 1.0f;
                        }
                        loaded = true;
                    }
                }
                if (loaded) {
                    logNotice() << "[ExportQueue] Loaded RAW: " << job.photoId;
                }
            }

            if (!loaded) {
                logWarning() << "[ExportQueue] Failed to load source: " << job.photoId;
                // Send failure result directly
                ExportResult fail;
                fail.photoId = job.photoId;
                fail.outPath = job.outPath;
                fail.type = job.type;
                fail.success = false;
                resultQueue_.send(std::move(fail));
                continue;
            }

            // Setup lens correction from cached JSON
            if (!job.lensCorrectionParams.empty()) {
                int pw = result.sourcePixels.getWidth();
                int ph = result.sourcePixels.getHeight();
                result.lensReady = result.lensCorrector.setupFromJson(
                    job.lensCorrectionParams, pw, ph);
            }

            // Apply noise reduction if needed (CPU, on loader thread)
            if (job.entry.chromaDenoise > 0 || job.entry.lumaDenoise > 0) {
                tp::guidedDenoise(result.sourcePixels,
                    job.entry.chromaDenoise, job.entry.lumaDenoise);
            }

            result.success = true;
            loadedQueue_.send(std::move(result));
        }
    }

    // ---- Main thread: GPU render + readback ----
    void processOneJob(ExportLoaderResult& loaded) {
        auto& job = loaded.job;
        auto& entry = job.entry;
        auto& pixels = loaded.sourcePixels;

        int srcW = pixels.getWidth();
        int srcH = pixels.getHeight();

        // 1. Upload source texture
        Texture srcTex;
        srcTex.allocate(pixels, TextureUsage::Immutable, true);
        exportShader_.setSourceTexture(srcTex);

        // 2. Setup lens correction GPU data
        exportShader_.clearLensData();
        if (loaded.lensReady) {
            auto distLut = loaded.lensCorrector.generateDistortionLUT();
            exportShader_.updateLensLUT(distLut.data(), 512);

            int vigRows, vigCols;
            auto vigMap = loaded.lensCorrector.generateVignettingMap(vigRows, vigCols);
            exportShader_.updateVigMap(vigMap.data(), vigRows, vigCols);
        }

        // 3. Setup lens uniform params
        {
            float cropRect[4];
            float optCenter[2];
            loaded.lensCorrector.getGpuCropRect(srcW, srcH, cropRect);
            loaded.lensCorrector.getGpuOpticalCenter(srcW, srcH, optCenter);
            float invDiag = loaded.lensCorrector.getGpuInvDiag(srcW, srcH);
            float autoScale = loaded.lensReady
                ? loaded.lensCorrector.getGpuAutoScale(srcW, srcH) : 1.0f;

            exportShader_.setLensParams(
                loaded.lensReady, autoScale,
                cropRect[0], cropRect[1], cropRect[2], cropRect[3],
                optCenter[0], optCenter[1], invDiag,
                (float)srcW, (float)srcH);
        }

        // 4. Setup develop params
        exportShader_.setExposure(entry.devExposure);
        exportShader_.setContrast(entry.devContrast);
        exportShader_.setHighlights(entry.devHighlights);
        exportShader_.setShadows(entry.devShadows);
        exportShader_.setWhites(entry.devWhites);
        exportShader_.setBlacks(entry.devBlacks);
        exportShader_.setVibrance(entry.devVibrance);
        exportShader_.setSaturation(entry.devSaturation);

        // White balance
        float asShotTemp = entry.asShotTemp;
        if (asShotTemp <= 0) asShotTemp = 5500.0f;
        float asShotTint = entry.asShotTint;
        float temperature = (entry.devTemperature > 0) ? entry.devTemperature : asShotTemp;
        auto wbMul = wb::kelvinToWbMultiplier(temperature, entry.devTint, asShotTemp, asShotTint);
        exportShader_.setWbMultiplier(wbMul.r, wbMul.g, wbMul.b);

        // 5. Load camera profile LUT
        string cubePath = profileManager_.findProfile(entry.camera, entry.creativeStyle);
        if (!cubePath.empty()) {
            if (cubePath != currentProfilePath_) {
                if (profileLut_.load(cubePath)) {
                    currentProfilePath_ = cubePath;
                } else {
                    currentProfilePath_.clear();
                }
            }
            if (currentProfilePath_ == cubePath) {
                exportShader_.setLut(profileLut_);
                exportShader_.setLutBlend(1.0f);
            } else {
                exportShader_.clearLut();
            }
        } else {
            exportShader_.clearLut();
        }

        // 6. GPU render offscreen
        exportShader_.renderOffscreen(srcW, srcH);

        // 7. Metal readback (U8 RGBA)
        Pixels fboPixels;
        bool readOk = PhotoExporter::readFboPixels(
            exportShader_.getFboImage(), srcW, srcH, fboPixels);

        // 8. Cleanup GPU resources (source texture destroyed when srcTex goes out of scope)
        exportShader_.clearSource();

        if (!readOk) {
            logError() << "[ExportQueue] readFboPixels failed: " << job.photoId;
            ExportResult fail;
            fail.photoId = job.photoId;
            fail.outPath = job.outPath;
            fail.type = job.type;
            fail.success = false;
            resultQueue_.send(std::move(fail));
            return;
        }

        // 9. Send to saver thread
        ExportSaverJob saverJob;
        saverJob.job = std::move(job);
        saverJob.fboPixels = std::move(fboPixels);
        saverJob.fboW = srcW;
        saverJob.fboH = srcH;
        saverQueue_.send(std::move(saverJob));
    }

    // ---- Saver thread (background) ----
    // CPU crop/rotation/perspective transform + resize + JPEG encode
    void saverFunc() {
        while (running_) {
            ExportSaverJob saverJob;
            if (!saverQueue_.tryReceive(saverJob, 200)) continue;

            auto& job = saverJob.job;
            auto& entry = job.entry;
            auto& fboPixels = saverJob.fboPixels;
            int srcW = saverJob.fboW;
            int srcH = saverJob.fboH;

            // Determine crop output size
            auto [outW, outH] = entry.getCropOutputSize(srcW, srcH);

            // Choose transform path
            Pixels transformed;
            Pixels* srcPtr = &fboPixels;

            bool hasCropOrRot = entry.hasCrop() || entry.hasRotation();
            if (entry.hasPerspective()) {
                PhotoExporter::transformPerspU8(fboPixels, transformed, entry, outW, outH);
                srcPtr = &transformed;
            } else if (hasCropOrRot) {
                auto quad = entry.getCropQuad(srcW, srcH);
                bool isIdentity = (outW == srcW && outH == srcH &&
                                   quad[0] == 0 && quad[1] == 0 &&
                                   quad[2] == 1 && quad[3] == 0 &&
                                   quad[4] == 1 && quad[5] == 1 &&
                                   quad[6] == 0 && quad[7] == 1);
                if (!isIdentity) {
                    PhotoExporter::transformU8(fboPixels, transformed, quad.data(), outW, outH);
                    srcPtr = &transformed;
                }
            }

            // Resize if needed
            Pixels resized;
            Pixels* outPtr = srcPtr;
            if (job.settings.maxEdge > 0) {
                int w = outPtr->getWidth(), h = outPtr->getHeight();
                int longEdge = max(w, h);
                if (longEdge > job.settings.maxEdge) {
                    float scale = (float)job.settings.maxEdge / longEdge;
                    int newW = max(1, (int)round(w * scale));
                    int newH = max(1, (int)round(h * scale));
                    PhotoExporter::resizeU8(*outPtr, resized, newW, newH);
                    outPtr = &resized;
                }
            }

            // Create output directory + save JPEG
            fs::create_directories(fs::path(job.outPath).parent_path());

            int w = outPtr->getWidth(), h = outPtr->getHeight();
            int ch = outPtr->getChannels();
            bool ok = false;
            if (ch == 4) {
                // Strip alpha: RGBA -> RGB
                vector<unsigned char> rgb(w * h * 3);
                auto* src = outPtr->getData();
                for (int i = 0; i < w * h; i++) {
                    rgb[i*3+0] = src[i*4+0];
                    rgb[i*3+1] = src[i*4+1];
                    rgb[i*3+2] = src[i*4+2];
                }
                ok = stbi_write_jpg(job.outPath.c_str(), w, h, 3,
                                    rgb.data(), job.settings.quality) != 0;
            } else {
                ok = stbi_write_jpg(job.outPath.c_str(), w, h, ch,
                                    outPtr->getData(), job.settings.quality) != 0;
            }

            if (ok) {
                logNotice() << "[ExportQueue] Saved: " << job.outPath;
            } else {
                logError() << "[ExportQueue] Failed to save: " << job.outPath;
            }

            ExportResult result;
            result.photoId = job.photoId;
            result.outPath = job.outPath;
            result.type = job.type;
            result.success = ok;
            resultQueue_.send(std::move(result));
        }
    }
};
