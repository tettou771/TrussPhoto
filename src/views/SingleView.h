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
#include "ui/PhotoGrid.h"
#include "ui/MetadataPanel.h"
#include "pipeline/CameraProfileManager.h"
#include "pipeline/LensCorrector.h"
#include "pipeline/DevelopShader.h"
#include "pipeline/GuidedFilter.h"
#include "ui/ContextMenu.h"
#include "pipeline/PhotoExporter.h"
#include "ui/ExportDialog.h"
#include "ui/VideoSeekBar.h"
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

    void suspendView() override {
        // Keep FBO + textures + ctx alive (CropView borrows them)
        if (isVideo_) videoPlayer_.togglePause();
    }

    bool hasState() const override { return selectedIndex_ >= 0; }

    bool wantsSearchBar() const override { return false; }
    bool wantsLeftSidebar() const override { return false; }

    // Callback when photo changes (for DevelopPanel slider sync)
    function<void(float exposure, float wbTemp, float wbTint,
                  float contrast, float highlights, float shadows,
                  float whites, float blacks,
                  float vibrance, float saturation,
                  float chroma, float luma)> onDevelopRestored;

    // Right-click context menu
    function<void(ContextMenu::Ptr)> onContextMenu;

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

        // Restore develop settings from entry
        exposure_ = entry->devExposure;
        wbTemp_ = entry->devWbTemp;
        wbTint_ = entry->devWbTint;
        contrast_ = entry->devContrast;
        highlights_ = entry->devHighlights;
        shadows_ = entry->devShadows;
        whites_ = entry->devWhites;
        blacks_ = entry->devBlacks;
        vibrance_ = entry->devVibrance;
        saturation_ = entry->devSaturation;
        chromaDenoise_ = entry->chromaDenoise;
        lumaDenoise_ = entry->lumaDenoise;
        developShader_.setExposure(exposure_);
        developShader_.setWbTemp(wbTemp_);
        developShader_.setWbTint(wbTint_);
        developShader_.setContrast(contrast_);
        developShader_.setHighlights(highlights_);
        developShader_.setShadows(shadows_);
        developShader_.setWhites(whites_);
        developShader_.setBlacks(blacks_);
        developShader_.setVibrance(vibrance_);
        developShader_.setSaturation(saturation_);
        if (onDevelopRestored) onDevelopRestored(exposure_, wbTemp_, wbTint_,
                                                 contrast_, highlights_, shadows_,
                                                 whites_, blacks_,
                                                 vibrance_, saturation_,
                                                 chromaDenoise_, lumaDenoise_);

        bool loaded = false;
        isSmartPreview_ = false;

        // Video playback
        if (entry->isVideo) {
            if (!entry->localPath.empty() && fs::exists(entry->localPath)) {
                if (videoPlayer_.load(entry->localPath)) {
                    isVideo_ = true;
                    videoPlayer_.play();
                    loaded = true;

                    // Create seek bar (lazily, once)
                    if (!videoSeekBar_) {
                        videoSeekBar_ = make_shared<VideoSeekBar>();
                        seekBarPlayPauseListener_ = videoSeekBar_->playPauseToggled.listen([this]() {
                            videoPlayer_.togglePause();
                            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
                        });
                        seekBarSeekListener_ = videoSeekBar_->seeked.listen([this](float& pct) {
                            videoPlayer_.setPosition(pct);
                            if (ctx_ && ctx_->redraw) ctx_->redraw(1);
                        });
                        addChild(videoSeekBar_);
                        videoSeekBar_->setActive(false);
                    }
                    videoSeekBar_->setRect(0, getHeight() - 40, getWidth(), 40);
                    videoSeekBar_->setActive(true);

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
                    setupIntermediateFromImage();
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
        // Sync video state to seek bar
        if (videoSeekBar_ && videoSeekBar_->getActive()) {
            videoSeekBar_->setPosition(videoPlayer_.getPosition());
            videoSeekBar_->setDuration(videoPlayer_.getDuration());
            videoSeekBar_->setPlaying(videoPlayer_.isPlaying());
            videoSeekBar_->setRect(0, getHeight() - 40, getWidth(), 40);
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
        if (!developShader_.hasSource()) return;
        if (!needsFboRender_) return;

        developShader_.renderOffscreen(displayW_, displayH_);
        needsFboRender_ = false;
    }

    // Draw the image (called by Node tree with clipping + local transform)
    void draw() override {
        // Fill background to cover any framebuffer artifacts from
        // suspend/resumeSwapchainPass during offscreen FBO rendering
        setColor(0.07f, 0.07f, 0.09f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());

        if (isVideo_) {
            drawVideoView();
            return;
        }

        bool hasFbo = developShader_.isFboReady();
        bool hasPreviewRaw = isRawImage_ && previewTexture_.isAllocated();
        bool hasImage = hasFbo || hasPreviewRaw || fullImage_.isAllocated();
        if (!hasImage) return;

        // Get user crop + rotation + perspective from entry (if available)
        float ucX = 0, ucY = 0, ucW = 1, ucH = 1;
        float ucAngle = 0;
        int ucRot90 = 0;
        float ucPerspV = 0, ucPerspH = 0, ucShear = 0;
        if (ctx_ && selectedIndex_ >= 0 && selectedIndex_ < (int)ctx_->grid->getPhotoIdCount()) {
            const string& pid = ctx_->grid->getPhotoId(selectedIndex_);
            auto* entry = ctx_->provider->getPhoto(pid);
            if (entry && entry->hasCrop()) {
                ucX = entry->userCropX;
                ucY = entry->userCropY;
                ucW = entry->userCropW;
                ucH = entry->userCropH;
            }
            if (entry && entry->hasRotation()) {
                ucAngle = entry->userAngle;
                ucRot90 = entry->userRotation90;
            }
            if (entry && entry->hasPerspective()) {
                ucPerspV = entry->userPerspV;
                ucPerspH = entry->userPerspH;
                ucShear = entry->userShear;
            }
        }

        // Source dimensions (what we're reading from)
        float srcW, srcH;
        if (hasFbo) {
            srcW = (float)displayW_;
            srcH = (float)displayH_;
        } else if (hasPreviewRaw) {
            srcW = previewTexture_.getWidth();
            srcH = previewTexture_.getHeight();
        } else {
            srcW = fullImage_.getWidth();
            srcH = fullImage_.getHeight();
        }

        float totalRot = ucRot90 * TAU / 4 + ucAngle;
        bool hasRotation = (totalRot != 0);
        bool hasPersp = (ucPerspV != 0 || ucPerspH != 0 || ucShear != 0);

        // Build temporary entry for perspective-aware BB calculation
        PhotoEntry tmpEntry;
        tmpEntry.userAngle = ucAngle;
        tmpEntry.userRotation90 = ucRot90;
        tmpEntry.userPerspV = ucPerspV;
        tmpEntry.userPerspH = ucPerspH;
        tmpEntry.userShear = ucShear;
        if (ctx_ && selectedIndex_ >= 0 && selectedIndex_ < (int)ctx_->grid->getPhotoIdCount()) {
            auto* e = ctx_->provider->getPhoto(ctx_->grid->getPhotoId(selectedIndex_));
            if (e) tmpEntry.focalLength35mm = e->focalLength35mm;
        }

        // Compute bounding box (perspective-aware)
        auto [bbW, bbH] = tmpEntry.computeBB((int)srcW, (int)srcH);

        // Crop area in BB pixels (the output dimensions)
        float cropPxW = ucW * bbW;
        float cropPxH = ucH * bbH;

        auto [x, y, drawW, drawH] = calcDrawRect(cropPxW, cropPxH);

        if ((hasRotation || hasPersp) && hasFbo) {
            float drawCX = x + drawW / 2;
            float drawCY = y + drawH / 2;

            float cropCenterBBX = (ucX + ucW / 2 - 0.5f) * bbW;
            float cropCenterBBY = (ucY + ucH / 2 - 0.5f) * bbH;

            float scale = drawW / cropPxW;

            // Screen point -> texture UV via inverse rotation + inverse perspective
            auto screenToUV = [&](float sx, float sy) -> pair<float,float> {
                float bbx = (sx - drawCX) / scale + cropCenterBBX;
                float bby = (sy - drawCY) / scale + cropCenterBBY;
                float cosR = cos(-totalRot), sinR = sin(-totalRot);
                float ix = bbx * cosR - bby * sinR;
                float iy = bbx * sinR + bby * cosR;
                float wu = ix / srcW + 0.5f;
                float wv = iy / srcH + 0.5f;
                if (hasPersp) {
                    return tmpEntry.inverseWarp(wu, wv);
                }
                return {wu, wv};
            };

            if (!hasPersp) {
                // Rotation only: simple 4-corner quad
                auto [u0, v0] = screenToUV(x, y);
                auto [u1, v1] = screenToUV(x + drawW, y);
                auto [u2, v2] = screenToUV(x + drawW, y + drawH);
                auto [u3, v3] = screenToUV(x, y + drawH);

                setColor(1.0f, 1.0f, 1.0f);
                sgl_enable_texture();
                sgl_texture(developShader_.getFboView(), developShader_.getFboSampler());
                Color col = getDefaultContext().getColor();
                sgl_begin_quads();
                sgl_c4f(col.r, col.g, col.b, col.a);
                sgl_v2f_t2f(x, y, u0, v0);
                sgl_v2f_t2f(x + drawW, y, u1, v1);
                sgl_v2f_t2f(x + drawW, y + drawH, u2, v2);
                sgl_v2f_t2f(x, y + drawH, u3, v3);
                sgl_end();
                sgl_disable_texture();
            } else {
                // Perspective: tessellated grid for correct UV mapping
                int tessN = 16;
                setColor(1.0f, 1.0f, 1.0f);
                sgl_enable_texture();
                sgl_texture(developShader_.getFboView(), developShader_.getFboSampler());
                Color col = getDefaultContext().getColor();

                sgl_begin_triangles();
                sgl_c4f(col.r, col.g, col.b, col.a);
                for (int j = 0; j < tessN; j++) {
                    for (int i = 0; i < tessN; i++) {
                        float tx0 = (float)i / tessN, tx1 = (float)(i+1) / tessN;
                        float ty0 = (float)j / tessN, ty1 = (float)(j+1) / tessN;

                        float sx00 = x + tx0 * drawW, sy00 = y + ty0 * drawH;
                        float sx10 = x + tx1 * drawW, sy10 = y + ty0 * drawH;
                        float sx11 = x + tx1 * drawW, sy11 = y + ty1 * drawH;
                        float sx01 = x + tx0 * drawW, sy01 = y + ty1 * drawH;

                        auto [u00, v00] = screenToUV(sx00, sy00);
                        auto [u10, v10] = screenToUV(sx10, sy10);
                        auto [u11, v11] = screenToUV(sx11, sy11);
                        auto [u01, v01] = screenToUV(sx01, sy01);

                        sgl_v2f_t2f(sx00, sy00, u00, v00);
                        sgl_v2f_t2f(sx10, sy10, u10, v10);
                        sgl_v2f_t2f(sx11, sy11, u11, v11);

                        sgl_v2f_t2f(sx00, sy00, u00, v00);
                        sgl_v2f_t2f(sx11, sy11, u11, v11);
                        sgl_v2f_t2f(sx01, sy01, u01, v01);
                    }
                }
                sgl_end();
                sgl_disable_texture();
            }
        } else {
            // No rotation: simple axis-aligned draw
            setColor(1.0f, 1.0f, 1.0f);
            if (hasFbo) {
                drawTextureView(developShader_.getFboView(), developShader_.getFboSampler(),
                                x, y, drawW, drawH,
                                ucX, ucY, ucX + ucW, ucY + ucH);
            } else if (hasPreviewRaw) {
                previewTexture_.draw(x, y, drawW, drawH);
            } else {
                fullImage_.draw(x, y, drawW, drawH);
            }
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

    // Called when DevelopPanel sliders change
    void onDevelopChanged(float exposure, float wbTemp, float wbTint,
                          float contrast, float highlights, float shadows,
                          float whites, float blacks,
                          float vibrance, float saturation,
                          float chroma, float luma) {
        // GPU-only params: update shader uniforms
        bool gpuChanged = (exposure_ != exposure || wbTemp_ != wbTemp || wbTint_ != wbTint ||
                           contrast_ != contrast || highlights_ != highlights ||
                           shadows_ != shadows || whites_ != whites || blacks_ != blacks ||
                           vibrance_ != vibrance || saturation_ != saturation);
        exposure_ = exposure;
        wbTemp_ = wbTemp;
        wbTint_ = wbTint;
        contrast_ = contrast;
        highlights_ = highlights;
        shadows_ = shadows;
        whites_ = whites;
        blacks_ = blacks;
        vibrance_ = vibrance;
        saturation_ = saturation;
        developShader_.setExposure(exposure_);
        developShader_.setWbTemp(wbTemp_);
        developShader_.setWbTint(wbTint_);
        developShader_.setContrast(contrast_);
        developShader_.setHighlights(highlights_);
        developShader_.setShadows(shadows_);
        developShader_.setWhites(whites_);
        developShader_.setBlacks(blacks_);
        developShader_.setVibrance(vibrance_);
        developShader_.setSaturation(saturation_);

        // NR: needs CPU re-processing (only if changed)
        bool nrChanged = (chromaDenoise_ != chroma || lumaDenoise_ != luma);
        if (nrChanged) {
            chromaDenoise_ = chroma;
            lumaDenoise_ = luma;

            if (isRawImage_ && rawPixels_.isAllocated()) {
                nrPixels_ = rawPixels_.clone();
                if (chromaDenoise_ > 0 || lumaDenoise_ > 0) {
                    tp::guidedDenoise(nrPixels_, chromaDenoise_, lumaDenoise_);
                }
                intermediateTexture_.allocate(nrPixels_, TextureUsage::Immutable, true);
                developShader_.setSourceTexture(intermediateTexture_);
            }
        }

        if (gpuChanged || nrChanged) needsFboRender_ = true;

        // Save all develop settings to DB
        if (ctx_ && selectedIndex_ >= 0 && selectedIndex_ < (int)ctx_->grid->getPhotoIdCount()) {
            const string& pid = ctx_->grid->getPhotoId(selectedIndex_);
            ctx_->provider->setDevelop(pid, exposure_, wbTemp_, wbTint_,
                                       contrast_, highlights_, shadows_,
                                       whites_, blacks_,
                                       vibrance_, saturation_,
                                       chromaDenoise_, lumaDenoise_);
        }

        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

protected:
    // Node event handlers (dispatched by the Node tree, not tcApp)
    bool onMousePress(Vec2 pos, int button) override {
        if (button == 1 && onContextMenu && ctx_) {
            buildContextMenu();
            return true;
        }
        if (button == 0) {
            if (isVideo_) return false;
            isDragging_ = true;
            dragStart_ = pos;
            return true;
        }
        return false;
    }

    bool onMouseRelease(Vec2 pos, int button) override {
        (void)pos;
        if (button == 0) {
            isDragging_ = false;
            return true;
        }
        return false;
    }

    bool onMouseDrag(Vec2 pos, int button) override {
        if (button == 0) {
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

    // FBO accessors (for CropView to borrow)
    sg_view fboView() const { return developShader_.getFboView(); }
    sg_sampler fboSampler() const { return developShader_.getFboSampler(); }
    int fboWidth() const { return developShader_.getFboWidth(); }
    int fboHeight() const { return developShader_.getFboHeight(); }
    bool hasFbo() const { return developShader_.isFboReady(); }
    int displayWidth() const { return displayW_; }
    int displayHeight() const { return displayH_; }

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

    void doExport() {
        if (!developShader_.isFboReady() || !ctx_) return;

        if (!exportDialog_) {
            exportDialog_ = make_shared<ExportDialog>();
            exportExportListener_ = exportDialog_->exportRequested.listen([this](ExportSettings& s) {
                executeExport(s);
            });
            exportCancelListener_ = exportDialog_->cancelled.listen([this]() {
                exportDialog_->hide();
                if (ctx_ && ctx_->redraw) ctx_->redraw(1);
            });
            addChild(exportDialog_);
            exportDialog_->setActive(false);
        }

        // Use crop output size (not full FBO) so presets > crop size get greyed out
        int srcW, srcH;
        string pid = currentPhotoId();
        auto* entry = ctx_ ? ctx_->provider->getPhoto(pid) : nullptr;
        if (entry && entry->hasCrop()) {
            auto [cw, ch] = entry->getCropOutputSize(
                developShader_.getFboWidth(), developShader_.getFboHeight());
            srcW = cw;
            srcH = ch;
        } else {
            srcW = developShader_.getFboWidth();
            srcH = developShader_.getFboHeight();
        }
        exportDialog_->setSize(getWidth(), getHeight());
        exportDialog_->show(lastExportSettings_, srcW, srcH);
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

    void joinRawLoadThread() {
        if (rawLoadThread_.joinable()) rawLoadThread_.join();
    }

private:
    void executeExport(const ExportSettings& settings) {
        if (exportDialog_) exportDialog_->hide();
        lastExportSettings_ = settings;

        string pid = currentPhotoId();
        auto* entry = ctx_->provider->getPhoto(pid);
        if (!entry) return;

        string outPath = PhotoExporter::makeExportPath(
            ctx_->provider->getCatalogDir(), entry->filename);

        if (PhotoExporter::exportJpeg(developShader_, outPath, settings, *entry)) {
            logNotice() << "[Export] " << outPath;
            revealInFinder(outPath);
        } else {
            logError() << "[Export] Failed";
        }
        if (ctx_ && ctx_->redraw) ctx_->redraw(1);
    }

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

    // Develop settings (GPU)
    float exposure_ = 0.0f;
    float wbTemp_ = 0.0f;
    float wbTint_ = 0.0f;
    float contrast_ = 0.0f;
    float highlights_ = 0.0f;
    float shadows_ = 0.0f;
    float whites_ = 0.0f;
    float blacks_ = 0.0f;
    float vibrance_ = 0.0f;
    float saturation_ = 0.0f;

    // Noise reduction
    float chromaDenoise_ = 0.5f;
    float lumaDenoise_ = 0.0f;

    // Video playback
    VideoPlayer videoPlayer_;
    bool isVideo_ = false;
    VideoSeekBar::Ptr videoSeekBar_;
    EventListener seekBarPlayPauseListener_;
    EventListener seekBarSeekListener_;

    // Export dialog
    ExportDialog::Ptr exportDialog_;
    EventListener exportExportListener_;
    EventListener exportCancelListener_;
    ExportSettings lastExportSettings_ = {0, 92}; // Full res, quality 92

    // Draw a texture by view+sampler via sgl (for FBO result)
    void drawTextureView(sg_view view, sg_sampler sampler,
                         float x, float y, float w, float h,
                         float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
        sgl_enable_texture();
        sgl_texture(view, sampler);
        Color col = getDefaultContext().getColor();
        sgl_begin_quads();
        sgl_c4f(col.r, col.g, col.b, col.a);
        sgl_v2f_t2f(x, y, u0, v0);
        sgl_v2f_t2f(x + w, y, u1, v0);
        sgl_v2f_t2f(x + w, y + h, u1, v1);
        sgl_v2f_t2f(x, y + h, u0, v1);
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
            if (videoSeekBar_) videoSeekBar_->setActive(false);
        }

        if (rawLoadThread_.joinable()) rawLoadThread_.join();
        rawLoadInProgress_ = false;
        rawLoadCompleted_ = false;

        // Clear shader source before destroying textures (prevent dangling refs)
        developShader_.clearSource();

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

    // Route JPEG/HEIF through DevelopShader FBO (no intermediate texture needed)
    void setupIntermediateFromImage() {
        auto& tex = fullImage_.getTexture();
        developShader_.setSourceTexture(tex);
        developShader_.clearLensData();
        setupDevelopShaderParams(tex.getWidth(), tex.getHeight());
        if (hasProfileLut_) {
            developShader_.setLut(profileLut_);
            developShader_.setLutBlend(profileEnabled_ ? profileBlend_ : 0.0f);
        }
        needsFboRender_ = true;
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
        int srcW, srcH;
        if (intermediateTexture_.isAllocated()) {
            srcW = intermediateTexture_.getWidth();
            srcH = intermediateTexture_.getHeight();
        } else if (fullImage_.isAllocated()) {
            srcW = fullImage_.getWidth();
            srcH = fullImage_.getHeight();
        } else {
            return;
        }

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
        float winH = getHeight() - 40; // reserve space for seek bar

        float fitScale = min(winW / imgW, winH / imgH);
        float drawW = imgW * fitScale;
        float drawH = imgH * fitScale;
        float x = (winW - drawW) / 2;
        float y = (winH - drawH) / 2;

        setColor(1.0f, 1.0f, 1.0f);
        tex.draw(x, y, drawW, drawH);
    }

    void buildContextMenu() {
        if (!ctx_ || selectedIndex_ < 0) return;
        if (selectedIndex_ >= (int)ctx_->grid->getPhotoIdCount()) return;

        const string& photoId = ctx_->grid->getPhotoId(selectedIndex_);
        auto* entry = ctx_->provider->getPhoto(photoId);
        if (!entry) return;

        auto menu = make_shared<ContextMenu>();

        // Reset develop settings
        menu->addChild(make_shared<MenuItem>("Reset Develop",
            [this, photoId]() {
                exposure_ = 0; wbTemp_ = 0; wbTint_ = 0;
                contrast_ = 0; highlights_ = 0; shadows_ = 0;
                whites_ = 0; blacks_ = 0;
                vibrance_ = 0; saturation_ = 0;
                chromaDenoise_ = 0.5f; lumaDenoise_ = 0;
                developShader_.setExposure(0);
                developShader_.setWbTemp(0);
                developShader_.setWbTint(0);
                developShader_.setContrast(0);
                developShader_.setHighlights(0);
                developShader_.setShadows(0);
                developShader_.setWhites(0);
                developShader_.setBlacks(0);
                developShader_.setVibrance(0);
                developShader_.setSaturation(0);
                needsFboRender_ = true;
                if (ctx_) ctx_->provider->setDevelop(photoId, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0);
                if (onDevelopRestored) onDevelopRestored(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0);
                if (ctx_ && ctx_->redraw) ctx_->redraw(1);
            }));

        menu->addChild(make_shared<MenuSeparator>());

        // Export JPEG
        if (developShader_.isFboReady()) {
            menu->addChild(make_shared<MenuItem>("Export JPEG",
                [this]() { doExport(); }));
        }

        // Show in Finder
        if (!entry->localPath.empty()) {
            menu->addChild(make_shared<MenuItem>("Show in Finder",
                [path = entry->localPath]() {
                    revealInFinder(path);
                }));
        }

        onContextMenu(menu);
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
