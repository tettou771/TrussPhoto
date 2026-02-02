#include "tcApp.h"

void tcApp::setup() {
    // Create photo grid
    grid_ = make_shared<PhotoGrid>();
    grid_->setRect(0, 0, getWindowWidth(), getWindowHeight());
    addChild(grid_);

    // Connect item click event
    grid_->onItemClick = [this](int index) {
        showFullImage(index);
    };

    logNotice() << "TrussPhoto - Drop a folder containing images to start";
}

void tcApp::update() {
    // Grid updates automatically via Node tree
}

void tcApp::draw() {
    clear(0.06f, 0.06f, 0.08f);

    if (viewMode_ == ViewMode::Single) {
        drawSingleView();
    } else {
        // Grid draws automatically via Node tree

        // Show hint if no images
        if (library_.getCount() == 0) {
            setColor(0.5f, 0.5f, 0.55f);
            string hint = "Drop a folder containing images";
            float x = getWindowWidth() / 2 - hint.length() * 4;
            float y = getWindowHeight() / 2;
            drawBitmapString(hint, x, y);
        }
    }

    // Status bar
    setColor(0.4f, 0.4f, 0.45f);
    drawBitmapString(format("Photos: {}  FPS: {:.1f}",
        library_.getCount(), getFrameRate()), 10, getWindowHeight() - 20);
}

void tcApp::keyPressed(int key) {
    if (viewMode_ == ViewMode::Single) {
        if (key == SAPP_KEYCODE_ESCAPE) {
            exitFullImage();
        } else if (key == SAPP_KEYCODE_LEFT && selectedIndex_ > 0) {
            showFullImage(selectedIndex_ - 1);
        } else if (key == SAPP_KEYCODE_RIGHT && selectedIndex_ < (int)library_.getCount() - 1) {
            showFullImage(selectedIndex_ + 1);
        } else if (key == '0') {
            zoomLevel_ = 1.0f;
            panOffset_ = {0, 0};
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
    if (viewMode_ == ViewMode::Single && fullImage_.isAllocated()) {
        float imgW = fullImage_.getWidth();
        float imgH = fullImage_.getHeight();
        float winW = getWindowWidth();
        float winH = getWindowHeight();

        // Calculate fit scale (minimum zoom = fit to window)
        float fitScale = min(winW / imgW, winH / imgH);
        float minZoom = 1.0f;  // 1.0 = fit to window
        float maxZoom = 10.0f;

        float oldZoom = zoomLevel_;
        zoomLevel_ *= (1.0f + delta.y * 0.1f);
        zoomLevel_ = clamp(zoomLevel_, minZoom, maxZoom);

        // Zoom toward mouse position
        Vec2 mousePos(getGlobalMouseX(), getGlobalMouseY());
        Vec2 windowCenter(winW / 2.0f, winH / 2.0f);

        // Current image center (with pan offset)
        Vec2 imageCenter = windowCenter + panOffset_;

        // Vector from image center to mouse
        Vec2 toMouse = mousePos - imageCenter;

        // Adjust pan to keep mouse point fixed
        float zoomRatio = zoomLevel_ / oldZoom;
        panOffset_ = panOffset_ - toMouse * (zoomRatio - 1.0f);
    }
}

void tcApp::windowResized(int width, int height) {
    // Update grid size
    if (grid_) {
        grid_->setSize(width, height);
    }
}

void tcApp::filesDropped(const vector<string>& files) {
    if (files.empty()) return;

    // Check if first dropped item is a directory
    fs::path path(files[0]);

    if (fs::is_directory(path)) {
        library_.scanFolder(path.string());
        grid_->populate(library_);
        // Thumbnails load automatically via async loader
    } else {
        // Single file - try to find parent folder
        fs::path folder = path.parent_path();
        library_.scanFolder(folder.string());
        grid_->populate(library_);
    }
}

void tcApp::exit() {
    logNotice() << "TrussPhoto exiting";
}

void tcApp::showFullImage(int index) {
    if (index < 0 || index >= (int)library_.getCount()) return;

    auto& entry = library_.getEntry(index);

    logNotice() << "Opening image: " << entry.getFileName();

    // Load full-size image
    if (fullImage_.load(entry.path)) {
        selectedIndex_ = index;
        viewMode_ = ViewMode::Single;
        zoomLevel_ = 1.0f;
        panOffset_ = {0, 0};

        // Disable grid (stops update/draw and events)
        grid_->setActive(false);
    } else {
        logWarning() << "Failed to load: " << entry.path.string();
    }
}

void tcApp::exitFullImage() {
    viewMode_ = ViewMode::Grid;
    fullImage_ = Image();  // Release memory
    selectedIndex_ = -1;

    // Re-enable grid
    grid_->setActive(true);
}

void tcApp::drawSingleView() {
    if (!fullImage_.isAllocated()) return;

    float imgW = fullImage_.getWidth();
    float imgH = fullImage_.getHeight();
    float winW = getWindowWidth();
    float winH = getWindowHeight();

    // Calculate fit scale
    float fitScale = min(winW / imgW, winH / imgH);
    float scale = fitScale * zoomLevel_;

    float drawW = imgW * scale;
    float drawH = imgH * scale;

    // Clamp pan offset to keep image within window
    if (drawW <= winW) {
        // Image fits horizontally - center it
        panOffset_.x = 0;
    } else {
        // Image larger than window - limit pan so edges don't go past window
        float maxPanX = (drawW - winW) / 2;
        panOffset_.x = clamp(panOffset_.x, -maxPanX, maxPanX);
    }

    if (drawH <= winH) {
        // Image fits vertically - center it
        panOffset_.y = 0;
    } else {
        float maxPanY = (drawH - winH) / 2;
        panOffset_.y = clamp(panOffset_.y, -maxPanY, maxPanY);
    }

    // Center position with pan offset
    float x = (winW - drawW) / 2 + panOffset_.x;
    float y = (winH - drawH) / 2 + panOffset_.y;

    setColor(1.0f, 1.0f, 1.0f);
    fullImage_.draw(x, y, drawW, drawH);

    // Info overlay
    setColor(0.8f, 0.8f, 0.85f);
    auto& entry = library_.getEntry(selectedIndex_);
    drawBitmapString(entry.getFileName(), 10, 20);
    drawBitmapString(format("{}x{}  Zoom: {:.0f}%",
        (int)imgW, (int)imgH, zoomLevel_ * 100), 10, 40);

    // Navigation hint
    setColor(0.5f, 0.5f, 0.55f);
    drawBitmapString("ESC: Back  Left/Right: Navigate  Scroll: Zoom  Drag: Pan", 10, 60);
}
