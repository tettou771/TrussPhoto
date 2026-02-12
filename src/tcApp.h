#pragma once

#include <TrussC.h>
#include <tcLut.h>
#include "LensCorrector.h"
#include <atomic>
#include <thread>
#include "AppConfig.h"
#include "AppPaths.h"
#include "Settings.h"
#include "PhotoProvider.h"
#include "PhotoGrid.h"
#include "UploadQueue.h"
#include "CameraProfileManager.h"
#include "ServerConfig.h"
#include "PhotoServer.h"
using namespace std;
using namespace tc;

// View mode
enum class ViewMode {
    Grid,       // Thumbnail grid
    Single      // Single image view
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
    Settings settings_;
    PhotoProvider provider_;
    PhotoGrid::Ptr grid_;
    UploadQueue uploadQueue_;
    ViewMode viewMode_ = ViewMode::Grid;
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

    // Modifier key tracking
    bool cmdDown_ = false;
    bool shiftDown_ = false;

    void showFullImage(int index);
    void deleteSelectedPhotos();
    void reprocessImage();   // re-apply lens correction from rawPixels_ cache
    void exitFullImage();
    void drawSingleView();
    void enqueueLocalOnlyPhotos();
    void configureServer(const string& url, const string& key = "");
    void loadProfileForEntry(const PhotoEntry& entry);
    void repairLibrary();
    void consolidateLibrary();
};
