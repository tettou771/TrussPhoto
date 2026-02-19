#pragma once

// =============================================================================
// SingleView.h - Full-size image viewer with RAW loading, GPU develop shader
// =============================================================================
// Pipeline: RAW → LibRaw → [CPU] NR → GPU upload (uncropped) →
//           develop shader (lens + crop + LUT) → display
// =============================================================================

#include <TrussC.h>
#include <tcLut.h>
#include <tcxLibRaw.h>
#include "ViewContainer.h"
#include "PhotoProvider.h"
#include "PhotoGrid.h"
#include "MetadataPanel.h"
#include "CameraProfileManager.h"
#include "LensCorrector.h"
#include "DevelopShader.h"
#include "GuidedFilter.h"
#include <atomic>
#include <thread>
#include <mutex>

using namespace std;
using namespace tc;

class SingleView : public ViewContainer {
public:
    using Ptr = shared_ptr<SingleView>;

    void beginView(ViewContext& ctx) override {
        ctx_ = &ctx;
    }

    void endView() override {
        cleanupState();
        ctx_ = nullptr;
    }

    bool wantsSearchBar() const override { return false; }
    bool wantsLeftSidebar() const override { return false; }

    // Callback when photo changes (for DevelopPanel slider sync)
    function<void(float chroma, float luma)> onDenoiseRestored;

    void setup() override {
        enableEvents();
        setClipping(true);
    }

    // Initialize GPU resources (call once from tcApp::setup after addChild)
    void init(const string& profileDir) {
        profileManager_.setProfileDir(profileDir);
        developShader_.load();
    }

    // Check if a profile exists for a given camera/style combo
    bool hasProfileFor(const string& camera, const string& style) const {
        return !profileManager_.findProfile(camera, style).empty();
    }

    // Open a specific photo by grid index
    void show(int index) {
        if (!ctx_) return;
        auto& grid = ctx_->grid;
        auto& provider = *ctx_->provider;

        if (index < 0 || index >= (int)grid->getPhotoIdCount()) return;

        const string& photoId = grid->getPhotoId(index);
        auto* entry = provider.getPhoto(photoId);
        if (!entry) return;

        logNotice() << "Opening: " << entry->filename;

        // Clean up previous state
        cleanupState();

        // Restore NR settings from entry
        chromaDenoise_ = entry->chromaDenoise;
        lumaDenoise_ = entry->lumaDenoise;
        if (onDenoiseRestored) onDenoiseRestored(chromaDenoise_, lumaDenoise_);

        bool loaded = false;
        isSmartPreview_ = false;

        // Video playback
        if (entry->isVideo) {
            if (!entry->localPath.empty() && fs::exists(entry->localPath)) {
                if (videoPlayer_.load(entry->localPath)) {
                    isVideo_ = true;
                    videoPlayer_.play();
                    loaded = true;
                    if (ctx_->redraw) ctx_->redraw(1);
                }
            }
        } else if (!entry->localPath.empty() && fs::exists(entry->localPath)) {
            if (entry->isRaw) {
                // Step 1: Try embedded JPEG first (fastest)
                Pixels previewPixels;
                bool hasPreview = RawLoader::loadEmbeddedPreview(entry->localPath, previewPixels);
                if (!hasPreview) {
                    hasPreview = RawLoader::loadFloatPreview(entry->localPath, previewPixels);
                }

                if (hasPreview) {
                    previewTexture_.allocate(previewPixels, TextureUsage::Immutable, true);
                    intermediateTexture_.clear();
                    rawPixels_.clear();
                    isRawImage_ = true;
                    loaded = true;

                    // Step 2: Start full-size load in background
                    string path = entry->localPath;

                    rawLoadInProgress_ = true;
                    rawLoadCompleted_ = false;
                    rawLoadTargetIndex_ = index;
                    lensCorrector_.reset();

                    if (rawLoadThread_.joinable()) rawLoadThread_.join();
                    rawLoadThread_ = thread([this, index, path]() {
                        Pixels loadedPixels;
                        if (RawLoader::loadFloat(path, loadedPixels)) {
                            lock_guard<mutex> lock(rawLoadMutex_);
                            pendingRawPixels_ = std::move(loadedPixels);
                            int pw = pendingRawPixels_.getWidth();
                            int ph = pendingRawPixels_.getHeight();
                            lensCorrector_.setupFromExif(path, pw, ph);
                            rawLoadCompleted_ = true;
                        }
                        rawLoadInProgress_ = false;
                    });
                }
            } else {
                if (fullImage_.load(entry->localPath)) {
                    previewTexture_.clear();
                    isRawImage_ = false;
                    loaded = true;
                }
            }
            if (ctx_->redraw) ctx_->redraw(1);
        }

        // Fallback: try smart preview
        if (!loaded && provider.hasSmartPreview(photoId)) {
            Pixels spPixels;
            if (provider.loadSmartPreview(photoId, spPixels)) {
                rawPixels_ = std::move(spPixels);
                if (!entry->lensCorrectionParams.empty()) {
                    lensCorrector_.setupFromJson(entry->lensCorrectionParams,
                        rawPixels_.getWidth(), rawPixels_.getHeight());
                }
                setupIntermediateFromRaw();
                previewTexture_.clear();
                isRawImage_ = true;
                isSmartPreview_ = true;
                loaded = true;
                logNotice() << "Loaded smart preview for: " << entry->filename;
                if (ctx_->redraw) ctx_->redraw(1);
            }
        }

        if (loaded) {
            selectedIndex_ = index;
            zoomLevel_ = 1.0f;
            panOffset_ = {0, 0};
            loadProfileForEntry(*entry);

            // Update metadata panel
            if (ctx_->metadataPanel) {
                ctx_->metadataPanel->clearThumbnail();
                ctx_->metadataPanel->setPhoto(entry);
                ctx_->metadataPanel->setStyleProfileStatus(!profileManager_.findProfile(
                    entry->camera, entry->creativeStyle).empty());
                ctx_->metadataPanel->setViewInfo({
                    .zoom = zoomLevel_,
                    .profileEnabled = profileEnabled_,
                    .profileBlend = profileBlend_,
                    .hasProfile = hasProfileLut_,
                    .lensEnabled = lensEnabled_,
                    .hasLensData = lensCorrector_.isReady(),
                    .isSmartPreview = isSmartPreview_,
                    .lensSource = lensCorrector_.correctionSource(),
                });
            }
        } else {
            logWarning() << "Failed to load: " << entry->localPath;
        }
    }

    // Process video frame updates (call from update)
    void processVideoUpdate() {
        if (!isVideo_ || !videoPlayer_.isLoaded()) return;
        videoPlayer_.update();
        if (videoPlayer_.isFrameNew()) {
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        }
    }

    // Process background RAW load completion (call from update)
    void processRawLoadCompletion() {
        if (!ctx_ || !rawLoadCompleted_ || !isRawImage_) return;

        if (rawLoadTargetIndex_ == selectedIndex_) {
            lock_guard<mutex> lock(rawLoadMutex_);
            if (pendingRawPixels_.isAllocated()) {
                rawPixels_ = std::move(pendingRawPixels_);

                // Apply NR then upload uncropped intermediate
                setupIntermediateFromRaw();
                previewTexture_.clear();

                logNotice() << "Full-size RAW loaded: "
                            << rawPixels_.getWidth() << "x" << rawPixels_.getHeight()
                            << " display=" << displayW_ << "x" << displayH_;

                const string& spId = ctx_->grid->getPhotoId(selectedIndex_);

                // Write intermediate dimensions + crop coords to DB
                if (lensCorrector_.isReady()) {
                    auto* entry2 = ctx_->provider->getPhoto(spId);
                    if (entry2 && !entry2->lensCorrectionParams.empty()) {
                        try {
                            auto j = nlohmann::json::parse(entry2->lensCorrectionParams);
                            if (!j.contains("intW")) {
                                int intW = lensCorrector_.intermediateWidth();
                                int intH = lensCorrector_.intermediateHeight();
                                if (intW == 0) {
                                    intW = rawPixels_.getWidth();
                                    intH = rawPixels_.getHeight();
                                }
                                j["intW"] = intW;
                                j["intH"] = intH;
                                if (lensCorrector_.hasDefaultCrop()) {
                                    j["cropX"] = lensCorrector_.cropX();
                                    j["cropY"] = lensCorrector_.cropY();
                                    j["cropW"] = lensCorrector_.cropW();
                                    j["cropH"] = lensCorrector_.cropH();
                                }
                                ctx_->provider->updateLensCorrectionParams(spId, j.dump());
                            }
                        } catch (...) {}
                    }
                }

                // Generate smart preview (CPU lens correction, background)
                if (!ctx_->provider->hasSmartPreview(spId)) {
                    ctx_->provider->generateSmartPreview(spId, rawPixels_);
                }

                updateViewInfo();
                if (ctx_->redraw) ctx_->redraw(1);
            }
        } else {
            lock_guard<mutex> lock(rawLoadMutex_);
            pendingRawPixels_.clear();
        }
        rawLoadCompleted_ = false;
    }

    // Render develop shader to offscreen FBO (call from tcApp::draw() before Node tree)
    // Uses suspend/resumeSwapchainPass internally; safe to call mid-frame.
    void renderDevelopFbo() {
        if (isVideo_) return;
        if (!isRawImage_ || !intermediateTexture_.isAllocated()) return;
        if (!needsFboRender_) return;

        developShader_.renderOffscreen(displayW_, displayH_);
        needsFboRender_ = false;
    }

    // Draw the image (called by Node tree with clipping + local transform)
    void draw() override {
        if (isVideo_) {
            drawVideoView();
            return;
        }

        bool hasFbo = isRawImage_ && developShader_.isFboReady();
        bool hasPreviewRaw = isRawImage_ && previewTexture_.isAllocated();
        bool hasImage = hasFbo || hasPreviewRaw || fullImage_.isAllocated();
        if (!hasImage) return;

        float imgW, imgH;
        if (hasFbo) {
            imgW = (float)displayW_;
            imgH = (float)displayH_;
        } else if (hasPreviewRaw) {
            imgW = previewTexture_.getWidth();
            imgH = previewTexture_.getHeight();
        } else {
            imgW = fullImage_.getWidth();
            imgH = fullImage_.getHeight();
        }

        auto [x, y, drawW, drawH] = calcDrawRect(imgW, imgH);

        setColor(1.0f, 1.0f, 1.0f);
        if (hasFbo) {
            // Draw FBO result texture via sgl (10-bit RGB10A2)
            drawTextureView(developShader_.getFboView(), developShader_.getFboSampler(),
                            x, y, drawW, drawH);
        } else if (hasPreviewRaw) {
            previewTexture_.draw(x, y, drawW, drawH);
        } else {
            fullImage_.draw(x, y, drawW, drawH);
        }
    }

    // Handle key input
    bool handleKey(int key) {
        if (!ctx_) return false;
        auto& grid = ctx_->grid;
        auto& provider = *ctx_->provider;

        // Video-specific keys
        if (isVideo_) {
            if (key == SAPP_KEYCODE_SPACE) {
                videoPlayer_.togglePause();
                return true;
            }
            if (key == SAPP_KEYCODE_LEFT) {
                float t = videoPlayer_.getCurrentTime() - 5.0f;
                videoPlayer_.setCurrentTime(max(0.0f, t));
                return true;
            }
            if (key == SAPP_KEYCODE_RIGHT) {
                float t = videoPlayer_.getCurrentTime() + 5.0f;
                videoPlayer_.setCurrentTime(min(t, videoPlayer_.getDuration()));
                return true;
            }
            return false;
        }

        if (key == SAPP_KEYCODE_LEFT && selectedIndex_ > 0) {
            show(selectedIndex_ - 1);
            return true;
        }
        if (key == SAPP_KEYCODE_RIGHT && selectedIndex_ < (int)grid->getPhotoIdCount() - 1) {
            show(selectedIndex_ + 1);
            return true;
        }
        if (key == 'P' || key == 'p') {
            if (hasProfileLut_) {
                profileEnabled_ = !profileEnabled_;
                developShader_.setLutBlend(profileEnabled_ ? profileBlend_ : 0.0f);
                needsFboRender_ = true;
                logNotice() << "[Profile] " << (profileEnabled_ ? "ON" : "OFF");
            }
            return true;
        }
        if (key == SAPP_KEYCODE_LEFT_BRACKET) {
            if (hasProfileLut_) {
                profileBlend_ = clamp(profileBlend_ - 0.1f, 0.0f, 1.0f);
                developShader_.setLutBlend(profileEnabled_ ? profileBlend_ : 0.0f);
                needsFboRender_ = true;
                logNotice() << "[Profile] Blend: " << (int)(profileBlend_ * 100) << "%";
            }
            return true;
        }
        if (key == SAPP_KEYCODE_RIGHT_BRACKET) {
            if (hasProfileLut_) {
                profileBlend_ = clamp(profileBlend_ + 0.1f, 0.0f, 1.0f);
                developShader_.setLutBlend(profileEnabled_ ? profileBlend_ : 0.0f);
                needsFboRender_ = true;
                logNotice() << "[Profile] Blend: " << (int)(profileBlend_ * 100) << "%";
            }
            return true;
        }
        if (key >= '0' && key <= '5') {
            if (selectedIndex_ >= 0 && selectedIndex_ < (int)grid->getPhotoIdCount()) {
                const string& photoId = grid->getPhotoId(selectedIndex_);
                int rating = key - '0';
                provider.setRating(photoId, rating);
                logNotice() << "[Rating] " << photoId << " -> " << rating;
            }
            return true;
        }
        if (key == 'Z' || key == 'z') {
            zoomLevel_ = 1.0f;
            panOffset_ = {0, 0};
            return true;
        }
        if (key == 'S' || key == 's') {
            // Debug: force load smart preview
            if (selectedIndex_ >= 0 && selectedIndex_ < (int)grid->getPhotoIdCount()) {
                const string& photoId = grid->getPhotoId(selectedIndex_);
                auto* spEntry = provider.getPhoto(photoId);
                Pixels spPixels;
                if (spEntry && provider.loadSmartPreview(photoId, spPixels)) {
                    rawPixels_ = std::move(spPixels);
                    if (!spEntry->lensCorrectionParams.empty()) {
                        lensCorrector_.setupFromJson(spEntry->lensCorrectionParams,
                            rawPixels_.getWidth(), rawPixels_.getHeight());
                    }
                    setupIntermediateFromRaw();
                    previewTexture_.clear();
                    isSmartPreview_ = true;
                    logNotice() << "[Debug] Forced smart preview: " << photoId;
                } else {
                    logNotice() << "[Debug] No smart preview for: " << photoId;
                }
            }
            return true;
        }
        if (key == 'L' || key == 'l') {
            lensEnabled_ = !lensEnabled_;
            logNotice() << "[LensCorrection] " << (lensEnabled_ ? "ON" : "OFF")
                        << " (" << lensCorrector_.correctionSource() << ")";
            // GPU uniform change only — instant!
            developShader_.setLensEnabled(lensEnabled_);
            needsFboRender_ = true;
            updateDisplayDimensions();
            return true;
        }

        return false;
    }

    // Called when DevelopPanel NR sliders change
    void onDenoiseChanged(float chroma, float luma) {
        chromaDenoise_ = chroma;
        lumaDenoise_ = luma;

        if (!isRawImage_ || !rawPixels_.isAllocated()) return;

        // Re-apply NR to raw pixels and re-upload
        nrPixels_ = rawPixels_.clone();
        if (chromaDenoise_ > 0 || lumaDenoise_ > 0) {
            tp::guidedDenoise(nrPixels_, chromaDenoise_, lumaDenoise_);
        }
        intermediateTexture_.allocate(nrPixels_, TextureUsage::Immutable, true);
        developShader_.setSourceTexture(intermediateTexture_);
        needsFboRender_ = true;

        // Save to DB
        if (ctx_ && selectedIndex_ >= 0 && selectedIndex_ < (int)ctx_->grid->getPhotoIdCount()) {
            const string& pid = ctx_->grid->getPhotoId(selectedIndex_);
            ctx_->provider->setDenoise(pid, chroma, luma);
        }

        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

protected:
    // Node event handlers (dispatched by the Node tree, not tcApp)
    bool onMousePress(Vec2 pos, int button) override {
        if (button == 0) {
            if (isVideo_) {
                float barY = getHeight() - seekBarHeight_;
                if (pos.y >= barY) {
                    seekDragging_ = true;
                    float pct = clamp(pos.x / getWidth(), 0.0f, 1.0f);
                    videoPlayer_.setPosition(pct);
                    return true;
                }
                return false;
            }
            isDragging_ = true;
            dragStart_ = pos;
            return true;
        }
        return false;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 0) {
            if (seekDragging_) { seekDragging_ = false; return true; }
            isDragging_ = false;
            return true;
        }
        return false;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (button == 0) {
            if (seekDragging_ && isVideo_) {
                float pct = clamp(pos.x / getWidth(), 0.0f, 1.0f);
                videoPlayer_.setPosition(pct);
                if (ctx_ && ctx_->redraw) ctx_->redraw(1);
                return true;
            }
            if (isDragging_ && !isVideo_) {
                Vec2 delta = pos - dragStart_;
                panOffset_ = panOffset_ + delta;
                dragStart_ = pos;
                if (ctx_ && ctx_->redraw) ctx_->redraw(1);
                return true;
            }
        }
        return false;
    }

    bool onMouseScroll(Vec2 pos, Vec2 scroll) override {
        if (isVideo_) return false;

        bool hasIntermediate = isRawImage_ && intermediateTexture_.isAllocated();
        bool hasPreviewRaw = isRawImage_ && previewTexture_.isAllocated();
        bool hasImage = isRawImage_ ? (hasIntermediate || hasPreviewRaw) : fullImage_.isAllocated();
        if (!hasImage) return false;

        float oldZoom = zoomLevel_;
        zoomLevel_ *= (1.0f + scroll.y * 0.1f);
        zoomLevel_ = clamp(zoomLevel_, 1.0f, 10.0f);

        // pos is already in local coords
        Vec2 windowCenter(getWidth() / 2.0f, getHeight() / 2.0f);
        Vec2 imageCenter = windowCenter + panOffset_;
        Vec2 toMouse = pos - imageCenter;

        float zoomRatio = zoomLevel_ / oldZoom;
        panOffset_ = panOffset_ - toMouse * (zoomRatio - 1.0f);

        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
        return true;
    }

public:

    // Accessors
    int selectedIndex() const { return selectedIndex_; }
    float zoom() const { return zoomLevel_; }
    bool profileEnabled() const { return profileEnabled_; }
    float profileBlend() const { return profileBlend_; }
    bool hasProfileLut() const { return hasProfileLut_; }
    bool lensEnabled() const { return lensEnabled_; }
    bool isSmartPreview() const { return isSmartPreview_; }
    bool isRawImage() const { return isRawImage_; }
    bool isVideo() const { return isVideo_; }
    float chromaDenoise() const { return chromaDenoise_; }
    float lumaDenoise() const { return lumaDenoise_; }

    void updateViewInfo() {
        if (ctx_ && ctx_->metadataPanel) {
            ctx_->metadataPanel->setViewInfo({
                .zoom = zoomLevel_,
                .profileEnabled = profileEnabled_,
                .profileBlend = profileBlend_,
                .hasProfile = hasProfileLut_,
                .lensEnabled = lensEnabled_,
                .hasLensData = lensCorrector_.isReady(),
                .isSmartPreview = isSmartPreview_,
                .lensSource = lensCorrector_.correctionSource(),
            });
        }
    }

    void updateMetadata() {
        if (!ctx_ || selectedIndex_ < 0) return;
        if (selectedIndex_ >= (int)ctx_->grid->getPhotoIdCount()) return;
        const string& pid = ctx_->grid->getPhotoId(selectedIndex_);
        auto* e = ctx_->provider->getPhoto(pid);
        if (e && ctx_->metadataPanel) {
            ctx_->metadataPanel->setPhoto(e);
            ctx_->metadataPanel->setStyleProfileStatus(!profileManager_.findProfile(
                e->camera, e->creativeStyle).empty());
        }
        updateViewInfo();
    }

    string currentPhotoId() const {
        if (!ctx_ || selectedIndex_ < 0) return "";
        if (selectedIndex_ >= (int)ctx_->grid->getPhotoIdCount()) return "";
        return ctx_->grid->getPhotoId(selectedIndex_);
    }

    bool hasEmbedding() const {
        if (!ctx_) return false;
        string id = currentPhotoId();
        return !id.empty() && ctx_->provider->getCachedEmbedding(id);
    }

    void joinRawLoadThread() {
        if (rawLoadThread_.joinable()) rawLoadThread_.join();
    }

private:
    ViewContext* ctx_ = nullptr;

    // Image state
    int selectedIndex_ = -1;
    Image fullImage_;
    Pixels rawPixels_;
    Pixels nrPixels_;            // NR result cache
    Texture intermediateTexture_; // Full uncropped intermediate (NR'd)
    Texture previewTexture_;
    bool isRawImage_ = false;
    bool isSmartPreview_ = false;
    bool needsFboRender_ = false;

    // Display dimensions (after crop, for fit-to-window calculation)
    int displayW_ = 0, displayH_ = 0;

    // Pan/zoom
    Vec2 panOffset_ = {0, 0};
    float zoomLevel_ = 1.0f;
    bool isDragging_ = false;
    Vec2 dragStart_;

    // Background RAW loading
    thread rawLoadThread_;
    atomic<bool> rawLoadInProgress_{false};
    atomic<bool> rawLoadCompleted_{false};
    atomic<int> rawLoadTargetIndex_{-1};
    Pixels pendingRawPixels_;
    mutex rawLoadMutex_;

    // Camera profile (LUT)
    CameraProfileManager profileManager_;
    lut::Lut3D profileLut_;
    bool hasProfileLut_ = false;
    bool profileEnabled_ = true;
    float profileBlend_ = 1.0f;
    string currentProfilePath_;

    // Unified develop shader (replaces lutShader_)
    DevelopShader developShader_;

    // Lens correction
    LensCorrector lensCorrector_;
    bool lensEnabled_ = true;

    // Noise reduction
    float chromaDenoise_ = 0.5f;
    float lumaDenoise_ = 0.0f;

    // Video playback
    VideoPlayer videoPlayer_;
    bool isVideo_ = false;
    bool seekDragging_ = false;
    float seekBarHeight_ = 40.0f;

    // Draw a texture by view+sampler via sgl (for FBO result)
    void drawTextureView(sg_view view, sg_sampler sampler,
                         float x, float y, float w, float h) {
        sgl_enable_texture();
        sgl_texture(view, sampler);
        Color col = getDefaultContext().getColor();
        sgl_begin_quads();
        sgl_c4f(col.r, col.g, col.b, col.a);
        sgl_v2f_t2f(x, y, 0, 0);
        sgl_v2f_t2f(x + w, y, 1, 0);
        sgl_v2f_t2f(x + w, y + h, 1, 1);
        sgl_v2f_t2f(x, y + h, 0, 1);
        sgl_end();
        sgl_disable_texture();
    }

    // Calculate draw rect (local coords) with pan clamping
    struct DrawRect { float x, y, w, h; };
    DrawRect calcDrawRect(float imgW, float imgH) {
        float winW = getWidth();
        float winH = getHeight();
        float fitScale = min(winW / imgW, winH / imgH);
        float scale = fitScale * zoomLevel_;
        float drawW = imgW * scale;
        float drawH = imgH * scale;

        if (drawW <= winW) panOffset_.x = 0;
        else {
            float maxPanX = (drawW - winW) / 2;
            panOffset_.x = clamp(panOffset_.x, -maxPanX, maxPanX);
        }
        if (drawH <= winH) panOffset_.y = 0;
        else {
            float maxPanY = (drawH - winH) / 2;
            panOffset_.y = clamp(panOffset_.y, -maxPanY, maxPanY);
        }

        float x = (winW - drawW) / 2 + panOffset_.x;
        float y = (winH - drawH) / 2 + panOffset_.y;
        return {x, y, drawW, drawH};
    }

    void cleanupState() {
        if (isVideo_) {
            videoPlayer_.close();
            isVideo_ = false;
            seekDragging_ = false;
        }

        if (rawLoadThread_.joinable()) rawLoadThread_.join();
        rawLoadInProgress_ = false;
        rawLoadCompleted_ = false;

        if (isRawImage_) {
            rawPixels_.clear();
            nrPixels_.clear();
            intermediateTexture_.clear();
            previewTexture_.clear();
            { lock_guard<mutex> lock(rawLoadMutex_); pendingRawPixels_.clear(); }
        } else {
            fullImage_ = Image();
        }
        isRawImage_ = false;
        isSmartPreview_ = false;
        selectedIndex_ = -1;
        displayW_ = displayH_ = 0;

        hasProfileLut_ = false;
        profileLut_.clear();
        currentProfilePath_.clear();
        developShader_.clearLut();
        developShader_.clearLensData();
    }

    // Apply NR to rawPixels_, upload as intermediate texture, setup develop shader
    void setupIntermediateFromRaw() {
        int srcW = rawPixels_.getWidth();
        int srcH = rawPixels_.getHeight();

        // Apply noise reduction
        nrPixels_ = rawPixels_.clone();
        if (chromaDenoise_ > 0 || lumaDenoise_ > 0) {
            tp::guidedDenoise(nrPixels_, chromaDenoise_, lumaDenoise_);
        }

        // Upload full uncropped intermediate
        intermediateTexture_.allocate(nrPixels_, TextureUsage::Immutable, true);
        developShader_.setSourceTexture(intermediateTexture_);

        // Setup lens correction data for GPU
        if (lensCorrector_.isReady()) {
            // Distortion + TCA LUT (Sony/Fuji path)
            auto distLut = lensCorrector_.generateDistortionLUT();
            developShader_.updateLensLUT(distLut.data(), 512);

            // Vignetting map
            int vigRows, vigCols;
            auto vigMap = lensCorrector_.generateVignettingMap(vigRows, vigCols);
            developShader_.updateVigMap(vigMap.data(), vigRows, vigCols);
        }

        // Setup uniform params
        setupDevelopShaderParams(srcW, srcH);

        // Setup LUT
        if (hasProfileLut_) {
            developShader_.setLut(profileLut_);
            developShader_.setLutBlend(profileEnabled_ ? profileBlend_ : 0.0f);
        }

        needsFboRender_ = true;
    }

    void setupDevelopShaderParams(int srcW, int srcH) {
        float cropRect[4];
        float optCenter[2];

        lensCorrector_.getGpuCropRect(srcW, srcH, cropRect);
        lensCorrector_.getGpuOpticalCenter(srcW, srcH, optCenter);
        float invDiag = lensCorrector_.getGpuInvDiag(srcW, srcH);
        float autoScale = lensCorrector_.isReady()
            ? lensCorrector_.getGpuAutoScale(srcW, srcH) : 1.0f;

        developShader_.setLensParams(
            lensEnabled_ && lensCorrector_.isReady(),
            autoScale,
            cropRect[0], cropRect[1], cropRect[2], cropRect[3],
            optCenter[0], optCenter[1], invDiag,
            (float)srcW, (float)srcH
        );

        updateDisplayDimensions();
    }

    void updateDisplayDimensions() {
        if (!intermediateTexture_.isAllocated()) return;
        int srcW = intermediateTexture_.getWidth();
        int srcH = intermediateTexture_.getHeight();

        if (lensEnabled_ && lensCorrector_.hasDefaultCrop()) {
            float cropRect[4];
            lensCorrector_.getGpuCropRect(srcW, srcH, cropRect);
            displayW_ = max(1, (int)round(cropRect[2] * srcW));
            displayH_ = max(1, (int)round(cropRect[3] * srcH));
        } else if (lensCorrector_.hasDefaultCrop()) {
            // Lens disabled but has crop data → still crop
            float cropRect[4];
            lensCorrector_.getGpuCropRect(srcW, srcH, cropRect);
            displayW_ = max(1, (int)round(cropRect[2] * srcW));
            displayH_ = max(1, (int)round(cropRect[3] * srcH));
        } else {
            displayW_ = srcW;
            displayH_ = srcH;
        }
    }

    void drawVideoView() {
        if (!videoPlayer_.isLoaded()) return;

        auto& tex = videoPlayer_.getTexture();
        float imgW = videoPlayer_.getWidth();
        float imgH = videoPlayer_.getHeight();
        float winW = getWidth();
        float winH = getHeight() - seekBarHeight_;

        float fitScale = min(winW / imgW, winH / imgH);
        float drawW = imgW * fitScale;
        float drawH = imgH * fitScale;
        float x = (winW - drawW) / 2;
        float y = (winH - drawH) / 2;

        setColor(1.0f, 1.0f, 1.0f);
        tex.draw(x, y, drawW, drawH);

        // Seek bar
        float barY = getHeight() - seekBarHeight_;
        float pos = videoPlayer_.getPosition();
        float dur = videoPlayer_.getDuration();
        float cur = pos * dur;

        setColor(0, 0, 0, 0.6f);
        fill();
        drawRect(0, barY, getWidth(), seekBarHeight_);

        float iconX = 20;
        float iconY = barY + seekBarHeight_ / 2;
        setColor(1, 1, 1, 0.9f);
        if (videoPlayer_.isPlaying()) {
            fill();
            drawRect(iconX - 4, iconY - 8, 4, 16);
            drawRect(iconX + 4, iconY - 8, 4, 16);
        } else {
            fill();
            drawTriangle(iconX - 4, iconY - 8,
                         iconX - 4, iconY + 8,
                         iconX + 8, iconY);
        }

        float barX = 44;
        float barW = getWidth() - barX - 100;
        float barMidY = barY + seekBarHeight_ / 2;

        setColor(0.3f, 0.3f, 0.35f);
        fill();
        drawRect(barX, barMidY - 2, barW, 4);

        setColor(0.5f, 0.7f, 1.0f);
        drawRect(barX, barMidY - 2, barW * pos, 4);
        drawCircle(barX + barW * pos, barMidY, 6);

        setColor(0.8f, 0.8f, 0.85f);
        string timeStr = formatTime(cur) + " / " + formatTime(dur);
        pushStyle();
        setTextAlign(Direction::Right, Direction::Center);
        drawBitmapString(timeStr, getWidth() - 10, barMidY);
        popStyle();
    }

    static string formatTime(float seconds) {
        int s = (int)seconds;
        int m = s / 60;
        s = s % 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", m, s);
        return buf;
    }

    void loadProfileForEntry(const PhotoEntry& entry) {
        string cubePath = profileManager_.findProfile(entry.camera, entry.creativeStyle);
        if (cubePath.empty() || cubePath == currentProfilePath_) {
            if (cubePath.empty()) {
                hasProfileLut_ = false;
                currentProfilePath_.clear();
                developShader_.clearLut();
            }
            return;
        }

        if (profileLut_.load(cubePath)) {
            hasProfileLut_ = true;
            currentProfilePath_ = cubePath;
            developShader_.setLut(profileLut_);
            developShader_.setLutBlend(profileEnabled_ ? profileBlend_ : 0.0f);
            needsFboRender_ = true;
            logNotice() << "[Profile] Loaded: " << cubePath;
        } else {
            hasProfileLut_ = false;
            currentProfilePath_.clear();
            developShader_.clearLut();
            needsFboRender_ = true;
            logWarning() << "[Profile] Failed to load: " << cubePath;
        }
    }
};
