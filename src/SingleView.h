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
    void init(const string& profileDir, const string& lensfunDir) {
        string home = getenv("HOME") ? getenv("HOME") : ".";
        profileManager_.setProfileDir(home + "/.trussc/profiles");
        lutShader_.load();
        lensCorrector_.loadDatabase(lensfunDir);
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

        logNotice() << "Opening image: " << entry->filename;

        // Cancel any pending background load
        if (rawLoadInProgress_) {
            rawLoadCompleted_ = false;
        }

        bool loaded = false;
        isSmartPreview_ = false;

        if (!entry->localPath.empty() && fs::exists(entry->localPath)) {
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
                    string cameraMake = entry->cameraMake;
                    string camera = entry->camera;
                    string lens = entry->lens;
                    float focalLength = entry->focalLength;
                    float aperture = entry->aperture;

                    rawLoadInProgress_ = true;
                    rawLoadCompleted_ = false;
                    rawLoadTargetIndex_ = index;

                    if (rawLoadThread_.joinable()) rawLoadThread_.join();
                    rawLoadThread_ = thread([this, index, path, cameraMake, camera, lens, focalLength, aperture]() {
                        Pixels loadedPixels;
                        if (RawLoader::loadFloat(path, loadedPixels)) {
                            lock_guard<mutex> lock(rawLoadMutex_);
                            pendingRawPixels_ = std::move(loadedPixels);
                            if (focalLength > 0 && aperture > 0) {
                                lensCorrector_.setup(cameraMake, camera, lens, focalLength, aperture,
                                    pendingRawPixels_.getWidth(), pendingRawPixels_.getHeight());
                            }
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
                if (entry->focalLength > 0 && entry->aperture > 0) {
                    lensCorrector_.setup(entry->cameraMake, entry->camera, entry->lens,
                        entry->focalLength, entry->aperture,
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
                ctx_->metadataPanel->setViewInfo(zoomLevel_, profileEnabled_, profileBlend_,
                    hasProfileLut_, lensEnabled_, isSmartPreview_);
            }
        } else {
            logWarning() << "Failed to load: " << entry->localPath;
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
                    lensCorrector_.apply(fullPixels_);
                }
                fullTexture_.allocate(fullPixels_, TextureUsage::Immutable, true);
                previewTexture_.clear();
                logNotice() << "Full-size RAW loaded: " << rawPixels_.getWidth() << "x" << rawPixels_.getHeight();

                // Generate smart preview if not yet done
                const string& spId = ctx_->grid->getPhotoId(selectedIndex_);
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
        bool useLut = hasProfileLut_ && profileEnabled_ && profileBlend_ > 0.0f;
        if (useLut) {
            lutShader_.setLut(profileLut_);
            lutShader_.setBlend(profileBlend_);
            if (isRawImage_) {
                lutShader_.setTexture(*rawTex);
                lutShader_.draw(x, y, drawW, drawH);
            } else {
                lutShader_.setTexture(fullImage_.getTexture());
                lutShader_.draw(x, y, drawW, drawH);
            }
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
                    if (spEntry->focalLength > 0 && spEntry->aperture > 0) {
                        lensCorrector_.setup(spEntry->cameraMake, spEntry->camera, spEntry->lens,
                            spEntry->focalLength, spEntry->aperture,
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
            logNotice() << "[Lensfun] " << (lensEnabled_ ? "ON" : "OFF");
            if (selectedIndex_ >= 0 && isRawImage_ && rawPixels_.isAllocated()) {
                reprocessImage();
            }
            return true;
        }

        return false;
    }

    // Handle mouse press for panning
    bool handleMousePress(Vec2 pos, int button) {
        if (button == 0) {
            isDragging_ = true;
            dragStart_ = pos;
            return true;
        }
        return false;
    }

    bool handleMouseRelease(int button) {
        if (button == 0) {
            isDragging_ = false;
            return true;
        }
        return false;
    }

    bool handleMouseDrag(Vec2 pos, int button) {
        if (button == 0 && isDragging_) {
            Vec2 delta = pos - dragStart_;
            panOffset_ = panOffset_ + delta;
            dragStart_ = pos;
            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
            return true;
        }
        return false;
    }

    bool handleMouseScroll(Vec2 delta) {
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

    // Update metadata panel with current view state
    void updateViewInfo() {
        if (ctx_ && ctx_->metadataPanel) {
            ctx_->metadataPanel->setViewInfo(zoomLevel_, profileEnabled_, profileBlend_,
                hasProfileLut_, lensEnabled_, isSmartPreview_);
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

    void cleanupState() {
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
            lensCorrector_.apply(fullPixels_);
        }
        fullTexture_.allocate(fullPixels_, TextureUsage::Immutable, true);
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
