#pragma once

// =============================================================================
// SingleView.h - Full-size image viewer with RAW loading, LUT, lens correction
// Extracted from tcApp.cpp's drawSingleView() + related state.
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

    // Initialize GPU resources (call once from tcApp::setup after addChild)
    void init(const string& profileDir) {
        profileManager_.setProfileDir(profileDir);
        lutShader_.load();
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
                    fullTexture_.clear();
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
                            // LibRaw outputs the zero-cropped intermediate image
                            // (e.g., 7041x4689). Lens correction uses this size as
                            // the reference frame. DefaultCrop is applied AFTER
                            // correction in processRawLoadCompletion/reprocessImage.
                            lock_guard<mutex> lock(rawLoadMutex_);
                            pendingRawPixels_ = std::move(loadedPixels);
                            int pw = pendingRawPixels_.getWidth();
                            int ph = pendingRawPixels_.getHeight();
                            // EXIF embedded correction (Sony ARW + DNG OpcodeList)
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
                reprocessImage();
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
                fullPixels_ = rawPixels_.clone();
                if (lensEnabled_ && lensCorrector_.isReady()) {
                    // apply() includes distortion + crop + auto-scale in one pass
                    lensCorrector_.apply(fullPixels_);
                } else {
                    // Lens disabled: just crop intermediate → EXIF declared size
                    lensCorrector_.applyDefaultCrop(fullPixels_);
                }
                fullTexture_.allocate(fullPixels_, TextureUsage::Immutable, true);
                previewTexture_.clear();
                logNotice() << "Full-size RAW loaded: " << rawPixels_.getWidth() << "x" << rawPixels_.getHeight()
                            << " → " << fullPixels_.getWidth() << "x" << fullPixels_.getHeight();

                const string& spId = ctx_->grid->getPhotoId(selectedIndex_);

                // Write intermediate dimensions + rotation-adjusted crop coords to DB.
                // setupFromExif has transformed crop for portrait orientation;
                // writing back ensures setupFromJson gets correct coords for SP display.
                if (lensCorrector_.isReady()) {
                    auto* entry2 = ctx_->provider->getPhoto(spId);
                    if (entry2 && !entry2->lensCorrectionParams.empty()) {
                        try {
                            auto j = nlohmann::json::parse(entry2->lensCorrectionParams);
                            if (!j.contains("intW")) {
                                // Use lensCorrector's intW if set (Sony path),
                                // otherwise use raw pixel dimensions (DNG path)
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

                // Generate smart preview if not yet done
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

    // Draw the image (called from tcApp::draw or node draw)
    void drawView() {
        // Video playback
        if (isVideo_) {
            drawVideoView();
            return;
        }

        bool hasFullRaw = isRawImage_ && fullTexture_.isAllocated();
        bool hasPreviewRaw = isRawImage_ && previewTexture_.isAllocated();
        bool hasImage = isRawImage_ ? (hasFullRaw || hasPreviewRaw) : fullImage_.isAllocated();
        if (!hasImage) return;

        Texture* rawTex = hasFullRaw ? &fullTexture_ : &previewTexture_;
        float imgW = isRawImage_ ? rawTex->getWidth() : fullImage_.getWidth();
        float imgH = isRawImage_ ? rawTex->getHeight() : fullImage_.getHeight();
        float winW = getWidth();
        float winH = getHeight();

        float fitScale = min(winW / imgW, winH / imgH);
        float scale = fitScale * zoomLevel_;

        float drawW = imgW * scale;
        float drawH = imgH * scale;

        // Clamp pan offset
        if (drawW <= winW) {
            panOffset_.x = 0;
        } else {
            float maxPanX = (drawW - winW) / 2;
            panOffset_.x = clamp(panOffset_.x, -maxPanX, maxPanX);
        }

        if (drawH <= winH) {
            panOffset_.y = 0;
        } else {
            float maxPanY = (drawH - winH) / 2;
            panOffset_.y = clamp(panOffset_.y, -maxPanY, maxPanY);
        }

        float x = (winW - drawW) / 2 + panOffset_.x;
        float y = (winH - drawH) / 2 + panOffset_.y;

        setColor(1.0f, 1.0f, 1.0f);
        // LUT only on LibRaw-decoded images (fullTexture_), NOT on:
        // - embedded JPEG preview (previewTexture_) — already has camera style
        // - non-RAW images (fullImage_) — already has camera style
        bool useLut = hasFullRaw && hasProfileLut_ && profileEnabled_ && profileBlend_ > 0.0f;
        if (useLut) {
            lutShader_.setLut(profileLut_);
            lutShader_.setBlend(profileBlend_);
            lutShader_.setTexture(fullTexture_);
            lutShader_.draw(x, y, drawW, drawH);
        } else if (isRawImage_) {
            rawTex->draw(x, y, drawW, drawH);
        } else {
            fullImage_.draw(x, y, drawW, drawH);
        }
    }

    // Handle key input (returns true if handled)
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
            // No other keys for video
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
                logNotice() << "[Profile] " << (profileEnabled_ ? "ON" : "OFF");
            }
            return true;
        }
        if (key == SAPP_KEYCODE_LEFT_BRACKET) {
            if (hasProfileLut_) {
                profileBlend_ = clamp(profileBlend_ - 0.1f, 0.0f, 1.0f);
                logNotice() << "[Profile] Blend: " << (int)(profileBlend_ * 100) << "%";
            }
            return true;
        }
        if (key == SAPP_KEYCODE_RIGHT_BRACKET) {
            if (hasProfileLut_) {
                profileBlend_ = clamp(profileBlend_ + 0.1f, 0.0f, 1.0f);
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
                    reprocessImage();
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
            if (selectedIndex_ >= 0 && isRawImage_ && rawPixels_.isAllocated()) {
                reprocessImage();
            }
            return true;
        }

        return false;
    }

    // Handle mouse press for panning / seek bar
    bool handleMousePress(Vec2 pos, int button) {
        if (button == 0) {
            // Video seek bar click
            if (isVideo_) {
                float barY = getHeight() - seekBarHeight_;
                if (pos.y >= barY) {
                    seekDragging_ = true;
                    float pct = clamp(pos.x / getWidth(), 0.0f, 1.0f);
                    videoPlayer_.setPosition(pct);
                    return true;
                }
                return false;  // no pan for video
            }
            isDragging_ = true;
            dragStart_ = pos;
            return true;
        }
        return false;
    }

    bool handleMouseRelease(int button) {
        if (button == 0) {
            if (seekDragging_) {
                seekDragging_ = false;
                return true;
            }
            isDragging_ = false;
            return true;
        }
        return false;
    }

    bool handleMouseDrag(Vec2 pos, int button) {
        if (button == 0) {
            // Video seek bar scrub
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

    bool handleMouseScroll(Vec2 delta) {
        if (isVideo_) return false;  // no zoom for video
        bool hasFullRaw = isRawImage_ && fullTexture_.isAllocated();
        bool hasPreviewRaw = isRawImage_ && previewTexture_.isAllocated();
        bool hasImage = isRawImage_ ? (hasFullRaw || hasPreviewRaw) : fullImage_.isAllocated();
        if (!hasImage) return false;

        Texture* rawTex = hasFullRaw ? &fullTexture_ : &previewTexture_;
        float imgW = isRawImage_ ? rawTex->getWidth() : fullImage_.getWidth();
        float imgH = isRawImage_ ? rawTex->getHeight() : fullImage_.getHeight();
        float winW = getWidth();
        float winH = getHeight();

        float oldZoom = zoomLevel_;
        zoomLevel_ *= (1.0f + delta.y * 0.1f);
        zoomLevel_ = clamp(zoomLevel_, 1.0f, 10.0f);

        Vec2 mousePos(getGlobalMouseX(), getGlobalMouseY());
        // Convert to local coordinates using node position
        mousePos.x -= getX();
        mousePos.y -= getY();

        Vec2 windowCenter(winW / 2.0f, winH / 2.0f);
        Vec2 imageCenter = windowCenter + panOffset_;
        Vec2 toMouse = mousePos - imageCenter;

        float zoomRatio = zoomLevel_ / oldZoom;
        panOffset_ = panOffset_ - toMouse * (zoomRatio - 1.0f);

        return true;
    }

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

    // Update metadata panel with current view state
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

    // Update metadata panel with current photo info
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

    // Get the current photo ID (for V key → related view, F key → relink)
    string currentPhotoId() const {
        if (!ctx_ || selectedIndex_ < 0) return "";
        if (selectedIndex_ >= (int)ctx_->grid->getPhotoIdCount()) return "";
        return ctx_->grid->getPhotoId(selectedIndex_);
    }

    // Check if an embedding exists for current photo
    bool hasEmbedding() const {
        if (!ctx_) return false;
        string id = currentPhotoId();
        return !id.empty() && ctx_->provider->getCachedEmbedding(id);
    }

    // Join background thread (for exit)
    void joinRawLoadThread() {
        if (rawLoadThread_.joinable()) rawLoadThread_.join();
    }

private:
    ViewContext* ctx_ = nullptr;

    // Image state
    int selectedIndex_ = -1;
    Image fullImage_;
    Pixels rawPixels_;
    Pixels fullPixels_;
    Texture fullTexture_;
    Texture previewTexture_;
    bool isRawImage_ = false;
    bool isSmartPreview_ = false;

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
    lut::LutShader lutShader_;
    lut::Lut3D profileLut_;
    bool hasProfileLut_ = false;
    bool profileEnabled_ = true;
    float profileBlend_ = 1.0f;
    string currentProfilePath_;

    // Lens correction
    LensCorrector lensCorrector_;
    bool lensEnabled_ = true;

    // Video playback
    VideoPlayer videoPlayer_;
    bool isVideo_ = false;
    bool seekDragging_ = false;
    float seekBarHeight_ = 40.0f;

    void cleanupState() {
        // Video cleanup
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
            fullPixels_.clear();
            fullTexture_.clear();
            previewTexture_.clear();
            { lock_guard<mutex> lock(rawLoadMutex_); pendingRawPixels_.clear(); }
        } else {
            fullImage_ = Image();
        }
        isRawImage_ = false;
        isSmartPreview_ = false;
        selectedIndex_ = -1;

        hasProfileLut_ = false;
        profileLut_.clear();
        currentProfilePath_.clear();
    }

    void reprocessImage() {
        fullPixels_ = rawPixels_.clone();
        if (lensEnabled_ && lensCorrector_.isReady()) {
            // apply() includes distortion + crop + auto-scale in one pass
            lensCorrector_.apply(fullPixels_);
        } else {
            // Lens disabled: just crop (full-size only; SP is auto-skipped)
            lensCorrector_.applyDefaultCrop(fullPixels_);
        }
        fullTexture_.allocate(fullPixels_, TextureUsage::Immutable, true);
    }

    void drawVideoView() {
        if (!videoPlayer_.isLoaded()) return;

        auto& tex = videoPlayer_.getTexture();
        float imgW = videoPlayer_.getWidth();
        float imgH = videoPlayer_.getHeight();
        float winW = getWidth();
        float winH = getHeight() - seekBarHeight_;  // leave room for seek bar

        // Fit-to-contain (no zoom)
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

        // Background
        setColor(0, 0, 0, 0.6f);
        fill();
        drawRect(0, barY, getWidth(), seekBarHeight_);

        // Play/pause icon (left side)
        float iconX = 20;
        float iconY = barY + seekBarHeight_ / 2;
        setColor(1, 1, 1, 0.9f);
        if (videoPlayer_.isPlaying()) {
            // Pause icon (two bars)
            fill();
            drawRect(iconX - 4, iconY - 8, 4, 16);
            drawRect(iconX + 4, iconY - 8, 4, 16);
        } else {
            // Play triangle
            fill();
            drawTriangle(iconX - 4, iconY - 8,
                         iconX - 4, iconY + 8,
                         iconX + 8, iconY);
        }

        // Progress bar
        float barX = 44;
        float barW = getWidth() - barX - 100;  // leave space for time label
        float barMidY = barY + seekBarHeight_ / 2;

        // Track background
        setColor(0.3f, 0.3f, 0.35f);
        fill();
        drawRect(barX, barMidY - 2, barW, 4);

        // Progress fill
        setColor(0.5f, 0.7f, 1.0f);
        drawRect(barX, barMidY - 2, barW * pos, 4);

        // Playhead
        drawCircle(barX + barW * pos, barMidY, 6);

        // Time label
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
            }
            return;
        }

        if (profileLut_.load(cubePath)) {
            hasProfileLut_ = true;
            currentProfilePath_ = cubePath;
            logNotice() << "[Profile] Loaded: " << cubePath;
        } else {
            hasProfileLut_ = false;
            currentProfilePath_.clear();
            logWarning() << "[Profile] Failed to load: " << cubePath;
        }
    }
};
