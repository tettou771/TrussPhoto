#include "tcApp.h"
#include <tcxLibRaw.h>

void tcApp::setup() {
    // 1. Load settings
    settings_.load();

    // 2. Ask for library folder if first run or folder not accessible
    bool needsFolder = settings_.isFirstRun();
    if (!needsFolder && !settings_.libraryFolder.empty()) {
        if (!fs::exists(settings_.libraryFolder)) {
            logWarning() << "Library folder not accessible: " << settings_.libraryFolder
                         << " (re-select to grant access)";
            needsFolder = true;
        }
    }

    if (needsFolder) {
        auto result = loadDialog(
            "Select Library Folder",
            "Choose where to store your photo library",
            "", true);

        if (result.success) {
            settings_.libraryFolder = result.filePath;
        } else {
            // Default location
            string home = getenv("HOME") ? getenv("HOME") : ".";
            settings_.libraryFolder = home + "/Pictures/TrussPhoto";
        }
        fs::create_directories(settings_.libraryFolder);
        settings_.save();
    }

    // 3. Configure provider
    provider_.setThumbnailCacheDir(getDataPath("thumbnail_cache"));
    provider_.setLibraryPath(getDataPath("library.json"));
    provider_.setLibraryFolder(settings_.libraryFolder);
    provider_.setServerUrl(settings_.serverUrl);

    // 4. Load library (instant display from previous session)
    bool hasLibrary = provider_.loadLibrary();

    // 5. Create grid
    grid_ = make_shared<PhotoGrid>();
    grid_->setRect(0, 0, getWindowWidth(), getWindowHeight());
    addChild(grid_);

    grid_->onItemClick = [this](int index) {
        showFullImage(index);
    };

    // Display previous library immediately
    if (hasLibrary && provider_.getCount() > 0) {
        grid_->populate(provider_);
    }

    // 6. Start upload queue (only if server configured)
    if (settings_.hasServer()) {
        uploadQueue_.setServerUrl(settings_.serverUrl);
        uploadQueue_.start();
        // 7. Trigger server sync on next frame
        needsServerSync_ = true;
    }

    // 8. MCP tools
    mcp::tool("load_folder", "Load a folder containing images")
        .arg<string>("path", "Path to folder")
        .bind([this](const string& path) {
            filesDropped({path});
            return json{
                {"status", "ok"},
                {"count", (int)provider_.getCount()}
            };
        });

    mcp::tool("set_server", "Set server URL (empty to disable)")
        .arg<string>("url", "Server URL (e.g. http://localhost:8080)")
        .bind([this](const string& url) {
            configureServer(url);
            return json{{"status", "ok"}, {"serverUrl", url}};
        });

    // 9. Camera profile manager
    string home = getenv("HOME") ? getenv("HOME") : ".";
    profileManager_.setProfileDir(home + "/.trussc/profiles");

    // 10. LUT shader
    lutShader_.load();

    // 11. Lens correction database
    lensCorrector_.loadDatabase(getDataPath("lensfun"));

    logNotice() << "TrussPhoto ready - Library: " << settings_.libraryFolder;
}

void tcApp::update() {
    // Launch server sync in background thread (non-blocking)
    if (needsServerSync_ && !syncInProgress_) {
        needsServerSync_ = false;
        syncInProgress_ = true;
        syncCompleted_ = false;

        if (syncThread_.joinable()) syncThread_.join();
        syncThread_ = thread([this]() {
            provider_.syncWithServer();
            syncInProgress_ = false;
            syncCompleted_ = true;
        });
        syncThread_.detach();
    }

    // Process sync completion on main thread
    if (syncCompleted_) {
        syncCompleted_ = false;
        enqueueLocalOnlyPhotos();

        if (provider_.getCount() > 0 && grid_->getItemCount() != provider_.getCount()) {
            grid_->populate(provider_);
        }
    }

    // Process background file copies
    provider_.processCopyResults();

    // Process upload results
    UploadResult uploadResult;
    while (uploadQueue_.tryGetResult(uploadResult)) {
        auto* photo = provider_.getPhoto(uploadResult.photoId);
        if (photo) {
            photo->syncState = uploadResult.success
                ? SyncState::Synced
                : SyncState::LocalOnly;
            provider_.markDirty();
        }
    }

    // Update sync state badges
    if (grid_) {
        grid_->updateSyncStates(provider_);
    }

    // Periodic server sync (every ~30 seconds at 60fps, only if server configured)
    static int syncCounter = 0;
    if (settings_.hasServer() && ++syncCounter % 1800 == 0 && !syncInProgress_) {
        needsServerSync_ = true;
    }

    // Periodic save (every ~5 seconds at 60fps)
    static int saveCounter = 0;
    if (++saveCounter % 300 == 0) {
        provider_.saveIfDirty();
    }
}

void tcApp::draw() {
    clear(0.06f, 0.06f, 0.08f);

    if (viewMode_ == ViewMode::Single) {
        drawSingleView();
    } else {
        // Show hint if no images
        if (provider_.getCount() == 0) {
            setColor(0.5f, 0.5f, 0.55f);
            string hint = "Drop a folder containing images";
            float x = getWindowWidth() / 2 - hint.length() * 4;
            float y = getWindowHeight() / 2;
            drawBitmapString(hint, x, y);
        }
    }

    // Status bar background
    float barHeight = 24;
    float barY = getWindowHeight() - barHeight;
    setColor(0.1f, 0.1f, 0.12f, 0.9f);
    fill();
    drawRect(0, barY, getWindowWidth(), barHeight);

    // Server status indicator
    float dotX = 10;
    float dotY = barY + barHeight / 2;
    fill();
    if (provider_.isServerConnected()) {
        setColor(0.3f, 0.8f, 0.4f);  // green
    } else {
        setColor(0.6f, 0.35f, 0.35f);  // dim red
    }
    drawCircle(dotX + 4, dotY, 4);

    // Status text
    setColor(0.55f, 0.55f, 0.6f);
    string serverLabel = provider_.isServerConnected() ? "Server" : "Offline";
    size_t pending = uploadQueue_.getPendingCount();
    string uploadStatus = pending > 0 ? format("  Upload: {}", pending) : "";
    drawBitmapString(format("{}  Photos: {}{}  FPS: {:.0f}",
        serverLabel, provider_.getCount(), uploadStatus, getFrameRate()),
        dotX + 14, barY + 7);
}

void tcApp::keyPressed(int key) {
    if (viewMode_ == ViewMode::Single) {
        if (key == SAPP_KEYCODE_ESCAPE) {
            exitFullImage();
        } else if (key == SAPP_KEYCODE_LEFT && selectedIndex_ > 0) {
            showFullImage(selectedIndex_ - 1);
        } else if (key == SAPP_KEYCODE_RIGHT && selectedIndex_ < (int)grid_->getPhotoIdCount() - 1) {
            showFullImage(selectedIndex_ + 1);
        } else if (key == '0') {
            zoomLevel_ = 1.0f;
            panOffset_ = {0, 0};
        } else if (key == 'P' || key == 'p') {
            // Toggle camera profile LUT
            if (hasProfileLut_) {
                profileEnabled_ = !profileEnabled_;
                logNotice() << "[Profile] " << (profileEnabled_ ? "ON" : "OFF");
            }
        } else if (key == SAPP_KEYCODE_LEFT_BRACKET) {
            // Decrease profile blend
            if (hasProfileLut_) {
                profileBlend_ = clamp(profileBlend_ - 0.1f, 0.0f, 1.0f);
                logNotice() << "[Profile] Blend: " << (int)(profileBlend_ * 100) << "%";
            }
        } else if (key == SAPP_KEYCODE_RIGHT_BRACKET) {
            // Increase profile blend
            if (hasProfileLut_) {
                profileBlend_ = clamp(profileBlend_ + 0.1f, 0.0f, 1.0f);
                logNotice() << "[Profile] Blend: " << (int)(profileBlend_ * 100) << "%";
            }
        } else if (key == 'L' || key == 'l') {
            // Toggle lens correction (requires re-loading image)
            lensEnabled_ = !lensEnabled_;
            logNotice() << "[Lensfun] " << (lensEnabled_ ? "ON" : "OFF");
            // Re-load current image with/without lens correction
            if (selectedIndex_ >= 0) {
                showFullImage(selectedIndex_);
            }
        }
    }
}

void tcApp::keyReleased(int key) {
    (void)key;
}

void tcApp::mousePressed(Vec2 pos, int button) {
    if (viewMode_ == ViewMode::Single && button == 0) {
        isDragging_ = true;
        dragStart_ = pos;
    }
}

void tcApp::mouseReleased(Vec2 pos, int button) {
    (void)pos;
    if (button == 0) {
        isDragging_ = false;
    }
}

void tcApp::mouseMoved(Vec2 pos) {
    (void)pos;
}

void tcApp::mouseDragged(Vec2 pos, int button) {
    if (viewMode_ == ViewMode::Single && button == 0 && isDragging_) {
        Vec2 delta = pos - dragStart_;
        panOffset_ = panOffset_ + delta;
        dragStart_ = pos;
    }
}

void tcApp::mouseScrolled(Vec2 delta) {
    if (viewMode_ != ViewMode::Single) return;

    bool hasImage = isRawImage_ ? fullTexture_.isAllocated() : fullImage_.isAllocated();
    if (!hasImage) return;

    {
        float imgW = isRawImage_ ? fullTexture_.getWidth() : fullImage_.getWidth();
        float imgH = isRawImage_ ? fullTexture_.getHeight() : fullImage_.getHeight();
        float winW = getWindowWidth();
        float winH = getWindowHeight();

        float minZoom = 1.0f;
        float maxZoom = 10.0f;

        float oldZoom = zoomLevel_;
        zoomLevel_ *= (1.0f + delta.y * 0.1f);
        zoomLevel_ = clamp(zoomLevel_, minZoom, maxZoom);

        Vec2 mousePos(getGlobalMouseX(), getGlobalMouseY());
        Vec2 windowCenter(winW / 2.0f, winH / 2.0f);
        Vec2 imageCenter = windowCenter + panOffset_;
        Vec2 toMouse = mousePos - imageCenter;

        float zoomRatio = zoomLevel_ / oldZoom;
        panOffset_ = panOffset_ - toMouse * (zoomRatio - 1.0f);
    }
}

void tcApp::windowResized(int width, int height) {
    if (grid_) {
        grid_->setSize(width, height);
    }
}

void tcApp::filesDropped(const vector<string>& files) {
    if (files.empty()) return;

    fs::path path(files[0]);

    if (fs::is_directory(path)) {
        provider_.scanFolder(path.string());
    } else {
        fs::path folder = path.parent_path();
        provider_.scanFolder(folder.string());
    }

    grid_->populate(provider_);
    provider_.saveLibrary();

    // Auto-upload new photos
    enqueueLocalOnlyPhotos();
}

void tcApp::exit() {
    uploadQueue_.stop();
    if (syncThread_.joinable()) syncThread_.join();
    provider_.saveLibrary();
    logNotice() << "TrussPhoto exiting";
}

void tcApp::enqueueLocalOnlyPhotos() {
    if (!settings_.hasServer()) return;

    auto localPhotos = provider_.getLocalOnlyPhotos();
    for (const auto& [id, path] : localPhotos) {
        uploadQueue_.enqueue(id, path);
    }
    if (!localPhotos.empty()) {
        logNotice() << "Enqueued " << localPhotos.size() << " photos for upload";
    }
}

void tcApp::configureServer(const string& url) {
    settings_.serverUrl = url;
    settings_.save();

    provider_.setServerUrl(url);
    provider_.resetServerCheck();

    if (settings_.hasServer()) {
        // Start upload queue if not running
        uploadQueue_.setServerUrl(url);
        uploadQueue_.start();
        // Trigger sync
        needsServerSync_ = true;
        logNotice() << "Server configured: " << url;
    } else {
        uploadQueue_.stop();
        logNotice() << "Server disabled, running in local-only mode";
    }
}

void tcApp::loadProfileForEntry(const PhotoEntry& entry) {
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

void tcApp::showFullImage(int index) {
    if (index < 0 || index >= (int)grid_->getPhotoIdCount()) return;

    const string& photoId = grid_->getPhotoId(index);
    auto* entry = provider_.getPhoto(photoId);
    if (!entry) return;

    logNotice() << "Opening image: " << entry->filename;

    bool loaded = false;

    if (!entry->localPath.empty() && fs::exists(entry->localPath)) {
        if (entry->isRaw) {
            if (RawLoader::loadFloat(entry->localPath, fullPixels_)) {
                // Apply lens correction on CPU before GPU upload
                if (lensEnabled_ && entry->focalLength > 0 && entry->aperture > 0) {
                    if (lensCorrector_.setup(
                            entry->cameraMake, entry->camera,
                            entry->lens, entry->focalLength, entry->aperture,
                            fullPixels_.getWidth(), fullPixels_.getHeight())) {
                        lensCorrector_.apply(fullPixels_);
                    }
                }
                fullTexture_.allocate(fullPixels_, TextureUsage::Immutable);
                isRawImage_ = true;
                loaded = true;
            }
        } else {
            if (fullImage_.load(entry->localPath)) {
                isRawImage_ = false;
                loaded = true;
            }
        }
    }

    if (loaded) {
        selectedIndex_ = index;
        viewMode_ = ViewMode::Single;
        zoomLevel_ = 1.0f;
        panOffset_ = {0, 0};
        grid_->setActive(false);
        loadProfileForEntry(*entry);
    } else {
        logWarning() << "Failed to load: " << entry->localPath;
    }
}

void tcApp::exitFullImage() {
    viewMode_ = ViewMode::Grid;

    if (isRawImage_) {
        fullPixels_.clear();
        fullTexture_.clear();
    } else {
        fullImage_ = Image();
    }
    isRawImage_ = false;
    selectedIndex_ = -1;

    // Clear profile LUT
    hasProfileLut_ = false;
    profileLut_.clear();
    currentProfilePath_.clear();

    grid_->setActive(true);
}

void tcApp::drawSingleView() {
    bool hasImage = isRawImage_ ? fullTexture_.isAllocated() : fullImage_.isAllocated();
    if (!hasImage) return;

    float imgW = isRawImage_ ? fullTexture_.getWidth() : fullImage_.getWidth();
    float imgH = isRawImage_ ? fullTexture_.getHeight() : fullImage_.getHeight();
    float winW = getWindowWidth();
    float winH = getWindowHeight();

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
            lutShader_.setTexture(fullTexture_);
            lutShader_.draw(x, y, drawW, drawH);
        } else {
            lutShader_.setTexture(fullImage_.getTexture());
            lutShader_.draw(x, y, drawW, drawH);
        }
    } else if (isRawImage_) {
        fullTexture_.draw(x, y, drawW, drawH);
    } else {
        fullImage_.draw(x, y, drawW, drawH);
    }

    // Info overlay
    if (selectedIndex_ >= 0 && selectedIndex_ < (int)grid_->getPhotoIdCount()) {
        const string& photoId = grid_->getPhotoId(selectedIndex_);
        auto* entry = provider_.getPhoto(photoId);
        if (entry) {
            setColor(0.8f, 0.8f, 0.85f);
            string typeStr = entry->isRaw ? " [RAW]" : "";
            drawBitmapString(entry->filename + typeStr, 10, 20);
            drawBitmapString(format("{}x{}  Zoom: {:.0f}%",
                (int)imgW, (int)imgH, zoomLevel_ * 100), 10, 40);

            // Camera / Lens / Shooting info
            string metaLine;
            if (!entry->camera.empty()) {
                metaLine = entry->camera;
            }
            if (!entry->lens.empty()) {
                if (!metaLine.empty()) metaLine += " | ";
                metaLine += entry->lens;
                if (entry->focalLength > 0) {
                    metaLine += format(" @{:.0f}mm", entry->focalLength);
                }
            }
            if (entry->aperture > 0) {
                metaLine += format(" f/{:.1f}", entry->aperture);
            }
            if (entry->iso > 0) {
                metaLine += format(" ISO{:.0f}", entry->iso);
            }
            if (!entry->creativeStyle.empty()) {
                metaLine += " | " + entry->creativeStyle;
            }
            if (!metaLine.empty()) {
                setColor(0.7f, 0.7f, 0.75f);
                drawBitmapString(metaLine, 10, 60);
            }

            // Profile status
            if (hasProfileLut_) {
                string profileStatus = format("Profile: {} {:.0f}%",
                    profileEnabled_ ? "ON" : "OFF", profileBlend_ * 100);
                setColor(0.5f, 0.75f, 0.5f);
                drawBitmapString(profileStatus, 10, 80);
            }
        }
    }

    float helpY = hasProfileLut_ ? 100.0f : 80.0f;
    setColor(0.5f, 0.5f, 0.55f);
    string helpStr = "ESC: Back  Left/Right: Navigate  Scroll: Zoom  Drag: Pan  L: Lens";
    if (hasProfileLut_) helpStr += "  P: Profile  [/]: Blend";
    drawBitmapString(helpStr, 10, helpY);
}
