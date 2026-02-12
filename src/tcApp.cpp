#include "tcApp.h"
#include <tcxLibRaw.h>

void tcApp::setup() {
    // 0. Ensure OS bootstrap directory exists
    AppPaths::ensureAppConfigDir();

    // 1. Determine catalog path
    // Priority: --catalog arg > bootstrap lastCatalogPath > GUI dialog
    bootstrap_.load(AppPaths::appConfigPath());

    if (!AppConfig::catalogDir.empty()) {
        catalogPath_ = AppConfig::catalogDir;
    } else if (!AppConfig::chooseCatalog &&
               !bootstrap_.lastCatalogPath.empty() &&
               fs::exists(bootstrap_.lastCatalogPath)) {
        catalogPath_ = bootstrap_.lastCatalogPath;
    } else if (AppConfig::serverMode) {
        // Server mode without catalog: use default
        string home = getenv("HOME") ? getenv("HOME") : ".";
        catalogPath_ = home + "/Pictures/TrussPhoto";
    } else {
        // GUI: ask user to select or create catalog folder
        auto result = loadDialog(
            "Select Catalog Folder",
            "Choose where to store your TrussPhoto catalog",
            "", true);

        if (result.success) {
            catalogPath_ = result.filePath;
        } else {
            string home = getenv("HOME") ? getenv("HOME") : ".";
            catalogPath_ = home + "/Pictures/TrussPhoto";
        }
    }

    // 2. Ensure catalog directories exist + migrate from legacy paths
    AppPaths::ensureCatalogDirectories(catalogPath_);
    AppPaths::migrateFromLegacy(catalogPath_);

    // 3. Load catalog settings
    catalogSettings_.load(catalogPath_ + "/catalog.json");

    // 4. Determine RAW storage path
    string rawStorage = catalogSettings_.rawStoragePath;
    if (!AppConfig::rawDir.empty()) {
        rawStorage = AppConfig::rawDir;
    }
    if (rawStorage.empty()) {
        rawStorage = catalogPath_ + "/originals";
    }
    fs::create_directories(rawStorage);

    // 5. Configure provider
    provider_.setCatalogDir(catalogPath_);
    provider_.setRawStoragePath(rawStorage);
    provider_.setJsonMigrationPath(catalogPath_ + "/library.json");
    provider_.setServerUrl(catalogSettings_.serverUrl);
    provider_.setApiKey(catalogSettings_.apiKey);

    // 6. Load library (instant display from previous session)
    bool hasLibrary = provider_.loadLibrary();

    // 7. Save bootstrap (remember catalog path for next launch)
    bootstrap_.lastCatalogPath = catalogPath_;
    bootstrap_.save(AppPaths::appConfigPath());

    if (AppConfig::serverMode) {
        // === Server mode setup ===
        serverConfig_.load(catalogPath_ + "/server_config.json");
        serverConfig_.generateKeyIfMissing();
        serverConfig_.save();

        int port = AppConfig::serverPort;
        server_.setup(provider_, catalogPath_ + "/thumbnail_cache");
        server_.start(port, serverConfig_.apiKey);

        logNotice() << "=== TrussPhoto Server ===";
        logNotice() << "Port: " << port;
        logNotice() << "API Key: " << serverConfig_.apiKey;
        logNotice() << "Catalog: " << catalogPath_;
        logNotice() << "RAW Storage: " << rawStorage;
        logNotice() << "Photos: " << provider_.getCount();
        return;
    }

    // === GUI mode setup (below) ===

    // 5. Create grid
    grid_ = make_shared<PhotoGrid>();
    addChild(grid_);

    // 5b. Create folder tree sidebar
    folderTree_ = make_shared<FolderTree>();
    addChild(folderTree_);

    folderTree_->onFolderSelected = [this](const string& path) {
        grid_->setFilterPath(path);
        grid_->populate(provider_);
        redraw();
    };

    updateLayout();

    grid_->onItemClick = [this](int index) {
        if (cmdDown_ && shiftDown_) {
            // Shift+Cmd+click: range select/deselect from anchor
            int anchor = grid_->getSelectionAnchor();
            if (anchor >= 0) {
                // If clicked item is already selected, deselect the range
                bool select = !grid_->isSelected(index);
                grid_->selectRange(anchor, index, select);
            } else {
                grid_->toggleSelection(index);
            }
        } else if (cmdDown_) {
            // Cmd+click: toggle single
            grid_->toggleSelection(index);
        } else {
            // Normal click: clear selection and open full view
            if (grid_->hasSelection()) {
                grid_->clearSelection();
            }
            showFullImage(index);
        }
    };

    // Display previous library immediately
    if (hasLibrary && provider_.getCount() > 0) {
        grid_->populate(provider_);
        rebuildFolderTree();
    }

    // 6. Start upload queue (only if server configured)
    if (catalogSettings_.hasServer()) {
        uploadQueue_.setServerUrl(catalogSettings_.serverUrl);
        uploadQueue_.setApiKey(catalogSettings_.apiKey);
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

    mcp::tool("set_server", "Set server URL and API key (empty URL to disable)")
        .arg<string>("url", "Server URL (e.g. http://localhost:8080)")
        .arg<string>("apiKey", "API key for authentication", false)
        .bind([this](const json& args) {
            string url = args.at("url").get<string>();
            string key = args.contains("apiKey") ? args["apiKey"].get<string>() : "";
            configureServer(url, key);
            return json{{"status", "ok"}, {"serverUrl", url}};
        });

    mcp::tool("repair_library", "Validate library and scan for unregistered files")
        .bind([this]() {
            int missing = provider_.validateLibrary();
            int added = provider_.scanLibraryFolder();
            if (missing > 0 || added > 0) {
                grid_->populate(provider_);
                rebuildFolderTree();
            }
            if (catalogSettings_.hasServer() && !syncInProgress_) {
                needsServerSync_ = true;
            }
            return json{
                {"status", "ok"},
                {"missing", missing},
                {"added", added},
                {"total", (int)provider_.getCount()}
            };
        });

    mcp::tool("set_rating", "Set rating for a photo (0-5)")
        .arg<string>("id", "Photo ID")
        .arg<int>("rating", "Rating value 0-5")
        .bind([this](const json& args) {
            string id = args.at("id").get<string>();
            int rating = args.at("rating").get<int>();
            if (!provider_.setRating(id, rating)) {
                return json{{"status", "error"}, {"message", "Photo not found"}};
            }
            return json{{"status", "ok"}, {"id", id}, {"rating", rating}};
        });

    mcp::tool("set_memo", "Set memo/description for a photo")
        .arg<string>("id", "Photo ID")
        .arg<string>("memo", "Memo text (markdown)")
        .bind([this](const json& args) {
            string id = args.at("id").get<string>();
            string memo = args.at("memo").get<string>();
            if (!provider_.setMemo(id, memo)) {
                return json{{"status", "error"}, {"message", "Photo not found"}};
            }
            return json{{"status", "ok"}, {"id", id}};
        });

    mcp::tool("set_tags", "Set tags for a photo")
        .arg<string>("id", "Photo ID")
        .arg<string>("tags", "JSON array of tag strings")
        .bind([this](const json& args) {
            string id = args.at("id").get<string>();
            string tags = args.at("tags").get<string>();
            if (!provider_.setTags(id, tags)) {
                return json{{"status", "error"}, {"message", "Photo not found"}};
            }
            return json{{"status", "ok"}, {"id", id}};
        });

    mcp::tool("consolidate_library", "Reorganize library into date-based directory structure")
        .bind([this]() {
            if (provider_.isConsolidateRunning()) {
                return json{{"status", "error"}, {"message", "Already running"}};
            }
            provider_.consolidateLibrary();
            return json{
                {"status", "ok"},
                {"total", provider_.getConsolidateTotal()}
            };
        });

    mcp::tool("generate_smart_previews", "Generate smart previews for all photos without one")
        .bind([this]() {
            int queued = provider_.queueAllMissingSP();
            return json{
                {"status", "ok"},
                {"queued", queued}
            };
        });

    mcp::tool("relink_photos", "Find and relink missing photos from a folder")
        .arg<string>("folder", "Folder path to search for missing files")
        .bind([this](const string& folder) {
            int missing = provider_.validateLibrary();
            int relinked = provider_.relinkFromFolder(folder);
            if (relinked > 0 && grid_) {
                grid_->populate(provider_);
                rebuildFolderTree();
            }
            return json{
                {"status", "ok"},
                {"missing", missing},
                {"relinked", relinked}
            };
        });

    // 9. Camera profile manager
    string home = getenv("HOME") ? getenv("HOME") : ".";
    profileManager_.setProfileDir(home + "/.trussc/profiles");

    // 10. LUT shader
    lutShader_.load();

    // 11. Lens correction database
    lensCorrector_.loadDatabase(getDataPath("lensfun"));

    // 12. Setup event driven mode
    setIndependentFps(VSYNC, 0);

    logNotice() << "TrussPhoto ready - Catalog: " << catalogPath_;
}

void tcApp::update() {
    if (AppConfig::serverMode) {
        // Server mode: minimal update
        provider_.processCopyResults();
        return;
    }

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
            rebuildFolderTree();
            redraw();
        }
    }

    // Process background file copies
    provider_.processCopyResults();

    // Process smart preview generation results
    provider_.processSPResults();

    // Process consolidation results
    provider_.processConsolidateResults();

    // Process background RAW load completion
    if (rawLoadCompleted_ && viewMode_ == ViewMode::Single && isRawImage_) {
        // Only apply if we're still viewing the same image
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

                // Generate smart preview if not yet done (uses decoded rawPixels_)
                const string& spId = grid_->getPhotoId(selectedIndex_);
                if (!provider_.hasSmartPreview(spId)) {
                    provider_.generateSmartPreview(spId, rawPixels_);
                }

                redraw();
            }
        } else {
            // User switched to different image, discard the loaded data
            lock_guard<mutex> lock(rawLoadMutex_);
            pendingRawPixels_.clear();
        }
        rawLoadCompleted_ = false;
    }

    // Process upload results
    UploadResult uploadResult;
    while (uploadQueue_.tryGetResult(uploadResult)) {
        SyncState newState = uploadResult.success
            ? SyncState::Synced
            : SyncState::LocalOnly;
        provider_.setSyncState(uploadResult.photoId, newState);
    }

    // Update sync state badges
    if (grid_ && grid_->updateSyncStates(provider_)) {
        redraw();
    }

    // Periodic server sync (every ~30 seconds at 60fps, only if server configured)
    static int syncCounter = 0;
    if (catalogSettings_.hasServer() && ++syncCounter % 1800 == 0 && !syncInProgress_) {
        needsServerSync_ = true;
    }

    // SQLite writes are immediate, no periodic save needed
}

void tcApp::draw() {
    if (AppConfig::serverMode) return;

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
    string consolidateStatus;
    if (provider_.isConsolidateRunning()) {
        consolidateStatus = format("  Consolidate: {}/{}",
            provider_.getConsolidateProgress(), provider_.getConsolidateTotal());
    }
    drawBitmapString(format("{}  Photos: {}{}{}  FPS: {:.0f}",
        serverLabel, provider_.getCount(), uploadStatus, consolidateStatus, getFrameRate()),
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
        } else if (key >= '0' && key <= '5') {
            // Rating shortcut: 0=unrated, 1-5=★
            if (selectedIndex_ >= 0 && selectedIndex_ < (int)grid_->getPhotoIdCount()) {
                const string& photoId = grid_->getPhotoId(selectedIndex_);
                int rating = key - '0';
                provider_.setRating(photoId, rating);
                logNotice() << "[Rating] " << photoId << " -> " << rating;
            }
        } else if (key == 'Z' || key == 'z') {
            zoomLevel_ = 1.0f;
            panOffset_ = {0, 0};
        } else if (key == 'S' || key == 's') {
            // Debug: force load smart preview (even if RAW available)
            if (selectedIndex_ >= 0 && selectedIndex_ < (int)grid_->getPhotoIdCount()) {
                const string& photoId = grid_->getPhotoId(selectedIndex_);
                auto* spEntry = provider_.getPhoto(photoId);
                Pixels spPixels;
                if (spEntry && provider_.loadSmartPreview(photoId, spPixels)) {
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
        } else if (key == 'L' || key == 'l') {
            lensEnabled_ = !lensEnabled_;
            logNotice() << "[Lensfun] " << (lensEnabled_ ? "ON" : "OFF");
            // Re-process from cached RAW pixels (no re-decode)
            if (selectedIndex_ >= 0 && isRawImage_ && rawPixels_.isAllocated()) {
                reprocessImage();
            }
        }
    } else {
        // Grid mode keys
        if (key == SAPP_KEYCODE_BACKSPACE || key == SAPP_KEYCODE_DELETE) {
            deleteSelectedPhotos();
        } else if (key == SAPP_KEYCODE_ESCAPE) {
            if (grid_ && grid_->hasSelection()) {
                grid_->clearSelection();
            }
        } else if (key == 'A' || key == 'a') {
            if (cmdDown_ && grid_) {
                if (shiftDown_) {
                    grid_->clearSelection();
                } else {
                    grid_->selectAll();
                }
            }
        } else if (key == 'R' || key == 'r') {
            repairLibrary();
        } else if (key == 'C') {  // Shift+C only (uppercase)
            consolidateLibrary();
        }
    }

    // Mode-independent keys
    if (key == 'F' || key == 'f') {
        relinkMissingPhotos();
    }
    if (key == 'T' || key == 't') {
        showSidebar_ = !showSidebar_;
        updateLayout();
    }

    // Track modifier key state
    if (key == SAPP_KEYCODE_LEFT_SUPER || key == SAPP_KEYCODE_RIGHT_SUPER) {
        cmdDown_ = true;
    }
    if (key == SAPP_KEYCODE_LEFT_SHIFT || key == SAPP_KEYCODE_RIGHT_SHIFT) {
        shiftDown_ = true;
    }

    redraw();
}

void tcApp::keyReleased(int key) {
    if (key == SAPP_KEYCODE_LEFT_SUPER || key == SAPP_KEYCODE_RIGHT_SUPER) {
        cmdDown_ = false;
    }
    if (key == SAPP_KEYCODE_LEFT_SHIFT || key == SAPP_KEYCODE_RIGHT_SHIFT) {
        shiftDown_ = false;
    }
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
        redraw();
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
        redraw();
    }
}

void tcApp::mouseScrolled(Vec2 delta) {
    redraw();

    if (viewMode_ != ViewMode::Single) return;

    bool hasFullRaw = isRawImage_ && fullTexture_.isAllocated();
    bool hasPreviewRaw = isRawImage_ && previewTexture_.isAllocated();
    bool hasImage = isRawImage_ ? (hasFullRaw || hasPreviewRaw) : fullImage_.isAllocated();
    if (!hasImage) return;

    {
        Texture* rawTex = hasFullRaw ? &fullTexture_ : &previewTexture_;
        float imgW = isRawImage_ ? rawTex->getWidth() : fullImage_.getWidth();
        float imgH = isRawImage_ ? rawTex->getHeight() : fullImage_.getHeight();
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
    (void)width; (void)height;
    updateLayout();
    redraw();
}

void tcApp::filesDropped(const vector<string>& files) {
    if (files.empty()) return;

    bool added = false;
    vector<string> filesToImport;

    for (const auto& f : files) {
        if (fs::is_directory(f)) {
            provider_.scanFolder(f);
            added = true;
        } else if (provider_.isSupportedFile(f)) {
            filesToImport.push_back(f);
        }
    }

    if (!filesToImport.empty()) {
        provider_.importFiles(filesToImport);
        added = true;
    }

    if (added) {
        grid_->populate(provider_);
        rebuildFolderTree();
        redraw();
        enqueueLocalOnlyPhotos();

        // Queue smart preview generation for new photos
        provider_.queueAllMissingSP();
    }
}

void tcApp::exit() {
    if (AppConfig::serverMode) {
        server_.stop();
    }

    uploadQueue_.stop();
    if (syncThread_.joinable()) syncThread_.join();
    if (rawLoadThread_.joinable()) rawLoadThread_.join();
    // Wait for consolidation to finish before saving
    if (provider_.isConsolidateRunning()) {
        logNotice() << "Waiting for consolidation to finish...";
    }
    provider_.joinConsolidate();
    provider_.processConsolidateResults();
    // SQLite writes are immediate, no final save needed
    logNotice() << "TrussPhoto exiting";
}

void tcApp::enqueueLocalOnlyPhotos() {
    if (!catalogSettings_.hasServer()) return;

    auto localPhotos = provider_.getLocalOnlyPhotos();
    for (const auto& [id, path] : localPhotos) {
        uploadQueue_.enqueue(id, path);
    }
    if (!localPhotos.empty()) {
        logNotice() << "Enqueued " << localPhotos.size() << " photos for upload";
    }
}

void tcApp::configureServer(const string& url, const string& key) {
    catalogSettings_.serverUrl = url;
    if (!key.empty()) {
        catalogSettings_.apiKey = key;
    }
    catalogSettings_.save();

    provider_.setServerUrl(url);
    provider_.setApiKey(catalogSettings_.apiKey);
    provider_.resetServerCheck();

    if (catalogSettings_.hasServer()) {
        // Start upload queue if not running
        uploadQueue_.setServerUrl(url);
        uploadQueue_.setApiKey(catalogSettings_.apiKey);
        uploadQueue_.start();
        // Trigger sync
        needsServerSync_ = true;
        logNotice() << "Server configured: " << url;
    } else {
        uploadQueue_.stop();
        logNotice() << "Server disabled, running in local-only mode";
    }
}

void tcApp::repairLibrary() {
    int missing = provider_.validateLibrary();
    int added = provider_.scanLibraryFolder();
    logNotice() << "[Repair] Missing: " << missing << ", Added: " << added;
    if (missing > 0 || added > 0) {
        grid_->populate(provider_);
        rebuildFolderTree();
        redraw();
    }
    // Trigger server sync to resolve Missing vs ServerOnly
    if (catalogSettings_.hasServer() && !syncInProgress_) {
        needsServerSync_ = true;
    }
}

void tcApp::relinkMissingPhotos() {
    if (viewMode_ == ViewMode::Single && selectedIndex_ >= 0) {
        // Single view: relink current photo via file dialog
        const string& photoId = grid_->getPhotoId(selectedIndex_);
        auto* photo = provider_.getPhoto(photoId);
        if (!photo) return;

        auto result = loadDialog(
            "Find " + photo->filename,
            "Locate: " + photo->filename);

        if (!result.success) return;

        // Verify the file matches by filename + filesize
        fs::path p(result.filePath);
        string fname = p.filename().string();
        uintmax_t fsize = fs::file_size(p);
        string newId = fname + "_" + to_string(fsize);

        if (newId != photoId) {
            logWarning() << "[Relink] Mismatch: expected " << photoId << ", got " << newId;
            return;
        }

        provider_.relinkPhoto(photoId, result.filePath);
        // Reload the image with updated path
        showFullImage(selectedIndex_);
    } else {
        // Grid mode: folder-based bulk relink
        provider_.validateLibrary();

        auto result = loadDialog(
            "Find Missing Photos",
            "Select folder to search for missing files",
            "", true);

        if (!result.success) return;

        int relinked = provider_.relinkFromFolder(result.filePath);
        logNotice() << "[Relink] Relinked " << relinked << " photos";

        if (relinked > 0) {
            grid_->populate(provider_);
            rebuildFolderTree();
            redraw();
        }
    }
}

void tcApp::consolidateLibrary() {
    if (provider_.isConsolidateRunning()) {
        logWarning() << "[Consolidate] Already running";
        return;
    }
    bool ok = confirmDialog("Consolidate Library",
        "Reorganize all files into date-based folders (YYYY/MM/DD)?\n"
        "This will move files within the library folder.");
    if (ok) {
        provider_.consolidateLibrary();
    }
}

void tcApp::deleteSelectedPhotos() {
    if (!grid_ || !grid_->hasSelection()) return;

    auto selectedIds = grid_->getSelectedIds();
    int count = (int)selectedIds.size();

    string msg = format("Delete {} photo{}?\nThis will permanently remove the file{} from disk.",
        count, count > 1 ? "s" : "", count > 1 ? "s" : "");

    bool ok = confirmDialog("Delete Photos", msg);
    if (!ok) return;

    int deleted = provider_.deletePhotos(selectedIds);
    logNotice() << "[Delete] Removed " << deleted << " photos";

    grid_->populate(provider_);
    rebuildFolderTree();
    redraw();
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

    // Cancel any pending background load
    if (rawLoadInProgress_) {
        // Let it finish, we'll ignore the result
        rawLoadCompleted_ = false;
    }

    bool loaded = false;
    isSmartPreview_ = false;

    if (!entry->localPath.empty() && fs::exists(entry->localPath)) {
        if (entry->isRaw) {
            // Step 1: Try embedded JPEG first (fastest, ~10-20ms)
            // Fallback to half-size preview if no embedded JPEG
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
                        // Store lens info for correction on main thread
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
        redraw();
    }

    // Fallback: try smart preview if RAW/file not available
    if (!loaded && provider_.hasSmartPreview(photoId)) {
        Pixels spPixels;
        if (provider_.loadSmartPreview(photoId, spPixels)) {
            rawPixels_ = std::move(spPixels);
            // Setup lens corrector for this photo
            if (entry->focalLength > 0 && entry->aperture > 0) {
                lensCorrector_.setup(entry->cameraMake, entry->camera, entry->lens,
                    entry->focalLength, entry->aperture,
                    rawPixels_.getWidth(), rawPixels_.getHeight());
            }
            reprocessImage();  // applies lens correction → fullTexture_
            previewTexture_.clear();
            isRawImage_ = true;
            isSmartPreview_ = true;
            loaded = true;
            logNotice() << "Loaded smart preview for: " << entry->filename;
            redraw();
        }
    }

    if (loaded) {
        selectedIndex_ = index;
        viewMode_ = ViewMode::Single;
        zoomLevel_ = 1.0f;
        panOffset_ = {0, 0};
        grid_->setActive(false);
        updateLayout();
        loadProfileForEntry(*entry);
    } else {
        logWarning() << "Failed to load: " << entry->localPath;
    }
}

void tcApp::reprocessImage() {
    fullPixels_ = rawPixels_.clone();
    if (lensEnabled_ && lensCorrector_.isReady()) {
        lensCorrector_.apply(fullPixels_);
    }
    fullTexture_.allocate(fullPixels_, TextureUsage::Immutable, true);
}

void tcApp::exitFullImage() {
    viewMode_ = ViewMode::Grid;

    // Wait for background load to finish
    if (rawLoadThread_.joinable()) {
        rawLoadThread_.join();
    }
    rawLoadInProgress_ = false;
    rawLoadCompleted_ = false;

    if (isRawImage_) {
        rawPixels_.clear();
        fullPixels_.clear();
        fullTexture_.clear();
        previewTexture_.clear();
        {
            lock_guard<mutex> lock(rawLoadMutex_);
            pendingRawPixels_.clear();
        }
    } else {
        fullImage_ = Image();
    }
    isRawImage_ = false;
    isSmartPreview_ = false;
    selectedIndex_ = -1;

    // Clear profile LUT
    hasProfileLut_ = false;
    profileLut_.clear();
    currentProfilePath_.clear();

    grid_->setActive(true);
    updateLayout();
    redraw();
}

void tcApp::drawSingleView() {
    // For RAW: prefer fullTexture, fallback to previewTexture
    bool hasFullRaw = isRawImage_ && fullTexture_.isAllocated();
    bool hasPreviewRaw = isRawImage_ && previewTexture_.isAllocated();
    bool hasImage = isRawImage_ ? (hasFullRaw || hasPreviewRaw) : fullImage_.isAllocated();
    if (!hasImage) return;

    Texture* rawTex = hasFullRaw ? &fullTexture_ : &previewTexture_;
    float imgW = isRawImage_ ? rawTex->getWidth() : fullImage_.getWidth();
    float imgH = isRawImage_ ? rawTex->getHeight() : fullImage_.getHeight();
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

    // Info overlay
    if (selectedIndex_ >= 0 && selectedIndex_ < (int)grid_->getPhotoIdCount()) {
        const string& photoId = grid_->getPhotoId(selectedIndex_);
        auto* entry = provider_.getPhoto(photoId);
        if (entry) {
            setColor(0.8f, 0.8f, 0.85f);
            string typeStr = isSmartPreview_ ? " [Smart Preview]" : (entry->isRaw ? " [RAW]" : "");
            drawBitmapStringHighlight(entry->filename + typeStr, 10, 20, Color(0, 0.3));
            drawBitmapStringHighlight(format("{}x{}  Zoom: {:.0f}%",
                (int)imgW, (int)imgH, zoomLevel_ * 100), 10, 40, Color(0, 0.3));

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
                drawBitmapStringHighlight(metaLine, 10, 60, Color(0, 0.3));
            }

            // Rating display
            float infoY = 80.0f;
            if (entry->rating > 0) {
                string stars = "Rating: ";
                for (int i = 0; i < 5; i++) {
                    stars += (i < entry->rating) ? '*' : '.';
                }
                setColor(1.0f, 0.85f, 0.2f);
                drawBitmapStringHighlight(stars, 10, infoY, Color(0, 0.3));
                infoY += 20.0f;
            }

            // Profile status
            if (hasProfileLut_) {
                string profileStatus = format("Profile: {} {:.0f}%",
                    profileEnabled_ ? "ON" : "OFF", profileBlend_ * 100);
                setColor(0.5f, 0.75f, 0.5f);
                drawBitmapStringHighlight(profileStatus, 10, infoY, Color(0, 0.3));
                infoY += 20.0f;
            }

            // Help line
            setColor(0.5f, 0.5f, 0.55f);
            string helpStr = "ESC: Back  Left/Right: Navigate  Scroll: Zoom  Drag: Pan  Z: Reset  0-5: Rating  L: Lens";
            if (hasProfileLut_) helpStr += "  P: Profile  [/]: Blend";
            drawBitmapStringHighlight(helpStr, 10, infoY, Color(0, 0.3));
        }
    }
}

void tcApp::updateLayout() {
    float w = getWindowWidth();
    float h = getWindowHeight();

    if (showSidebar_ && viewMode_ == ViewMode::Grid && folderTree_) {
        folderTree_->setActive(true);
        folderTree_->setRect(0, 0, sidebarWidth_, h);
        if (grid_) grid_->setRect(sidebarWidth_, 0, w - sidebarWidth_, h);
        
    } else {
        if (folderTree_) folderTree_->setActive(false);
        if (grid_) grid_->setRect(0, 0, w, h);
    }
}

void tcApp::rebuildFolderTree() {
    if (!folderTree_) return;
    auto folders = provider_.buildFolderList();
    folderTree_->buildTree(folders, provider_.getRawStoragePath());
}
