#pragma once

#include <TrussC.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "AppConfig.h"
#include "AppPaths.h"
#include "CatalogSettings.h"
#include "PhotoProvider.h"
#include "FolderTree.h"
#include "MetadataPanel.h"
#include "PaneToggle.h"
#include "SearchBar.h"
#include "ViewManager.h"
#include "UploadQueue.h"
#include "ServerConfig.h"
#include "PhotoServer.h"
#include "LrcatImporter.h"
using namespace std;
using namespace tc;

class tcApp : public App {
public:
    void setup() override;
    void update() override;
    void draw() override;

    void keyPressed(int key) override;
    void keyReleased(int key) override;

    void mousePressed(Vec2 pos, int button) override;
    void mouseReleased(Vec2 pos, int button) override;
    void mouseMoved(Vec2 pos) override;
    void mouseDragged(Vec2 pos, int button) override;
    void mouseScrolled(Vec2 delta) override;

    void windowResized(int width, int height) override;
    void filesDropped(const vector<string>& files) override;
    void exit() override;

private:
    CatalogSettings catalogSettings_;
    AppBootstrap bootstrap_;
    string catalogPath_;
    PhotoProvider provider_;

    // View management
    ViewManager::Ptr viewManager_;
    ViewContext viewCtx_;

    // UI
    FolderTree::Ptr folderTree_;
    MetadataPanel::Ptr metadataPanel_;
    SearchBar::Ptr searchBar_;
    float searchBarHeight_ = 36;
    PaneToggle::Ptr leftToggle_;
    PaneToggle::Ptr rightToggle_;
    bool showSidebar_ = true;
    float sidebarWidth_ = 220;
    bool showMetadata_ = true;
    float metadataWidth_ = 260;

    // Server / sync
    UploadQueue uploadQueue_;
    bool needsServerSync_ = false;
    atomic<bool> syncInProgress_{false};
    atomic<bool> syncCompleted_{false};
    thread syncThread_;
    PhotoServer server_;
    ServerConfig serverConfig_;

    // Layout
    float statusBarHeight_ = 24.0f;
    Tween<float> leftTween_;
    Tween<float> rightTween_;
    float leftPaneWidth_ = 220;
    float rightPaneWidth_ = 260;
    double lastTime_ = 0;

    // Background pipeline flags
    bool spQueued_ = false;
    bool exifBackfillQueued_ = false;
    bool embeddingsQueued_ = false;
    bool visionModelUnloaded_ = false;

    // Modifier key tracking
    bool cmdDown_ = false;
    bool shiftDown_ = false;

    // Double-click detection
    chrono::steady_clock::time_point lastClickTime_;
    int lastClickIndex_ = -1;

    // Fonts
    Font font_;
    Font fontSmall_;

    // Helper accessors (convenience shortcuts into ViewManager)
    PhotoGrid::Ptr grid() { return viewManager_ ? viewManager_->gridView()->grid() : nullptr; }
    ViewMode viewMode() const { return viewManager_ ? viewManager_->activeView() : ViewMode::Grid; }

    void deleteSelectedPhotos();
    void enqueueLocalOnlyPhotos();
    void configureServer(const string& url, const string& key = "");
    void repairLibrary();
    void relinkMissingPhotos();
    void consolidateLibrary();
    void updateLayout();
    void rebuildFolderTree();
    void updateMetadataPanel();
};
