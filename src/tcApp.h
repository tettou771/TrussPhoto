#pragma once

#include <TrussC.h>
#include <tcLut.h>
#include "LensCorrector.h"
#include <atomic>
#include <chrono>
#include <thread>
#include "AppConfig.h"
#include "AppPaths.h"
#include "CatalogSettings.h"
#include "PhotoProvider.h"
#include "PhotoGrid.h"
#include "FolderTree.h"
#include "MetadataPanel.h"
#include "PaneToggle.h"
#include "SearchBar.h"
#include "MapView.h"
#include "RelatedView.h"
#include "PeopleView.h"
#include "UploadQueue.h"
#include "CameraProfileManager.h"
#include "ServerConfig.h"
#include "PhotoServer.h"
#include "LrcatImporter.h"
using namespace std;
using namespace tc;

// View mode
enum class ViewMode {
    Grid,       // Thumbnail grid
    Single,     // Single image view
    Map,        // Map view with GPS pins
    Related,    // Related photos view
    People      // People / face clusters view
};

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
    PhotoGrid::Ptr grid_;
    FolderTree::Ptr folderTree_;
    bool showSidebar_ = true;
    float sidebarWidth_ = 220;
    bool showMetadata_ = true;
    float metadataWidth_ = 260;
    MetadataPanel::Ptr metadataPanel_;
    MapView::Ptr mapView_;
    RelatedView::Ptr relatedView_;
    PeopleView::Ptr peopleView_;
    SearchBar::Ptr searchBar_;
    float searchBarHeight_ = 36;
    PaneToggle::Ptr leftToggle_;
    PaneToggle::Ptr rightToggle_;
    UploadQueue uploadQueue_;
    ViewMode viewMode_ = ViewMode::Grid;
    ViewMode previousViewMode_ = ViewMode::Grid;
    bool needsServerSync_ = false;
    atomic<bool> syncInProgress_{false};
    atomic<bool> syncCompleted_{false};
    thread syncThread_;

    // Single view state
    int selectedIndex_ = -1;
    Image fullImage_;
    Pixels rawPixels_;       // decoded RAW cache (before lens correction)
    Pixels fullPixels_;
    Texture fullTexture_;
    Texture previewTexture_; // half-size preview (shown while full loads)
    bool isRawImage_ = false;
    bool isSmartPreview_ = false;  // viewing smart preview (RAW unavailable)
    Vec2 panOffset_ = {0, 0};
    float zoomLevel_ = 1.0f;
    bool isDragging_ = false;
    Vec2 dragStart_;

    // Background full-size RAW loading
    thread rawLoadThread_;
    atomic<bool> rawLoadInProgress_{false};
    atomic<bool> rawLoadCompleted_{false};
    atomic<int> rawLoadTargetIndex_{-1};  // which image the load is for
    Pixels pendingRawPixels_;    // loaded in background thread
    mutex rawLoadMutex_;

    // Camera profile (LUT)
    CameraProfileManager profileManager_;
    lut::LutShader lutShader_;
    lut::Lut3D profileLut_;
    bool hasProfileLut_ = false;
    bool profileEnabled_ = true;
    float profileBlend_ = 1.0f;
    string currentProfilePath_;  // track which LUT is loaded

    // Lens correction
    LensCorrector lensCorrector_;
    bool lensEnabled_ = true;

    // Server mode
    PhotoServer server_;
    ServerConfig serverConfig_;

    // Layout
    float statusBarHeight_ = 24.0f;
    Tween<float> leftTween_;
    Tween<float> rightTween_;
    float leftPaneWidth_ = 220;   // current animated width
    float rightPaneWidth_ = 260;  // current animated width
    double lastTime_ = 0;

    // Background pipeline flags
    bool spQueued_ = false;
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

    void showFullImage(int index);
    void enterRelatedView(const string& photoId);
    void exitRelatedView();
    void enterPeopleView();
    void exitPeopleView();
    void deleteSelectedPhotos();
    void reprocessImage();   // re-apply lens correction from rawPixels_ cache
    void exitFullImage(bool returnToPrevious = true);
    void drawSingleView();
    void enqueueLocalOnlyPhotos();
    void configureServer(const string& url, const string& key = "");
    void loadProfileForEntry(const PhotoEntry& entry);
    void repairLibrary();
    void relinkMissingPhotos();
    void consolidateLibrary();
    void updateLayout();
    void rebuildFolderTree();
    void updateMetadataPanel();
};
