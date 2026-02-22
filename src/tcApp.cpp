#include "tcApp.h"

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
        string home = getenv("HOME") ? getenv("HOME") : ".";
        catalogPath_ = home + "/Pictures/TrussPhoto";
    } else {
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

        int facesAdded = provider_.importFaces(result.faces);

        logNotice() << "[LrcatImport] imported=" << added
                    << " total=" << result.stats.totalImages
                    << " missing_files=" << result.stats.missingFile
                    << " faces=" << facesAdded
                    << " persons=" << result.stats.persons;

        // Resolve stacks after lrcat import (RAW+JPG, Live Photo grouping)
        provider_.resolveStacks();

        // Backfill EXIF metadata for imported references
        int exifQueued = provider_.queueAllMissingExifData();
        if (exifQueued > 0) {
            logNotice() << "[LrcatImport] EXIF backfill queued: " << exifQueued;
        }
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

    // 4b. Create status bar (bottom bar — added early so it draws behind everything)
    statusBar_ = make_shared<StatusBar>();
    addChild(statusBar_);

    // 5. Create ViewManager (owns Grid, Single, Map, Related, People views)
    viewManager_ = make_shared<ViewManager>();
    addChild(viewManager_);

    // Build ViewContext
    viewCtx_.provider = &provider_;
    viewCtx_.grid = viewManager_->gridView()->grid();
    viewCtx_.cmdDown = &cmdDown_;
    viewCtx_.shiftDown = &shiftDown_;
    viewCtx_.redraw = [this](int frames) { redraw(frames); };

    // 5a. Create search bar
    searchBar_ = make_shared<SearchBar>();
    addChild(searchBar_);

    searchListener_ = searchBar_->searched.listen([this](string& query) {
        auto g = grid();
        if (!g) return;

        auto parsed = SearchBar::parseQuery(query);

        // Clear previous geo filter
        g->clearGeoBBox();

        if (query.empty()) {
            // Empty query: clear all filters
            g->clearClipResults();
            g->clearTextMatchIds();
            g->setTextFilter("");
            g->populate(provider_);
        } else if (parsed.location.empty()) {
            // Text-only search (no @location)
            runTextSearch(g, parsed.text);
        } else {
            // Has @location → async Nominatim query
            // Don't search yet — wait for geo result, then search with bbox + text together
            searchLocation(parsed.location, parsed.text);
        }
        redraw();
    });

    // 5b. Create folder tree sidebar
    folderTree_ = make_shared<FolderTree>();
    addChild(folderTree_);

    folderTree_->onFolderSelected = [this](const string& path) {
        grid()->setFilterPath(path);
        grid()->populate(provider_);
        redraw();
    };

    // 5c. Configure map view callbacks
    auto mapView = viewManager_->mapView();
    mapView->setTileCacheDir(catalogPath_ + "/tile_cache");
    mapView->onPinClick = [this](int index, const string& photoId) {
        auto* entry = provider_.getPhoto(photoId);
        if (!entry) return;
        if (metadataPanel_) {
            metadataPanel_->setPhoto(entry);
            metadataPanel_->setStyleProfileStatus(
                viewManager_->singleView()->hasProfileFor(entry->camera, entry->creativeStyle));
            Pixels thumbPixels;
            if (provider_.getThumbnail(photoId, thumbPixels)) {
                Texture tex;
                tex.allocate(thumbPixels, TextureUsage::Immutable, false);
                metadataPanel_->setThumbnail(std::move(tex));
            }
        }
        redraw();
    };
    mapView->onPinDoubleClick = [this](int index, const string& photoId) {
        viewManager_->showFullImage(index);
        if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
        leftPaneWidth_ = 0;
        leftTween_.finish();
        updateLayout();
    };
    mapView->onRedraw = [this]() { redraw(); };
    mapView->cmdDownRef = &cmdDown_;
    mapView->shiftDownRef = &shiftDown_;
    mapView->onGeotagConfirm = [this](const string& photoId, double lat, double lon) {
        provider_.setGps(photoId, lat, lon);
        logNotice() << "[MapView] Geotag confirmed: " << photoId
                     << " lat=" << lat << " lon=" << lon;
    };

    // 5c2. Configure related view callbacks
    auto relatedView = viewManager_->relatedView();
    relatedView->onPhotoClick = [this](const string& photoId) {
        auto* entry = provider_.getPhoto(photoId);
        if (!entry) return;
        if (metadataPanel_) {
            metadataPanel_->setPhoto(entry);
            metadataPanel_->setStyleProfileStatus(
                viewManager_->singleView()->hasProfileFor(entry->camera, entry->creativeStyle));
            Pixels thumbPixels;
            if (provider_.getThumbnail(photoId, thumbPixels)) {
                Texture tex;
                tex.allocate(thumbPixels, TextureUsage::Immutable, false);
                metadataPanel_->setThumbnail(std::move(tex));
            }
        }
        redraw();
    };
    relatedView->onCenterDoubleClick = [this](const string& photoId) {
        auto g = grid();
        for (int i = 0; i < (int)g->getPhotoIdCount(); i++) {
            if (g->getPhotoId(i) == photoId) {
                viewManager_->showFullImage(i);
                if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
                leftPaneWidth_ = 0;
                leftTween_.finish();
                updateLayout();
                return;
            }
        }
    };
    relatedView->onRedraw = [this]() { redraw(); };

    // 5c3. Configure people view callbacks
    auto peopleView = viewManager_->peopleView();
    peopleView->onRedraw = [this]() { redraw(); };
    peopleView->cmdDownRef = &cmdDown_;
    peopleView->shiftDownRef = &shiftDown_;
    peopleView->onFaceSelect = [this](const string& photoId) {
        auto* e = provider_.getPhoto(photoId);
        if (e && metadataPanel_) {
            metadataPanel_->setPhoto(e);
            metadataPanel_->setStyleProfileStatus(
                viewManager_->singleView()->hasProfileFor(e->camera, e->creativeStyle));
            Pixels thumbPixels;
            if (provider_.getThumbnail(photoId, thumbPixels)) {
                Texture tex;
                tex.allocate(thumbPixels, TextureUsage::Immutable, false);
                metadataPanel_->setThumbnail(std::move(tex));
            }
        }
        redraw();
    };
    peopleView->onOverlayUpdate = [this](const vector<OverlayRect>& overlays) {
        if (metadataPanel_) metadataPanel_->setOverlays(overlays);
        redraw();
    };
    peopleView->onFaceDoubleClick = [this](const string& photoId) {
        auto g = grid();
        for (int i = 0; i < (int)g->getPhotoIdCount(); i++) {
            if (g->getPhotoId(i) == photoId) {
                viewManager_->showFullImage(i);
                if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
                leftPaneWidth_ = 0;
                leftTween_.finish();
                updateLayout();
                return;
            }
        }
    };

    // 5d. Create metadata panel (right sidebar)
    metadataPanel_ = make_shared<MetadataPanel>();
    addChild(metadataPanel_);

    // 5d2. Create develop panel (right sidebar, exclusive with metadata)
    developPanel_ = make_shared<DevelopPanel>();
    addChild(developPanel_);
    developPanel_->setActive(false);

    developListener_ = developPanel_->settingsChanged.listen([this]() {
        auto sv = viewManager_->singleView();
        if (viewMode() == ViewMode::Single && sv) {
            sv->onDevelopChanged(developPanel_->getExposure(),
                                 developPanel_->getTemperature(),
                                 developPanel_->getTint(),
                                 developPanel_->getChromaDenoise(),
                                 developPanel_->getLumaDenoise());
        }
    });

    // Set metadataPanel in ViewContext after creation
    viewCtx_.metadataPanel = metadataPanel_;
    viewManager_->setContext(viewCtx_);

    // 5e. Create pane toggle buttons
    leftToggle_ = make_shared<PaneToggle>();
    addChild(leftToggle_);
    leftToggleListener_ = leftToggle_->clicked.listen([this]() {
        showSidebar_ = !showSidebar_;
        float from = leftPaneWidth_;
        float to = showSidebar_ ? sidebarWidth_ : 0;
        leftTween_.from(from).to(to).duration(0.2f).ease(EaseType::Cubic, EaseMode::Out).start();
    });

    rightToggle_ = make_shared<PaneToggle>();
    addChild(rightToggle_);
    rightToggleListener_ = rightToggle_->clicked.listen([this]() {
        showMetadata_ = !showMetadata_;
        float from = rightPaneWidth_;
        float to = showMetadata_ ? metadataWidth_ : 0;
        rightTween_.from(from).to(to).duration(0.2f).ease(EaseType::Cubic, EaseMode::Out).start();
    });

    // Initialize tween state
    lastTime_ = getElapsedTime();
    leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
    rightPaneWidth_ = showMetadata_ ? metadataWidth_ : 0;

    updateLayout();

    gridClickListener_ = grid()->itemClicked.listen([this](int& index) {
        auto now = chrono::steady_clock::now();
        bool isDoubleClick = (index == lastClickIndex_ &&
            chrono::duration_cast<chrono::milliseconds>(now - lastClickTime_).count() < 400);
        lastClickTime_ = now;
        lastClickIndex_ = index;

        auto g = grid();
        if (shiftDown_) {
            int anchor = g->getSelectionAnchor();
            if (anchor >= 0) {
                if (!cmdDown_) g->clearSelection();
                g->selectRange(anchor, index, true);
            } else {
                if (!cmdDown_) g->clearSelection();
                g->toggleSelection(index);
            }
            updateMetadataPanel();
        } else if (cmdDown_) {
            g->toggleSelection(index);
            updateMetadataPanel();
        } else if (isDoubleClick) {
            g->clearSelection();
            viewManager_->showFullImage(index);
            if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
            leftPaneWidth_ = 0;
            leftTween_.finish();
            updateLayout();
        } else {
            g->clearSelection();
            g->toggleSelection(index);
            updateMetadataPanel();
        }
    });

    // Context menu callbacks
    gridContextMenuListener_ = grid()->contextMenuRequested.listen([this](ContextMenu::Ptr& menu) {
        showContextMenu(menu);
    });
    gridRepairListener_ = grid()->repairRequested.listen([this]() { repairLibrary(); });
    gridConsolidateListener_ = grid()->consolidateRequested.listen([this]() { consolidateLibrary(); });
    gridDeleteListener_ = grid()->deleteRequested.listen([this](vector<string>& ids) {
        int count = (int)ids.size();
        string msg = format("Delete {} photo{}?\nThis will permanently remove the file{} from disk.",
            count, count > 1 ? "s" : "", count > 1 ? "s" : "");
        if (confirmDialog("Delete Photos", msg)) {
            int deleted = provider_.deletePhotos(ids);
            logNotice() << "[Delete] Removed " << deleted << " photos";
            grid()->populate(provider_);
            rebuildFolderTree();
            redraw();
        }
    });
    viewManager_->singleView()->onContextMenu = [this](ContextMenu::Ptr menu) {
        showContextMenu(menu);
    };

    // Display previous library immediately
    if (hasLibrary && provider_.getCount() > 0) {
        grid()->populate(provider_);
        rebuildFolderTree();
    }

    // 6. Start upload queue (only if server configured)
    if (catalogSettings_.hasServer()) {
        uploadQueue_.setServerUrl(catalogSettings_.serverUrl);
        uploadQueue_.setApiKey(catalogSettings_.apiKey);
        uploadQueue_.start();
        needsServerSync_ = true;
    }

    // 7b. Initial status bar state
    if (catalogSettings_.hasServer()) {
        statusBar_->setServerStatus("Offline", Color(0.6f, 0.35f, 0.35f));
    } else {
        statusBar_->setServerStatus("Local", Color(0.5f, 0.5f, 0.55f));
    }
    statusBar_->setPhotoCount(provider_.getCount());

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
                grid()->populate(provider_);
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
            if (relinked > 0) {
                grid()->populate(provider_);
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
        string md = AppPaths::modelsDir() + "/";
        provider_.initFaceModels(md + "det_10g.onnx",
                                 md + "w600k_r50.onnx");
    }

    // 10. SingleView init (camera profiles, LUT shader, lens correction)
    viewManager_->singleView()->init(getDataPath("profiles"));

    // Sync DevelopPanel sliders when photo changes
    viewManager_->singleView()->onDevelopRestored = [this](float exp, float temp, float tint,
                                                            float chroma, float luma) {
        if (developPanel_ && showDevelop_) {
            developPanel_->setValues(exp, temp, tint, chroma, luma);
            developPanel_->setNrEnabled(viewManager_->singleView()->isRawImage());
        }
    };

    // 11. Fonts
    loadJapaneseFont(font_, 14);
    loadJapaneseFont(fontSmall_, 12);

    // 14. Setup event driven mode
    setIndependentFps(VSYNC, 0);

    logNotice() << "TrussPhoto ready - Catalog: " << catalogPath_;
}

void tcApp::update() {
    if (AppConfig::serverMode) {
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

        // Update status bar after sync
        statusBar_->setPhotoCount(provider_.getCount());
        if (catalogSettings_.hasServer()) {
            if (provider_.isServerConnected()) {
                statusBar_->setServerStatus("Server", Color(0.3f, 0.8f, 0.4f));
            } else {
                statusBar_->setServerStatus("Offline", Color(0.6f, 0.35f, 0.35f));
            }
        }

        auto g = grid();
        if (provider_.getCount() > 0 && g->getItemCount() != provider_.getCount()) {
            g->populate(provider_);
            rebuildFolderTree();
            redraw();
        }
    }

    // Process background file copies
    provider_.processCopyResults();
    statusBar_->setPhotoCount(provider_.getCount());

    // Process smart preview generation results
    provider_.processSPResults();
    if (provider_.isSPGenerationRunning()) {
        statusBar_->setTaskProgress("SP", provider_.getSPCompletedCount(), provider_.getSPTotalCount());
    } else {
        statusBar_->clearTask("SP");
    }

    // Auto-queue SP generation on startup (one-shot)
    if (!spQueued_) {
        spQueued_ = true;
        int queued = provider_.queueAllMissingSP();
        if (queued > 0) {
            logNotice() << "[SmartPreview] Auto-queued " << queued << " photos";
        }
    }

    // Process EXIF backfill results
    provider_.processExifBackfillResults();

    // Process embedding generation results
    provider_.processEmbeddingResults();
    if (provider_.isEmbeddingRunning()) {
        statusBar_->setTaskProgress("Embedding",
            provider_.getEmbeddingCompletedCount(), provider_.getEmbeddingTotalCount());
    } else {
        statusBar_->clearTask("Embedding");
    }

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
    provider_.processFaceDetectionResults();
    if (provider_.isFaceDetectionRunning()) {
        statusBar_->setTaskProgress("Faces",
            provider_.getFaceDetectionCompletedCount(), provider_.getFaceDetectionTotalCount());
    } else {
        statusBar_->clearTask("Faces");
    }

    if (provider_.isFaceModelsReady() && !provider_.isFaceDetectionRunning()) {
        int queued = provider_.queueAllMissingFaceDetections();
        if (queued > 0) {
            logNotice() << "[FaceDetection] Queued " << queued << " photos";
        }
    }

    // Redraw during background tasks + update FPS/RAM
    if (provider_.isEmbedderInitializing() || provider_.isSPGenerationRunning() ||
        provider_.isEmbeddingRunning() || provider_.isFaceDetectionRunning()) {
        statusBar_->setFps(getFrameRate());
        statusBar_->setRamGiB(StatusBar::measureRamGiB());
        redraw();
    }

    // Process consolidation results
    provider_.processConsolidateResults();
    if (provider_.isConsolidateRunning()) {
        statusBar_->setTaskProgress("Consolidate",
            provider_.getConsolidateProgress(), provider_.getConsolidateTotal());
    } else {
        statusBar_->clearTask("Consolidate");
    }

    // Process background RAW load / video update
    if (viewMode() == ViewMode::Single || viewMode() == ViewMode::Crop) {
        viewManager_->singleView()->processRawLoadCompletion();
        if (viewMode() == ViewMode::Single) {
            viewManager_->singleView()->processVideoUpdate();
        }
    }

    // Process upload results
    UploadResult uploadResult;
    while (uploadQueue_.tryGetResult(uploadResult)) {
        SyncState newState = uploadResult.success
            ? SyncState::Synced
            : SyncState::LocalOnly;
        provider_.setSyncState(uploadResult.photoId, newState);
    }
    statusBar_->setUploadPending(uploadQueue_.getPendingCount());

    // Process geo search results (Nominatim)
    {
        lock_guard<mutex> lock(geoMutex_);
        if (geoResult_.valid) {
            geoResult_.valid = false;
            auto g2 = grid();
            if (g2) {
                g2->setGeoBBox(geoResult_.south, geoResult_.north,
                               geoResult_.west, geoResult_.east);
                runTextSearch(g2, geoResult_.textQuery);
                logNotice() << "[GeoSearch] bbox=["
                            << geoResult_.south << "," << geoResult_.north << ","
                            << geoResult_.west << "," << geoResult_.east << "]";
            }
            redraw();
        }
    }

    // Update sync state badges
    auto g = grid();
    if (g && g->updateSyncStates(provider_)) {
        redraw();
    }

    // Periodic server sync
    static int syncCounter = 0;
    if (catalogSettings_.hasServer() && ++syncCounter % 1800 == 0 && !syncInProgress_) {
        needsServerSync_ = true;
    }
}

void tcApp::draw() {
    if (AppConfig::serverMode) return;

    clear(0.06f, 0.06f, 0.08f);

    // Render develop shader to offscreen FBO (before Node tree draws)
    if (viewMode() == ViewMode::Single || viewMode() == ViewMode::Crop) {
        viewManager_->singleView()->renderDevelopFbo();
    }

    if (viewMode() == ViewMode::Grid) {
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
    // Map/Related/People draw themselves via Node tree

    // Model download / loading progress
    if (provider_.isEmbedderInitializing()) {
        pushStyle();
        float leftW = leftPaneWidth_;
        float rightW = rightPaneWidth_;
        float contentW = getWindowWidth() - leftW - rightW;
        float centerX = leftW + contentW * 0.5f;
        float centerY = getWindowHeight() * 0.5f;

        setColor(0.6f, 0.6f, 0.65f);
        fontSmall_.drawString(provider_.getEmbedderStatus(),
            centerX, centerY - 20, Direction::Center, Direction::Center);

        popStyle();
    }

    // Status bar draws itself via Node tree (addChild in setup)
    statusBar_->setFps(getFrameRate());
    statusBar_->setRamGiB(StatusBar::measureRamGiB());
}

void tcApp::keyPressed(int key) {
    redraw(3);

    // ESC closes context menu first (if open)
    if (key == SAPP_KEYCODE_ESCAPE && contextMenu_) {
        closeContextMenu();
        return;
    }

    auto singleView = viewManager_->singleView();
    auto peopleView = viewManager_->peopleView();
    auto g = grid();

    if (viewMode() == ViewMode::Single) {
        singleView->handleKey(key);

        if (key == SAPP_KEYCODE_ESCAPE) {
            // ESC: go back to previous view
            // Hide develop panel if visible
            if (showDevelop_) {
                showDevelop_ = false;
                if (developPanel_) developPanel_->setActive(false);
                if (metadataPanel_) metadataPanel_->setActive(true);
            }
            viewManager_->goBack();
            // Restore layout for target view
            auto active = viewManager_->activeView();
            if (active == ViewMode::Grid || active == ViewMode::People) {
                if (active == ViewMode::Grid) {
                    leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
                    leftTween_.finish();
                }
                if (metadataPanel_) {
                    metadataPanel_->clearViewInfo();
                    updateMetadataPanel();
                }
            }
            updateLayout();
        }

        if ((key == 'O' || key == 'o') && viewManager_->previousView() == ViewMode::People) {
            // O: go back to People view (with state restore)
            viewManager_->goBack();
            leftPaneWidth_ = 0;
            leftTween_.finish();
            if (metadataPanel_) {
                metadataPanel_->clearViewInfo();
                metadataPanel_->clearThumbnail();
            }
            updateLayout();
            redraw();
            return;
        }

        if (key == 'V' || key == 'v') {
            string photoId = singleView->currentPhotoId();
            if (!photoId.empty() && singleView->hasEmbedding()) {
                viewManager_->switchTo(ViewMode::Grid);  // exit single first
                // Now enter related
                viewManager_->relatedView()->setCenter(photoId, provider_);
                viewManager_->switchTo(ViewMode::Related);
                leftPaneWidth_ = 0;
                leftTween_.finish();
                if (metadataPanel_) {
                    auto* entry = provider_.getPhoto(photoId);
                    metadataPanel_->setPhoto(entry);
                    if (entry) metadataPanel_->setStyleProfileStatus(
                        viewManager_->singleView()->hasProfileFor(entry->camera, entry->creativeStyle));
                    metadataPanel_->clearViewInfo();
                    metadataPanel_->clearThumbnail();
                }
                updateLayout();
            }
        }

        if (key == 'D' || key == 'd') {
            showDevelop_ = !showDevelop_;
            if (showDevelop_) {
                // Show develop panel, hide metadata panel
                if (developPanel_) {
                    developPanel_->setActive(true);
                    developPanel_->setNrEnabled(singleView->isRawImage());
                    // Restore slider values from current photo
                    string photoId = singleView->currentPhotoId();
                    auto* entry = provider_.getPhoto(photoId);
                    if (entry) {
                        developPanel_->setValues(entry->devExposure, entry->devWbTemp,
                                                 entry->devWbTint,
                                                 entry->chromaDenoise, entry->lumaDenoise);
                    }
                }
                if (metadataPanel_) metadataPanel_->setActive(false);
            } else {
                // Hide develop panel, show metadata panel
                if (developPanel_) developPanel_->setActive(false);
                if (metadataPanel_) metadataPanel_->setActive(true);
            }
            updateLayout();
        }

        if (key == 'E' || key == 'e') {
            singleView->doExport();
        }

        if ((key == 'R' || key == 'r') && singleView->hasFbo() && !singleView->isVideo()) {
            // Enter crop mode
            auto cv = viewManager_->cropView();
            cropDoneListener_ = cv->doneEvent.listen([this]() {
                // Return to single view
                viewManager_->switchTo(ViewMode::Single);
                if (showDevelop_) {
                    if (developPanel_) developPanel_->setActive(true);
                    if (metadataPanel_) metadataPanel_->setActive(false);
                } else {
                    if (metadataPanel_) metadataPanel_->setActive(true);
                }
                updateLayout();
            });
            viewManager_->switchTo(ViewMode::Crop);
            cv->enterCrop();
            // Hide metadata/develop panels (CropView has its own panel)
            if (metadataPanel_) metadataPanel_->setActive(false);
            if (developPanel_) developPanel_->setActive(false);
            rightPaneWidth_ = 0;
            rightTween_.finish();
            leftPaneWidth_ = 0;
            leftTween_.finish();
            updateLayout();
        }

        // Update metadata panel with current state
        singleView->updateMetadata();
    } else if (viewMode() == ViewMode::Crop) {
        auto cv = viewManager_->cropView();
        if (key == SAPP_KEYCODE_ENTER || key == SAPP_KEYCODE_KP_ENTER) {
            cv->commitCrop();
            cv->doneEvent.notify();
        } else if (key == SAPP_KEYCODE_ESCAPE) {
            if (!cv->hasChanges() ||
                confirmDialog("Discard Crop", "Discard crop changes?")) {
                cv->cancelCrop();
                cv->doneEvent.notify();
            }
        } else if ((key == 'Z' || key == 'z') && cmdDown_) {
            cv->undo();
        }
    } else if (viewMode() == ViewMode::People) {
        if (key == SAPP_KEYCODE_ESCAPE) {
            if (!peopleView->hasSelection() && !peopleView->isNameEditing()) {
                viewManager_->switchTo(ViewMode::Grid);
                leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
                leftTween_.finish();
                if (metadataPanel_) {
                    metadataPanel_->clearViewInfo();
                    metadataPanel_->clearThumbnail();
                    updateMetadataPanel();
                }
                updateLayout();
            }
        }
    } else if (viewMode() == ViewMode::Map) {
        auto mapView = viewManager_->mapView();
        if (mapView->isSearchFocused()) {
            // While search bar is focused, only ESC blurs it;
            // all other keys go to IME (don't process map shortcuts)
            if (key == SAPP_KEYCODE_ESCAPE) {
                mapView->blurSearch();
            }
            redraw();
            return;
        }
        if (key == SAPP_KEYCODE_ESCAPE) {
            if (mapView->hasProvisionalPins()) {
                int n = (int)mapView->provisionalPinCount();
                string msg = format("{}件の仮タグを破棄しますか？", n);
                if (confirmDialog("仮タグ破棄", msg)) {
                    mapView->clearProvisionalPins();
                }
            } else if (mapView->hasGpxTracks()) {
                mapView->clearGpxTracks();
            }
            // else: do nothing (G key to go back to grid)
        } else if (key == SAPP_KEYCODE_ENTER || key == SAPP_KEYCODE_KP_ENTER) {
            if (mapView->hasProvisionalPins()) {
                int n = (int)mapView->provisionalPinCount();
                string msg = format("{}件の仮タグを確定しますか？", n);
                if (confirmDialog("仮タグ確定", msg)) {
                    mapView->confirmAllPins();
                }
            }
        } else if (key == 'A' || key == 'a') {
            mapView->runAutoGeotag();
        } else if (key == SAPP_KEYCODE_BACKSPACE || key == SAPP_KEYCODE_DELETE) {
            // Collect selected photos that have GPS
            auto allSelected = mapView->selectedPhotoIds();
            vector<string> gpsIds;
            for (auto& id : allSelected) {
                auto* e = provider_.getPhoto(id);
                if (e && e->hasGps()) gpsIds.push_back(id);
            }
            if (!gpsIds.empty()) {
                int n = (int)gpsIds.size();
                string msg = n == 1
                    ? "Remove geotag from the selected photo?"
                    : format("Remove geotag from {} photos?", n);
                confirmDialogAsync("Remove Geotag", msg,
                    [this, gpsIds](bool yes) {
                        if (yes) {
                            auto mv = viewManager_->mapView();
                            for (auto& id : gpsIds) {
                                mv->removeGeotag(id, provider_);
                            }
                            redraw();
                        }
                    });
            }
        }
    } else if (viewMode() == ViewMode::Grid) {
        // Grid mode: if search bar is active, only handle ESC
        if (searchBar_ && searchBar_->isActive()) {
            if (key == SAPP_KEYCODE_ESCAPE) {
                searchBar_->deactivate();
            }
            redraw();
            return;
        }

        // Grid mode keys
        if (key == SAPP_KEYCODE_BACKSPACE || key == SAPP_KEYCODE_DELETE) {
            deleteSelectedPhotos();
        } else if (key == SAPP_KEYCODE_ESCAPE) {
            if (searchBar_ && !searchBar_->getQuery().empty()) {
                searchBar_->clear();
                g->clearClipResults();
                g->clearFilterPhotoIds();
                g->populate(provider_);
            } else if (g->hasSelection()) {
                g->clearSelection();
                updateMetadataPanel();
            } else if (g->hasFilterPhotoIds()) {
                g->clearFilterPhotoIds();
                g->populate(provider_);
            }
        } else if (key == SAPP_KEYCODE_SLASH) {
            if (searchBar_) searchBar_->activate();
        } else if (key == 'A' || key == 'a') {
            if (cmdDown_) {
                if (shiftDown_) {
                    g->clearSelection();
                } else {
                    g->selectAll();
                }
                updateMetadataPanel();
            }
        } else if (key == 'V' || key == 'v') {
            if (g->getSelectionCount() == 1) {
                auto ids = g->getSelectedIds();
                if (!ids.empty() && provider_.getCachedEmbedding(ids[0])) {
                    viewManager_->relatedView()->setCenter(ids[0], provider_);
                    viewManager_->switchTo(ViewMode::Related);
                    if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
                    leftPaneWidth_ = 0;
                    leftTween_.finish();
                    if (metadataPanel_) {
                        auto* entry = provider_.getPhoto(ids[0]);
                        metadataPanel_->setPhoto(entry);
                        if (entry) metadataPanel_->setStyleProfileStatus(
                            viewManager_->singleView()->hasProfileFor(entry->camera, entry->creativeStyle));
                        metadataPanel_->clearViewInfo();
                        metadataPanel_->clearThumbnail();
                    }
                    updateLayout();
                }
            }
        } else if (key == 'D' || key == 'd') {
            // D key: open selected photo in single (develop) view
            if (g->getSelectionCount() == 1) {
                auto ids = g->getSelectedIds();
                if (!ids.empty()) {
                    for (int i = 0; i < (int)g->getPhotoIdCount(); i++) {
                        if (g->getPhotoId(i) == ids[0]) {
                            g->clearSelection();
                            viewManager_->showFullImage(i);
                            if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
                            leftPaneWidth_ = 0;
                            leftTween_.finish();
                            updateLayout();
                            break;
                        }
                    }
                }
            }
        }
    }
    // Map/Related: no mode-specific keys (just G to go back, handled below)

    // Track modifier key state (must run before any early return)
    if (key == SAPP_KEYCODE_LEFT_SUPER || key == SAPP_KEYCODE_RIGHT_SUPER) {
        cmdDown_ = true;
    }
    if (key == SAPP_KEYCODE_LEFT_SHIFT || key == SAPP_KEYCODE_RIGHT_SHIFT) {
        shiftDown_ = true;
    }

    // Mode-independent keys (skip in Crop mode — it handles its own keys)
    if (viewMode() == ViewMode::Crop) {
        redraw();
        return;
    }
    if (key == 'G' || key == 'g') {
        if (viewMode() != ViewMode::Grid) {
            viewManager_->switchTo(ViewMode::Grid);
            leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
            leftTween_.finish();
            if (metadataPanel_) {
                metadataPanel_->clearViewInfo();
                metadataPanel_->clearThumbnail();
                updateMetadataPanel();
            }
            updateLayout();
        }
    }
    if (key == 'O' || key == 'o') {
        if (viewMode() == ViewMode::People) {
            viewManager_->switchTo(ViewMode::Grid);
            leftPaneWidth_ = showSidebar_ ? sidebarWidth_ : 0;
            leftTween_.finish();
            if (metadataPanel_) {
                metadataPanel_->clearViewInfo();
                metadataPanel_->clearThumbnail();
                updateMetadataPanel();
            }
            updateLayout();
        } else if (viewMode() == ViewMode::Grid) {
            if (!peopleView->hasState()) {
                peopleView->populate(provider_);
            }
            viewManager_->switchTo(ViewMode::People);
            if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
            leftPaneWidth_ = 0;
            leftTween_.finish();
            if (metadataPanel_) {
                metadataPanel_->setPhoto(nullptr);
                metadataPanel_->clearViewInfo();
                metadataPanel_->clearThumbnail();
            }
            updateLayout();
        }
    }
    if (key == 'M' || key == 'm') {
        if (viewMode() == ViewMode::Single || viewMode() == ViewMode::Grid ||
            viewMode() == ViewMode::Related || viewMode() == ViewMode::People) {
            // Capture current photo before switching away from SingleView
            string focusPhotoId;
            if (viewMode() == ViewMode::Single) {
                focusPhotoId = singleView->currentPhotoId();
            }

            // Switch to map from any view
            viewManager_->switchTo(ViewMode::Grid);  // clean up current view first

            // Populate map pins from current photo list
            auto mapView = viewManager_->mapView();
            vector<string> ids;
            vector<PhotoEntry> photos;
            for (size_t i = 0; i < g->getPhotoIdCount(); i++) {
                const string& id = g->getPhotoId((int)i);
                ids.push_back(id);
                auto* e = provider_.getPhoto(id);
                if (e) photos.push_back(*e);
                else photos.push_back(PhotoEntry{});
            }
            mapView->setPhotos(photos, ids, provider_);

            // Determine which photo to center on
            if (focusPhotoId.empty()) {
                auto selectedIds = g->getSelectedIds();
                if (!selectedIds.empty()) focusPhotoId = selectedIds[0];
            }

            if (!focusPhotoId.empty()) {
                mapView->setStripSelection(focusPhotoId);
            }

            viewManager_->switchTo(ViewMode::Map);

            // Center on focus photo at zoom 14, or fit all bounds
            if (!focusPhotoId.empty()) {
                mapView->centerOnPhoto(focusPhotoId);
            } else {
                mapView->fitBounds();
            }

            if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
            leftPaneWidth_ = 0;
            leftTween_.finish();
            if (metadataPanel_) {
                metadataPanel_->clearViewInfo();
                metadataPanel_->clearThumbnail();
            }
            updateLayout();
        }
    }
    if ((key == 'F' || key == 'f') && cmdDown_) {
        if (viewMode() == ViewMode::Grid && searchBar_) {
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

    redraw();
}

void tcApp::keyReleased(int key) {
    if (key == SAPP_KEYCODE_LEFT_SUPER || key == SAPP_KEYCODE_RIGHT_SUPER) {
        cmdDown_ = false;
    }
    if (key == SAPP_KEYCODE_LEFT_SHIFT || key == SAPP_KEYCODE_RIGHT_SHIFT) {
        shiftDown_ = false;
    }
    redraw();
}

void tcApp::mousePressed(Vec2 pos, int button) {
    // Record right-click position for context menu placement
    if (button == 1) {
        lastRightClickPos_ = pos;
    }

    if (viewMode() == ViewMode::Map || viewMode() == ViewMode::Related ||
        viewMode() == ViewMode::People || viewMode() == ViewMode::Single ||
        viewMode() == ViewMode::Crop) return;
}

void tcApp::showContextMenu(ContextMenu::Ptr menu) {
    closeContextMenu();

    // Full-screen overlay to catch outside clicks
    menuOverlay_ = make_shared<MenuOverlay>();
    menuOverlay_->setSize(getWindowWidth(), getWindowHeight());
    menuOverlay_->onClick = [this]() { closeContextMenu(); };
    addChild(menuOverlay_);

    // Position and add menu (last child = z-order frontmost)
    menu->setPos(lastRightClickPos_.x, lastRightClickPos_.y);
    addChild(menu);
    menu->finalizeLayout();
    menu->onClose = [this]() { closeContextMenu(); };
    contextMenu_ = menu;
    redraw();
}

void tcApp::closeContextMenu() {
    if (contextMenu_) {
        contextMenu_->destroy();  // deferred removal (safe during event/draw)
        contextMenu_ = nullptr;
    }
    if (menuOverlay_) {
        menuOverlay_->destroy();
        menuOverlay_ = nullptr;
    }
    redraw();
}

void tcApp::mouseReleased(Vec2 pos, int button) {
    (void)pos;
    if (button == 0) {
        redraw();
    }
}

void tcApp::mouseMoved(Vec2 pos) {
    if (viewMode() == ViewMode::Crop) {
        viewManager_->cropView()->updateHoverCursor(pos);
    }
}

void tcApp::mouseDragged(Vec2 pos, int button) {
    if (viewMode() == ViewMode::Map || viewMode() == ViewMode::Related ||
        viewMode() == ViewMode::People || viewMode() == ViewMode::Single ||
        viewMode() == ViewMode::Crop) return;
}

void tcApp::mouseScrolled(Vec2 delta) {
    redraw();
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
    vector<string> gpxFiles;

    for (const auto& f : files) {
        if (fs::is_directory(f)) {
            provider_.scanFolder(f);
            added = true;
        } else if (fs::path(f).extension() == ".gpx") {
            gpxFiles.push_back(f);
        } else if (provider_.isSupportedFile(f)) {
            filesToImport.push_back(f);
        }
    }

    // Handle GPX files → map view
    if (!gpxFiles.empty()) {
        auto mapView = viewManager_->mapView();
        for (auto& gf : gpxFiles) {
            mapView->loadGpx(gf);
        }
        // Switch to map mode if not already there
        if (viewMode() != ViewMode::Map) {
            auto g = grid();
            vector<string> ids;
            vector<PhotoEntry> photos;
            for (size_t i = 0; i < g->getPhotoIdCount(); i++) {
                const string& id = g->getPhotoId((int)i);
                ids.push_back(id);
                auto* e = provider_.getPhoto(id);
                if (e) photos.push_back(*e);
                else photos.push_back(PhotoEntry{});
            }
            mapView->setPhotos(photos, ids, provider_);
            viewManager_->switchTo(ViewMode::Map);

            if (searchBar_ && searchBar_->isActive()) searchBar_->deactivate();
            leftPaneWidth_ = 0;
            leftTween_.finish();
            if (metadataPanel_) {
                metadataPanel_->clearViewInfo();
                metadataPanel_->clearThumbnail();
            }
            updateLayout();
        }
        mapView->fitGpxBounds();

        // Match photos against GPX timestamps → provisional pins
        int matchCount = mapView->countGpxMatches();
        if (matchCount > 0) {
            string msg = format("{}枚の写真に仮ピンを打ちますか？", matchCount);
            if (confirmDialog("GPX ジオタグ", msg)) {
                mapView->applyGpxGeotags();
                logNotice() << "[GPX] Created " << matchCount << " provisional pins";
            }
        }
        redraw();
    }

    if (!filesToImport.empty()) {
        provider_.importFiles(filesToImport);
        added = true;
    }

    if (added) {
        grid()->populate(provider_);
        rebuildFolderTree();
        redraw();
        enqueueLocalOnlyPhotos();

        provider_.queueAllMissingSP();
        provider_.queueAllMissingEmbeddings();
        provider_.queueAllMissingFaceDetections();
        provider_.queueAllMissingExifData();
    }
}

void tcApp::exit() {
    if (AppConfig::serverMode) {
        server_.stop();
    }

    if (viewManager_) viewManager_->shutdownAll();
    uploadQueue_.stop();
    if (syncThread_.joinable()) syncThread_.join();
    provider_.shutdown();
    provider_.processConsolidateResults();
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
        uploadQueue_.setServerUrl(url);
        uploadQueue_.setApiKey(catalogSettings_.apiKey);
        uploadQueue_.start();
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
    int styles = provider_.refreshCreativeStyles();
    logNotice() << "[Repair] Missing: " << missing << ", Added: " << added << ", Styles: " << styles;
    if (missing > 0 || added > 0) {
        grid()->populate(provider_);
        rebuildFolderTree();
        redraw();
    }
    if (catalogSettings_.hasServer() && !syncInProgress_) {
        needsServerSync_ = true;
    }
}

void tcApp::relinkMissingPhotos() {
    auto singleView = viewManager_->singleView();
    if (viewMode() == ViewMode::Single && singleView->selectedIndex() >= 0) {
        string photoId = singleView->currentPhotoId();
        auto* photo = provider_.getPhoto(photoId);
        if (!photo) return;

        auto result = loadDialog(
            "Find " + photo->filename,
            "Locate: " + photo->filename);

        if (!result.success) return;

        fs::path p(result.filePath);
        string fname = p.filename().string();
        uintmax_t fsize = fs::file_size(p);
        string newId = fname + "_" + to_string(fsize);

        if (newId != photoId) {
            logWarning() << "[Relink] Mismatch: expected " << photoId << ", got " << newId;
            return;
        }

        provider_.relinkPhoto(photoId, result.filePath);
        viewManager_->showFullImage(singleView->selectedIndex());
        leftPaneWidth_ = 0;
        leftTween_.finish();
        updateLayout();
    } else {
        provider_.validateLibrary();

        auto result = loadDialog(
            "Find Missing Photos",
            "Select folder to search for missing files",
            "", true);

        if (!result.success) return;

        int relinked = provider_.relinkFromFolder(result.filePath);
        logNotice() << "[Relink] Relinked " << relinked << " photos";

        if (relinked > 0) {
            grid()->populate(provider_);
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
    auto g = grid();
    if (!g || !g->hasSelection()) return;

    auto selectedIds = g->getSelectedIds();
    int count = (int)selectedIds.size();

    string msg = format("Delete {} photo{}?\nThis will permanently remove the file{} from disk.",
        count, count > 1 ? "s" : "", count > 1 ? "s" : "");

    bool ok = confirmDialog("Delete Photos", msg);
    if (!ok) return;

    int deleted = provider_.deletePhotos(selectedIds);
    logNotice() << "[Delete] Removed " << deleted << " photos";

    g->populate(provider_);
    rebuildFolderTree();
    redraw();
}

void tcApp::updateLayout() {
    float w = getWindowWidth();
    float h = getWindowHeight() - statusBarHeight_;

    // Status bar at bottom
    if (statusBar_) {
        statusBar_->setRect(0, h, w, statusBarHeight_);
    }

    // SearchBar — only in grid mode
    bool inGrid = viewMode() == ViewMode::Grid;
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

    // FolderTree
    if (folderTree_) {
        bool leftActive = leftInGrid && leftPaneWidth_ > 0;
        folderTree_->setActive(leftActive);
        if (leftActive) {
            folderTree_->setRect(leftW - sidebarWidth_, contentY, sidebarWidth_, contentH);
        }
    }

    // ViewManager gets the content area
    if (viewManager_) {
        viewManager_->setRect(contentX, contentY, contentW, contentH);
        viewManager_->layoutViews();
    }

    // MetadataPanel (hidden when develop panel is shown)
    if (metadataPanel_) {
        bool rightActive = rightPaneWidth_ > 0 && !showDevelop_;
        metadataPanel_->setActive(rightActive);
        if (rightActive) {
            metadataPanel_->setRect(w - rightW, contentY, metadataWidth_, contentH);
        }
    }

    // DevelopPanel (exclusive with metadata panel)
    if (developPanel_) {
        bool devActive = rightPaneWidth_ > 0 && showDevelop_;
        developPanel_->setActive(devActive);
        if (devActive) {
            developPanel_->setRect(w - rightW, contentY, metadataWidth_, contentH);
        }
    }

    // Left toggle
    if (leftToggle_) {
        if (inGrid) {
            leftToggle_->setActive(true);
            leftToggle_->direction = showSidebar_ ? PaneToggle::Left : PaneToggle::Right;
            leftToggle_->setRect(leftW - 12, contentY + contentH / 2 - 15, 12, 30);
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

    if (viewMode() == ViewMode::Single) {
        auto sv = viewManager_->singleView();
        if (sv && sv->selectedIndex() >= 0) {
            sv->updateMetadata();
        }
    } else if (viewMode() == ViewMode::Grid) {
        auto g = grid();
        if (g && g->hasSelection()) {
            auto ids = g->getSelectedIds();
            if (!ids.empty()) {
                auto* e = provider_.getPhoto(ids[0]);
                metadataPanel_->setPhoto(e);
                if (e) metadataPanel_->setStyleProfileStatus(
                    viewManager_->singleView()->hasProfileFor(e->camera, e->creativeStyle));
            }
        } else {
            metadataPanel_->setPhoto(nullptr);
        }
    } else {
        metadataPanel_->setPhoto(nullptr);
    }
    redraw();
}

void tcApp::runTextSearch(PhotoGrid::Ptr g, const string& query) {
    if (!g) return;

    if (query.empty()) {
        g->clearClipResults();
        g->clearTextMatchIds();
        g->setTextFilter("");
        g->populate(provider_);
        return;
    }

    if (provider_.isTextEncoderReady()) {
        auto results = provider_.searchByText(query);

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

        g->clearClipResults();
        g->setTextFilter("");
        g->setTextMatchIds(unordered_set<string>(textMatches.begin(), textMatches.end()));
        g->setClipResults(results);
        g->populate(provider_);
        logNotice() << "[Search] query=\"" << query
                    << "\" text=" << textCount
                    << " clip=" << results.size() - textCount;
    } else {
        g->clearClipResults();
        g->clearTextMatchIds();
        g->setTextFilter(query);
        g->populate(provider_);
    }
}

void tcApp::searchLocation(const string& location, const string& textQuery) {
    thread([this, location, textQuery]() {
        tcx::HttpClient client;
        client.addHeader("User-Agent", "TrussPhoto/1.0");
        client.setBaseUrl("");
        string url = string("https://nominatim.openstreetmap.org")
            + "/search?q=" + urlEncode(location)
            + "&format=json&limit=1";
        auto res = client.get(url);
        if (res.ok()) {
            auto j = nlohmann::json::parse(res.body, nullptr, false);
            if (j.is_array() && !j.empty()) {
                double south, north, west, east;

                if (j[0].contains("boundingbox") && j[0]["boundingbox"].is_array()
                    && j[0]["boundingbox"].size() == 4) {
                    auto& bb = j[0]["boundingbox"];
                    south = stod(bb[0].get<string>());
                    north = stod(bb[1].get<string>());
                    west  = stod(bb[2].get<string>());
                    east  = stod(bb[3].get<string>());
                } else {
                    // No bbox → use point ±0.05 degrees (~5km)
                    double lat = stod(j[0]["lat"].get<string>());
                    double lon = stod(j[0]["lon"].get<string>());
                    south = lat - 0.05;
                    north = lat + 0.05;
                    west  = lon - 0.05;
                    east  = lon + 0.05;
                }

                // Ensure minimum bbox span (~5km each direction)
                constexpr double MIN_SPAN = 0.05;
                double latCenter = (south + north) * 0.5;
                double lonCenter = (west + east) * 0.5;
                if (north - south < MIN_SPAN) {
                    south = latCenter - MIN_SPAN;
                    north = latCenter + MIN_SPAN;
                }
                if (east - west < MIN_SPAN) {
                    west = lonCenter - MIN_SPAN;
                    east = lonCenter + MIN_SPAN;
                }

                lock_guard<mutex> lock(geoMutex_);
                geoResult_ = {true, south, north, west, east, textQuery};
            }
        }
    }).detach();
}
