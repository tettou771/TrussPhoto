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

    // 6b. Import from Lightroom Classic catalog (if --import-lrcat specified)
    if (!AppConfig::importLrcatPath.empty()) {
        auto result = LrcatImporter::import(AppConfig::importLrcatPath);
        int added = provider_.importReferences(result.entries);
        if (added > 0) hasLibrary = true;

        // Import faces/persons
        int facesAdded = provider_.importFaces(result.faces);

        logNotice() << "[LrcatImport] imported=" << added
                    << " total=" << result.stats.totalImages
                    << " missing_files=" << result.stats.missingFile
                    << " faces=" << facesAdded
                    << " persons=" << result.stats.persons;
    }

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

    // 5a. Create search bar
    searchBar_ = make_shared<SearchBar>();
    addChild(searchBar_);

    searchBar_->onSearch = [this](const string& query) {
        if (!grid_) return;

        if (query.empty()) {
            // Clear all filters
            grid_->clearClipResults();
            grid_->clearTextMatchIds();
            grid_->setTextFilter("");
            grid_->populate(provider_);
        } else if (provider_.isTextEncoderReady()) {
            // CLIP semantic search + text field matching
            auto results = provider_.searchByText(query);

            // Text matches (filename, camera, tags, person names, etc.) go first
            auto textMatches = provider_.searchByTextFields(query);
            int textCount = 0;
            if (!textMatches.empty()) {
                unordered_set<string> clipIds;
                for (const auto& r : results) clipIds.insert(r.photoId);
                float boostScore = results.empty() ? 1.0f : results.front().score + 0.01f;
                for (const auto& id : textMatches) {
                    if (!clipIds.count(id)) {
                        results.insert(results.begin(), {id, boostScore});
                        textCount++;
                    }
                }
            }

            grid_->clearClipResults();
            grid_->setTextFilter("");
            grid_->setTextMatchIds(unordered_set<string>(textMatches.begin(), textMatches.end()));
            grid_->setClipResults(results);
            grid_->populate(provider_);
            logNotice() << "[Search] query=\"" << query
                        << "\" text=" << textCount
                        << " clip=" << results.size() - textCount;
        } else {
            // Fallback to text field matching
            grid_->clearClipResults();
            grid_->clearTextMatchIds();
            grid_->setTextFilter(query);
            grid_->populate(provider_);
        }
        redraw();
    };

    // 5b. Create folder tree sidebar
    folderTree_ = make_shared<FolderTree>();
    addChild(folderTree_);

    folderTree_->onFolderSelected = [this](const string& path) {
        grid_->setFilterPath(path);
        grid_->populate(provider_);
        redraw();
    };

    // 5c. Create map view
    mapView_ = make_shared<MapView>();
    addChild(mapView_);
    mapView_->setTileCacheDir(catalogPath_ + "/tile_cache");
    mapView_->setActive(false);
    mapView_->onPinClick = [this](int index, const string& photoId) {
        // Single click: show thumbnail in metadata panel
        auto* entry = provider_.getPhoto(photoId);
        if (!entry) return;
        if (metadataPanel_) {
            metadataPanel_->setPhoto(entry);
            // Load thumbnail and display
            Pixels thumbPixels;
            if (provider_.getThumbnail(photoId, thumbPixels)) {
                Texture tex;
                tex.allocate(thumbPixels, TextureUsage::Immutable, false);
                metadataPanel_->setThumbnail(std::move(tex));
            }
        }
        redraw();
    };
    mapView_->onPinDoubleClick = [this](int index, const string& photoId) {
        showFullImage(index);
    };
    mapView_->onRedraw = [this]() { redraw(); };

    // 5c2. Create related view
    relatedView_ = make_shared<RelatedView>();
    addChild(relatedView_);
    relatedView_->setActive(false);
    relatedView_->onPhotoClick = [this](const string& photoId) {
        auto* entry = provider_.getPhoto(photoId);
        if (!entry) return;
        if (metadataPanel_) {
            metadataPanel_->setPhoto(entry);
            Pixels thumbPixels;
            if (provider_.getThumbnail(photoId, thumbPixels)) {
                Texture tex;
                tex.allocate(thumbPixels, TextureUsage::Immutable, false);
                metadataPanel_->setThumbnail(std::move(tex));
            }
        }
        redraw();
    };
    relatedView_->onCenterDoubleClick = [this](const string& photoId) {
        // Center double-click → open in single view
        for (int i = 0; i < (int)grid_->getPhotoIdCount(); i++) {
            if (grid_->getPhotoId(i) == photoId) {
                showFullImage(i);
                return;
            }
        }
    };
    relatedView_->onRedraw = [this]() { redraw(); };

    // 5c3. Create people view
    peopleView_ = make_shared<PeopleView>();
    addChild(peopleView_);
    peopleView_->setActive(false);
    peopleView_->onPersonClick = [this](const PhotoProvider::FaceCluster& cluster) {
        // People → Grid (filtered by person's photos)
        auto photoIds = provider_.getPhotoIdsForFaceIds(cluster.faceIds);
        exitPeopleView();
        grid_->clearClipResults();
        grid_->clearTextMatchIds();
        grid_->setTextFilter("");
        grid_->setFilterPhotoIds(unordered_set<string>(photoIds.begin(), photoIds.end()));
        grid_->populate(provider_);
        redraw();
    };
    peopleView_->onRedraw = [this]() { redraw(); };

    // 5d. Create metadata panel (right sidebar)
    metadataPanel_ = make_shared<MetadataPanel>();
    addChild(metadataPanel_);

    // 5e. Create pane toggle buttons
    leftToggle_ = make_shared<PaneToggle>();
    addChild(leftToggle_);
    leftToggle_->onClick = [this]() {
        showSidebar_ = !showSidebar_;
        float from = leftPaneWidth_;
        float to = showSidebar_ ? sidebarWidth_ : 0;
        leftTween_.from(from).to(to).duration(0.2f).ease(EaseType::Cubic, EaseMode::Out).start();
    };

    rightToggle_ = make_shared<PaneToggle>();
    addChild(rightToggle_);
    rightToggle_->onClick = [this]() {
        showMetadata_ = !showMetadata_;
        float from = rightPaneWidth_;
        float to = showMetadata_ ? metadataWidth_ : 0;
        rightTween_.from(from).to(to).duration(0.2f).ease(EaseType::Cubic, EaseMode::Out).start();
    };

    // Initialize tween state
    lastTime_ = getElapsedTime();
    leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
    rightPaneWidth_ = showMetadata_ ? metadataWidth_ : 0;

    updateLayout();

    grid_->onItemClick = [this](int index) {
        auto now = chrono::steady_clock::now();
        bool isDoubleClick = (index == lastClickIndex_ &&
            chrono::duration_cast<chrono::milliseconds>(now - lastClickTime_).count() < 400);
        lastClickTime_ = now;
        lastClickIndex_ = index;

        if (cmdDown_ && shiftDown_) {
            // Shift+Cmd+click: range select/deselect from anchor
            int anchor = grid_->getSelectionAnchor();
            if (anchor >= 0) {
                bool select = !grid_->isSelected(index);
                grid_->selectRange(anchor, index, select);
            } else {
                grid_->toggleSelection(index);
            }
            updateMetadataPanel();
        } else if (cmdDown_) {
            // Cmd+click: toggle single
            grid_->toggleSelection(index);
            updateMetadataPanel();
        } else if (isDoubleClick) {
            // Double click: open full view
            grid_->clearSelection();
            showFullImage(index);
        } else {
            // Single click: select item
            grid_->clearSelection();
            grid_->toggleSelection(index);
            updateMetadataPanel();
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

    mcp::tool("generate_embeddings", "Generate CLIP embeddings for all photos without one")
        .bind([this]() {
            if (!provider_.isEmbedderReady()) {
                return json{{"status", "error"}, {"message", "Embedder not ready"}};
            }
            int queued = provider_.queueAllMissingEmbeddings();
            return json{
                {"status", "ok"},
                {"queued", queued}
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

    // 9. CLIP embedder (async: downloads model in background if needed)
    provider_.initEmbedder(AppPaths::modelsDir());

    // 9.5 Face detection models (InsightFace SCRFD + ArcFace)
    {
        string insightDir = getDataPath("insightface/");
        provider_.initFaceModels(insightDir + "det_10g.onnx",
                                 insightDir + "w600k_r50.onnx");
    }

    // 10. Camera profile manager
    string home = getenv("HOME") ? getenv("HOME") : ".";
    profileManager_.setProfileDir(home + "/.trussc/profiles");

    // 11. Fonts
    loadJapaneseFont(font_, 14);
    loadJapaneseFont(fontSmall_, 12);

    // 12. LUT shader
    lutShader_.load();

    // 13. Lens correction database
    lensCorrector_.loadDatabase(getDataPath("lensfun"));

    // 14. Setup event driven mode
    setIndependentFps(VSYNC, 0);

    logNotice() << "TrussPhoto ready - Catalog: " << catalogPath_;
}

void tcApp::update() {
    if (AppConfig::serverMode) {
        // Server mode: minimal update
        provider_.processCopyResults();
        return;
    }

    // Animate pane tweens
    {
        double now = getElapsedTime();
        float dt = (float)(now - lastTime_);
        lastTime_ = now;

        bool animating = false;
        if (leftTween_.isPlaying()) {
            leftTween_.update(dt);
            leftPaneWidth_ = leftTween_.getValue();
            animating = true;
        }
        if (rightTween_.isPlaying()) {
            rightTween_.update(dt);
            rightPaneWidth_ = rightTween_.getValue();
            animating = true;
        }
        if (animating) {
            updateLayout();
            redraw();
        }
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

    // Auto-queue SP generation on startup (one-shot)
    if (!spQueued_) {
        spQueued_ = true;
        int queued = provider_.queueAllMissingSP();
        if (queued > 0) {
            logNotice() << "[SmartPreview] Auto-queued " << queued << " photos";
        }
    }

    // Process embedding generation results
    provider_.processEmbeddingResults();

    // When embedder becomes ready, load cache and queue missing embeddings
    if (provider_.isEmbedderReady() && !embeddingsQueued_) {
        embeddingsQueued_ = true;
        provider_.loadEmbeddingCache();
        int queued = provider_.queueAllMissingEmbeddings();
        if (queued > 0) {
            logNotice() << "[CLIP] Queued " << queued << " photos for embedding";
        }
    }

    // Unload vision model after all embeddings are generated (free ~340MB)
    if (embeddingsQueued_ && !visionModelUnloaded_ &&
        !provider_.isEmbeddingRunning() && provider_.isEmbedderReady()) {
        visionModelUnloaded_ = true;
        provider_.unloadVisionModel();
        logNotice() << "[CLIP] Vision model unloaded (all embeddings done)";
    }

    // Face detection pipeline: process results + auto re-queue
    // (no one-shot flag: checks every frame, but faceScanned bool makes iteration cheap)
    provider_.processFaceDetectionResults();

    if (provider_.isFaceModelsReady() && !provider_.isFaceDetectionRunning()) {
        int queued = provider_.queueAllMissingFaceDetections();
        if (queued > 0) {
            logNotice() << "[FaceDetection] Queued " << queued << " photos";
        }
    }

    // Redraw during background tasks (model loading, SP, embedding, face detection)
    if (provider_.isEmbedderInitializing() || provider_.isSPGenerationRunning() ||
        provider_.isEmbeddingRunning() || provider_.isFaceDetectionRunning()) {
        redraw();
    }


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

                // Update metadata panel view info
                if (metadataPanel_) {
                    metadataPanel_->setViewInfo(zoomLevel_, profileEnabled_, profileBlend_,
                        hasProfileLut_, lensEnabled_, isSmartPreview_);
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
    } else if (viewMode_ == ViewMode::Map) {
        // MapView draws itself via Node tree
    } else if (viewMode_ == ViewMode::Related) {
        // RelatedView draws itself via Node tree
    } else if (viewMode_ == ViewMode::People) {
        // PeopleView draws itself via Node tree
    } else {
        // Show hint if no images
        if (provider_.getCount() == 0) {
            float leftW = leftPaneWidth_;
            float rightW = rightPaneWidth_;
            float contentW = getWindowWidth() - leftW - rightW;
            float centerX = leftW + contentW * 0.5f;

            setColor(0.5f, 0.5f, 0.55f);
            string hint = "Drop a folder containing images";
            font_.drawString(hint, centerX, (getWindowHeight() - statusBarHeight_) / 2.0f,
                Direction::Center, Direction::Center);
        }
    }

    // Model download / loading progress
    if (provider_.isEmbedderInitializing()) {
        pushStyle();
        float leftW = leftPaneWidth_;
        float rightW = rightPaneWidth_;
        float contentW = getWindowWidth() - leftW - rightW;
        float centerX = leftW + contentW * 0.5f;
        float centerY = getWindowHeight() * 0.5f;

        // Status text
        setColor(0.6f, 0.6f, 0.65f);
        fontSmall_.drawString(provider_.getEmbedderStatus(),
            centerX, centerY - 20, Direction::Center, Direction::Center);

        popStyle();
    }

    // Status bar background
    float barHeight = 24;
    float barY = getWindowHeight() - barHeight;
    setColor(0.1f, 0.1f, 0.12f);
    fill();
    drawRect(0, barY, getWindowWidth(), barHeight);

    // Server status indicator
    float textX = 10;
    float textY = barY + barHeight / 2;
    string serverLabel;

    if (catalogSettings_.hasServer()) {
        // Show dot badge only when server is configured
        fill();
        if (provider_.isServerConnected()) {
            setColor(0.3f, 0.8f, 0.4f);  // green
            serverLabel = "Server";
        } else {
            setColor(0.6f, 0.35f, 0.35f);  // dim red
            serverLabel = "Offline";
        }
        drawCircle(textX + 4, textY, 4);
        textX += 14;
    } else {
        serverLabel = "Local";
    }

    // Status text
    setColor(0.55f, 0.55f, 0.6f);
    size_t pending = uploadQueue_.getPendingCount();
    string uploadStatus = pending > 0 ? format("  Upload: {}", pending) : "";
    string consolidateStatus;
    if (provider_.isConsolidateRunning()) {
        consolidateStatus = format("  Consolidate: {}/{}",
            provider_.getConsolidateProgress(), provider_.getConsolidateTotal());
    }
    string spStatus;
    if (provider_.isSPGenerationRunning()) {
        spStatus = format("  SP: {}/{}",
            provider_.getSPCompletedCount(), provider_.getSPTotalCount());
    }
    string embeddingStatus;
    if (provider_.isEmbeddingRunning()) {
        embeddingStatus = format("  Embedding: {}/{}",
            provider_.getEmbeddingCompletedCount(),
            provider_.getEmbeddingTotalCount());
    }
    string faceStatus;
    if (provider_.isFaceDetectionRunning()) {
        faceStatus = format("  Faces: {}/{}",
            provider_.getFaceDetectionCompletedCount(),
            provider_.getFaceDetectionTotalCount());
    }
    fontSmall_.drawString(format("{}  Photos: {}{}{}{}{}{}  FPS: {:.0f}",
        serverLabel, provider_.getCount(), uploadStatus, consolidateStatus,
        spStatus, embeddingStatus, faceStatus, getFrameRate()),
        textX, textY, Direction::Left, Direction::Center);
}

void tcApp::keyPressed(int key) {
    redraw(3);

    if (viewMode_ == ViewMode::Single) {
        if (key == SAPP_KEYCODE_LEFT && selectedIndex_ > 0) {
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

        if (key == 'V' || key == 'v') {
            // Enter related view from single view
            if (selectedIndex_ >= 0 && selectedIndex_ < (int)grid_->getPhotoIdCount()) {
                const string& photoId = grid_->getPhotoId(selectedIndex_);
                if (provider_.getCachedEmbedding(photoId)) {
                    exitFullImage();
                    enterRelatedView(photoId);
                }
            }
        }

        // Update metadata panel with current state
        if (metadataPanel_ && selectedIndex_ >= 0 && selectedIndex_ < (int)grid_->getPhotoIdCount()) {
            const string& pid = grid_->getPhotoId(selectedIndex_);
            auto* e = provider_.getPhoto(pid);
            if (e) metadataPanel_->setPhoto(e);
            metadataPanel_->setViewInfo(zoomLevel_, profileEnabled_, profileBlend_,
                hasProfileLut_, lensEnabled_, isSmartPreview_);
        }
    } else if (viewMode_ == ViewMode::Map) {
        // G key handles return to grid (mode-independent below)
    } else if (viewMode_ == ViewMode::Related) {
        // G key handles return to grid (mode-independent below)
    } else if (viewMode_ == ViewMode::People) {
        // ESC → return to grid
        if (key == SAPP_KEYCODE_ESCAPE) {
            exitPeopleView();
        }
    } else {
        // Grid mode: if search bar is active, only handle ESC
        if (searchBar_ && searchBar_->isActive()) {
            if (key == SAPP_KEYCODE_ESCAPE) {
                searchBar_->deactivate();
            }
            // Let IME handle all other keys
            redraw();
            return;
        }

        // Grid mode keys
        if (key == SAPP_KEYCODE_BACKSPACE || key == SAPP_KEYCODE_DELETE) {
            deleteSelectedPhotos();
        } else if (key == SAPP_KEYCODE_ESCAPE) {
            if (searchBar_ && !searchBar_->getQuery().empty()) {
                searchBar_->clear();
                grid_->clearClipResults();
                grid_->clearFilterPhotoIds();
                grid_->populate(provider_);
            } else if (grid_ && grid_->hasSelection()) {
                grid_->clearSelection();
                updateMetadataPanel();
            } else if (grid_ && grid_->hasFilterPhotoIds()) {
                // Clear person/face filter
                grid_->clearFilterPhotoIds();
                grid_->populate(provider_);
            }
        } else if (key == SAPP_KEYCODE_SLASH) {
            // '/' to activate search
            if (searchBar_) searchBar_->activate();
        } else if (key == 'A' || key == 'a') {
            if (cmdDown_ && grid_) {
                if (shiftDown_) {
                    grid_->clearSelection();
                } else {
                    grid_->selectAll();
                }
                updateMetadataPanel();
            }
        } else if (key == 'V' || key == 'v') {
            // Related view: requires exactly 1 selected photo with embedding
            if (grid_ && grid_->getSelectionCount() == 1) {
                auto ids = grid_->getSelectedIds();
                if (!ids.empty() && provider_.getCachedEmbedding(ids[0])) {
                    enterRelatedView(ids[0]);
                }
            }
        } else if (key == 'R' || key == 'r') {
            repairLibrary();
        } else if (key == 'C') {  // Shift+C only (uppercase)
            consolidateLibrary();
        }
    }

    // Mode-independent keys
    if (key == 'G' || key == 'g') {
        if (viewMode_ == ViewMode::Single) {
            exitFullImage();
        } else if (viewMode_ == ViewMode::Map) {
            viewMode_ = ViewMode::Grid;
            mapView_->setActive(false);
            grid_->setActive(true);
            if (metadataPanel_) metadataPanel_->clearThumbnail();
            updateLayout();
        } else if (viewMode_ == ViewMode::Related) {
            exitRelatedView();
        } else if (viewMode_ == ViewMode::People) {
            exitPeopleView();
        }
    }
    if (key == 'O' || key == 'o') {
        if (viewMode_ == ViewMode::People) {
            exitPeopleView();
        } else if (viewMode_ == ViewMode::Grid) {
            enterPeopleView();
        }
    }
    if (key == 'M' || key == 'm') {
        if (viewMode_ == ViewMode::Related) {
            exitRelatedView();
        }
        if (viewMode_ == ViewMode::People) {
            exitPeopleView();
        }
        if (viewMode_ == ViewMode::Single || viewMode_ == ViewMode::Grid) {
            // Clean up single view resources if needed
            if (viewMode_ == ViewMode::Single) {
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

            // Switch to map
            viewMode_ = ViewMode::Map;
            grid_->setActive(false);
            if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
            if (metadataPanel_) {
                metadataPanel_->clearViewInfo();
                metadataPanel_->clearThumbnail();
            }

            // Populate map pins from current photo list
            vector<string> ids;
            vector<PhotoEntry> photos;
            for (size_t i = 0; i < grid_->getPhotoIdCount(); i++) {
                const string& id = grid_->getPhotoId((int)i);
                ids.push_back(id);
                auto* e = provider_.getPhoto(id);
                if (e) photos.push_back(*e);
                else photos.push_back(PhotoEntry{});
            }
            mapView_->setPhotos(photos, ids);
            mapView_->setActive(true);
            mapView_->fitBounds();
            updateLayout();
        }
    }
    if ((key == 'F' || key == 'f') && cmdDown_) {
        // Cmd+F: activate search bar (grid mode only)
        if (viewMode_ == ViewMode::Grid && searchBar_) {
            searchBar_->activate();
        }
    } else if (key == 'F' || key == 'f') {
        relinkMissingPhotos();
    }
    if (key == 'T' || key == 't') {
        showSidebar_ = !showSidebar_;
        float from = leftPaneWidth_;
        float to = showSidebar_ ? sidebarWidth_ : 0;
        leftTween_.from(from).to(to).duration(0.2f).ease(EaseType::Cubic, EaseMode::Out).start();
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
    if (viewMode_ == ViewMode::Map || viewMode_ == ViewMode::Related || viewMode_ == ViewMode::People) return;  // handled via Node events
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
    if (viewMode_ == ViewMode::Map || viewMode_ == ViewMode::Related || viewMode_ == ViewMode::People) return;
    if (viewMode_ == ViewMode::Single && button == 0 && isDragging_) {
        Vec2 delta = pos - dragStart_;
        panOffset_ = panOffset_ + delta;
        dragStart_ = pos;
        redraw();
    }
}

void tcApp::mouseScrolled(Vec2 delta) {
    redraw();

    // Map/Related/People view handles its own scroll via Node events
    if (viewMode_ == ViewMode::Map || viewMode_ == ViewMode::Related || viewMode_ == ViewMode::People) return;
    if (viewMode_ != ViewMode::Single) return;

    bool hasFullRaw = isRawImage_ && fullTexture_.isAllocated();
    bool hasPreviewRaw = isRawImage_ && previewTexture_.isAllocated();
    bool hasImage = isRawImage_ ? (hasFullRaw || hasPreviewRaw) : fullImage_.isAllocated();
    if (!hasImage) return;

    {
        Texture* rawTex = hasFullRaw ? &fullTexture_ : &previewTexture_;
        float imgW = isRawImage_ ? rawTex->getWidth() : fullImage_.getWidth();
        float imgH = isRawImage_ ? rawTex->getHeight() : fullImage_.getHeight();
        float rightW = rightPaneWidth_;
        float winW = getWindowWidth() - rightW;
        float winH = getWindowHeight() - statusBarHeight_;

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

        // Queue embedding generation for new photos
        provider_.queueAllMissingEmbeddings();

        // Queue face detection for new photos (requires SP)
        provider_.queueAllMissingFaceDetections();
    }
}

void tcApp::exit() {
    if (AppConfig::serverMode) {
        server_.stop();
    }

    if (mapView_) mapView_->shutdown();
    if (relatedView_) relatedView_->shutdown();
    if (peopleView_) peopleView_->shutdown();
    uploadQueue_.stop();
    if (syncThread_.joinable()) syncThread_.join();
    if (rawLoadThread_.joinable()) rawLoadThread_.join();
    // Signal all background threads to stop, then join
    provider_.shutdown();
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
        if (mapView_) mapView_->setActive(false);
        if (relatedView_) relatedView_->setActive(false);
        if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
        leftPaneWidth_ = 0;  // hide left pane in single view (no animation)
        leftTween_.finish();
        updateLayout();
        loadProfileForEntry(*entry);

        // Update metadata panel
        if (metadataPanel_) {
            metadataPanel_->clearThumbnail();  // clear map pin thumbnail
            metadataPanel_->setPhoto(entry);
            metadataPanel_->setViewInfo(zoomLevel_, profileEnabled_, profileBlend_,
                hasProfileLut_, lensEnabled_, isSmartPreview_);
        }
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

    // Restore left pane width (no animation)
    leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
    leftTween_.finish();

    // Clear view info, update with grid selection
    if (metadataPanel_) {
        metadataPanel_->clearViewInfo();
        updateMetadataPanel();
    }

    updateLayout();
    redraw();
}

void tcApp::enterRelatedView(const string& photoId) {
    viewMode_ = ViewMode::Related;
    grid_->setActive(false);
    if (mapView_) mapView_->setActive(false);
    if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();

    relatedView_->setCenter(photoId, provider_);
    relatedView_->setActive(true);

    // Hide left sidebar in related view
    leftPaneWidth_ = 0;
    leftTween_.finish();

    // Update metadata panel with center photo
    if (metadataPanel_) {
        auto* entry = provider_.getPhoto(photoId);
        metadataPanel_->setPhoto(entry);
        metadataPanel_->clearViewInfo();
        metadataPanel_->clearThumbnail();
    }

    updateLayout();
    redraw();
}

void tcApp::exitRelatedView() {
    if (viewMode_ != ViewMode::Related) return;

    viewMode_ = ViewMode::Grid;
    relatedView_->setActive(false);
    relatedView_->shutdown();
    grid_->setActive(true);

    // Restore left pane
    leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
    leftTween_.finish();

    if (metadataPanel_) {
        metadataPanel_->clearViewInfo();
        metadataPanel_->clearThumbnail();
        updateMetadataPanel();
    }

    updateLayout();
    redraw();
}

void tcApp::enterPeopleView() {
    viewMode_ = ViewMode::People;
    grid_->setActive(false);
    if (mapView_) mapView_->setActive(false);
    if (relatedView_ && relatedView_->getActive()) {
        relatedView_->setActive(false);
        relatedView_->shutdown();
    }
    if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();

    peopleView_->populate(provider_);
    peopleView_->setActive(true);

    // Hide left sidebar in people view
    leftPaneWidth_ = 0;
    leftTween_.finish();

    if (metadataPanel_) {
        metadataPanel_->setPhoto(nullptr);
        metadataPanel_->clearViewInfo();
        metadataPanel_->clearThumbnail();
    }

    updateLayout();
    redraw();
}

void tcApp::exitPeopleView() {
    if (viewMode_ != ViewMode::People) return;

    viewMode_ = ViewMode::Grid;
    peopleView_->setActive(false);
    peopleView_->shutdown();
    grid_->setActive(true);

    // Restore left pane
    leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
    leftTween_.finish();

    if (metadataPanel_) {
        metadataPanel_->clearViewInfo();
        metadataPanel_->clearThumbnail();
        updateMetadataPanel();
    }

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
    float rightW = rightPaneWidth_;
    float winW = getWindowWidth() - rightW;
    float winH = getWindowHeight() - statusBarHeight_;

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

void tcApp::updateLayout() {
    float w = getWindowWidth();
    float h = getWindowHeight() - statusBarHeight_;

    // SearchBar — only in grid mode
    bool inGrid = viewMode_ == ViewMode::Grid;
    float searchH = inGrid ? searchBarHeight_ : 0;
    if (searchBar_) {
        searchBar_->setActive(inGrid);
        if (inGrid) {
            searchBar_->setRect(0, 0, w, searchBarHeight_);
        }
    }

    float contentY = searchH;
    float contentH = h - searchH;

    // Animated pane widths (slide in/out)
    bool leftInGrid = inGrid && folderTree_;
    float leftW = leftInGrid ? leftPaneWidth_ : 0;
    float rightW = rightPaneWidth_;

    // Center content
    float contentX = leftW;
    float contentW = w - leftW - rightW;

    // FolderTree — always full width, slide via X position
    if (folderTree_) {
        bool leftActive = leftInGrid && leftPaneWidth_ > 0;
        folderTree_->setActive(leftActive);
        if (leftActive) {
            folderTree_->setRect(leftW - sidebarWidth_, contentY, sidebarWidth_, contentH);
        }
    }

    // Grid / Map / Related
    if (grid_) grid_->setRect(contentX, contentY, contentW, contentH);
    if (mapView_) {
        if (viewMode_ == ViewMode::Map) {
            mapView_->setRect(contentX, contentY, contentW, contentH);
        }
    }
    if (relatedView_) {
        if (viewMode_ == ViewMode::Related) {
            relatedView_->setRect(contentX, contentY, contentW, contentH);
        }
    }
    if (peopleView_) {
        if (viewMode_ == ViewMode::People) {
            peopleView_->setRect(contentX, contentY, contentW, contentH);
        }
    }

    // MetadataPanel — always full width, slide via X position
    if (metadataPanel_) {
        bool rightActive = rightPaneWidth_ > 0;
        metadataPanel_->setActive(rightActive);
        if (rightActive) {
            metadataPanel_->setRect(w - rightW, contentY, metadataWidth_, contentH);
        }
    }

    // Left toggle
    if (leftToggle_) {
        if (inGrid) {
            leftToggle_->setActive(true);
            leftToggle_->direction = showSidebar_ ? PaneToggle::Left : PaneToggle::Right;
            leftToggle_->setRect(leftW - 12, contentY + contentH / 2 - 15, 12, 30);
            // Clamp to screen edge when collapsed
            if (leftToggle_->getX() < 0) leftToggle_->setPos(0, leftToggle_->getY());
        } else {
            leftToggle_->setActive(false);
        }
    }

    // Right toggle
    if (rightToggle_) {
        rightToggle_->setActive(true);
        rightToggle_->direction = showMetadata_ ? PaneToggle::Right : PaneToggle::Left;
        float toggleX = w - rightW;
        if (toggleX > w - 12) toggleX = w - 12;
        rightToggle_->setRect(toggleX, contentY + contentH / 2 - 15, 12, 30);
    }
}

void tcApp::rebuildFolderTree() {
    if (!folderTree_) return;
    auto folders = provider_.buildFolderList();
    folderTree_->buildTree(folders, provider_.getRawStoragePath());
    redraw();
}

void tcApp::updateMetadataPanel() {
    if (!metadataPanel_) return;

    if (viewMode_ == ViewMode::Single && selectedIndex_ >= 0 &&
        selectedIndex_ < (int)grid_->getPhotoIdCount()) {
        const string& pid = grid_->getPhotoId(selectedIndex_);
        auto* e = provider_.getPhoto(pid);
        metadataPanel_->setPhoto(e);
    } else if (viewMode_ == ViewMode::Grid && grid_ && grid_->hasSelection()) {
        // Show first selected photo's metadata
        auto ids = grid_->getSelectedIds();
        if (!ids.empty()) {
            auto* e = provider_.getPhoto(ids[0]);
            metadataPanel_->setPhoto(e);
        }
    } else {
        metadataPanel_->setPhoto(nullptr);
    }
    redraw();
}
