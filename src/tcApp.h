#pragma once

#include <TrussC.h>
#include "PhotoLibrary.h"
#include "PhotoGrid.h"
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
    PhotoLibrary library_;
    PhotoGrid::Ptr grid_;
    ViewMode viewMode_ = ViewMode::Grid;

    // Single view state
    int selectedIndex_ = -1;
    Image fullImage_;
    Vec2 panOffset_ = {0, 0};
    float zoomLevel_ = 1.0f;
    bool isDragging_ = false;
    Vec2 dragStart_;

    void showFullImage(int index);
    void exitFullImage();
    void drawSingleView();
};
