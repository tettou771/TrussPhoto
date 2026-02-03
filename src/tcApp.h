#pragma once

#include <TrussC.h>
#include <tcLut.h>
#include "LensCorrector.h"
#include <atomic>
#include <thread>
#include "Settings.h"
#include "PhotoProvider.h"
#include "PhotoGrid.h"
#include "UploadQueue.h"
#include "CameraProfileManager.h"
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
    Pixels fullPixels_;
    Texture fullTexture_;
    bool isRawImage_ = false;
    Vec2 panOffset_ = {0, 0};
    float zoomLevel_ = 1.0f;
    bool isDragging_ = false;
    Vec2 dragStart_;

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

    void showFullImage(int index);
    void exitFullImage();
    void drawSingleView();
    void enqueueLocalOnlyPhotos();
    void configureServer(const string& url);
    void loadProfileForEntry(const PhotoEntry& entry);
};
